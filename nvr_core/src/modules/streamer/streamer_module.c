/**
 * @file streamer_module.c
 * @brief Live camera re-streaming with frame capture for AI
 */

#define _POSIX_C_SOURCE 200809L

#include "streamer_module.h"
#include "../logger/logger.h"
#include "../config/config_module.h"
#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

/* -------------------------------------------------------------------------
 * appsink callback -> forward frame to AI module
 * ------------------------------------------------------------------------- */
static GstFlowReturn new_sample_cb(GstAppSink *appsink, gpointer data)
{
    CamStreamer *cam = (CamStreamer *)data;
    if (!cam->on_frame) return GST_FLOW_OK;

    GstSample *sample = gst_app_sink_pull_sample(appsink);
    if (!sample) return GST_FLOW_OK;

    GstBuffer *buf  = gst_sample_get_buffer(sample);
    GstCaps   *caps = gst_sample_get_caps(sample);

    int width = 0, height = 0;
    if (caps) {
        GstStructure *s = gst_caps_get_structure(caps, 0);
        gst_structure_get_int(s, "width",  &width);
        gst_structure_get_int(s, "height", &height);
    }

    GstMapInfo map;
    if (gst_buffer_map(buf, &map, GST_MAP_READ)) {
        uint64_t pts_ms = GST_BUFFER_PTS_IS_VALID(buf)
            ? GST_TIME_AS_MSECONDS(GST_BUFFER_PTS(buf)) : 0;

        cam->on_frame(cam->camera_id,
                      map.data, width, height,
                      pts_ms, cam->frame_user_data);

        gst_buffer_unmap(buf, &map);
    }

    gst_sample_unref(sample);
    return GST_FLOW_OK;
}

/* -------------------------------------------------------------------------
 * Dynamic pad from rtspsrc
 * ------------------------------------------------------------------------- */
static void pad_added_cb(GstElement *src, GstPad *pad, gpointer data)
{
    (void)src;
    CamStreamer *cam = (CamStreamer *)data;

    GstCaps *caps = gst_pad_get_current_caps(pad);
    if (!caps) return;
    GstStructure *s = gst_caps_get_structure(caps, 0);
    const gchar  *media = gst_structure_get_string(s, "media");

    if (!media || strcmp(media, "video") != 0) {
        gst_caps_unref(caps); return;
    }

    GstPad *sink = gst_element_get_static_pad(cam->depay, "sink");
    if (!gst_pad_is_linked(sink))
        gst_pad_link(pad, sink);
    gst_object_unref(sink);
    gst_caps_unref(caps);
}

/* -------------------------------------------------------------------------
 * Bus callback
 * ------------------------------------------------------------------------- */
static gboolean bus_cb(GstBus *bus, GstMessage *msg, gpointer data)
{
    (void)bus;
    CamStreamer *cam = (CamStreamer *)data;
    switch (GST_MESSAGE_TYPE(msg)) {
        case GST_MESSAGE_EOS:
            LOG_INFO(cam->ctx,"STREAMER","[%s] EOS",cam->camera_name);
            g_main_loop_quit(cam->loop);
            break;
        case GST_MESSAGE_ERROR: {
            GError *e = NULL;
            gst_message_parse_error(msg, &e, NULL);
            LOG_ERROR(cam->ctx,"STREAMER","[%s] %s",cam->camera_name, e ? e->message : "?");
            if (e) g_error_free(e);
            g_main_loop_quit(cam->loop);
            break;
        }
        default: break;
    }
    return TRUE;
}

/* -------------------------------------------------------------------------
 * Build GStreamer pipeline
 * ------------------------------------------------------------------------- */
/* Refreshes the GPS/speed textoverlay roughly once a second. Runs on the
 * streaming thread inside the pad's data-flow, so it must be cheap and
 * non-blocking — it only formats a string and calls g_object_set. */
static GstPadProbeReturn gps_overlay_probe_cb(GstPad *pad, GstPadProbeInfo *info,
                                               gpointer user_data)
{
    (void)pad; (void)info;
    CamStreamer *cam = (CamStreamer *)user_data;

    gint64 now_us = g_get_monotonic_time();
    if (now_us - cam->gps_overlay_last_update_us < G_USEC_PER_SEC)
        return GST_PAD_PROBE_OK;
    cam->gps_overlay_last_update_us = now_us;

    char text[96];
    if (cam->gps_has_fix) {
        snprintf(text, sizeof(text), "GPS: %.5f, %.5f  Speed: %.0f km/h",
                 cam->gps_lat, cam->gps_lon, cam->gps_speed_kmh);
    } else {
        snprintf(text, sizeof(text), "GPS: NO FIX");
    }
    g_object_set(cam->gps_overlay, "text", text, NULL);

    return GST_PAD_PROBE_OK;
}

static MnvrResult build_streamer_pipeline(CamStreamer *cam)
{
    gchar *pname = g_strdup_printf("streamer-cam-%d", cam->camera_id);
    cam->pipeline     = gst_pipeline_new(pname);
    g_free(pname);

    cam->rtspsrc      = gst_element_factory_make("rtspsrc",       NULL);
    cam->depay        = gst_element_factory_make("rtph264depay",   NULL);
    cam->parser       = gst_element_factory_make("h264parse",      NULL);
    cam->tee          = gst_element_factory_make("tee",            NULL);

    /* AI decode branch */
    cam->decode_queue = gst_element_factory_make("queue",          NULL);
    cam->decoder      = gst_element_factory_make("avdec_h264",     NULL);
    cam->videoconvert = gst_element_factory_make("videoconvert",   NULL);
    cam->appsink      = gst_element_factory_make("appsink",        NULL);

    /* Watermark branch — shares the already-decoded frames from the AI
     * branch above instead of decoding a second time. */
    cam->raw_tee        = gst_element_factory_make("tee",          NULL);
    cam->overlay_queue  = gst_element_factory_make("queue",        NULL);
    cam->clock_overlay  = gst_element_factory_make("clockoverlay", NULL);
    cam->gps_overlay    = gst_element_factory_make("textoverlay",  NULL);
    cam->overlay_convert= gst_element_factory_make("videoconvert", NULL);
    cam->stream_encoder = gst_element_factory_make("x264enc",      NULL);

    /* Re-stream branch (watermarked + re-encoded video) — feeds the RTSP
     * relay only. Recording no longer shares this stream: recorder.c now
     * connects directly to the camera's own dedicated recording profile
     * (see recorder.c), so a single destination is enough here again. */
    cam->stream_queue = gst_element_factory_make("queue",          NULL);
    cam->rtppay       = gst_element_factory_make("rtph264pay",     NULL);
    cam->udpsink      = gst_element_factory_make("udpsink",        NULL);

    if (!cam->pipeline || !cam->rtspsrc || !cam->depay || !cam->parser ||
        !cam->tee      || !cam->decode_queue || !cam->decoder ||
        !cam->videoconvert || !cam->appsink  ||
        !cam->raw_tee || !cam->overlay_queue || !cam->clock_overlay ||
        !cam->gps_overlay || !cam->overlay_convert || !cam->stream_encoder ||
        !cam->stream_queue || !cam->rtppay   || !cam->udpsink) {
        LOG_FATAL(cam->ctx,"STREAMER","[%s] Element creation failed",cam->camera_name);
        return MNVR_ERR_GST;
    }

    /* Configure rtspsrc — credentials via user-id/user-pw, not embedded in URL.
     * Embedding passwords containing '@' or ':' in the URL breaks rtspsrc's
     * URL parser and causes spurious 401 Unauthorized errors. */
    g_object_set(cam->rtspsrc,
                 "location", cam->rtsp_url,
                 "latency",  100,
                 "protocols", 4,   /* TCP only */
                 NULL);
    if (cam->rtsp_username[0])
        g_object_set(cam->rtspsrc, "user-id", cam->rtsp_username, NULL);
    if (cam->rtsp_password[0])
        g_object_set(cam->rtspsrc, "user-pw", cam->rtsp_password, NULL);

    /* Configure appsink - I420, drop old frames */
    GstCaps *ai_caps = gst_caps_new_simple("video/x-raw",
                                            "format", G_TYPE_STRING, "I420",
                                            NULL);
    g_object_set(cam->appsink,
                 "caps",             ai_caps,
                 "emit-signals",     TRUE,
                 "drop",             TRUE,
                 "max-buffers",      2,
                 "sync",             FALSE,
                 NULL);
    gst_caps_unref(ai_caps);

    GstAppSinkCallbacks cbs = {
        .eos         = NULL,
        .new_preroll = NULL,
        .new_sample  = new_sample_cb,
    };
    gst_app_sink_set_callbacks(GST_APP_SINK(cam->appsink), &cbs, cam, NULL);

    /* --- Watermark: date/time (top-left) --- */
    g_object_set(cam->clock_overlay,
                 "time-format", "%Y-%m-%d %H:%M:%S",
                 "halignment",  0,   /* left   */
                 "valignment",  0,   /* top    */
                 "font-desc",   "Sans 14",
                 "shaded-background", TRUE,
                 NULL);

    /* --- Watermark: GPS + speed (bottom-left). Text is refreshed roughly
     * once a second from a buffer probe below; starts as "NO FIX" until
     * streamer_update_telemetry() is called by the GPS/telemetry module. */
    g_object_set(cam->gps_overlay,
                 "text",        "GPS: NO FIX",
                 "halignment",  0,   /* left   */
                 "valignment",  2,   /* bottom */
                 "font-desc",   "Sans 14",
                 "shaded-background", TRUE,
                 NULL);

    /* --- Re-encoder for the watermarked restream branch ---
     * zerolatency + ultrafast keeps CPU cost and latency low — this feeds
     * the live view only now, recording has its own separate encode again
     * (see recorder.c). */
    g_object_set(cam->stream_encoder,
                 "tune",       4,  /* GST_X264_ENC_TUNE_ZEROLATENCY = 0x4 */
                 "speed-preset", 1, /* ultrafast   */
                 "bitrate",    2048,
                 NULL);

    /* Configure udpsink for re-streaming on loopback (consumed by the
     * RTSP relay — see rtsp_relay.c). */
    int udp_port = 5000 + cam->camera_id * 2;
    g_object_set(cam->udpsink,
                 "host", "127.0.0.1",
                 "port", udp_port,
                 "sync", FALSE,
                 NULL);

    /* Add all elements */
    gst_bin_add_many(GST_BIN(cam->pipeline),
                     cam->rtspsrc, cam->depay, cam->parser, cam->tee,
                     cam->decode_queue, cam->decoder, cam->videoconvert, cam->appsink,
                     cam->raw_tee, cam->overlay_queue, cam->clock_overlay,
                     cam->gps_overlay, cam->overlay_convert, cam->stream_encoder,
                     cam->stream_queue, cam->rtppay, cam->udpsink,
                     NULL);

    /* Link: depay -> parser -> tee */
    if (!gst_element_link_many(cam->depay, cam->parser, cam->tee, NULL)) {
        LOG_FATAL(cam->ctx,"STREAMER","[%s] Link depay->tee failed",cam->camera_name);
        return MNVR_ERR_GST;
    }

    /* Link tee -> decode branch (AI + watermark both consume decoded video) */
    GstPad *tee_ai   = gst_element_request_pad_simple(cam->tee, "src_%u");
    GstPad *dq_sink  = gst_element_get_static_pad(cam->decode_queue, "sink");
    gst_pad_link(tee_ai, dq_sink);
    gst_object_unref(tee_ai); gst_object_unref(dq_sink);

    if (!gst_element_link_many(cam->decode_queue, cam->decoder,
                                cam->videoconvert, cam->raw_tee, NULL)) {
        LOG_WARN(cam->ctx,"STREAMER","[%s] AI decode branch link failed (no decoder?)",
                 cam->camera_name);
    }

    /* raw_tee branch 1: existing AI frame path (unchanged) */
    GstPad *rawtee_ai = gst_element_request_pad_simple(cam->raw_tee, "src_%u");
    GstPad *appsink_sink = gst_element_get_static_pad(cam->appsink, "sink");
    gst_pad_link(rawtee_ai, appsink_sink);
    gst_object_unref(rawtee_ai); gst_object_unref(appsink_sink);

    /* raw_tee branch 2: watermark (date/time + GPS/speed) -> re-encode ->
     * restream. This replaces the old compressed passthrough branch so the
     * live view and the recording both carry the same burned-in overlay. */
    GstPad *rawtee_ov = gst_element_request_pad_simple(cam->raw_tee, "src_%u");
    GstPad *ovq_sink  = gst_element_get_static_pad(cam->overlay_queue, "sink");
    gst_pad_link(rawtee_ov, ovq_sink);
    gst_object_unref(rawtee_ov); gst_object_unref(ovq_sink);

    if (!gst_element_link_many(cam->overlay_queue, cam->clock_overlay,
                                cam->gps_overlay, cam->overlay_convert,
                                cam->stream_encoder, cam->stream_queue,
                                cam->rtppay, cam->udpsink, NULL)) {
        LOG_WARN(cam->ctx,"STREAMER","[%s] Watermark/stream branch link failed",
                 cam->camera_name);
    }

    /* Refresh the GPS/speed overlay text roughly once a second from a
     * buffer probe on the encoder's sink pad — avoids sharing a
     * GMainContext timer across the per-camera threads. */
    GstPad *enc_sink = gst_element_get_static_pad(cam->stream_encoder, "sink");
    gst_pad_add_probe(enc_sink, GST_PAD_PROBE_TYPE_BUFFER,
                       gps_overlay_probe_cb, cam, NULL);
    gst_object_unref(enc_sink);

    /* Dynamic RTSP pad */
    g_signal_connect(cam->rtspsrc, "pad-added", G_CALLBACK(pad_added_cb), cam);

    /* Bus */
    GstBus *bus = gst_pipeline_get_bus(GST_PIPELINE(cam->pipeline));
    gst_bus_add_watch(bus, bus_cb, cam);
    gst_object_unref(bus);

    return MNVR_OK;
}

/* -------------------------------------------------------------------------
 * Streamer thread
 * ------------------------------------------------------------------------- */
static void *streamer_thread_fn(void *arg)
{
    CamStreamer *cam = (CamStreamer *)arg;
    LOG_INFO(cam->ctx,"STREAMER","[%s] Thread started",cam->camera_name);

    /* Retry loop: rebuild pipeline on error/disconnect */
    while (cam->running) {
        if (!cam->loop)
            cam->loop = g_main_loop_new(NULL, FALSE);

        gst_element_set_state(cam->pipeline, GST_STATE_PLAYING);
        g_main_loop_run(cam->loop);   /* blocks until error or EOS */

        if (!cam->running) break;

        LOG_WARN(cam->ctx,"STREAMER","[%s] Pipeline stopped, retrying in 5s",
                 cam->camera_name);
        sleep(5);

        if (!cam->running) break;

        /* Tear down old pipeline */
        gst_element_set_state(cam->pipeline, GST_STATE_NULL);
        gst_element_get_state(cam->pipeline, NULL, NULL, 5 * GST_SECOND);
        gst_object_unref(cam->pipeline);
        cam->pipeline = NULL;
        g_main_loop_unref(cam->loop);
        cam->loop = NULL;

        /* Rebuild */
        if (build_streamer_pipeline(cam) != MNVR_OK) {
            LOG_ERROR(cam->ctx,"STREAMER","[%s] Rebuild failed, retrying in 10s",
                      cam->camera_name);
            sleep(10);
        }
    }

    /* Final cleanup */
    if (cam->pipeline) {
        gst_element_set_state(cam->pipeline, GST_STATE_NULL);
        gst_element_get_state(cam->pipeline, NULL, NULL, GST_CLOCK_TIME_NONE);
        gst_object_unref(cam->pipeline);
        cam->pipeline = NULL;
    }
    if (cam->loop) {
        g_main_loop_unref(cam->loop);
        cam->loop = NULL;
    }
    cam->running = false;

    LOG_INFO(cam->ctx,"STREAMER","[%s] Thread stopped",cam->camera_name);
    return NULL;
}

/* -------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */

StreamerModule *streamer_module_create(AppContext *ctx,
                                        OnFrameCallback cb, void *user_data)
{
    StreamerModule *sm = calloc(1, sizeof(StreamerModule));
    if (!sm) return NULL;
    sm->ctx             = ctx;
    sm->on_frame        = cb;
    sm->frame_user_data = user_data;
    pthread_mutex_init(&sm->mutex, NULL);
    return sm;
}

MnvrResult streamer_module_start(StreamerModule *sm)
{
    if (!sm || !sm->ctx) return MNVR_ERR_GENERIC;
    AppContext *ctx = sm->ctx;

    sm->num_cams = 0;
    for (int i = 0; i < ctx->num_cameras; i++) {
        CameraInfo  *info = &ctx->cameras[i];
        CamStreamer *cam  = &sm->cams[sm->num_cams];
        memset(cam, 0, sizeof(CamStreamer));

        cam->ctx             = ctx;
        cam->camera_id       = info->camera_id;
        cam->on_frame        = sm->on_frame;
        cam->frame_user_data = sm->frame_user_data;
        strncpy(cam->camera_name,    info->name,          MNVR_MAX_NAME-1);
        strncpy(cam->rtsp_url,       info->rtsp_url,      MNVR_MAX_URL-1);
        strncpy(cam->rtsp_username,  info->rtsp_username, sizeof(cam->rtsp_username)-1);
        strncpy(cam->rtsp_password,  info->rtsp_password, sizeof(cam->rtsp_password)-1);
        snprintf(cam->mount_point, sizeof(cam->mount_point), "/cam_%d", info->camera_id);

        if (build_streamer_pipeline(cam) != MNVR_OK) {
            LOG_ERROR(ctx,"STREAMER","[%s] Pipeline failed",cam->camera_name);
            continue;
        }

        cam->running = true;
        pthread_create(&cam->thread, NULL, streamer_thread_fn, cam);
        sm->num_cams++;
        LOG_INFO(ctx,"STREAMER","[%s] Streaming on udp://127.0.0.1:%d",
                 cam->camera_name, 5000 + info->camera_id * 2);

        { struct timespec _ts = {0, 100000000L}; nanosleep(&_ts, NULL); };
    }
    return MNVR_OK;
}

void streamer_module_stop(StreamerModule *sm)
{
    if (!sm) return;
    for (int i = 0; i < sm->num_cams; i++) {
        CamStreamer *cam = &sm->cams[i];
        if (!cam->running) continue;
        if (cam->loop) g_main_loop_quit(cam->loop);
        pthread_join(cam->thread, NULL);
    }
}

void streamer_module_destroy(StreamerModule *sm)
{
    if (!sm) return;
    streamer_module_stop(sm);
    pthread_mutex_destroy(&sm->mutex);
    free(sm);
}

const char *streamer_get_url(StreamerModule *sm, int camera_id)
{
    for (int i = 0; i < sm->num_cams; i++)
        if (sm->cams[i].camera_id == camera_id)
            return sm->cams[i].mount_point;
    return NULL;
}

/* -------------------------------------------------------------------------
 * Hot-add a camera at runtime (mirrors the setup in streamer_module_start)
 * ------------------------------------------------------------------------- */
MnvrResult streamer_add_camera(StreamerModule *sm, const CameraInfo *info)
{
    if (!sm || !info || !sm->ctx) return MNVR_ERR_GENERIC;

    /* Already have a streamer for this camera? */
    for (int i = 0; i < sm->num_cams; i++) {
        if (sm->cams[i].camera_id == info->camera_id)
            return MNVR_OK;
    }

    if (sm->num_cams >= MNVR_MAX_CAMERAS)
        return MNVR_ERR_BUSY;

    pthread_mutex_lock(&sm->mutex);

    CamStreamer *cam = &sm->cams[sm->num_cams];
    memset(cam, 0, sizeof(CamStreamer));

    cam->ctx             = sm->ctx;
    cam->camera_id       = info->camera_id;
    cam->on_frame        = sm->on_frame;
    cam->frame_user_data = sm->frame_user_data;
    strncpy(cam->camera_name,   info->name,          MNVR_MAX_NAME - 1);
    strncpy(cam->rtsp_url,      info->rtsp_url,      MNVR_MAX_URL - 1);
    strncpy(cam->rtsp_username, info->rtsp_username, sizeof(cam->rtsp_username) - 1);
    strncpy(cam->rtsp_password, info->rtsp_password, sizeof(cam->rtsp_password) - 1);
    snprintf(cam->mount_point, sizeof(cam->mount_point), "/cam_%d", info->camera_id);

    if (build_streamer_pipeline(cam) != MNVR_OK) {
        LOG_ERROR(sm->ctx, "STREAMER",
                  "[%s] Pipeline failed (hot-add)", cam->camera_name);
        pthread_mutex_unlock(&sm->mutex);
        return MNVR_ERR_GST;
    }

    cam->running = true;
    pthread_create(&cam->thread, NULL, streamer_thread_fn, cam);
    sm->num_cams++;

    pthread_mutex_unlock(&sm->mutex);

    LOG_INFO(sm->ctx, "STREAMER",
             "[%s] Hot-added streamer on udp://127.0.0.1:%d",
             cam->camera_name, 5000 + info->camera_id * 2);
    return MNVR_OK;
}

void streamer_update_telemetry(CamStreamer *cam, double lat, double lon,
                                double speed_kmh, bool has_fix)
{
    if (!cam) return;
    cam->gps_lat       = lat;
    cam->gps_lon       = lon;
    cam->gps_speed_kmh = speed_kmh;
    cam->gps_has_fix   = has_fix;
    /* Actual text update happens on the streaming thread via
     * gps_overlay_probe_cb so we never touch GStreamer objects from an
     * arbitrary caller thread. */
}