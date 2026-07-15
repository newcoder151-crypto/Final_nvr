/**
 * @file streamer_module.h
 * @brief Live camera re-streaming module for web application
 *
 * For each active camera, creates an RTSP re-stream that re-publishes
 * the camera's RTSP feed so multiple web clients can watch the live
 * view without each opening a direct connection to the camera.
 *
 * Architecture:
 *   Camera RTSP feed
 *       ???
 *       ???
 *   GStreamer pipeline (one per camera):
 *     rtspsrc -> rtph264depay -> h264parse
 *             -> tee
 *               ????????? queue -> rtph264pay -> udpsink  (RTSP server)
 *               ????????? queue -> appsink                (frame capture for AI)
 *       ???
 *       ???
 *   GstRTSPServer  (port 8554)
 *       ???
 *       ???
 *   Web app (via HLS or WebRTC bridge)
 *
 * HLS live view is provided by the HLS module (separate).
 * This module provides low-latency RTSP re-stream + per-frame
 * callback for the AI module.
 */

#ifndef STREAMER_MODULE_H
#define STREAMER_MODULE_H

#include "mnvr_system.h"
#include <gst/gst.h>

/* Called with each decoded YUV frame for AI processing.
 * data: I420 plane data, width?height pixels.
 * Callback must return quickly (copy data if needed). */

/* YUV frame passed to AI module (I420 format) */
typedef struct {
    int             camera_id;
    int             width;
    int             height;
    uint64_t        pts_ms;
    const uint8_t  *y;
    const uint8_t  *u;
    const uint8_t  *v;
} YuvFrame;

typedef void (*OnFrameCallback)(int camera_id,
                                 const uint8_t *y_plane,
                                 int width, int height,
                                 uint64_t pts_ms,
                                 void *user_data);

typedef struct {
    int          camera_id;
    char         camera_name[MNVR_MAX_NAME];
    char         rtsp_url[MNVR_MAX_URL];
    char         rtsp_username[64];    /* set via user-id property — safe with any chars */
    char         rtsp_password[128];   /* set via user-pw property — safe with @, :, etc. */
    int          listen_port;         /* e.g. 8554 */
    char         mount_point[64];     /* e.g. /cam_1 */

    GstElement  *pipeline;
    GstElement  *rtspsrc;
    GstElement  *depay;
    GstElement  *parser;
    GstElement  *tee;
    GstElement  *decode_queue;
    GstElement  *decoder;            /* avdec_h264 */
    GstElement  *videoconvert;
    GstElement  *appsink;            /* for AI frame callback */
    GstElement  *raw_tee;            /* splits decoded video: AI vs. watermark+restream */
    GstElement  *overlay_queue;
    GstElement  *clock_overlay;      /* clockoverlay: date/time burned into frame */
    GstElement  *gps_overlay;        /* textoverlay: GPS + speed, text updated periodically */
    GstElement  *overlay_convert;    /* videoconvert back to encoder-friendly format */
    GstElement  *stream_encoder;     /* x264enc: re-encode watermarked video for restream */
    gint64       gps_overlay_last_update_us; /* monotonic; refresh text ~1x/sec from a pad probe */
    GstElement  *stream_queue;
    GstElement  *rtppay;             /* rtph264pay */
    GstElement  *udpsink;

    GMainLoop   *loop;
    pthread_t    thread;
    volatile bool running;

    OnFrameCallback on_frame;
    void           *frame_user_data;

    /* Live telemetry for the watermark overlay. Updated by whatever module
     * owns the GPS receiver (e.g. via streamer_update_telemetry()); read
     * once per second by the gps_overlay_timer_id callback. has_fix=false
     * renders "GPS: NO FIX" instead of stale/zero coordinates. */
    volatile double gps_lat;
    volatile double gps_lon;
    volatile double gps_speed_kmh;
    volatile bool   gps_has_fix;

    AppContext   *ctx;
    /* Consecutive failure count, drives exponential retry backoff — see
     * streamer_thread_fn(). Resets to 0 on any successful connection. */
    int retry_count;
} CamStreamer;

/**
 * Update the GPS/speed telemetry shown in the live-view and recording
 * watermark. Call this from the GPS/telemetry module whenever a new fix
 * arrives. Thread-safe (plain aligned writes; overlay timer reads the
 * latest value, brief staleness is acceptable for an on-screen readout).
 */
void streamer_update_telemetry(CamStreamer *cam, double lat, double lon,
                                double speed_kmh, bool has_fix);

struct StreamerModule {
    AppContext    *ctx;
    CamStreamer    cams[MNVR_MAX_CAMERAS];
    int            num_cams;
    pthread_mutex_t mutex;

    /* Shared frame callback (forwarded to AI module) */
    OnFrameCallback on_frame;
    void           *frame_user_data;
};

/* ---- Lifecycle ---- */
StreamerModule *streamer_module_create(AppContext *ctx,
                                        OnFrameCallback cb, void *user_data);
MnvrResult      streamer_module_start(StreamerModule *sm);
void            streamer_module_stop(StreamerModule *sm);
void            streamer_module_destroy(StreamerModule *sm);

/* ---- Query stream URL for a camera ---- */
const char *streamer_get_url(StreamerModule *sm, int camera_id);

/* ---- Hot-add a camera at runtime ---- */
MnvrResult streamer_add_camera(StreamerModule *sm, const CameraInfo *info);

#endif /* STREAMER_MODULE_H */
