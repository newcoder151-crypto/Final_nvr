/**
 * @file logger.h
 * @brief Async ring-buffer logger module
 *
 * Thread-safe, non-blocking logger backed by a fixed ring buffer.
 * A dedicated background thread drains the buffer to file / syslog.
 *
 * Usage:
 *   LoggerModule *log = logger_create(&cfg);
 *   logger_start(log);
 *   mnvr_log(ctx, LOG_LEVEL_INFO, "MAIN", "System started");
 *   logger_stop(log);
 *   logger_destroy(log);
 */

#ifndef LOGGER_H
#define LOGGER_H

#include "mnvr_system.h"
#include <stdio.h>

#define LOGGER_RING_SIZE    4096   /* Number of queued log entries */
#define LOGGER_MAX_FILE_MB  50     /* Rotate log file at this size */
#define LOGGER_MAX_FILES    5      /* Keep N rotated log files */

typedef struct {
    char     log_dir[MNVR_MAX_PATH];  /* Directory for log files */
    LogLevel min_level;               /* Minimum level to record */
    bool     log_to_stderr;           /* Also print to stderr */
    bool     log_to_syslog;           /* Also send to syslog */
    size_t   max_file_mb;
    int      max_files;
} LoggerConfig;

/* Internal ring-buffer entry */
typedef struct {
    time_t    ts;
    LogLevel  level;
    char      module[32];
    char      message[MNVR_MAX_LOG_MSG];
} LogEntry;

struct LoggerModule {
    LoggerConfig    cfg;
    LogEntry        ring[LOGGER_RING_SIZE];
    volatile int    head;          /* Producer writes here */
    volatile int    tail;          /* Consumer reads here */
    pthread_mutex_t mutex;
    pthread_cond_t  cond;
    pthread_t       thread;
    volatile bool   running;
    FILE           *log_file;
    char            current_log_path[MNVR_MAX_PATH + 32];
    uint64_t        bytes_written;
    int             file_index;
};

/* ---- Lifecycle ---- */
LoggerModule *logger_create(const LoggerConfig *cfg);
MnvrResult    logger_start(LoggerModule *log);
void          logger_stop(LoggerModule *log);
void          logger_destroy(LoggerModule *log);

/* ---- Write (called via mnvr_log macro) ---- */
void          logger_write(LoggerModule *log, LogLevel level,
                           const char *module, const char *msg);

/* ---- Flush all pending entries synchronously ---- */
void          logger_flush(LoggerModule *log);

#endif /* LOGGER_H */
