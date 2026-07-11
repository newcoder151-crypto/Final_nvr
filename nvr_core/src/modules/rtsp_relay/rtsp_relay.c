/**
 * @file rtsp_relay.c
 * @brief See rtsp_relay.h for the "why".
 */
#include "rtsp_relay.h"
#include "../logger/logger.h"
#include <gst/rtsp-server/rtsp-server.h>
#include <stdlib.h>
#include <string.h>

struct RtspRelayModule {
    AppContext     *ctx;
    GstRTSPServer  *server;
    GMainContext   *gcontext;
    GMainLoop      *loop;
    pthread_t       thread;
    int             listen_port;
    volatile bool   running;
};

static void *relay_thread_fn(void *arg)
{
    RtspRelayModule *m = (RtspRelayModule *)arg;
    g_main_loop_run(m->loop);
    return NULL;
}

RtspRelayModule *rtsp_relay_module_create(AppContext *ctx, int listen_port)
{
    RtspRelayModule *m = (RtspRelayModule *)calloc(1, sizeof(RtspRelayModule));
    if (!m) return NULL;

    m->ctx         = ctx;
    m->listen_port = listen_port;
    m->server      = gst_rtsp_server_new();

    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", listen_port);
    gst_rtsp_server_set_service(m->server, port_str);

    /* Dedicated GMainContext so this doesn't collide with any other
     * module's use of the process-default context. */
    m->gcontext = g_main_context_new();
    m->loop     = g_main_loop_new(m->gcontext, FALSE);

    return m;
}

MnvrResult rtsp_relay_add_camera(RtspRelayModule *m, int camera_id)
{
    if (!m || !m->server) return MNVR_ERR_GENERIC;

    GstRTSPMountPoints  *mounts  = gst_rtsp_server_get_mount_points(m->server);
    GstRTSPMediaFactory *factory = gst_rtsp_media_factory_new();

    /* Same port formula streamer_module.c already uses for its udpsink
     * restream — see MNVR_UDP_RESTREAM_PORT(camera_id) convention. */
    int udp_port = 5000 + camera_id * 2;

    char launch[512];
    snprintf(launch, sizeof(launch),
        "( udpsrc port=%d "
        "caps=\"application/x-rtp,media=video,clock-rate=90000,encoding-name=H264,payload=96\" "
        "! rtpjitterbuffer latency=200 ! rtph264depay ! rtph264pay name=pay0 pt=96 config-interval=1 )",
        udp_port);

    gst_rtsp_media_factory_set_launch(factory, launch);
    /* Shared: one decode/pipeline instance serves every viewer of this
     * path (MediaMTX plus anyone else), instead of re-running the launch
     * pipeline (and re-binding the UDP port) per viewer, which would fail
     * on the second viewer since only one udpsrc can bind a given port. */
    gst_rtsp_media_factory_set_shared(factory, TRUE);

    char path[32];
    snprintf(path, sizeof(path), "/cam_%d", camera_id);
    gst_rtsp_mount_points_add_factory(mounts, path, factory);
    g_object_unref(mounts);

    LOG_INFO(m->ctx, "RTSP-RELAY",
             "Mounted rtsp://127.0.0.1:%d/cam_%d  <-  udp 127.0.0.1:%d (watermarked restream)",
             m->listen_port, camera_id, udp_port);
    return MNVR_OK;
}

MnvrResult rtsp_relay_module_start(RtspRelayModule *m)
{
    if (!m) return MNVR_ERR_GENERIC;

    GSource *src = gst_rtsp_server_create_source(m->server, NULL, NULL);
    if (!src) {
        LOG_FATAL(m->ctx, "RTSP-RELAY", "Failed to bind RTSP relay port %d "
                  "(already in use?)", m->listen_port);
        return MNVR_ERR_GST;
    }
    g_source_attach(src, m->gcontext);
    g_source_unref(src);

    m->running = true;
    if (pthread_create(&m->thread, NULL, relay_thread_fn, m) != 0) {
        LOG_FATAL(m->ctx, "RTSP-RELAY", "Failed to start relay thread");
        m->running = false;
        return MNVR_ERR_GENERIC;
    }

    LOG_INFO(m->ctx, "RTSP-RELAY", "RTSP relay server listening on :%d", m->listen_port);
    return MNVR_OK;
}

void rtsp_relay_module_stop(RtspRelayModule *m)
{
    if (!m || !m->running) return;
    m->running = false;
    g_main_loop_quit(m->loop);
    pthread_join(m->thread, NULL);
}

void rtsp_relay_module_destroy(RtspRelayModule *m)
{
    if (!m) return;
    if (m->running) rtsp_relay_module_stop(m);
    if (m->loop)     g_main_loop_unref(m->loop);
    if (m->gcontext) g_main_context_unref(m->gcontext);
    if (m->server)   g_object_unref(m->server);
    free(m);
}
