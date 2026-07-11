/**
 * @file health_module.h
 * @brief System and camera health monitoring module
 *
 * Runs a background thread that polls system metrics (CPU, RAM, disk,
 * network) and camera connectivity, persists to the DB, and acts as a
 * watchdog for recorder threads.
 *
 * Responsibilities:
 *   1. Poll /proc for CPU/RAM every health_poll_interval_sec.
 *   2. Check disk usage of storage_base and hls_base.
 *   3. Ping each camera RTSP endpoint; update camera health DB table.
 *   4. Watchdog: if a recorder thread has died, attempt restart.
 *   5. Storage cleanup: delete recordings older than retention_days.
 *   6. Write all metrics to system_health and camera_health DB tables.
 */

#ifndef HEALTH_MODULE_H
#define HEALTH_MODULE_H

#include "mnvr_system.h"

typedef struct {
    /* System metrics */
    float    cpu_usage_pct;
    float    mem_usage_pct;
    float    disk_usage_pct;      /* storage_base volume */
    float    hls_disk_usage_pct;
    uint64_t disk_free_bytes;
    uint64_t disk_total_bytes;
    float    net_rx_mbps;
    float    net_tx_mbps;
    time_t   sampled_at;

    /* Per-camera status */
    struct {
        int   camera_id;
        bool  reachable;
        int   consecutive_failures;
        float stream_fps;
        int   pipeline_restarts;
    } cam_health[MNVR_MAX_CAMERAS];
    int num_cams;
} HealthSnapshot;

struct HealthModule {
    AppContext      *ctx;
    pthread_t        thread;
    volatile bool    running;
    int              poll_interval_sec;
    HealthSnapshot   last;
    pthread_mutex_t  mutex;
};

/* ---- Lifecycle ---- */
HealthModule *health_module_create(AppContext *ctx);
MnvrResult    health_module_start(HealthModule *hm);
void          health_module_stop(HealthModule *hm);
void          health_module_destroy(HealthModule *hm);

/* ---- Snapshot query (thread-safe copy) ---- */
void health_get_snapshot(HealthModule *hm, HealthSnapshot *out);

/* ---- Triggered by API / external call ---- */
MnvrResult health_run_storage_cleanup(HealthModule *hm);

#endif /* HEALTH_MODULE_H */
