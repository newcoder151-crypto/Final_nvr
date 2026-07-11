/**
 * @file health_module.c
 * @brief Health monitoring - system metrics, camera watchdog, storage cleanup
 *
 * Thread: health_poll_thread runs every poll_interval_sec:
 *   1. Read /proc/stat  -> CPU usage
 *   2. Read /proc/meminfo -> RAM usage
 *   3. statvfs(storage_base) -> disk usage
 *   4. TCP connect to each camera RTSP port -> reachability
 *   5. Check recorder_camera_is_recording() -> watchdog restart
 *   6. Write snapshot to DB (system_health table)
 *   7. Periodic storage cleanup (every 6 hours)
 */

#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include "health_module.h"
#include "../logger/logger.h"
#include "../config/config_module.h"
#include "../recorder/recorder_module.h"
#include "../../db/db_module.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/statvfs.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>
#include <inttypes.h>

/* -------------------------------------------------------------------------
 * CPU usage from /proc/stat
 * ------------------------------------------------------------------------- */
typedef struct { uint64_t user, nice, sys, idle, iowait, irq, softirq; } CpuStat;

static bool read_cpu_stat(CpuStat *s)
{
    FILE *fp = fopen("/proc/stat", "r");
    if (!fp) return false;
    int r = fscanf(fp, "cpu %"SCNu64" %"SCNu64" %"SCNu64" %"SCNu64
                       " %"SCNu64" %"SCNu64" %"SCNu64,
                   &s->user, &s->nice, &s->sys, &s->idle,
                   &s->iowait, &s->irq, &s->softirq);
    fclose(fp);
    return r == 7;
}

static float calc_cpu_usage(const CpuStat *a, const CpuStat *b)
{
    uint64_t idle_a = a->idle + a->iowait;
    uint64_t idle_b = b->idle + b->iowait;
    uint64_t total_a = a->user + a->nice + a->sys + idle_a + a->irq + a->softirq;
    uint64_t total_b = b->user + b->nice + b->sys + idle_b + b->irq + b->softirq;
    uint64_t dt = total_b - total_a;
    if (dt == 0) return 0.0f;
    return 100.0f * (float)(dt - (idle_b - idle_a)) / (float)dt;
}

/* -------------------------------------------------------------------------
 * RAM usage from /proc/meminfo
 * ------------------------------------------------------------------------- */
static float read_mem_usage(void)
{
    FILE *fp = fopen("/proc/meminfo", "r");
    if (!fp) return 0.0f;

    uint64_t total = 0, available = 0;
    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        uint64_t val;
        if (sscanf(line, "MemTotal: %"SCNu64" kB", &val)     == 1) total     = val;
        if (sscanf(line, "MemAvailable: %"SCNu64" kB", &val) == 1) available = val;
    }
    fclose(fp);
    if (total == 0) return 0.0f;
    return 100.0f * (float)(total - available) / (float)total;
}

/* -------------------------------------------------------------------------
 * Disk usage via statvfs
 * ------------------------------------------------------------------------- */
static float read_disk_usage(const char *path, uint64_t *free_b, uint64_t *total_b)
{
    struct statvfs st;
    if (statvfs(path, &st) != 0) return 0.0f;
    *total_b = (uint64_t)st.f_blocks * st.f_frsize;
    *free_b  = (uint64_t)st.f_bavail * st.f_frsize;
    if (*total_b == 0) return 0.0f;
    return 100.0f * (float)(*total_b - *free_b) / (float)*total_b;
}

/* -------------------------------------------------------------------------
 * Camera reachability - non-blocking TCP connect to RTSP port
 * ------------------------------------------------------------------------- */
static bool camera_is_reachable(const char *ip, int port, int timeout_ms)
{
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return false;

    /* Set non-blocking */
    int flags = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, flags | O_NONBLOCK);

    struct sockaddr_in addr = {0};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(port);
    inet_pton(AF_INET, ip, &addr.sin_addr);

    connect(s, (struct sockaddr *)&addr, sizeof(addr));

    fd_set wset;
    FD_ZERO(&wset); FD_SET(s, &wset);
    struct timeval tv = { timeout_ms / 1000, (timeout_ms % 1000) * 1000 };
    int rc = select(s + 1, NULL, &wset, NULL, &tv);

    close(s);
    return rc == 1;
}

/* -------------------------------------------------------------------------
 * Storage cleanup - delete MP4/TS files older than retention_days
 * ------------------------------------------------------------------------- */
static int delete_old_files(const char *dir_path, int retention_days)
{
    DIR *dp = opendir(dir_path);
    if (!dp) return 0;

    time_t cutoff = time(NULL) - (time_t)retention_days * 86400;
    int deleted = 0;

    struct dirent *ent;
    while ((ent = readdir(dp))) {
        if (ent->d_name[0] == '.') continue;

        char full[MNVR_MAX_PATH];
        snprintf(full, sizeof(full), "%s/%s", dir_path, ent->d_name);

        struct stat st;
        if (stat(full, &st) != 0) continue;

        if (S_ISDIR(st.st_mode)) {
            deleted += delete_old_files(full, retention_days);
        } else if (S_ISREG(st.st_mode)) {
            /* Match .mp4 and .ts recording files */
            const char *ext = strrchr(ent->d_name, '.');
            if (!ext) continue;
            if (strcmp(ext, ".mp4") != 0 && strcmp(ext, ".ts") != 0) continue;

            if (st.st_mtime < cutoff) {
                if (unlink(full) == 0) deleted++;
            }
        }
    }
    closedir(dp);
    return deleted;
}

/* -------------------------------------------------------------------------
 * Write health snapshot to DB
 * ------------------------------------------------------------------------- */
static void persist_health(AppContext *ctx, const HealthSnapshot *snap)
{
    if (!ctx || !ctx->db) return;

    /* Derive health_status string from thresholds.
     * system_health CONSTRAINT allows: HEALTHY, WARNING, CRITICAL, FAILED */
    const char *health_status = "HEALTHY";
    if (snap->cpu_usage_pct > 90.0f || snap->mem_usage_pct > 95.0f ||
        snap->disk_usage_pct > 95.0f)
        health_status = "CRITICAL";
    else if (snap->cpu_usage_pct > 75.0f || snap->mem_usage_pct > 85.0f ||
             snap->disk_usage_pct > 85.0f)
        health_status = "WARNING";

    /* Write system-level metrics to system_health table */
    char sql[512];
    snprintf(sql, sizeof(sql),
        "INSERT INTO system_health "
        "(timestamp, cpu_usage_percent, memory_usage_percent, health_status) "
        "VALUES (NOW(), %.2f, %.2f, '%s');",
        snap->cpu_usage_pct,
        snap->mem_usage_pct,
        health_status);
    db_async_exec(ctx->db, sql);

    /* Write per-camera reachability to component_health (UPSERT).
     * component_health health_status CONSTRAINT: HEALTHY, DEGRADED, FAILED */
    for (int i = 0; i < snap->num_cams && i < MNVR_MAX_CAMERAS; i++) {
        const char *cam_status  = snap->cam_health[i].reachable ? "RUNNING" : "ERROR";
        const char *cam_health  = snap->cam_health[i].reachable ? "HEALTHY" : "FAILED";

        /* Two separate snprintf to stay within buffer limits */
        char name_buf[32];
        snprintf(name_buf, sizeof(name_buf), "CAM_%d",
                 snap->cam_health[i].camera_id);

        char cam_sql[512];
        snprintf(cam_sql, sizeof(cam_sql),
            "INSERT INTO component_health "
            "(component_name, component_type, status, health_status) "
            "VALUES ('%s', 'CAMERA', '%s', '%s') "
            "ON CONFLICT (component_name) DO UPDATE "
            "SET status=EXCLUDED.status, "
            "health_status=EXCLUDED.health_status, "
            "updated_at=NOW();",
            name_buf, cam_status, cam_health);
        db_async_exec(ctx->db, cam_sql);
    }
}

/* -------------------------------------------------------------------------
 * Health poll thread
 * ------------------------------------------------------------------------- */
static void *health_poll_thread(void *arg)
{
    HealthModule *hm = (HealthModule *)arg;
    AppContext   *ctx = hm->ctx;
    const SystemConfig *cfg = config_get(ctx->config);

    LOG_INFO(ctx, "HEALTH", "Health monitor started (interval %ds)",
             hm->poll_interval_sec);

    CpuStat cpu_prev = {0};
    read_cpu_stat(&cpu_prev);

    time_t last_cleanup = time(NULL);

    while (hm->running) {
        /* Sleep first to allow first CPU delta to be meaningful */
        for (int s = 0; s < hm->poll_interval_sec && hm->running; s++)
            sleep(1);

        if (!hm->running) break;

        HealthSnapshot snap = {0};
        snap.sampled_at = time(NULL);

        /* ---- CPU ---- */
        CpuStat cpu_now = {0};
        if (read_cpu_stat(&cpu_now)) {
            snap.cpu_usage_pct = calc_cpu_usage(&cpu_prev, &cpu_now);
            cpu_prev = cpu_now;
        }

        /* ---- RAM ---- */
        snap.mem_usage_pct = read_mem_usage();

        /* ---- Disk ---- */
        cfg = config_get(ctx->config);   /* re-read in case of reload */
        snap.disk_usage_pct = read_disk_usage(
            cfg ? cfg->storage_base : "/storage",
            &snap.disk_free_bytes, &snap.disk_total_bytes);
        uint64_t hls_free = 0, hls_total = 0;
        snap.hls_disk_usage_pct = read_disk_usage(
            cfg ? cfg->hls_base : "/storage/hls",
            &hls_free, &hls_total);

        /* ---- Cameras ---- */
        pthread_mutex_lock(&ctx->cameras_mutex);
        snap.num_cams = ctx->num_cameras;
        for (int i = 0; i < ctx->num_cameras && i < MNVR_MAX_CAMERAS; i++) {
            CameraInfo *cam = &ctx->cameras[i];
            snap.cam_health[i].camera_id = cam->camera_id;

            /* TCP reachability check on RTSP port */
            bool reach = camera_is_reachable(cam->ip_address, 554, 1500);
            snap.cam_health[i].reachable = reach;

            if (!reach) {
                snap.cam_health[i].consecutive_failures++;
                LOG_WARN(ctx, "HEALTH", "[cam %d] %s unreachable",
                         cam->camera_id, cam->name);
            } else {
                snap.cam_health[i].consecutive_failures = 0;
            }

            /* Recorder watchdog */
            if (ctx->recorder &&
                !recorder_camera_is_recording(ctx->recorder, cam->camera_id)) {
                snap.cam_health[i].pipeline_restarts++;
                LOG_WARN(ctx, "HEALTH",
                         "[cam %d] Recorder not running - attempting restart",
                         cam->camera_id);
                recorder_start_camera(ctx->recorder, cam->camera_id);
            }
        }
        pthread_mutex_unlock(&ctx->cameras_mutex);

        /* ---- Threshold warnings ---- */
        if (snap.cpu_usage_pct > (cfg ? cfg->cpu_warn_threshold : 85.0f))
            LOG_WARN(ctx, "HEALTH", "CPU high: %.1f%%", snap.cpu_usage_pct);
        if (snap.mem_usage_pct > (cfg ? cfg->mem_warn_threshold : 90.0f))
            LOG_WARN(ctx, "HEALTH", "RAM high: %.1f%%", snap.mem_usage_pct);
        if (snap.disk_usage_pct > (cfg ? cfg->disk_warn_threshold : 90.0f))
            LOG_WARN(ctx, "HEALTH", "Disk high: %.1f%%", snap.disk_usage_pct);

        LOG_DEBUG(ctx, "HEALTH",
                  "CPU:%.1f%% MEM:%.1f%% DISK:%.1f%% cams:%d",
                  snap.cpu_usage_pct, snap.mem_usage_pct,
                  snap.disk_usage_pct, snap.num_cams);

        /* ---- Persist to DB ---- */
        persist_health(ctx, &snap);

        /* ---- Update shared snapshot ---- */
        pthread_mutex_lock(&hm->mutex);
        hm->last = snap;
        pthread_mutex_unlock(&hm->mutex);

        /* ---- Storage cleanup every 6 hours ---- */
        if ((time(NULL) - last_cleanup) >= 6 * 3600) {
            health_run_storage_cleanup(hm);
            last_cleanup = time(NULL);
        }
    }

    LOG_INFO(ctx, "HEALTH", "Health monitor stopped");
    return NULL;
}

/* -------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */

HealthModule *health_module_create(AppContext *ctx)
{
    HealthModule *hm = calloc(1, sizeof(HealthModule));
    if (!hm) return NULL;
    hm->ctx = ctx;
    const SystemConfig *cfg = config_get(ctx->config);
    hm->poll_interval_sec = cfg ? cfg->health_poll_interval_sec : 10;
    pthread_mutex_init(&hm->mutex, NULL);
    return hm;
}

MnvrResult health_module_start(HealthModule *hm)
{
    if (!hm) return MNVR_ERR_GENERIC;
    hm->running = true;
    if (pthread_create(&hm->thread, NULL, health_poll_thread, hm) != 0)
        return MNVR_ERR_GENERIC;
    return MNVR_OK;
}

void health_module_stop(HealthModule *hm)
{
    if (!hm) return;
    hm->running = false;
    pthread_join(hm->thread, NULL);
}

void health_module_destroy(HealthModule *hm)
{
    if (!hm) return;
    health_module_stop(hm);
    pthread_mutex_destroy(&hm->mutex);
    free(hm);
}

void health_get_snapshot(HealthModule *hm, HealthSnapshot *out)
{
    if (!hm || !out) return;
    pthread_mutex_lock(&hm->mutex);
    *out = hm->last;
    pthread_mutex_unlock(&hm->mutex);
}

MnvrResult health_run_storage_cleanup(HealthModule *hm)
{
    if (!hm || !hm->ctx) return MNVR_ERR_GENERIC;
    const SystemConfig *cfg = config_get(hm->ctx->config);
    int days = cfg ? cfg->recording_retention_days : 30;
    const char *base = cfg ? cfg->storage_base : "/storage";

    LOG_INFO(hm->ctx, "HEALTH", "Storage cleanup: deleting files older than %d days from %s",
             days, base);

    int n = delete_old_files(base, days);
    LOG_INFO(hm->ctx, "HEALTH", "Storage cleanup: deleted %d file(s)", n);
    return MNVR_OK;
}
