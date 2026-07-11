/**
 * @file onvif_module.h
 * @brief Full ONVIF Profile S implementation for mNVR
 *
 * Implements:
 *   - WS-Discovery (multicast + unicast direct probe)
 *   - WS-UsernameToken Password Digest authentication
 *   - Camera time synchronisation for digest computation
 *   - Device service:  GetDeviceInformation, GetCapabilities,
 *                       GetServices, GetScopes, SystemReboot
 *   - Media service:   GetProfiles, GetStreamUri, GetSnapshotUri,
 *                       GetVideoSources, GetVideoEncoderConfigurations,
 *                       GetVideoEncoderConfiguration,
 *                       SetVideoEncoderConfiguration,
 *                       GetAudioSources, GetAudioEncoderConfigurations
 *   - PTZ service:     ContinuousMove, RelativeMove, AbsoluteMove,
 *                       Stop, GetPresets, GotoPreset, SetPreset,
 *                       RemovePreset, GetStatus, GetConfigurations,
 *                       GetNode, GetNodes
 *   - Imaging service: GetImagingSettings, SetImagingSettings
 *   - Events service:  CreatePullPointSubscription, PullMessages,
 *                       Unsubscribe
 *
 * Configuration:
 *   Global ONVIF settings are read from the  onvif_settings  table.
 *   Per-camera ONVIF credentials come from the  cameras_config_details  table.
 *   Both are managed by the VMS software; the INI file is no longer used
 *   for ONVIF configuration.
 *
 *   cameras_config_details columns mapped to OnvifCameraConfig:
 *
 *     camera_slot     -> slot
 *     ip_address      -> ip
 *     onvif_port      -> port
 *     onvif_username  -> user
 *     onvif_password  -> pass
 *     rtsp_username   -> rtsp_user
 *     rtsp_password   -> rtsp_pass
 *     camera_type     -> camera_type
 *     location        -> location
 *
 *   Global settings (onvif_settings table):
 *     multicast_ip, multicast_port, discovery_interval_sec,
 *     probe_timeout_ms, enable_discovery
 */

#ifndef ONVIF_MODULE_H
#define ONVIF_MODULE_H

#include "mnvr_system.h"

/* =========================================================================
 * Compile-time limits
 * ========================================================================= */
#define ONVIF_MAX_PROFILES       8
#define ONVIF_MAX_PRESETS       64
#define ONVIF_MAX_VIDEO_SRCS     4
#define ONVIF_MAX_EVENT_MSGS    16
#define ONVIF_SOAP_BUF_SIZE  16384
#define ONVIF_RESP_BUF_SIZE  32768

/* =========================================================================
 * Per-camera ONVIF configuration (loaded from mnvr.conf)
 * ========================================================================= */
typedef struct {
    int   slot;                           /* 1..MNVR_MAX_CAMERAS */
    char  ip[64];                         /* camera IP */
    int   port;                           /* ONVIF HTTP port (default 80) */
    char  user[64];                       /* ONVIF username */
    char  pass[64];                       /* ONVIF password */
    char  rtsp_user[64];                  /* RTSP username (for stream URL auth) */
    char  rtsp_pass[64];                  /* RTSP password */
    char  camera_type[16];                /* INTERIOR, EXTERIOR, DOOR, DRIVER_CAB */
    char  location[128];                  /* location description */
    char  xaddr[MNVR_MAX_URL];           /* derived: http://ip:port/onvif/device_service */
    bool  enabled;                        /* false if ip is empty */
} OnvifCameraConfig;

/* =========================================================================
 * Global ONVIF configuration
 * ========================================================================= */
typedef struct {
    char  multicast_ip[64];               /* default 239.255.255.250 */
    int   multicast_port;                 /* default 3702 */
    int   discovery_interval_sec;         /* default 60 */
    int   probe_timeout_ms;               /* default 3000 */
    bool  enable_discovery;               /* default true */

    OnvifCameraConfig cameras[MNVR_MAX_CAMERAS];
    int               num_cameras;        /* count of enabled slots */
} OnvifConfig;

/* =========================================================================
 * Data structures
 * ========================================================================= */

typedef struct {
    char  device_uuid[64];
    char  xaddrs[MNVR_MAX_URL];
    char  ip_address[64];
    char  manufacturer[64];
    char  model[64];
    char  firmware[64];
    char  serial_number[64];
    char  hardware_id[64];
    char  stream_uri[MNVR_MAX_URL];
    /* Dedicated recording-profile stream URI, if the camera exposes a
     * separate profile for it (e.g. tokens/names containing "REC" vs
     * "LIVE" — several BEL-provisioned cameras do this deliberately so
     * recording and live view can each have their own RTSP session
     * instead of racing for one shared connection). Falls back to the
     * same value as stream_uri when no distinct recording profile exists. */
    char  rec_stream_uri[MNVR_MAX_URL];
    char  snapshot_uri[MNVR_MAX_URL];
    bool  ptz_supported;
    bool  audio_supported;
    bool  imaging_supported;
    bool  events_supported;
    time_t discovered_at;
    char  media_service_url[MNVR_MAX_URL];
    char  ptz_service_url[MNVR_MAX_URL];
    char  imaging_service_url[MNVR_MAX_URL];
    char  events_service_url[MNVR_MAX_URL];
    /* First profile details (for auto-registration) */
    int   profile_width;
    int   profile_height;
    int   profile_fps;
    char  profile_encoding[16];           /* "H264", "H265" */
    int   num_profiles;
    int   config_slot;                    /* which mnvr.conf slot (1-16), 0=discovered */
} OnvifDevice;

typedef struct {
    char  token[64];
    char  name[64];
    int   width;
    int   height;
    char  encoding[16];
    int   fps;
    int   bitrate_kbps;
    int   gov_length;
    char  quality[16];
    char  video_source_token[64];
    char  video_encoder_token[64];
    char  audio_encoder_token[64];
    char  ptz_config_token[64];
    bool  has_ptz;
    bool  has_audio;
} OnvifProfile;

typedef struct {
    char  token[64];
    char  name[64];
    float x;
    float y;
    float z;
} OnvifPreset;

typedef struct {
    float pan;
    float tilt;
    float zoom;
    bool  moving;
    char  error[128];
} OnvifPtzStatus;

typedef struct {
    char  token[64];
    int   width;
    int   height;
    float framerate;
} OnvifVideoSource;

typedef struct {
    char  token[64];
    char  name[64];
    char  encoding[16];
    int   width;
    int   height;
    int   fps;
    int   bitrate_kbps;
    int   gov_length;
    float quality;
    char  profile[16];
} OnvifVideoEncoderConfig;

typedef struct {
    float brightness;
    float contrast;
    float saturation;
    float sharpness;
    char  ir_cut_filter[16];
    char  exposure_mode[16];
    float exposure_time;
    float gain;
    char  wb_mode[16];
    float wb_cr_gain;
    float wb_cb_gain;
    bool  backlight_comp;
    bool  wdr_enabled;
    float wdr_level;
} OnvifImagingSettings;

typedef struct {
    char  topic[128];
    char  source[64];
    char  data_name[64];
    char  data_value[128];
    char  timestamp[32];
} OnvifEventMessage;

typedef struct {
    char  subscription_ref[MNVR_MAX_URL];
    char  current_time[32];
    char  termination_time[32];
} OnvifSubscription;

/* =========================================================================
 * Callbacks
 * ========================================================================= */
typedef void (*OnNewDeviceFound)(const OnvifDevice *dev, void *user_data);
typedef void (*OnOnvifEvent)(int camera_slot, const OnvifEventMessage *msg,
                              void *user_data);

/* =========================================================================
 * Module context
 * ========================================================================= */
struct OnvifModule {
    AppContext        *ctx;
    OnvifConfig        config;

    pthread_t          discovery_thread;
    volatile bool      running;

    OnvifDevice        discovered[MNVR_MAX_CAMERAS * 2];
    int                num_discovered;
    pthread_mutex_t    mutex;

    OnNewDeviceFound   on_new_device;
    void              *cb_user_data;

    OnvifSubscription  subscriptions[MNVR_MAX_CAMERAS];
    bool               sub_active[MNVR_MAX_CAMERAS];

    OnOnvifEvent       on_event;
    void              *event_user_data;
};

/* =========================================================================
 * Configuration
 * ========================================================================= */
void onvif_config_load(OnvifConfig *cfg, DbModule *db);
const OnvifCameraConfig *onvif_config_get_camera(const OnvifConfig *cfg,
                                                   int slot);

/* =========================================================================
 * Lifecycle
 * ========================================================================= */
OnvifModule *onvif_module_create(AppContext *ctx,
                                  OnNewDeviceFound cb, void *user_data);
MnvrResult   onvif_module_start(OnvifModule *om);
void         onvif_module_stop(OnvifModule *om);
void         onvif_module_destroy(OnvifModule *om);

void onvif_module_set_event_callback(OnvifModule *om,
                                      OnOnvifEvent cb, void *user_data);

/* =========================================================================
 * Startup probe
 * ========================================================================= */
int onvif_probe_all_configured(OnvifModule *om);

/* =========================================================================
 * Device service
 * ========================================================================= */
MnvrResult onvif_get_device_info(const char *xaddr,
                                  const char *user, const char *pass,
                                  OnvifDevice *out);

MnvrResult onvif_get_capabilities(const char *xaddr,
                                    const char *user, const char *pass,
                                    OnvifDevice *dev);

MnvrResult onvif_get_system_date_time(const char *xaddr,
                                        char *created_utc, size_t len);

MnvrResult onvif_reboot_device(const char *xaddr,
                                const char *user, const char *pass);

/* =========================================================================
 * Media service (Profile S)
 * ========================================================================= */
MnvrResult onvif_get_profiles(const char *xaddr,
                               const char *user, const char *pass,
                               OnvifProfile *profiles, int *count,
                               int max_profiles);

MnvrResult onvif_get_stream_uri(const char *xaddr,
                                 const char *user, const char *pass,
                                 const char *profile_token,
                                 char *uri_out, size_t uri_len);

MnvrResult onvif_get_snapshot_uri(const char *xaddr,
                                    const char *user, const char *pass,
                                    const char *profile_token,
                                    char *uri_out, size_t uri_len);

MnvrResult onvif_get_video_sources(const char *xaddr,
                                     const char *user, const char *pass,
                                     OnvifVideoSource *srcs, int *count,
                                     int max_srcs);

MnvrResult onvif_get_video_encoder_configs(const char *xaddr,
                                             const char *user, const char *pass,
                                             OnvifVideoEncoderConfig *cfgs,
                                             int *count, int max_cfgs);

MnvrResult onvif_get_video_encoder_config(const char *xaddr,
                                            const char *user, const char *pass,
                                            const char *token,
                                            OnvifVideoEncoderConfig *cfg);

MnvrResult onvif_set_video_encoder_config(const char *xaddr,
                                            const char *user, const char *pass,
                                            const OnvifVideoEncoderConfig *cfg);

MnvrResult onvif_get_audio_sources(const char *xaddr,
                                     const char *user, const char *pass,
                                     int *count);

/* =========================================================================
 * PTZ service
 * ========================================================================= */
MnvrResult onvif_ptz_continuous_move(const char *xaddr,
                                      const char *user, const char *pass,
                                      const char *profile_token,
                                      float pan_speed, float tilt_speed,
                                      float zoom_speed);

MnvrResult onvif_ptz_relative_move(const char *xaddr,
                                     const char *user, const char *pass,
                                     const char *profile_token,
                                     float pan, float tilt, float zoom);

MnvrResult onvif_ptz_absolute_move(const char *xaddr,
                                     const char *user, const char *pass,
                                     const char *profile_token,
                                     float pan, float tilt, float zoom);

MnvrResult onvif_ptz_stop(const char *xaddr,
                            const char *user, const char *pass,
                            const char *profile_token,
                            bool stop_pan_tilt, bool stop_zoom);

MnvrResult onvif_ptz_get_presets(const char *xaddr,
                                  const char *user, const char *pass,
                                  const char *profile_token,
                                  OnvifPreset *presets, int *count,
                                  int max_presets);

MnvrResult onvif_ptz_goto_preset(const char *xaddr,
                                   const char *user, const char *pass,
                                   const char *profile_token,
                                   const char *preset_token);

MnvrResult onvif_ptz_set_preset(const char *xaddr,
                                  const char *user, const char *pass,
                                  const char *profile_token,
                                  const char *preset_name,
                                  char *preset_token_out, size_t token_len);

MnvrResult onvif_ptz_remove_preset(const char *xaddr,
                                     const char *user, const char *pass,
                                     const char *profile_token,
                                     const char *preset_token);

MnvrResult onvif_ptz_get_status(const char *xaddr,
                                  const char *user, const char *pass,
                                  const char *profile_token,
                                  OnvifPtzStatus *status);

MnvrResult onvif_ptz_goto_home(const char *xaddr,
                                 const char *user, const char *pass,
                                 const char *profile_token);

MnvrResult onvif_ptz_set_home(const char *xaddr,
                                const char *user, const char *pass,
                                const char *profile_token);

/* =========================================================================
 * Imaging service
 * ========================================================================= */
MnvrResult onvif_imaging_get_settings(const char *xaddr,
                                        const char *user, const char *pass,
                                        const char *video_source_token,
                                        OnvifImagingSettings *settings);

MnvrResult onvif_imaging_set_settings(const char *xaddr,
                                        const char *user, const char *pass,
                                        const char *video_source_token,
                                        const OnvifImagingSettings *settings);

/* =========================================================================
 * Events service
 * ========================================================================= */
MnvrResult onvif_events_subscribe(const char *xaddr,
                                    const char *user, const char *pass,
                                    int timeout_sec,
                                    OnvifSubscription *sub);

MnvrResult onvif_events_pull(const char *subscription_ref,
                               const char *user, const char *pass,
                               int timeout_sec, int max_messages,
                               OnvifEventMessage *msgs, int *count);

MnvrResult onvif_events_unsubscribe(const char *subscription_ref,
                                      const char *user, const char *pass);

/* =========================================================================
 * Discovery results
 * ========================================================================= */
int onvif_get_discovered(OnvifModule *om, OnvifDevice *out, int max_count);

/* =========================================================================
 * Direct probe — query a single known IP
 * ========================================================================= */
MnvrResult onvif_probe_direct(const char *ip_address, int port,
                               const char *user, const char *pass,
                               OnvifDevice *out);

#endif /* ONVIF_MODULE_H */