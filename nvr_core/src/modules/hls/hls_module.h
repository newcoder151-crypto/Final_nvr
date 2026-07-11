/**
 * @file hls_module.h
 * @brief HLS (HTTP Live Streaming) segmenter module
 *
 * Converts finished MP4 segments (produced by the recorder module) into
 * HLS-compatible .ts segments and keeps a rolling m3u8 playlist per camera.
 *
 * Design:
 *   - RecorderModule calls hls_on_segment_ready() when an MP4 is done.
 *   - A work queue (ring buffer) decouples the recorder thread from the
 *     HLS transcoder thread(s).
 *   - One HLS worker thread per camera processes the queue:
 *       MP4 file -> ffmpeg/GStreamer pipeline -> N?.ts + update m3u8
 *   - Sliding window playlist (hls_window_size segments).
 *   - Old .ts files deleted as they fall out of the window.
 *
 * Output layout (per camera):
 *   /storage/hls/cam_N/
 *       stream.m3u8          <- live playlist (always current)
 *       seg_YYYYMMDD_HHMMSS_00001.ts
 *       seg_YYYYMMDD_HHMMSS_00002.ts
 *       ...
 */

#ifndef HLS_MODULE_H
#define HLS_MODULE_H

#include "mnvr_system.h"

#define HLS_WORK_QUEUE_SIZE  64

typedef struct {
    char    mp4_path[MNVR_MAX_PATH];
    int     camera_id;
    int64_t recording_id;   /* recordings.recording_id for DB update */
} HlsWorkItem;

/* Per-camera HLS state */
typedef struct {
    int      camera_id;
    char     hls_dir[MNVR_MAX_PATH];
    char     playlist_path[MNVR_MAX_PATH];

    /* Ring work queue (producer = recorder callback, consumer = hls thread) */
    HlsWorkItem work_queue[HLS_WORK_QUEUE_SIZE];
    volatile int q_head;
    volatile int q_tail;
    pthread_mutex_t q_mutex;
    pthread_cond_t  q_cond;

    pthread_t       thread;
    volatile bool   running;

    /* Playlist state */
    char    segment_files[MNVR_HLS_MAX_SEGMENTS * 2][MNVR_MAX_PATH]; /* ring */
    float   segment_durations[MNVR_HLS_MAX_SEGMENTS * 2];
    int     seg_count;        /* total segments ever written */
    int     seg_window_start; /* first segment in current playlist */
    uint32_t media_sequence;

    AppContext *ctx;
} CamHlsContext;

struct HlsModule {
    AppContext       *ctx;
    CamHlsContext     cams[MNVR_MAX_CAMERAS];
    int               num_cams;
    pthread_mutex_t   mutex;
};

/* ---- Lifecycle ---- */
HlsModule *hls_module_create(AppContext *ctx);
MnvrResult hls_module_start(HlsModule *hm);
void       hls_module_stop(HlsModule *hm);
void       hls_module_destroy(HlsModule *hm);

/* ---- Called by recorder's OnSegmentComplete callback ---- */
void hls_on_segment_ready(int camera_id, const char *mp4_path,
                           int64_t recording_id, void *user_data);

/* ---- Hot-add a camera at runtime ---- */
MnvrResult hls_add_camera(HlsModule *hm, const CameraInfo *info);

#endif /* HLS_MODULE_H */