/**
 * @file mnvr_system.h
 * @brief mNVR System - Master header for all modules
 *
 * Defines shared types, constants, inter-module interfaces, and
 * the central AppContext structure that binds all modules together.
 *
 * Architecture:
 *   main.c
 *     ????????? config_module     (INI + DB config)
 *     ????????? logger_module     (ring-buffer async log)
 *     ????????? db_module         (PostgreSQL via libpq)
 *     ????????? health_module     (system + camera health thread)
 *     ????????? onvif_module      (ONVIF discovery + management)
 *     ????????? recorder_module   (GStreamer RTSP->MP4, per-camera thread)
 *     ????????? hls_module        (MP4->HLS segmenter thread)
 *     ????????? streamer_module   (RTSP re-stream for web live view)
 *     ????????? ai_module         (face detection, motion, RDAS)
 *     ????????? api_module        (REST/WebSocket HTTP server)
 */

#ifndef MNVR_SYSTEM_H
#define MNVR_SYSTEM_H

#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include <time.h>

/* =========================================================================
 * Version & Build
 * ========================================================================= */
#define MNVR_VERSION_MAJOR  1
#define MNVR_VERSION_MINOR  0
#define MNVR_VERSION_PATCH  0
#define MNVR_VERSION_STR    "1.0.0"

/* =========================================================================
 * Compile-time limits
 * ========================================================================= */
#define MNVR_MAX_CAMERAS        16
#define MNVR_MAX_MICS           8
#define MNVR_MAX_PATH           512
#define MNVR_MAX_NAME           64
#define MNVR_MAX_URL            256
#define MNVR_MAX_LOG_MSG        1024
#define MNVR_HLS_MAX_SEGMENTS   10       /* Sliding window in playlist */
#define MNVR_HLS_SEGMENT_SEC    4        /* HLS target segment duration */

/* =========================================================================
 * Return codes
 * ========================================================================= */
typedef enum {
    MNVR_OK             =  0,
    MNVR_ERR_GENERIC    = -1,
    MNVR_ERR_NOMEM      = -2,
    MNVR_ERR_IO         = -3,
    MNVR_ERR_CONFIG     = -4,
    MNVR_ERR_DB         = -5,
    MNVR_ERR_GST        = -6,
    MNVR_ERR_NETWORK    = -7,
    MNVR_ERR_BUSY       = -8,
} MnvrResult;

/* =========================================================================
 * Camera state
 * ========================================================================= */
typedef enum {
    CAM_STATE_UNKNOWN    = 0,
    CAM_STATE_ACTIVE,
    CAM_STATE_INACTIVE,
    CAM_STATE_FAULTY,
    CAM_STATE_RECORDING,
    CAM_STATE_STREAMING,
} CameraState;

/* =========================================================================
 * Log levels (mirrors syslog severity)
 * ========================================================================= */
typedef enum {
    LOG_LEVEL_TRACE = 0,
    LOG_LEVEL_DEBUG,
    LOG_LEVEL_INFO,
    LOG_LEVEL_WARN,
    LOG_LEVEL_ERROR,
    LOG_LEVEL_FATAL,
} LogLevel;

/* =========================================================================
 * Camera descriptor (runtime, populated from DB/config)
 * ========================================================================= */
typedef struct {
    int          camera_id;
    char         name[MNVR_MAX_NAME];
    char         rtsp_url[MNVR_MAX_URL];
    /* Dedicated recording-profile RTSP URL (e.g. camera's own "REC"
     * profile, separate from the "LIVE" one in rtsp_url above). Falls
     * back to rtsp_url when the camera has no distinct recording profile.
     * recorder.c connects here; streamer_module.c still connects to
     * rtsp_url — two separate RTSP sessions/profiles so they never
     * compete for the same connection slot on the camera. */
    char         rec_rtsp_url[MNVR_MAX_URL];
    char         onvif_device_id[MNVR_MAX_NAME];
    char         ip_address[64];
    int          resolution_width;
    int          resolution_height;
    int          target_fps;
    char         codec[16];               /* "H.264" or "H.265" */
    char         location_desc[128];
    char         rtsp_username[64];       /* kept separate from URL to handle special chars */
    char         rtsp_password[128];      /* kept separate from URL (may contain @, :, etc.)  */
    CameraState  state;
    bool         audio_enabled;
    bool         ptz_supported;
    time_t       last_seen;
    /* Derived paths set at startup */
    char         rec_output_dir[MNVR_MAX_PATH];   /* /storage/cam_N/ */
    char         hls_output_dir[MNVR_MAX_PATH];   /* /storage/hls/cam_N/ */
    char         stream_url[MNVR_MAX_URL];         /* re-stream URL */
} CameraInfo;

/* =========================================================================
 * Forward declarations for module context types
 * ========================================================================= */
typedef struct ConfigModule    ConfigModule;
typedef struct LoggerModule    LoggerModule;
typedef struct DbModule        DbModule;
typedef struct HealthModule    HealthModule;
typedef struct OnvifModule     OnvifModule;
typedef struct RecorderModule  RecorderModule;
typedef struct HlsModule       HlsModule;
typedef struct StreamerModule  StreamerModule;
typedef struct AiModule        AiModule;
typedef struct RtspRelayModule RtspRelayModule;
typedef struct GpsModule       GpsModule;
typedef struct ApiModule       ApiModule;

/* =========================================================================
 * Central application context - passed to every module
 * ========================================================================= */
typedef struct AppContext {
    /* ---- identity ---- */
    char device_id[MNVR_MAX_NAME];
    char device_name[MNVR_MAX_NAME];
    char firmware_version[32];

    /* ---- paths ---- */
    char config_file[MNVR_MAX_PATH];
    char storage_base[MNVR_MAX_PATH];    /* e.g. /storage */
    char hls_base[MNVR_MAX_PATH];        /* e.g. /storage/hls */
    char db_path[MNVR_MAX_PATH];         /* e.g. /etc/mnvr/mnvr.db */

    /* ---- cameras ---- */
    CameraInfo  cameras[MNVR_MAX_CAMERAS];
    int         num_cameras;
    pthread_mutex_t cameras_mutex;

    /* ---- module handles ---- */
    ConfigModule   *config;
    LoggerModule   *logger;
    DbModule       *db;
    HealthModule   *health;
    OnvifModule    *onvif;
    RecorderModule *recorder;
    HlsModule      *hls;
    StreamerModule *streamer;
    AiModule       *ai;
    ApiModule      *api;
    RtspRelayModule *rtsp_relay;
    GpsModule       *gps;

    /* ---- global shutdown flag ---- */
    volatile bool  shutdown_requested;
    pthread_mutex_t shutdown_mutex;
    pthread_cond_t  shutdown_cond;
} AppContext;

/* =========================================================================
 * Convenience logging macro (resolved at link time to logger_module)
 * ========================================================================= */
void mnvr_log(AppContext *ctx, LogLevel level, const char *module,
              const char *fmt, ...);

#define LOG_TRACE(ctx, mod, ...) mnvr_log(ctx, LOG_LEVEL_TRACE, mod, __VA_ARGS__)
#define LOG_DEBUG(ctx, mod, ...) mnvr_log(ctx, LOG_LEVEL_DEBUG, mod, __VA_ARGS__)
#define LOG_INFO(ctx, mod, ...)  mnvr_log(ctx, LOG_LEVEL_INFO,  mod, __VA_ARGS__)
#define LOG_WARN(ctx, mod, ...)  mnvr_log(ctx, LOG_LEVEL_WARN,  mod, __VA_ARGS__)
#define LOG_ERROR(ctx, mod, ...) mnvr_log(ctx, LOG_LEVEL_ERROR, mod, __VA_ARGS__)
#define LOG_FATAL(ctx, mod, ...) mnvr_log(ctx, LOG_LEVEL_FATAL, mod, __VA_ARGS__)

#endif /* MNVR_SYSTEM_H */