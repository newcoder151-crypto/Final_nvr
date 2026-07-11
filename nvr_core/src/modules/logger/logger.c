/**
 * @file logger.c
 * @brief Async ring-buffer logger implementation
 *
 * Thread model:
 *   - N producer threads  -> logger_write() (non-blocking if ring not full)
 *   - 1 consumer thread   -> logger_drain_thread() -> disk / stderr / syslog
 */

#define _POSIX_C_SOURCE 200809L

#include "logger.h"
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdio.h>
#include <syslog.h>
#include <sys/stat.h>
#include <errno.h>
#include <unistd.h>
#include <time.h>

static const char *level_str[] = {
    "TRACE", "DEBUG", "INFO ", "WARN ", "ERROR", "FATAL"
};

/* -------------------------------------------------------------------------
 * Internal helpers
 * ------------------------------------------------------------------------- */

static void rotate_log_file(LoggerModule *log)
{
    if (log->log_file) {
        fclose(log->log_file);
        log->log_file = NULL;
    }

    /* Shift existing rotated files: .4 -> deleted, .3 -> .4, ??? .1 -> .2 */
    for (int i = log->cfg.max_files - 1; i >= 1; i--) {
        char src[MNVR_MAX_PATH + 32], dst[MNVR_MAX_PATH + 32];
        snprintf(src, sizeof(src), "%s/mnvr.log.%d", log->cfg.log_dir, i);
        snprintf(dst, sizeof(dst), "%s/mnvr.log.%d", log->cfg.log_dir, i + 1);
        rename(src, dst);   /* ignore error - best-effort */
    }

    /* Rename current -> .1 */
    char rotated[MNVR_MAX_PATH + 32];
    snprintf(rotated, sizeof(rotated), "%s/mnvr.log.1", log->cfg.log_dir);
    rename(log->current_log_path, rotated);

    /* Open fresh log */
    log->log_file = fopen(log->current_log_path, "a");
    log->bytes_written = 0;
}

static void open_log_file(LoggerModule *log)
{
    mkdir(log->cfg.log_dir, 0755);   /* ensure directory */
    snprintf(log->current_log_path, sizeof(log->current_log_path),
             "%.500s/mnvr.log", log->cfg.log_dir);
    log->log_file = fopen(log->current_log_path, "a");
    if (!log->log_file)
        fprintf(stderr, "[LOGGER] Cannot open log file: %s\n",
                log->current_log_path);
}

static void write_entry(LoggerModule *log, const LogEntry *e)
{
    /* Format: 2025-10-13 14:22:01 [INFO ] [RECORDER] message */
    struct tm tm_info;
    localtime_r(&e->ts, &tm_info);
    char ts_buf[24];
    strftime(ts_buf, sizeof(ts_buf), "%Y-%m-%d %H:%M:%S", &tm_info);

    char line[MNVR_MAX_LOG_MSG + 128];
    int len = snprintf(line, sizeof(line), "%s [%s] [%-12s] %s\n",
                       ts_buf, level_str[e->level], e->module, e->message);

    /* Write to file */
    if (log->log_file) {
        fwrite(line, 1, len, log->log_file);
        fflush(log->log_file);
        log->bytes_written += len;

        /* Rotate if needed */
        if (log->bytes_written >= log->cfg.max_file_mb * 1024 * 1024)
            rotate_log_file(log);
    }

    /* Write to stderr */
    if (log->cfg.log_to_stderr)
        fputs(line, stderr);

    /* Write to syslog */
    if (log->cfg.log_to_syslog) {
        int priority = LOG_INFO;
        if (e->level >= LOG_LEVEL_ERROR) priority = LOG_ERR;
        else if (e->level == LOG_LEVEL_WARN) priority = LOG_WARNING;
        else if (e->level == LOG_LEVEL_DEBUG) priority = LOG_DEBUG;
        syslog(priority, "%s", line);
    }
}

/* -------------------------------------------------------------------------
 * Consumer thread
 * ------------------------------------------------------------------------- */

static void *logger_drain_thread(void *arg)
{
    LoggerModule *log = (LoggerModule *)arg;

    while (log->running || log->tail != log->head) {
        pthread_mutex_lock(&log->mutex);

        /* Wait for entries or shutdown */
        while (log->tail == log->head && log->running)
            pthread_cond_wait(&log->cond, &log->mutex);

        /* Drain all available entries */
        while (log->tail != log->head) {
            LogEntry entry = log->ring[log->tail % LOGGER_RING_SIZE];
            log->tail++;
            pthread_mutex_unlock(&log->mutex);
            write_entry(log, &entry);
            pthread_mutex_lock(&log->mutex);
        }

        pthread_mutex_unlock(&log->mutex);
    }
    return NULL;
}

/* -------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */

LoggerModule *logger_create(const LoggerConfig *cfg)
{
    LoggerModule *log = calloc(1, sizeof(LoggerModule));
    if (!log) return NULL;

    log->cfg = *cfg;
    if (log->cfg.max_file_mb == 0) log->cfg.max_file_mb = LOGGER_MAX_FILE_MB;
    if (log->cfg.max_files   == 0) log->cfg.max_files   = LOGGER_MAX_FILES;

    pthread_mutex_init(&log->mutex, NULL);
    pthread_cond_init(&log->cond, NULL);

    open_log_file(log);

    if (log->cfg.log_to_syslog)
        openlog("mnvr", LOG_PID | LOG_NDELAY, LOG_DAEMON);

    return log;
}

MnvrResult logger_start(LoggerModule *log)
{
    if (!log) return MNVR_ERR_GENERIC;
    log->running = true;
    if (pthread_create(&log->thread, NULL, logger_drain_thread, log) != 0)
        return MNVR_ERR_GENERIC;
    return MNVR_OK;
}

void logger_write(LoggerModule *log, LogLevel level,
                  const char *module, const char *msg)
{
    if (!log || level < log->cfg.min_level) return;

    pthread_mutex_lock(&log->mutex);

    /* Drop if ring is full (non-blocking producer) */
    if ((log->head - log->tail) >= LOGGER_RING_SIZE) {
        pthread_mutex_unlock(&log->mutex);
        return;   /* drop - avoids producer blocking */
    }

    LogEntry *e = &log->ring[log->head % LOGGER_RING_SIZE];
    e->ts    = time(NULL);
    e->level = level;
    strncpy(e->module,  module ? module : "?", sizeof(e->module)  - 1);
    strncpy(e->message, msg    ? msg    : "",  sizeof(e->message) - 1);

    log->head++;
    pthread_cond_signal(&log->cond);
    pthread_mutex_unlock(&log->mutex);
}

void logger_flush(LoggerModule *log)
{
    if (!log) return;
    /* Spin-wait until ring drains - only for clean shutdown */
    while (log->tail != log->head)
        { struct timespec _ts = {0, 1000000L}; nanosleep(&_ts, NULL); };
}

void logger_stop(LoggerModule *log)
{
    if (!log) return;
    pthread_mutex_lock(&log->mutex);
    log->running = false;
    pthread_cond_signal(&log->cond);
    pthread_mutex_unlock(&log->mutex);
    pthread_join(log->thread, NULL);
}

void logger_destroy(LoggerModule *log)
{
    if (!log) return;
    if (log->log_file) fclose(log->log_file);
    if (log->cfg.log_to_syslog) closelog();
    pthread_mutex_destroy(&log->mutex);
    pthread_cond_destroy(&log->cond);
    free(log);
}

/* -------------------------------------------------------------------------
 * Global entry-point - called by LOG_* macros through mnvr_log()
 * ------------------------------------------------------------------------- */

void mnvr_log(AppContext *ctx, LogLevel level, const char *module,
              const char *fmt, ...)
{
    if (!ctx || !ctx->logger) return;

    char msg[MNVR_MAX_LOG_MSG];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    logger_write(ctx->logger, level, module, msg);
}
