/**
 * @file rtsp_relay.h
 * @brief Local RTSP server that re-exposes each camera's watermarked
 *        restream (produced by streamer_module.c's udpsink on
 *        127.0.0.1:5000+camera_id*2) as rtsp://<host>:<port>/cam_<id>.
 *
 * WHY THIS EXISTS:
 * Before this module, MediaMTX connected to each physical camera's RTSP
 * URL directly (its own independent RTSP session), while mnvrd ALSO opened
 * its own independent RTSP session to the same camera for recording/AI.
 * Many IP cameras only allow 1-2 concurrent RTSP client connections, so
 * these two independent connections raced each other — whichever grabbed
 * the camera's single slot worked, the other failed. It also meant
 * MediaMTX's live view showed the camera's raw feed, never mnvrd's
 * watermarked one.
 *
 * This module makes mnvrd the ONLY thing that ever opens an RTSP
 * connection to the physical camera. MediaMTX is repointed (via
 * mediamtx-sync.py) at this local relay instead, which just re-wraps the
 * already-running UDP restream — no second camera connection, and the
 * live view now carries the same watermark as recordings.
 */
#ifndef RTSP_RELAY_H
#define RTSP_RELAY_H

#include "mnvr_system.h"

typedef struct RtspRelayModule RtspRelayModule;

/** Create the relay server (does not bind/listen yet). listen_port is
 *  typically 8555 — must not collide with MediaMTX's own RTSP port 8554. */
RtspRelayModule *rtsp_relay_module_create(AppContext *ctx, int listen_port);

/** Bind and start listening in a dedicated background thread. Call once,
 *  after creating the module and adding the cameras known at startup. */
MnvrResult rtsp_relay_module_start(RtspRelayModule *m);

/** Mount /cam_<camera_id> onto the relay, sourced from that camera's
 *  existing udpsink restream port (5000 + camera_id*2). Safe to call
 *  before or after rtsp_relay_module_start(). */
MnvrResult rtsp_relay_add_camera(RtspRelayModule *m, int camera_id);

/** Stop the background thread (blocks until it exits). */
void rtsp_relay_module_stop(RtspRelayModule *m);

/** Stop (if needed) and free everything. */
void rtsp_relay_module_destroy(RtspRelayModule *m);

#endif /* RTSP_RELAY_H */
