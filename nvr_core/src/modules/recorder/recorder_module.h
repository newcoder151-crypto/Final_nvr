/**
 * @file recorder_module.h
 * @brief GStreamer-based multi-camera recorder module
 *
 * ADAPTER between the mNVR module system and the original standalone
 * gstreamer-nvr-recorder-module (manager.c / recorder.c / callbacks.c / utils.c).
 *
 * The original module files are compiled alongside this file.
 * See recorder_module.c for full integration notes.
 *
 * Pipeline per camera (original module, H.264 or H.265):
 *   rtspsrc -> rtpXXXdepay -> XXXparse -> queue -> splitmuxsink(mp4mux)
 *                                                          |
 *                                          "format-location" signal wrapper
 *                                                          |
 *                                              OnSegmentComplete callback
 *                                                          |
 *                                                   HLS module
 */

#ifndef RECORDER_MODULE_H
#define RECORDER_MODULE_H

#include "mnvr_system.h"
#include <gst/gst.h>

/**
 * Called by the recorder each time a new MP4 segment is complete.
 * file_path is the absolute path to the finished segment.
 * user_data is whatever was passed to recorder_module_create().
 */
typedef void (*OnSegmentComplete)(int camera_id, const char *file_path,
                                  void *user_data);

/**
 * Per-camera adapter context.
 * Holds mNVR-side state alongside a pointer to the original CameraRecorder.
 */
typedef struct {
    int          camera_id;
    char         camera_name[MNVR_MAX_NAME];
    char         output_dir[MNVR_MAX_PATH];

    /* Pointer to the original CameraRecorder struct (opaque here) */
    void        *_orig_rec;

    volatile bool is_recording;

    /* Callbacks */
    OnSegmentComplete on_segment_complete;
    void             *cb_user_data;

    /* Track previous segment path for completion notification */
    char              _prev_segment_path[MNVR_MAX_PATH];

    AppContext   *ctx;
} CamRecContext;

struct RecorderModule {
    AppContext       *ctx;
    CamRecContext     cams[MNVR_MAX_CAMERAS];
    int               num_cams;
    pthread_mutex_t   mutex;

    /* Pointer to the original RecorderManager (opaque here) */
    void             *_internal_manager;

    OnSegmentComplete on_segment_complete;
    void             *cb_user_data;
};

/* ---- Lifecycle ---- */
RecorderModule *recorder_module_create(AppContext *ctx,
                                        OnSegmentComplete cb,
                                        void *user_data);
MnvrResult      recorder_module_start(RecorderModule *rm);
void            recorder_module_stop(RecorderModule *rm);
void            recorder_module_destroy(RecorderModule *rm);

/* ---- Per-camera control (used by health watchdog) ---- */
MnvrResult recorder_start_camera(RecorderModule *rm, int camera_id);
void       recorder_stop_camera(RecorderModule *rm, int camera_id);
bool       recorder_camera_is_recording(RecorderModule *rm, int camera_id);

/* ---- Hot-add a camera at runtime (used by ONVIF auto-register) ---- */
MnvrResult recorder_add_camera(RecorderModule *rm, const CameraInfo *info);

#endif /* RECORDER_MODULE_H */
