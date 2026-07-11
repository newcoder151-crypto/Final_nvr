/**
 * @file config_module.h
 * @brief Configuration module - INI file + PostgreSQL config reader
 *
 * Reads the system INI file (mnvr.conf) and merges it with the
 * persisted configuration stored in PostgreSQL
 * (system_config + cameras tables).  Provides typed accessors
 * and a reload mechanism (SIGHUP).
 *
 * Precedence (highest first):
 *   1. Command-line overrides
 *   2. mnvr.conf [override] section
 *   3. PostgreSQL system_config table
 *   4. mnvr.conf [default] / compiled-in defaults
 */

#ifndef CONFIG_MODULE_H
#define CONFIG_MODULE_H

#include "mnvr_system.h"

/* -------------------------------------------------------------------------
 * Flat config values loaded from INI + DB
 * ------------------------------------------------------------------------- */
typedef struct {
    /* ---- paths ---- */
    char storage_base[MNVR_MAX_PATH];
    char hls_base[MNVR_MAX_PATH];
    char db_path[MNVR_MAX_PATH];
    char log_dir[MNVR_MAX_PATH];

    /* ---- device identity ---- */
    char device_id[MNVR_MAX_NAME];
    char device_name[MNVR_MAX_NAME];

    /* ---- recording ---- */
    int  recording_retention_days;
    int  segment_duration_sec;      /* MP4 splitmux segment duration */
    int  segment_max_size_mb;       /* MP4 splitmux max size (0=unlimited) */
    bool enable_audio;
    bool enable_watermark;

    /* ---- HLS ---- */
    int  hls_segment_sec;           /* Target HLS segment duration */
    int  hls_window_size;           /* HLS playlist sliding window */
    bool hls_delete_old_segments;

    /* ---- streaming ---- */
    int  rtsp_server_port;
    int  webrtc_server_port;

    /* ---- AI ---- */
    bool enable_face_detection;
    bool enable_motion_detection;
    bool enable_rdas;
    float motion_threshold;         /* 0.0 - 1.0 */
    char face_model_path[MNVR_MAX_PATH];
    int  face_detect_interval;      /* Run face detector every N frames */

    /* ---- API server ---- */
    int  api_port;
    bool api_tls_enabled;
    char api_tls_cert[MNVR_MAX_PATH];
    char api_tls_key[MNVR_MAX_PATH];

    /* ---- time sync ---- */
    char time_sync_method[16];      /* "PTP", "NTP", "GPS" */
    char ntp_server[128];
    int  ptp_domain;

    /* ---- GPS telemetry (date/time + GPS/speed watermark) ---- */
    bool gps_enabled;
    char gps_serial_device[MNVR_MAX_PATH];  /* e.g. /dev/ttyUSB0 */
    int  gps_baud_rate;                     /* e.g. 9600 */

    /* ---- health ---- */
    int  health_poll_interval_sec;
    float cpu_warn_threshold;
    float mem_warn_threshold;
    float disk_warn_threshold;

    /* ---- log ---- */
    LogLevel log_min_level;
    bool log_to_syslog;
} SystemConfig;

struct ConfigModule {
    SystemConfig    sys;
    char            ini_path[MNVR_MAX_PATH];
    AppContext      *ctx;
    pthread_mutex_t mutex;
    volatile bool   dirty;       /* Set by SIGHUP handler, triggers reload */
};

/* ---- Lifecycle ---- */
ConfigModule *config_create(const char *ini_path, AppContext *ctx);
MnvrResult    config_load(ConfigModule *cfg);    /* Reads INI then DB */
MnvrResult    config_reload(ConfigModule *cfg);  /* Same but thread-safe */
void          config_destroy(ConfigModule *cfg);

/* ---- Accessors ---- */
const SystemConfig *config_get(ConfigModule *cfg);
bool  config_get_bool  (ConfigModule *cfg, const char *key, bool   def);
int   config_get_int   (ConfigModule *cfg, const char *key, int    def);
const char *config_get_str(ConfigModule *cfg, const char *key, const char *def);

/* ---- Camera loader (fills ctx->cameras[] from DB) ---- */
MnvrResult config_load_cameras(ConfigModule *cfg);

/* ---- SIGHUP trigger ---- */
void config_mark_dirty(ConfigModule *cfg);

#endif /* CONFIG_MODULE_H */
