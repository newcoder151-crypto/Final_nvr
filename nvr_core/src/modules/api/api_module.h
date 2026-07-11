/**
 * @file api_module.h
 * @brief REST + WebSocket HTTP API server module
 *
 * Provides the web-facing API used by the web application.
 * Built on libmicrohttpd (MHD) for HTTP/HTTPS serving.
 *
 * Endpoints:
 *   GET  /api/v1/system/status       - overall health snapshot
 *   GET  /api/v1/cameras             - list cameras + state
 *   GET  /api/v1/cameras/{id}/stream - redirect to HLS playlist URL
 *   GET  /api/v1/cameras/{id}/live   - redirect to RTSP re-stream URL
 *   GET  /api/v1/recordings          - list recordings (paginated)
 *   GET  /api/v1/recordings/{id}     - recording metadata
 *   POST /api/v1/cameras/{id}/ptz    - PTZ move command (body: JSON)
 *   GET  /api/v1/events              - list AI events (paginated)
 *   GET  /hls/cam_{id}/stream.m3u8   - serve HLS playlist (static)
 *   GET  /hls/cam_{id}/<seg>.ts     - serve HLS segment files (static)
 *
 * WebSocket (upgrade from GET /ws/status):
 *   Server pushes HealthSnapshot JSON every 5 seconds.
 *
 * Authentication:
 *   Bearer token (JWT) checked on every API request.
 *   Token issued by POST /api/v1/auth/login.
 */

#ifndef API_MODULE_H
#define API_MODULE_H

#include "mnvr_system.h"

/* MHD types - only available when libmicrohttpd is present */
#ifdef MNVR_WITH_API
struct MHD_Daemon;
#endif

struct ApiModule {
    AppContext        *ctx;
#ifdef MNVR_WITH_API
    struct MHD_Daemon *daemon;
#else
    void             *daemon;   /* NULL when API disabled */
#endif
    int                port;
    bool               tls_enabled;
    char               tls_cert_path[MNVR_MAX_PATH];
    char               tls_key_path[MNVR_MAX_PATH];
    pthread_t          thread;        /* heartbeat / WS push thread */
    volatile bool      running;
};

/* ---- Lifecycle ---- */
ApiModule *api_module_create(AppContext *ctx);
MnvrResult api_module_start(ApiModule *am);
void       api_module_stop(ApiModule *am);
void       api_module_destroy(ApiModule *am);

#endif /* API_MODULE_H */
