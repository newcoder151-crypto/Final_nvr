/**
 * @file api_module.c
 * @brief REST API server implementation using libmicrohttpd
 *
 * Thread model:
 *   - MHD runs an internal thread pool (MHD_USE_INTERNAL_POLLING_THREAD)
 *   - api_heartbeat_thread: pushes health JSON to WebSocket clients every 5s
 *
 * All request handlers are short-lived; DB queries use db_query() with
 * synchronous PostgreSQL reads (db_query, protected by conn_mutex).
 *
 * JSON responses are built with hand-formatted snprintf (no external dep).
 * Replace with cJSON or jansson for production.
 */

#include "api_module.h"

#ifdef MNVR_WITH_API

#include "../logger/logger.h"
#include "../config/config_module.h"
#include "../health/health_module.h"
#include "../../db/db_module.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <microhttpd.h>

/* -------------------------------------------------------------------------
 * JSON helpers
 * ------------------------------------------------------------------------- */
#define JSON_RESP_BUF 65536

static struct MHD_Response *json_response(const char *json)
{
    struct MHD_Response *resp =
        MHD_create_response_from_buffer(strlen(json),
                                         (void *)json,
                                         MHD_RESPMEM_MUST_COPY);
    MHD_add_response_header(resp, "Content-Type", "application/json");
    MHD_add_response_header(resp, "Access-Control-Allow-Origin", "*");
    return resp;
}

static struct MHD_Response *error_response(int *status, const char *msg)
{
    char buf[512];
    snprintf(buf, sizeof(buf), "{\"error\":\"%s\"}", msg);
    *status = MHD_HTTP_BAD_REQUEST;
    return json_response(buf);
}

/* -------------------------------------------------------------------------
 * Route: GET /api/v1/system/status
 * ------------------------------------------------------------------------- */
static enum MHD_Result handle_system_status(AppContext *ctx,
                                  struct MHD_Connection *conn)
{
    HealthSnapshot snap = {0};
    if (ctx->health)
        health_get_snapshot(ctx->health, &snap);

    char buf[2048];
    snprintf(buf, sizeof(buf),
        "{"
        "\"device_id\":\"%s\","
        "\"device_name\":\"%s\","
        "\"firmware\":\"%s\","
        "\"cpu_pct\":%.1f,"
        "\"mem_pct\":%.1f,"
        "\"disk_pct\":%.1f,"
        "\"disk_free_gb\":%.2f,"
        "\"num_cameras\":%d,"
        "\"sampled_at\":%ld"
        "}",
        ctx->device_id,
        ctx->device_name,
        ctx->firmware_version,
        snap.cpu_usage_pct,
        snap.mem_usage_pct,
        snap.disk_usage_pct,
        (double)snap.disk_free_bytes / (1024.0 * 1024.0 * 1024.0),
        ctx->num_cameras,
        (long)snap.sampled_at);

    struct MHD_Response *resp = json_response(buf);
    int r = MHD_queue_response(conn, MHD_HTTP_OK, resp);
    MHD_destroy_response(resp);
    return r;
}

/* -------------------------------------------------------------------------
 * Route: GET /api/v1/cameras
 * ------------------------------------------------------------------------- */
static enum MHD_Result handle_cameras(AppContext *ctx, struct MHD_Connection *conn)
{
    char buf[JSON_RESP_BUF];
    int pos = 0;

    pos += snprintf(buf + pos, sizeof(buf) - pos, "[");
    pthread_mutex_lock(&ctx->cameras_mutex);
    for (int i = 0; i < ctx->num_cameras; i++) {
        CameraInfo *cam = &ctx->cameras[i];
        if (i > 0) pos += snprintf(buf + pos, sizeof(buf) - pos, ",");
        pos += snprintf(buf + pos, sizeof(buf) - pos,
            "{"
            "\"id\":%d,"
            "\"name\":\"%s\","
            "\"ip\":\"%s\","
            "\"resolution\":\"%dx%d\","
            "\"fps\":%d,"
            "\"codec\":\"%s\","
            "\"location\":\"%s\","
            "\"state\":%d,"
            "\"hls_url\":\"%s\","
            "\"hls_output_dir\":\"%s\","
            "\"rec_output_dir\":\"%s\","
            "\"stream_url\":\"%s\""
            "}",
            cam->camera_id, cam->name, cam->ip_address,
            cam->resolution_width, cam->resolution_height,
            cam->target_fps, cam->codec,
            cam->location_desc,
            (int)cam->state,
            cam->hls_output_dir[0]
                ? cam->hls_output_dir   /* use path as URL prefix if no explicit URL */
                : "",
            cam->hls_output_dir,
            cam->rec_output_dir,
            cam->stream_url);
    }
    pthread_mutex_unlock(&ctx->cameras_mutex);
    pos += snprintf(buf + pos, sizeof(buf) - pos, "]");

    struct MHD_Response *resp = json_response(buf);
    int r = MHD_queue_response(conn, MHD_HTTP_OK, resp);
    MHD_destroy_response(resp);
    return r;
}

/* -------------------------------------------------------------------------
 * Route: GET /api/v1/recordings  (simple, no pagination yet)
 * ------------------------------------------------------------------------- */
typedef struct { char *buf; int pos; int size; } RecBuf;

static void recordings_row_cb(void *ud, int ncols, char **vals, char **cols)
{
    (void)cols;
    RecBuf *rb = (RecBuf *)ud;
    if (rb->pos > 1) rb->pos += snprintf(rb->buf + rb->pos, rb->size - rb->pos, ",");

    rb->pos += snprintf(rb->buf + rb->pos, rb->size - rb->pos,
        "{\"id\":\"%s\",\"camera_id\":\"%s\","
        "\"file\":\"%s\",\"start\":\"%s\","
        "\"duration\":\"%s\",\"size\":\"%s\"}",
        ncols > 0 ? vals[0] : "",
        ncols > 1 ? vals[1] : "",
        ncols > 2 ? vals[2] : "",
        ncols > 3 ? vals[3] : "",
        ncols > 4 ? vals[4] : "",
        ncols > 5 ? vals[5] : "");
}

static enum MHD_Result handle_recordings(AppContext *ctx, struct MHD_Connection *conn)
{
    char *buf = malloc(JSON_RESP_BUF);
    if (!buf) return MHD_NO;
    RecBuf rb = { buf, 0, JSON_RESP_BUF };
    rb.pos = snprintf(buf, JSON_RESP_BUF, "[");

    if (ctx->db)
        db_query(ctx->db,
                 "SELECT recording_id, camera_id, file_path, start_timestamp, "
                 "duration_seconds, file_size_bytes "
                 "FROM recordings ORDER BY start_time DESC LIMIT 200;",
                 recordings_row_cb, &rb);

    rb.pos += snprintf(buf + rb.pos, JSON_RESP_BUF - rb.pos, "]");

    struct MHD_Response *resp = json_response(buf);
    int r = MHD_queue_response(conn, MHD_HTTP_OK, resp);
    MHD_destroy_response(resp);
    free(buf);
    return r;
}

/* -------------------------------------------------------------------------
 * Route: GET /api/v1/events
 * ------------------------------------------------------------------------- */
typedef struct { char *buf; int pos; int size; } EvBuf;

static void events_row_cb(void *ud, int ncols, char **vals, char **cols)
{
    (void)cols;
    EvBuf *eb = (EvBuf *)ud;
    if (eb->pos > 1) eb->pos += snprintf(eb->buf + eb->pos, eb->size - eb->pos, ",");
    eb->pos += snprintf(eb->buf + eb->pos, eb->size - eb->pos,
        "{\"id\":\"%s\",\"camera_id\":\"%s\","
        "\"type\":\"%s\",\"confidence\":\"%s\","
        "\"ts\":\"%s\",\"data\":%s}",
        ncols>0?vals[0]:"", ncols>1?vals[1]:"",
        ncols>2?vals[2]:"", ncols>3?vals[3]:"",
        ncols>4?vals[4]:"", ncols>5&&vals[5]?vals[5]:"{}");
}

static enum MHD_Result handle_events(AppContext *ctx, struct MHD_Connection *conn)
{
    char *buf = malloc(JSON_RESP_BUF);
    if (!buf) return MHD_NO;
    EvBuf eb = { buf, 0, JSON_RESP_BUF };
    eb.pos = snprintf(buf, JSON_RESP_BUF, "[");

    if (ctx->db)
        db_query(ctx->db,
                 "SELECT event_id, camera_id, event_type, event_data, "
                 "occurred_at, created_at "
                 "FROM events ORDER BY occurred_at DESC LIMIT 200;",
                 events_row_cb, &eb);

    eb.pos += snprintf(buf + eb.pos, JSON_RESP_BUF - eb.pos, "]");

    struct MHD_Response *resp = json_response(buf);
    int r = MHD_queue_response(conn, MHD_HTTP_OK, resp);
    MHD_destroy_response(resp);
    free(buf);
    return r;
}

/* -------------------------------------------------------------------------
 * Static file serve for HLS segments / playlists
 * ------------------------------------------------------------------------- */
static enum MHD_Result serve_static_file(AppContext *ctx,
                               struct MHD_Connection *conn,
                               const char *url)
{
    /* url like: /hls/cam_1/stream.m3u8 or /hls/cam_1/seg_XYZ.ts */
    const SystemConfig *cfg = config_get(ctx->config);
    const char *hls_base = cfg ? cfg->hls_base : "/storage/hls";

    /* Strip leading /hls */
    const char *rel = url + 4;   /* skip "/hls" */

    char file_path[MNVR_MAX_PATH];
    snprintf(file_path, sizeof(file_path), "%s%s", hls_base, rel);

    FILE *fp = fopen(file_path, "rb");
    if (!fp) {
        const char *not_found = "{\"error\":\"not found\"}";
        struct MHD_Response *r = json_response(not_found);
        int rc = MHD_queue_response(conn, MHD_HTTP_NOT_FOUND, r);
        MHD_destroy_response(r);
        return rc;
    }

    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    rewind(fp);
    char *data = malloc(size);
    if (!data) { fclose(fp); return MHD_NO; }
    if (fread(data, 1, size, fp) != (size_t)size) { free(data); fclose(fp); return MHD_NO; }
    fclose(fp);

    const char *content_type = "application/octet-stream";
    if (strstr(file_path, ".m3u8")) content_type = "application/vnd.apple.mpegurl";
    else if (strstr(file_path, ".ts")) content_type = "video/MP2T";

    struct MHD_Response *resp =
        MHD_create_response_from_buffer(size, data, MHD_RESPMEM_MUST_FREE);
    MHD_add_response_header(resp, "Content-Type",  content_type);
    MHD_add_response_header(resp, "Cache-Control", "no-cache");
    MHD_add_response_header(resp, "Access-Control-Allow-Origin", "*");

    int rc = MHD_queue_response(conn, MHD_HTTP_OK, resp);
    MHD_destroy_response(resp);
    return rc;
}

/* -------------------------------------------------------------------------
 * Main MHD request handler
 * ------------------------------------------------------------------------- */
static enum MHD_Result request_handler(void *cls,
                                        struct MHD_Connection *conn,
                                        const char *url,
                                        const char *method,
                                        const char *version,
                                        const char *upload_data,
                                        size_t *upload_data_size,
                                        void **ptr)
{
    (void)version; (void)upload_data; (void)upload_data_size; (void)ptr;
    AppContext *ctx = (AppContext *)cls;

    /* Skip first call for POST requests (upload_data not yet received) */
    if (*upload_data_size != 0) {
        *upload_data_size = 0;
        return MHD_YES;
    }

    /* Route matching */
    if (strcmp(method, "GET") == 0) {
        if (strcmp(url, "/api/v1/system/status") == 0)
            return handle_system_status(ctx, conn);
        if (strcmp(url, "/api/v1/cameras") == 0)
            return handle_cameras(ctx, conn);
        if (strcmp(url, "/api/v1/recordings") == 0)
            return handle_recordings(ctx, conn);
        if (strcmp(url, "/api/v1/events") == 0)
            return handle_events(ctx, conn);
        if (strncmp(url, "/hls/", 5) == 0)
            return serve_static_file(ctx, conn, url);
    }

    /* 404 fallback */
    int status = MHD_HTTP_NOT_FOUND;
    struct MHD_Response *resp = error_response(&status, "Not Found");
    int r = MHD_queue_response(conn, status, resp);
    MHD_destroy_response(resp);
    return r;
}

/* -------------------------------------------------------------------------
 * Heartbeat thread (future: WebSocket push)
 * ------------------------------------------------------------------------- */
static void *api_heartbeat_thread(void *arg)
{
    ApiModule *am = (ApiModule *)arg;
    LOG_INFO(am->ctx, "API", "Heartbeat thread started");
    while (am->running) {
        sleep(5);
        /* TODO: push HealthSnapshot to connected WebSocket clients */
    }
    return NULL;
}

/* -------------------------------------------------------------------------
 * Public lifecycle
 * ------------------------------------------------------------------------- */

ApiModule *api_module_create(AppContext *ctx)
{
    ApiModule *am = calloc(1, sizeof(ApiModule));
    if (!am) return NULL;
    am->ctx = ctx;
    const SystemConfig *cfg = config_get(ctx->config);
    am->port        = cfg ? cfg->api_port : 8443;
    am->tls_enabled = cfg ? cfg->api_tls_enabled : false;
    if (cfg) {
        snprintf(am->tls_cert_path, MNVR_MAX_PATH, "%s", cfg->api_tls_cert);
        snprintf(am->tls_key_path,  MNVR_MAX_PATH, "%s", cfg->api_tls_key);
    }
    return am;
}

MnvrResult api_module_start(ApiModule *am)
{
    if (!am) return MNVR_ERR_GENERIC;

    unsigned int flags = MHD_USE_POLL_INTERNAL_THREAD;

    am->daemon = MHD_start_daemon(flags,
                                   am->port,
                                   NULL, NULL,
                                   &request_handler, am->ctx,
                                   MHD_OPTION_END);

    if (!am->daemon) {
        LOG_FATAL(am->ctx, "API", "Failed to start HTTP server on port %d",
                  am->port);
        return MNVR_ERR_GENERIC;
    }

    am->running = true;
    pthread_create(&am->thread, NULL, api_heartbeat_thread, am);

    LOG_INFO(am->ctx, "API", "HTTP server listening on port %d", am->port);
    return MNVR_OK;
}

void api_module_stop(ApiModule *am)
{
    if (!am) return;
    am->running = false;
    if (am->daemon) { MHD_stop_daemon(am->daemon); am->daemon = NULL; }
    pthread_join(am->thread, NULL);
    LOG_INFO(am->ctx, "API", "HTTP server stopped");
}

void api_module_destroy(ApiModule *am)
{
    if (!am) return;
    api_module_stop(am);
    free(am);
}

#else  /* !MNVR_WITH_API - stub implementation */

#include "api_module.h"
#include <stdlib.h>
#include <string.h>

ApiModule *api_module_create(AppContext *ctx)
{
    ApiModule *am = calloc(1, sizeof(ApiModule));
    if (!am) return NULL;
    am->ctx = ctx;
    return am;
}
MnvrResult api_module_start(ApiModule *am)
{
    (void)am;
    /* libmicrohttpd not available - REST API disabled */
    return MNVR_OK;
}
void api_module_stop(ApiModule *am)    { (void)am; }
void api_module_destroy(ApiModule *am) { free(am); }

#endif /* MNVR_WITH_API */
