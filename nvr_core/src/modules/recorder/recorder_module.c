/**
 * @file recorder_module.c
 * @brief GStreamer recorder module - ADAPTER wrapping the original gstreamer-nvr-recorder-module
 *
 * Integration design:
 *   The original standalone module (manager.c / recorder.c / callbacks.c / utils.c)
 *   is compiled as-is alongside this file. This adapter:
 *
 *   1. Creates one RecorderManager (original) per mNVR RecorderModule.
 *   2. For each camera in ctx->cameras[], calls manager_add_camera() then
 *      configures the CameraRecorder with segment duration/size from SystemConfig.
 *   3. Calls manager_start_all() which spawns per-camera pthreads + GLib loops.
 *   4. After start, replaces the original "format-location" GStreamer signal
 *      with a wrapper that also notifies the HLS module when each MP4 segment
 *      finishes (detected when the next segment starts).
 *   5. Provides recorder_start_camera() / recorder_stop_camera() for the health
 *      module watchdog to restart dead pipelines.
 *
 * Original module files compiled alongside (see Makefile ORIG_SRCS):
 *   original_gstreamer_module/callbacks.c
 *   original_gstreamer_module/recorder.c
 *   original_gstreamer_module/manager.c
 *   original_gstreamer_module/utils.c
 *   (original config.c is NOT used - RecordingConfig set directly here)
 */

#define _POSIX_C_SOURCE 200809L

#include "recorder_module.h"
#include "../logger/logger.h"
#include "../config/config_module.h"
#include "../../db/db_module.h"

/* Original module public headers */
#include "original_gstreamer_module/manager.h"
#include "original_gstreamer_module/recorder.h"
#include "original_gstreamer_module/callbacks.h"
#include "original_gstreamer_module/utils.h"

#include <gst/gst.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <errno.h>

/* -------------------------------------------------------------------------
 * Segment completion tracking via the "format-location" signal.
 *
 * splitmuxsink emits "format-location" at the START of each new segment,
 * which means the PREVIOUS segment just finished writing.  We wrap the
 * original format_location_callback so we can also notify the mNVR HLS
 * module about the completed segment.
 * ------------------------------------------------------------------------- */

/* Per-camera state to track the previous segment path */
static void on_segment_written(CamRecContext *cam, const char *prev_path)
{
    if (!cam || !prev_path || !prev_path[0]) return;
    LOG_INFO(cam->ctx, "RECORDER", "[%s] Segment closed: %s",
             cam->camera_name, prev_path);
    if (cam->on_segment_complete)
        cam->on_segment_complete(cam->camera_id, prev_path, cam->cb_user_data);
}

/*
 * Replacement format-location callback that wraps the original.
 * It first notifies about the PREVIOUS segment, then delegates to
 * the original format_location_callback for the new filename.
 */
static gchar *format_location_wrapper(GstElement *splitmux,
                                       guint       fragment_id,
                                       gpointer    user_data)
{
    CamRecContext *cam = (CamRecContext *)user_data;
    if (!cam) return NULL;
    CameraRecorder *orig = (CameraRecorder *)cam->_orig_rec;
    if (!orig) return NULL;

    /* Notify about the previous completed segment (fragment_id > 0) */
    if (fragment_id > 0 && cam->_prev_segment_path[0]) {
        on_segment_written(cam, cam->_prev_segment_path);
    }

    /* Call original to generate the new filename */
    gchar *new_path = format_location_callback(splitmux, fragment_id, orig);

    /* Remember this path so we can notify when the NEXT segment starts */
    if (new_path)
        g_strlcpy(cam->_prev_segment_path, new_path,
                  sizeof(cam->_prev_segment_path));

    return new_path;
}

/* Replace the original format-location signal with our wrapper */
static void attach_segment_cb(CamRecContext *cam, CameraRecorder *rec)
{
    if (!rec->splitmux) {
        LOG_WARN(cam->ctx, "RECORDER",
                 "[%s] splitmux element NULL - callback not attached",
                 cam->camera_name);
        return;
    }

    /* Disconnect the original format-location handler that recorder_start()
     * connected, and replace it with our wrapper. */
    g_signal_handlers_disconnect_by_func(rec->splitmux,
                                          G_CALLBACK(format_location_callback),
                                          rec);
    g_signal_connect(rec->splitmux, "format-location",
                     G_CALLBACK(format_location_wrapper), cam);
    cam->_prev_segment_path[0] = '\0';
    LOG_DEBUG(cam->ctx, "RECORDER",
              "[%s] format-location wrapper connected", cam->camera_name);
}

/* -------------------------------------------------------------------------
 * Configure a CameraRecorder from SystemConfig
 * ------------------------------------------------------------------------- */
static void configure_recorder(CameraRecorder *rec,
                                const SystemConfig *cfg,
                                const char *out_prefix,
                                const char *video_codec)
{
    config_init_defaults(&rec->config);
    rec->config.enable_segmentation   = TRUE;
    rec->config.max_file_duration_sec = (guint64)(cfg ? cfg->segment_duration_sec : 900);
    rec->config.max_file_size_mb      = (guint64)(cfg ? cfg->segment_max_size_mb  : 2048);
    rec->config.rtsp_latency_ms       = 200;
    rec->config.rtsp_timeout_sec      = 10;
    rec->config.use_tcp               = TRUE;
    rec->config.add_timestamp         = TRUE;
    rec->config.enable_fragments      = FALSE;  /* fMP4 requires PTS on every buffer; disable for reliability */
    rec->config.fragment_duration_ms  = 0;
    g_strlcpy(rec->config.file_extension, "mp4",
              sizeof(rec->config.file_extension));
    /* output_file is already set by manager_add_camera(); re-confirm */
    g_strlcpy(rec->output_file, out_prefix, sizeof(rec->output_file));
    /* Set video codec so recorder_start picks correct depay/parser */
    g_strlcpy(rec->config.video_codec,
              (video_codec && video_codec[0]) ? video_codec : "H.264",
              sizeof(rec->config.video_codec));
}

/* =========================================================================
 * Public API
 * ========================================================================= */

RecorderModule *recorder_module_create(AppContext *ctx,
                                        OnSegmentComplete cb,
                                        void *user_data)
{
    RecorderModule *rm = calloc(1, sizeof(RecorderModule));
    if (!rm) return NULL;
    rm->ctx                 = ctx;
    rm->on_segment_complete = cb;
    rm->cb_user_data        = user_data;
    pthread_mutex_init(&rm->mutex, NULL);
    return rm;
}

MnvrResult recorder_module_start(RecorderModule *rm)
{
    if (!rm || !rm->ctx) return MNVR_ERR_GENERIC;
    AppContext         *ctx = rm->ctx;
    const SystemConfig *cfg = config_get(ctx->config);

    RecorderManager *mgr = manager_create();
    if (!mgr) { LOG_FATAL(ctx,"RECORDER","manager_create failed"); return MNVR_ERR_NOMEM; }
    rm->_internal_manager = mgr;

    pthread_mutex_lock(&ctx->cameras_mutex);
    rm->num_cams = 0;

    for (int i = 0; i < ctx->num_cameras && rm->num_cams < MNVR_MAX_CAMERAS; i++) {
        CameraInfo *info = &ctx->cameras[i];

        /* Output prefix: dir/cam_N  (callbacks.c appends _YYYYMMDD_HHMMSS_segNNNNN.mp4) */
        char out_prefix[MNVR_MAX_PATH];
        snprintf(out_prefix, sizeof(out_prefix),
                 "%.460s/cam_%d", info->rec_output_dir, info->camera_id);

        /* Ensure output dir exists - pass the file prefix so dirname()
         * extracts the full directory path (e.g. /storage/recordings/cam_1) */
        ensure_output_dir_exists(out_prefix);

        int id = manager_add_camera(mgr, info->name,
                                     info->rec_rtsp_url, out_prefix);
        if (id < 0) {
            LOG_ERROR(ctx,"RECORDER","[%s] manager_add_camera failed",info->name);
            continue;
        }

        /* Configure the CameraRecorder struct the original manager created */
        CameraRecorder *orig = mgr->cameras[id];
        configure_recorder(orig, cfg, out_prefix, info->codec);
        /* Copy credentials separately so recorder_start() can set user-id/user-pw */
        g_strlcpy(orig->rtsp_username, info->rtsp_username, sizeof(orig->rtsp_username));
        g_strlcpy(orig->rtsp_password, info->rtsp_password, sizeof(orig->rtsp_password));

        /* Populate CamRecContext (our adapter state) */
        CamRecContext *cam = &rm->cams[rm->num_cams];
        memset(cam, 0, sizeof(CamRecContext));
        cam->ctx                 = ctx;
        cam->camera_id           = info->camera_id;
        cam->on_segment_complete = rm->on_segment_complete;
        cam->cb_user_data        = rm->cb_user_data;
        cam->_orig_rec           = orig;
        snprintf(cam->camera_name, sizeof(cam->camera_name), "%.63s", info->name);
        snprintf(cam->output_dir,  sizeof(cam->output_dir),  "%.511s", info->rec_output_dir);

        rm->num_cams++;
        LOG_INFO(ctx,"RECORDER","[%s] queued  url=%s  out=%s",
                 info->name, info->rec_rtsp_url, out_prefix);
    }
    pthread_mutex_unlock(&ctx->cameras_mutex);

    if (rm->num_cams == 0) {
        LOG_WARN(ctx,"RECORDER","No cameras to start");
        manager_destroy(mgr); rm->_internal_manager = NULL;
        return MNVR_OK;
    }

    /* Start all cameras - spawns per-camera pthreads + GLib main loops */
    manager_start_all(mgr);

    /* Attach format-location wrapper now that pipelines are PLAYING */
    for (int i = 0; i < rm->num_cams; i++) {
        CamRecContext  *cam  = &rm->cams[i];
        CameraRecorder *orig = (CameraRecorder *)cam->_orig_rec;
        if (orig && orig->is_recording) {
            attach_segment_cb(cam, orig);
            cam->is_recording = true;
        }
    }

    /* Write computed output paths back to cameras table so DB is authoritative */
    if (ctx->db) {
        for (int i = 0; i < rm->num_cams; i++) {
            CamRecContext *cam  = &rm->cams[i];
            CameraInfo    *info = NULL;
            for (int j = 0; j < ctx->num_cameras; j++) {
                if (ctx->cameras[j].camera_id == cam->camera_id)
                    { info = &ctx->cameras[j]; break; }
            }
            if (!info) continue;
            char hls_url[MNVR_MAX_URL];
            snprintf(hls_url, sizeof(hls_url),
                     "/hls/cam_%d/stream.m3u8", cam->camera_id);
            char sql[1536];
            snprintf(sql, sizeof(sql),
                "UPDATE cameras "
                "SET rec_output_dir='%.460s', "
                "    hls_output_dir='%.460s', "
                "    hls_playlist_url='%.240s', "
                "    updated_at=NOW() "
                "WHERE camera_id=%d;",
                info->rec_output_dir, info->hls_output_dir,
                hls_url, cam->camera_id);
            db_async_exec(ctx->db, sql);
            LOG_DEBUG(ctx,"RECORDER","[cam %d] DB paths updated",cam->camera_id);
        }
    }

    LOG_INFO(ctx,"RECORDER","Started %d camera(s) via original GStreamer module",
             rm->num_cams);
    return MNVR_OK;
}

void recorder_module_stop(RecorderModule *rm)
{
    if (!rm || !rm->_internal_manager) return;
    manager_stop_all((RecorderManager *)rm->_internal_manager);
    for (int i = 0; i < rm->num_cams; i++) rm->cams[i].is_recording = false;
    LOG_INFO(rm->ctx,"RECORDER","All cameras stopped");
}

void recorder_module_destroy(RecorderModule *rm)
{
    if (!rm) return;
    recorder_module_stop(rm);
    if (rm->_internal_manager) {
        manager_destroy((RecorderManager *)rm->_internal_manager);
        rm->_internal_manager = NULL;
    }
    pthread_mutex_destroy(&rm->mutex);
    free(rm);
}

MnvrResult recorder_start_camera(RecorderModule *rm, int camera_id)
{
    if (!rm) return MNVR_ERR_GENERIC;
    for (int i = 0; i < rm->num_cams; i++) {
        CamRecContext  *cam  = &rm->cams[i];
        CameraRecorder *orig = (CameraRecorder *)cam->_orig_rec;
        if (cam->camera_id != camera_id || !orig) continue;
        if (orig->is_recording) return MNVR_OK;
        if (recorder_start(orig) == 0) {
            cam->is_recording = true;
            attach_segment_cb(cam, orig);
            LOG_INFO(rm->ctx,"RECORDER","[cam %d] Restarted by watchdog", camera_id);
            return MNVR_OK;
        }
        return MNVR_ERR_GST;
    }
    return MNVR_ERR_GENERIC;
}

void recorder_stop_camera(RecorderModule *rm, int camera_id)
{
    if (!rm) return;
    for (int i = 0; i < rm->num_cams; i++) {
        CamRecContext  *cam  = &rm->cams[i];
        CameraRecorder *orig = (CameraRecorder *)cam->_orig_rec;
        if (cam->camera_id != camera_id || !orig) continue;
        recorder_stop(orig);
        cam->is_recording = false;
        return;
    }
}

bool recorder_camera_is_recording(RecorderModule *rm, int camera_id)
{
    if (!rm) return false;
    for (int i = 0; i < rm->num_cams; i++) {
        CamRecContext  *cam  = &rm->cams[i];
        CameraRecorder *orig = (CameraRecorder *)cam->_orig_rec;
        if (cam->camera_id == camera_id)
            return orig ? (bool)orig->is_recording : false;
    }
    return false;
}

/* =========================================================================
 * Hot-add a single camera at runtime (called from ONVIF discovery callback)
 * ========================================================================= */
MnvrResult recorder_add_camera(RecorderModule *rm, const CameraInfo *info)
{
    if (!rm || !info || !rm->ctx) return MNVR_ERR_GENERIC;
    AppContext         *ctx = rm->ctx;
    const SystemConfig *cfg = config_get(ctx->config);

    /* Check if already added */
    for (int i = 0; i < rm->num_cams; i++) {
        if (rm->cams[i].camera_id == info->camera_id) {
            LOG_DEBUG(ctx, "RECORDER", "[cam %d] Already in recorder, skipping hot-add",
                      info->camera_id);
            return MNVR_OK;
        }
    }

    if (rm->num_cams >= MNVR_MAX_CAMERAS) {
        LOG_WARN(ctx, "RECORDER", "Max cameras (%d) reached, cannot hot-add cam %d",
                 MNVR_MAX_CAMERAS, info->camera_id);
        return MNVR_ERR_BUSY;
    }

    /* Create manager if it doesn't exist yet (was NULL if started with 0 cameras) */
    pthread_mutex_lock(&rm->mutex);

    RecorderManager *mgr = (RecorderManager *)rm->_internal_manager;
    if (!mgr) {
        mgr = manager_create();
        if (!mgr) {
            pthread_mutex_unlock(&rm->mutex);
            return MNVR_ERR_NOMEM;
        }
        rm->_internal_manager = mgr;
    }

    /* Build output prefix */
    char out_prefix[MNVR_MAX_PATH];
    snprintf(out_prefix, sizeof(out_prefix),
             "%.460s/cam_%d", info->rec_output_dir, info->camera_id);
    ensure_output_dir_exists(out_prefix);

    /* Add to original manager */
    int id = manager_add_camera(mgr, info->name, info->rec_rtsp_url, out_prefix);
    if (id < 0) {
        LOG_ERROR(ctx, "RECORDER", "[%s] hot-add: manager_add_camera failed",
                  info->name);
        pthread_mutex_unlock(&rm->mutex);
        return MNVR_ERR_GENERIC;
    }

    CameraRecorder *orig = mgr->cameras[id];
    configure_recorder(orig, cfg, out_prefix, info->codec);
    g_strlcpy(orig->rtsp_username, info->rtsp_username, sizeof(orig->rtsp_username));
    g_strlcpy(orig->rtsp_password, info->rtsp_password, sizeof(orig->rtsp_password));

    /* Populate CamRecContext */
    CamRecContext *cam = &rm->cams[rm->num_cams];
    memset(cam, 0, sizeof(CamRecContext));
    cam->ctx                 = ctx;
    cam->camera_id           = info->camera_id;
    cam->on_segment_complete = rm->on_segment_complete;
    cam->cb_user_data        = rm->cb_user_data;
    cam->_orig_rec           = orig;
    snprintf(cam->camera_name, sizeof(cam->camera_name), "%.63s", info->name);
    snprintf(cam->output_dir,  sizeof(cam->output_dir),  "%.511s", info->rec_output_dir);
    rm->num_cams++;

    /* Start recording */
    if (recorder_start(orig) == 0) {
        cam->is_recording = true;
        attach_segment_cb(cam, orig);
        LOG_INFO(ctx, "RECORDER",
                 "[%s] Hot-added and recording  url=%s  out=%s",
                 info->name, info->rec_rtsp_url, out_prefix);
    } else {
        LOG_ERROR(ctx, "RECORDER", "[%s] Hot-add: recorder_start failed",
                  info->name);
        pthread_mutex_unlock(&rm->mutex);
        return MNVR_ERR_GST;
    }

    /* Update DB paths */
    if (ctx->db) {
        char hls_url[MNVR_MAX_URL];
        snprintf(hls_url, sizeof(hls_url), "/hls/cam_%d/stream.m3u8", info->camera_id);
        char sql[1536];
        snprintf(sql, sizeof(sql),
            "UPDATE cameras SET rec_output_dir='%.460s', hls_output_dir='%.460s', "
            "hls_playlist_url='%.240s', updated_at=NOW() WHERE camera_id=%d;",
            info->rec_output_dir, info->hls_output_dir, hls_url, info->camera_id);
        db_async_exec(ctx->db, sql);
    }

    pthread_mutex_unlock(&rm->mutex);
    return MNVR_OK;
}