/**
 * @file hls_module.c
 * @brief HLS segmenter - converts MP4 recorder segments to HLS .ts + m3u8
 *
 * Worker thread per camera:
 *   1. Block on work queue condvar.
 *   2. Dequeue MP4 path.
 *   3. Run GStreamer pipeline: filesrc -> qtdemux -> h264parse ->
 *      mpegtsmux -> filesink  (produces one .ts per MP4 segment).
 *   4. Probe duration of produced .ts via GStreamer query.
 *   5. Append segment to sliding-window m3u8 playlist.
 *   6. Delete expired .ts files outside window.
 *   7. Atomically replace m3u8 (write tmp -> rename).
 *
 * The playlist is EXT-X-TARGETDURATION aligned to segment duration.
 * EXT-X-MEDIA-SEQUENCE increments per evicted segment.
 */

#include "hls_module.h"
#include "../../db/db_module.h"
#include "../logger/logger.h"
#include "../config/config_module.h"
#include <gst/gst.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>

/* -------------------------------------------------------------------------
 * Internal: MP4 -> TS via GStreamer
 * Returns duration in seconds, or -1 on error
 * ------------------------------------------------------------------------- */
static float transcode_mp4_to_ts(AppContext *ctx,
                                  const char *mp4_path,
                                  const char *ts_path)
{
    /* Build pipeline string */
    gchar *pipe_str = g_strdup_printf(
        "filesrc location=\"%s\" "
        "! qtdemux "
        "! h264parse "
        "! mpegtsmux "
        "! filesink location=\"%s\"",
        mp4_path, ts_path);

    GError *err = NULL;
    GstElement *pipeline = gst_parse_launch(pipe_str, &err);
    g_free(pipe_str);

    if (!pipeline || err) {
        LOG_ERROR(ctx,"HLS","transcode pipeline error: %s", err ? err->message : "?");
        if (err) g_error_free(err);
        return -1.0f;
    }

    gst_element_set_state(pipeline, GST_STATE_PLAYING);

    /* Wait for EOS or error with 30 s timeout */
    GstBus *bus = gst_element_get_bus(pipeline);
    GstMessage *msg = gst_bus_timed_pop_filtered(
        bus, 30 * GST_SECOND,
        GST_MESSAGE_EOS | GST_MESSAGE_ERROR);

    float duration = -1.0f;
    if (msg) {
        if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_EOS) {
            /* Query duration */
            gint64 dur_ns = 0;
            if (gst_element_query_duration(pipeline, GST_FORMAT_TIME, &dur_ns) && dur_ns > 0)
                duration = (float)(dur_ns / (double)GST_SECOND);
            else
                duration = 4.0f;   /* fallback estimate */
        } else {
            GError *e = NULL;
            gst_message_parse_error(msg, &e, NULL);
            LOG_ERROR(ctx,"HLS","transcode error: %s", e ? e->message : "?");
            if (e) g_error_free(e);
        }
        gst_message_unref(msg);
    }

    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_element_get_state(pipeline, NULL, NULL, GST_CLOCK_TIME_NONE);
    gst_object_unref(bus);
    gst_object_unref(pipeline);

    return duration;
}

/* -------------------------------------------------------------------------
 * Internal: write m3u8 playlist (atomic: tmp file -> rename)
 * ------------------------------------------------------------------------- */
static void write_playlist(CamHlsContext *cam, int target_dur_sec)
{
    char tmp_path[MNVR_MAX_PATH + 8];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", cam->playlist_path);

    FILE *fp = fopen(tmp_path, "w");
    if (!fp) {
        LOG_ERROR(cam->ctx,"HLS","Cannot write playlist: %s", tmp_path);
        return;
    }

    /* Calculate window */
    const SystemConfig *cfg = config_get(cam->ctx->config);
    int window = cfg ? cfg->hls_window_size : MNVR_HLS_MAX_SEGMENTS;
    int start = cam->seg_count - window;
    if (start < 0) start = 0;

    /* Find max duration for EXT-X-TARGETDURATION */
    float max_dur = (float)target_dur_sec;
    for (int i = start; i < cam->seg_count; i++) {
        float d = cam->segment_durations[i % (MNVR_HLS_MAX_SEGMENTS * 2)];
        if (d > max_dur) max_dur = d;
    }

    fprintf(fp, "#EXTM3U\n");
    fprintf(fp, "#EXT-X-VERSION:3\n");
    fprintf(fp, "#EXT-X-TARGETDURATION:%d\n", (int)max_dur + 1);
    fprintf(fp, "#EXT-X-MEDIA-SEQUENCE:%u\n", cam->media_sequence + (uint32_t)start);

    for (int i = start; i < cam->seg_count; i++) {
        int idx = i % (MNVR_HLS_MAX_SEGMENTS * 2);
        float dur = cam->segment_durations[idx];
        const char *seg_file = cam->segment_files[idx];
        /* Use basename only in playlist */
        const char *base = strrchr(seg_file, '/');
        base = base ? base + 1 : seg_file;
        fprintf(fp, "#EXTINF:%.3f,\n%s\n", dur, base);
    }

    fclose(fp);
    rename(tmp_path, cam->playlist_path);   /* atomic */
    LOG_DEBUG(cam->ctx,"HLS","[cam %d] Playlist updated (%d segs)",
              cam->camera_id, cam->seg_count - start);
}

/* -------------------------------------------------------------------------
 * Internal: delete expired segments outside the window
 * ------------------------------------------------------------------------- */
static void delete_expired(CamHlsContext *cam)
{
    const SystemConfig *cfg = config_get(cam->ctx->config);
    if (!cfg || !cfg->hls_delete_old_segments) return;

    int window = cfg->hls_window_size;
    int expire = cam->seg_count - window - 1;
    if (expire < 0) return;

    int idx = expire % (MNVR_HLS_MAX_SEGMENTS * 2);
    if (cam->segment_files[idx][0]) {
        unlink(cam->segment_files[idx]);
        cam->segment_files[idx][0] = '\0';
    }
}

/* -------------------------------------------------------------------------
 * Internal: process one MP4 -> TS
 * ------------------------------------------------------------------------- */
static void process_segment(CamHlsContext *cam, const char *mp4_path,
                             int64_t recording_id)
{
    const SystemConfig *cfg = config_get(cam->ctx->config);
    int target_dur = cfg ? cfg->hls_segment_sec : MNVR_HLS_SEGMENT_SEC;

    /* Build .ts path from mp4 path */
    char ts_path[MNVR_MAX_PATH];
    snprintf(ts_path, sizeof(ts_path), "%.511s", mp4_path);
    char *dot = strrchr(ts_path, '.');
    if (dot) strncpy(dot, ".ts", 4);

    /* Redirect .ts to hls_dir */
    const char *base = strrchr(mp4_path, '/');
    base = base ? base + 1 : mp4_path;
    char ts_basename[MNVR_MAX_PATH];
    snprintf(ts_basename, sizeof(ts_basename), "%.511s", base);
    dot = strrchr(ts_basename, '.');
    if (dot) strncpy(dot, ".ts", 4);

    snprintf(ts_path, MNVR_MAX_PATH, "%.240s/%.240s", cam->hls_dir, ts_basename);

    float dur = transcode_mp4_to_ts(cam->ctx, mp4_path, ts_path);
    if (dur < 0) {
        LOG_ERROR(cam->ctx,"HLS","[cam %d] Transcode failed for %s",
                  cam->camera_id, mp4_path);
        if (cam->ctx && cam->ctx->db && recording_id >= 0)
            db_mark_recording_hls_failed(cam->ctx->db, recording_id);
        return;
    }

    /* Record in ring */
    int idx = cam->seg_count % (MNVR_HLS_MAX_SEGMENTS * 2);
    snprintf(cam->segment_files[idx], MNVR_MAX_PATH, "%.511s", ts_path);
    cam->segment_durations[idx] = dur;
    cam->seg_count++;

    delete_expired(cam);
    write_playlist(cam, target_dur);

    /* Update recordings and recording_segments tables in PostgreSQL */
    if (cam->ctx && cam->ctx->db && recording_id >= 0) {
        struct stat ts_st;
        int64_t ts_size = 0;
        if (stat(ts_path, &ts_st) == 0) ts_size = (int64_t)ts_st.st_size;
        db_update_recording_hls(
            cam->ctx->db,
            recording_id,
            ts_path,
            ts_size,
            (double)dur,
            cam->playlist_path,
            cam->seg_count - 1   /* 0-based segment index */
        );
        LOG_DEBUG(cam->ctx,"HLS","[cam %d] DB updated for recording_id=%lld ts=%s",
                  cam->camera_id, (long long)recording_id, ts_path);
    }
}

/* -------------------------------------------------------------------------
 * HLS worker thread (one per camera)
 * ------------------------------------------------------------------------- */
static void *hls_worker_thread(void *arg)
{
    CamHlsContext *cam = (CamHlsContext *)arg;
    LOG_INFO(cam->ctx,"HLS","[cam %d] HLS worker started -> %s",
             cam->camera_id, cam->hls_dir);

    while (cam->running) {
        pthread_mutex_lock(&cam->q_mutex);
        while (cam->q_tail == cam->q_head && cam->running)
            pthread_cond_wait(&cam->q_cond, &cam->q_mutex);

        if (!cam->running && cam->q_tail == cam->q_head) {
            pthread_mutex_unlock(&cam->q_mutex);
            break;
        }

        HlsWorkItem item = cam->work_queue[cam->q_tail % HLS_WORK_QUEUE_SIZE];
        cam->q_tail++;
        pthread_mutex_unlock(&cam->q_mutex);

        process_segment(cam, item.mp4_path, item.recording_id);
    }

    LOG_INFO(cam->ctx,"HLS","[cam %d] HLS worker stopped", cam->camera_id);
    return NULL;
}

/* -------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */

/* Called from RecorderModule's OnSegmentComplete (may be any thread) */
void hls_on_segment_ready(int camera_id, const char *mp4_path,
                           int64_t recording_id, void *user_data)
{
    HlsModule *hm = (HlsModule *)user_data;
    if (!hm) return;

    for (int i = 0; i < hm->num_cams; i++) {
        CamHlsContext *cam = &hm->cams[i];
        if (cam->camera_id != camera_id) continue;

        pthread_mutex_lock(&cam->q_mutex);
        if ((cam->q_head - cam->q_tail) < HLS_WORK_QUEUE_SIZE) {
            HlsWorkItem *item = &cam->work_queue[cam->q_head % HLS_WORK_QUEUE_SIZE];
            strncpy(item->mp4_path, mp4_path, MNVR_MAX_PATH-1);
            item->camera_id    = camera_id;
            item->recording_id = recording_id;
            cam->q_head++;
            pthread_cond_signal(&cam->q_cond);
        } else {
            LOG_WARN(hm->ctx,"HLS","[cam %d] HLS work queue full, dropping segment",camera_id);
        }
        pthread_mutex_unlock(&cam->q_mutex);
        return;
    }
}

HlsModule *hls_module_create(AppContext *ctx)
{
    HlsModule *hm = calloc(1, sizeof(HlsModule));
    if (!hm) return NULL;
    hm->ctx = ctx;
    pthread_mutex_init(&hm->mutex, NULL);
    return hm;
}

MnvrResult hls_module_start(HlsModule *hm)
{
    if (!hm || !hm->ctx) return MNVR_ERR_GENERIC;
    AppContext *ctx = hm->ctx;

    hm->num_cams = 0;
    for (int i = 0; i < ctx->num_cameras; i++) {
        CameraInfo    *info = &ctx->cameras[i];
        CamHlsContext *cam  = &hm->cams[hm->num_cams];
        memset(cam, 0, sizeof(CamHlsContext));

        cam->ctx        = ctx;
        cam->camera_id  = info->camera_id;
        strncpy(cam->hls_dir, info->hls_output_dir, MNVR_MAX_PATH-1);
        {
            char _tmp[MNVR_MAX_PATH];
            snprintf(_tmp, MNVR_MAX_PATH, "%s/stream.m3u8", cam->hls_dir);
            memcpy(cam->playlist_path, _tmp, MNVR_MAX_PATH);
        }

        /* Ensure HLS output dir */
        {
            char tmp[MNVR_MAX_PATH];
            strncpy(tmp, cam->hls_dir, MNVR_MAX_PATH-1);
            mkdir(tmp, 0755);
        }

        pthread_mutex_init(&cam->q_mutex, NULL);
        pthread_cond_init(&cam->q_cond, NULL);
        cam->running = true;

        if (pthread_create(&cam->thread, NULL, hls_worker_thread, cam) != 0) {
            LOG_ERROR(ctx,"HLS","Failed to start HLS worker for cam %d", info->camera_id);
            cam->running = false;
            continue;
        }
        hm->num_cams++;
        LOG_INFO(ctx,"HLS","[cam %d] HLS worker started", info->camera_id);
    }
    return MNVR_OK;
}

void hls_module_stop(HlsModule *hm)
{
    if (!hm) return;
    for (int i = 0; i < hm->num_cams; i++) {
        CamHlsContext *cam = &hm->cams[i];
        pthread_mutex_lock(&cam->q_mutex);
        cam->running = false;
        pthread_cond_signal(&cam->q_cond);
        pthread_mutex_unlock(&cam->q_mutex);
        pthread_join(cam->thread, NULL);
    }
    LOG_INFO(hm->ctx,"HLS","All HLS workers stopped");
}

void hls_module_destroy(HlsModule *hm)
{
    if (!hm) return;
    hls_module_stop(hm);
    for (int i = 0; i < hm->num_cams; i++) {
        pthread_mutex_destroy(&hm->cams[i].q_mutex);
        pthread_cond_destroy(&hm->cams[i].q_cond);
    }
    pthread_mutex_destroy(&hm->mutex);
    free(hm);
}

/* -------------------------------------------------------------------------
 * Hot-add a camera at runtime (mirrors the setup in hls_module_start)
 * ------------------------------------------------------------------------- */
MnvrResult hls_add_camera(HlsModule *hm, const CameraInfo *info)
{
    if (!hm || !info || !hm->ctx) return MNVR_ERR_GENERIC;

    /* Already have a worker for this camera? */
    for (int i = 0; i < hm->num_cams; i++) {
        if (hm->cams[i].camera_id == info->camera_id)
            return MNVR_OK;
    }

    if (hm->num_cams >= MNVR_MAX_CAMERAS)
        return MNVR_ERR_BUSY;

    pthread_mutex_lock(&hm->mutex);

    CamHlsContext *cam = &hm->cams[hm->num_cams];
    memset(cam, 0, sizeof(CamHlsContext));

    cam->ctx       = hm->ctx;
    cam->camera_id = info->camera_id;
    strncpy(cam->hls_dir, info->hls_output_dir, MNVR_MAX_PATH - 1);
    snprintf(cam->playlist_path, MNVR_MAX_PATH, "%s/stream.m3u8", cam->hls_dir);

    /* Ensure output dir exists */
    mkdir(cam->hls_dir, 0755);

    pthread_mutex_init(&cam->q_mutex, NULL);
    pthread_cond_init(&cam->q_cond, NULL);
    cam->running = true;

    if (pthread_create(&cam->thread, NULL, hls_worker_thread, cam) != 0) {
        LOG_ERROR(hm->ctx, "HLS", "[cam %d] Failed to start HLS worker (hot-add)",
                  info->camera_id);
        cam->running = false;
        pthread_mutex_unlock(&hm->mutex);
        return MNVR_ERR_GENERIC;
    }

    hm->num_cams++;
    pthread_mutex_unlock(&hm->mutex);

    LOG_INFO(hm->ctx, "HLS", "[cam %d] HLS worker hot-added  dir=%s",
             info->camera_id, cam->hls_dir);
    return MNVR_OK;
}