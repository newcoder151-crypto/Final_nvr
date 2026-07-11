/**
 * @file main.c
 * @brief mNVR Application - Master Orchestrator
 *
 * ============================================================
 * THREAD MAP  (what runs in a while loop vs one-shot)
 * ============================================================
 *
 *  Thread                   Loop type        Owner module
 *  ???????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????
 *  logger_drain_thread      while(running)   LoggerModule
 *  db_writer_thread_fn      while(running)   DbModule
 *  discovery_thread_fn      while(running)   OnvifModule     [sleep 60s]
 *  health_poll_thread        while(running)   HealthModule    [sleep 10s]
 *  cam_record_thread ?N     g_main_loop_run  RecorderModule  [GLib event loop]
 *  hls_worker_thread ?N     while(running)   HlsModule       [condvar wait]
 *  streamer_thread_fn ?N    g_main_loop_run  StreamerModule  [GLib event loop]
 *  ai_worker_thread ?N      while(running)   AiModule        [condvar wait]
 *  MHD internal pool        epoll/select     ApiModule       [libmicrohttpd]
 *  api_heartbeat_thread      while(running)   ApiModule       [sleep 5s]
 *  MAIN thread              pthread_cond_timedwait           [shutdown gate]
 *
 * ============================================================
 * STARTUP ORDER
 * ============================================================
 *   1. GStreamer init
 *   2. Signal handlers (SIGINT, SIGTERM, SIGHUP)
 *   3. logger_create / logger_start
 *   4. config_create / config_load
 *   5. db_module_create / db_module_start  (apply schema.sch)
 *   6. config_load_cameras                 (fills ctx->cameras[])
 *   7. onvif_module_create / onvif_module_start
 *   8. health_module_create / health_module_start
 *   9. recorder_module_create / recorder_module_start
 *  10. hls_module_create / hls_module_start
 *  11. streamer_module_create / streamer_module_start
 *  12. ai_module_create / ai_module_start
 *  13. api_module_create / api_module_start
 *  14. main while loop  (config reload + heartbeat)
 *
 * ============================================================
 * SHUTDOWN ORDER  (reverse)
 * ============================================================
 *  SIGINT -> shutdown_requested=true -> main loop exits
 *  api_stop -> ai_stop -> streamer_stop -> hls_stop
 *  -> recorder_stop -> onvif_stop -> health_stop
 *  -> db_stop -> logger_flush -> logger_stop
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <signal.h>
#include <unistd.h>
#include <time.h>
#include <sys/stat.h>
#include <gst/gst.h>

#include "include/mnvr_system.h"
#include "modules/logger/logger.h"
#include "modules/config/config_module.h"
#include "modules/onvif/onvif_module.h"
#include "modules/recorder/recorder_module.h"
#include "modules/hls/hls_module.h"
#include "modules/streamer/streamer_module.h"
#include "modules/ai/ai_module.h"
#include "modules/health/health_module.h"
#include "modules/api/api_module.h"
#include "modules/rtsp_relay/rtsp_relay.h"
#include "modules/gps/gps_module.h"
#include "db/db_module.h"

/* =========================================================================
 * Global AppContext (singleton - needed for signal handler access)
 * ========================================================================= */
static AppContext g_ctx;

/* =========================================================================
 * Signal handlers
 * ========================================================================= */
static void handle_sigint_sigterm(int sig)
{
    (void)sig;
    /* Non-blocking: just set the flag and wake the main loop */
    g_ctx.shutdown_requested = true;
    pthread_cond_signal(&g_ctx.shutdown_cond);
}

static void handle_sighup(int sig)
{
    (void)sig;
    /* Mark config dirty - main loop will call config_reload() */
    if (g_ctx.config)
        config_mark_dirty(g_ctx.config);
}

/* =========================================================================
 * AI event callback  ->  write to DB
 * ========================================================================= */
static void on_ai_event(const AiEvent *ev, void *user_data)
{
    AppContext *ctx = (AppContext *)user_data;
    if (!ctx || !ctx->db) return;

    const char *type_str = "UNKNOWN";
    switch (ev->type) {
        case AI_EVENT_MOTION:        type_str = "MOTION";        break;
        case AI_EVENT_FACE_DETECTED: type_str = "FACE_DETECTED"; break;
        case AI_EVENT_TAMPERING:     type_str = "TAMPERING";     break;
        case AI_EVENT_RDAS_ALERT:    type_str = "RDAS_ALERT";    break;
    }

    db_insert_event(ctx->db, ev->camera_id, type_str,
                    ev->confidence, ev->metadata, ev->pts_ms);

    LOG_DEBUG(ctx, "MAIN", "AI event cam%d %s conf=%.2f",
              ev->camera_id, type_str, ev->confidence);
}

/* =========================================================================
 * Simple SQL string escaper (doubles single quotes for PostgreSQL)
 * ========================================================================= */
static void sql_escape(const char *src, char *dst, size_t dst_len)
{
    if (!src || !dst || dst_len == 0) { if (dst) dst[0] = '\0'; return; }
    size_t j = 0;
    for (size_t i = 0; src[i] && j < dst_len - 2; i++) {
        if (src[i] == '\'') {
            dst[j++] = '\'';
            if (j < dst_len - 1) dst[j++] = '\'';
        } else {
            dst[j++] = src[i];
        }
    }
    dst[j] = '\0';
}

/* =========================================================================
 * ONVIF new device callback  ->  log, persist to DB, auto-register camera
 * ========================================================================= */
static void on_new_onvif_device(const OnvifDevice *dev, void *user_data)
{
    AppContext *ctx = (AppContext *)user_data;
    LOG_INFO(ctx, "MAIN", "ONVIF device: %s @ %s (%s %s) stream=%s",
             dev->device_uuid[0] ? dev->device_uuid : "(direct)",
             dev->ip_address,
             dev->manufacturer, dev->model,
             dev->stream_uri[0] ? dev->stream_uri : "(none)");

    if (!ctx->db) return;

    /* Escape all strings from ONVIF responses before SQL insertion */
    char e_uuid[128], e_ip[128], e_mfr[128], e_model[128];
    char e_fw[128], e_sn[128], e_xaddr[512], e_stream[512];
    sql_escape(dev->device_uuid,   e_uuid,  sizeof(e_uuid));
    sql_escape(dev->ip_address,    e_ip,    sizeof(e_ip));
    sql_escape(dev->manufacturer,  e_mfr,   sizeof(e_mfr));
    sql_escape(dev->model,         e_model, sizeof(e_model));
    sql_escape(dev->firmware,      e_fw,    sizeof(e_fw));
    sql_escape(dev->serial_number, e_sn,    sizeof(e_sn));
    sql_escape(dev->xaddrs,        e_xaddr, sizeof(e_xaddr));
    sql_escape(dev->stream_uri,    e_stream,sizeof(e_stream));

    /* Persist to device_discovery table (PostgreSQL ON CONFLICT) */
    char sql[4096];
    snprintf(sql, sizeof(sql),
        "INSERT INTO device_discovery "
        "(device_uuid, ip_address, manufacturer, model, "
        " firmware_version, serial_number, xaddrs, "
        " stream_uri, ptz_supported, audio_supported, discovered_at) "
        "VALUES ('%s','%s','%s','%s','%s','%s','%s','%s',%s,%s,NOW()) "
        "ON CONFLICT (ip_address) DO UPDATE SET "
        " manufacturer=EXCLUDED.manufacturer, model=EXCLUDED.model, "
        " firmware_version=EXCLUDED.firmware_version, "
        " serial_number=EXCLUDED.serial_number, "
        " stream_uri=EXCLUDED.stream_uri, "
        " ptz_supported=EXCLUDED.ptz_supported, "
        " audio_supported=EXCLUDED.audio_supported, "
        " discovered_at=NOW();",
        e_uuid, e_ip, e_mfr, e_model, e_fw, e_sn, e_xaddr,
        e_stream,
        dev->ptz_supported ? "true" : "false",
        dev->audio_supported ? "true" : "false");
    db_async_exec(ctx->db, sql);

    /* ---------------------------------------------------------------
     * Auto-register camera into cameras table
     * Only if:
     *   1. Device has video profiles (it's a camera, not a sensor)
     *   2. Stream URI was discovered
     *   3. IP does not already exist in cameras table
     *   4. Device came from a configured slot (has RTSP credentials)
     * --------------------------------------------------------------- */
    if (dev->num_profiles <= 0 || !dev->stream_uri[0]) {
        LOG_DEBUG(ctx, "MAIN",
                  "Skipping auto-register for %s: no profiles or stream URI",
                  dev->ip_address);
        return;
    }

    if (dev->config_slot <= 0 || dev->config_slot > MNVR_MAX_CAMERAS) {
        LOG_DEBUG(ctx, "MAIN",
                  "Skipping auto-register for %s: no config slot (multicast discovery)",
                  dev->ip_address);
        return;
    }

    /* Get config for this slot to read RTSP credentials and camera type */
    const OnvifCameraConfig *cam_cfg = NULL;
    if (ctx->onvif)
        cam_cfg = onvif_config_get_camera(&ctx->onvif->config, dev->config_slot);

    if (!cam_cfg) return;

    /* Store the CLEAN stream URI (no embedded credentials) in cameras.rtsp_url.
     * Credentials belong exclusively in cameras_config_details
     * (rtsp_username/rtsp_password) — every consumer (streamer_module.c,
     * recorder.c, config_module.c's build_rtsp_url) reads credentials from
     * there and sets them via rtspsrc's user-id/user-pw properties, not by
     * parsing them back out of the URL. Embedding raw, unencoded
     * credentials here (the old behavior) corrupted the URL the moment a
     * password contained an '@' — e.g. "bel@1234" produced a URL with two
     * '@' characters, which is genuinely ambiguous to parse and was the
     * root cause of a credential-doubling bug that kept reappearing no
     * matter how many times the database was patched, since this code
     * re-generated the broken URL fresh on every ONVIF (re)discovery. */
    char rtsp_url[MNVR_MAX_URL] = {0};
    const char *ruser = cam_cfg->rtsp_user[0] ? cam_cfg->rtsp_user : cam_cfg->user;
    const char *rpass = cam_cfg->rtsp_pass[0] ? cam_cfg->rtsp_pass : cam_cfg->pass;
    strncpy(rtsp_url, dev->stream_uri, sizeof(rtsp_url) - 1);

    /* Map encoding to video_codec format the DB expects */
    const char *codec = "H.264";
    if (dev->profile_encoding[0]) {
        if (strcasecmp(dev->profile_encoding, "H265") == 0 ||
            strcasecmp(dev->profile_encoding, "H.265") == 0)
            codec = "H.265";
    }

    /* Camera type from config, default to INTERIOR */
    const char *cam_type = cam_cfg->camera_type[0]
                            ? cam_cfg->camera_type : "INTERIOR";

    /* Camera name: use config location if set, else "Cam_<slot>" */
    char cam_name[128];
    if (cam_cfg->location[0])
        snprintf(cam_name, sizeof(cam_name), "%s", cam_cfg->location);
    else
        snprintf(cam_name, sizeof(cam_name), "Camera_%d", dev->config_slot);

    /* Location description */
    char e_location[256], e_name[256], e_rtsp[512];
    sql_escape(cam_cfg->location, e_location, sizeof(e_location));
    sql_escape(cam_name,          e_name,     sizeof(e_name));
    sql_escape(rtsp_url,          e_rtsp,     sizeof(e_rtsp));

    /* Escape storage paths from AppContext */
    char e_storage[MNVR_MAX_PATH], e_hls[MNVR_MAX_PATH];
    sql_escape(ctx->storage_base, e_storage, sizeof(e_storage));
    sql_escape(ctx->hls_base,     e_hls,     sizeof(e_hls));

    /* Debug: verify storage paths before INSERT */
    fprintf(stderr,
        "  [AUTO-REG-DBG] ctx->storage_base='%s' e_storage='%s'\n"
        "  [AUTO-REG-DBG] ctx->hls_base='%s' e_hls='%s'\n",
        ctx->storage_base, e_storage,
        ctx->hls_base, e_hls);

    char e_rec_rtsp[512];
    sql_escape(dev->rec_stream_uri[0] ? dev->rec_stream_uri : rtsp_url,
               e_rec_rtsp, sizeof(e_rec_rtsp));

    /* INSERT camera — skip if IP already exists.
     * rec_output_dir and hls_output_dir store the BASE paths from config
     * (e.g. /storage/recordings, /storage/hls).
     * The recorder module appends /cam_N when building the output prefix. */
    snprintf(sql, sizeof(sql),
        "INSERT INTO cameras "
        "(camera_name, camera_type, ip_address, rtsp_url, rec_rtsp_url, username, "
        " manufacturer, model, firmware_version, "
        " resolution_width, resolution_height, target_fps, video_codec, "
        " location_description, ptz_supported, audio_supported, status, "
        " rec_output_dir, hls_output_dir) "
        "SELECT '%s', '%s', '%s', '%s', '%s', '%s', "
        "       '%s', '%s', '%s', "
        "       %d, %d, %d, '%s', "
        "       '%s', %d, %d, 'ACTIVE', "
        "       '%s', '%s' "
        "WHERE NOT EXISTS (SELECT 1 FROM cameras WHERE ip_address='%s');",
        e_name, cam_type, e_ip, e_rtsp, e_rec_rtsp, ruser,
        e_mfr, e_model, e_fw,
        dev->profile_width > 0 ? dev->profile_width : 1920,
        dev->profile_height > 0 ? dev->profile_height : 1080,
        dev->profile_fps > 0 ? dev->profile_fps : 25,
        codec,
        e_location,
        dev->ptz_supported ? 1 : 0,
        dev->audio_supported ? 1 : 0,
        e_storage,
        e_hls,
        e_ip);

    /* Use synchronous db_exec (not async) so the row is committed
     * before config_load_cameras() runs and the recorder starts. */
    fprintf(stderr, "  [AUTO-REG-DBG] SQL:\n%s\n", sql);
    MnvrResult db_r = db_exec(ctx->db, sql);
    if (db_r != MNVR_OK) {
        LOG_WARN(ctx, "MAIN",
                 "Auto-register INSERT failed for %s (result=%d)",
                 dev->ip_address, db_r);
        return;
    }

    /* The INSERT above only fires for brand-new IPs (guarded by
     * WHERE NOT EXISTS). For cameras that already exist in the DB — e.g.
     * ones registered before rec_rtsp_url existed — refresh rtsp_url and
     * rec_rtsp_url unconditionally here instead, so every camera picks up
     * the dual-profile URLs within one discovery cycle (60s) without
     * needing a manual SQL fix. Harmless no-op for rows that already
     * match. */
    char sql_update[2048];
    snprintf(sql_update, sizeof(sql_update),
        "UPDATE cameras SET rtsp_url='%s', rec_rtsp_url='%s' "
        "WHERE ip_address='%s';",
        e_rtsp, e_rec_rtsp, e_ip);
    db_async_exec(ctx->db, sql_update);

    LOG_INFO(ctx, "MAIN",
             "Auto-registered camera: %s (%s %s) %dx%d %s %dfps @ %s "
             "rec=%s hls=%s",
             cam_name, dev->manufacturer, dev->model,
             dev->profile_width, dev->profile_height,
             codec, dev->profile_fps, dev->ip_address,
             ctx->storage_base, ctx->hls_base);

    /* Upsert credentials into cameras_config_details — this is the table
     * every consumer (streamer_module.c, recorder.c, config_module.c's
     * build_rtsp_url) actually reads rtsp_username/rtsp_password from, via
     * rtspsrc's user-id/user-pw properties. Values are stored as-is (not
     * URL-embedded), so an '@' or any other character in the password is
     * never ambiguous. Matched by ip_address; camera_slot must also be
     * unique so use config_slot (falls back sensibly since config slots
     * are 1:1 with discovered devices here). */
    if (ruser[0] || rpass[0]) {
        char e_ruser[128], e_rpass[256], e_ip2[64];
        sql_escape(ruser,           e_ruser, sizeof(e_ruser));
        sql_escape(rpass,           e_rpass, sizeof(e_rpass));
        sql_escape(dev->ip_address, e_ip2,   sizeof(e_ip2));
        char sql2[2048];
        snprintf(sql2, sizeof(sql2),
            "INSERT INTO cameras_config_details "
            "(camera_slot, name, ip_address, rtsp_username, rtsp_password) "
            "VALUES (%d, '%s', '%s', '%s', '%s') "
            "ON CONFLICT (ip_address) DO UPDATE SET "
            " rtsp_username=EXCLUDED.rtsp_username, "
            " rtsp_password=EXCLUDED.rtsp_password, "
            " updated_at=NOW();",
            dev->config_slot, e_name, e_ip2, e_ruser, e_rpass);
        MnvrResult cd_r = db_exec(ctx->db, sql2);
        if (cd_r != MNVR_OK)
            LOG_WARN(ctx, "MAIN",
                     "cameras_config_details upsert failed for %s (result=%d)",
                     dev->ip_address, cd_r);
    }

    /* Hot-add to recorder if it's already running.
     * This handles cameras discovered AFTER the recorder has started
     * (e.g. via multicast discovery or SIGHUP config reload). */
    if (ctx->recorder) {
        /* Build a CameraInfo for the new camera */
        CameraInfo hot_cam;
        memset(&hot_cam, 0, sizeof(hot_cam));
        hot_cam.camera_id = dev->config_slot; /* will be overwritten below */
        strncpy(hot_cam.name, cam_name, sizeof(hot_cam.name) - 1);
        strncpy(hot_cam.rtsp_url, rtsp_url, sizeof(hot_cam.rtsp_url) - 1);
        strncpy(hot_cam.ip_address, dev->ip_address, sizeof(hot_cam.ip_address) - 1);
        hot_cam.resolution_width  = dev->profile_width > 0 ? dev->profile_width : 1920;
        hot_cam.resolution_height = dev->profile_height > 0 ? dev->profile_height : 1080;
        hot_cam.target_fps        = dev->profile_fps > 0 ? dev->profile_fps : 25;
        strncpy(hot_cam.codec, codec, sizeof(hot_cam.codec) - 1);
        hot_cam.audio_enabled = dev->audio_supported;
        hot_cam.ptz_supported = dev->ptz_supported;
        hot_cam.state = CAM_STATE_ACTIVE;

        snprintf(hot_cam.rec_output_dir, sizeof(hot_cam.rec_output_dir),
                 "%s/cam_%d", ctx->storage_base, dev->config_slot);
        snprintf(hot_cam.hls_output_dir, sizeof(hot_cam.hls_output_dir),
                 "%s/cam_%d", ctx->hls_base, dev->config_slot);

        /* Query the actual camera_id assigned by PostgreSQL SERIAL */
        {
            /* Reload cameras from DB to find the newly inserted camera_id */
            config_load_cameras(ctx->config);
            for (int ci = 0; ci < ctx->num_cameras; ci++) {
                if (strcmp(ctx->cameras[ci].ip_address, dev->ip_address) == 0) {
                    hot_cam.camera_id = ctx->cameras[ci].camera_id;
                    strncpy(hot_cam.rec_output_dir,
                            ctx->cameras[ci].rec_output_dir,
                            sizeof(hot_cam.rec_output_dir) - 1);
                    strncpy(hot_cam.hls_output_dir,
                            ctx->cameras[ci].hls_output_dir,
                            sizeof(hot_cam.hls_output_dir) - 1);
                    break;
                }
            }
        }

        if (hot_cam.camera_id > 0) {
            MnvrResult add_r = recorder_add_camera(ctx->recorder, &hot_cam);
            if (add_r == MNVR_OK)
                LOG_INFO(ctx, "MAIN",
                         "Hot-added camera %d (%s) to recorder",
                         hot_cam.camera_id, cam_name);
            else
                LOG_WARN(ctx, "MAIN",
                         "Hot-add to recorder failed for %s (result=%d)",
                         cam_name, add_r);

            /* Also hot-add to HLS module so segment conversion starts */
            if (ctx->hls) {
                MnvrResult hls_r = hls_add_camera(ctx->hls, &hot_cam);
                if (hls_r == MNVR_OK)
                    LOG_INFO(ctx, "MAIN",
                             "Hot-added camera %d (%s) to HLS",
                             hot_cam.camera_id, cam_name);
                else
                    LOG_WARN(ctx, "MAIN",
                             "Hot-add to HLS failed for %s (result=%d)",
                             cam_name, hls_r);
            }

            /* Hot-add to streamer so AI gets frame callbacks */
            if (ctx->streamer) {
                MnvrResult str_r = streamer_add_camera(ctx->streamer, &hot_cam);
                if (str_r == MNVR_OK)
                    LOG_INFO(ctx, "MAIN",
                             "Hot-added camera %d (%s) to streamer",
                             hot_cam.camera_id, cam_name);
                else
                    LOG_WARN(ctx, "MAIN",
                             "Hot-add to streamer failed for %s (result=%d)",
                             cam_name, str_r);
            }

            /* Also hot-add to AI module so analytics start */
            if (ctx->ai) {
                MnvrResult ai_r = ai_add_camera(ctx->ai, &hot_cam);
                if (ai_r == MNVR_OK)
                    LOG_INFO(ctx, "MAIN",
                             "Hot-added camera %d (%s) to AI",
                             hot_cam.camera_id, cam_name);
                else
                    LOG_WARN(ctx, "MAIN",
                             "Hot-add to AI failed for %s (result=%d)",
                             cam_name, ai_r);
            }
        }
    }
}

/* =========================================================================
 * Segment complete callback  ->  notify HLS module
 * ========================================================================= */
static void on_segment_complete(int camera_id, const char *file_path,
                                 void *user_data)
{
    AppContext *ctx = (AppContext *)user_data;
    if (!ctx) return;

    LOG_DEBUG(ctx, "MAIN", "Segment ready cam%d: %s", camera_id, file_path);

    /* 1. Insert recording row first to get recording_id (RETURNING clause)
     *    This is synchronous so we have the id before notifying HLS. */
    int64_t recording_id = -1;
    if (ctx->db) {
        struct stat st;
        int64_t sz = 0;
        if (stat(file_path, &st) == 0) sz = st.st_size;
        recording_id = db_insert_recording(ctx->db, camera_id, file_path,
                                           time(NULL), 0, sz);
        LOG_DEBUG(ctx, "MAIN", "Recording inserted: id=%lld cam=%d",
                  (long long)recording_id, camera_id);
    }

    /* 2. Notify HLS module - pass recording_id so it can update DB after
     *    conversion (updates hls_ts_file_path, hls_conversion_status, etc.) */
    if (ctx->hls)
        hls_on_segment_ready(camera_id, file_path, recording_id, ctx->hls);
}

/* =========================================================================
 * Frame callback  ->  AI module
 * ========================================================================= */
static void on_frame(int camera_id, const uint8_t *data,
                     int width, int height,
                     uint64_t pts_ms, void *user_data)
{
    AppContext *ctx = (AppContext *)user_data;
    if (!ctx || !ctx->ai) return;

    /* I420 layout: Y plane (w*h), U plane (w/2 * h/2), V plane (w/2 * h/2) */
    YuvFrame frame;
    frame.camera_id = camera_id;
    frame.width     = width;
    frame.height    = height;
    frame.pts_ms    = pts_ms;
    frame.y         = data;
    frame.u         = data + (size_t)(width * height);
    frame.v         = data + (size_t)(width * height) + (size_t)((width/2) * (height/2));
    ai_push_yuv_frame(&frame, ctx->ai);
}

/* =========================================================================
 * Print banner
 * ========================================================================= */
static void print_banner(void)
{
    printf("\n");
    printf("  ????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????\n");
    printf("  ???   mNVR - Mobile Network Video Recorder   ???\n");
    printf("  ???   Version %-6s                          ???\n", MNVR_VERSION_STR);
    printf("  ????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????\n\n");
}

/* =========================================================================
 * Usage
 * ========================================================================= */
static void print_usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s [options]\n"
            "  -c <path>   Path to mnvr.conf          (default: /etc/mnvr/mnvr.conf)\n"
            "  -s <path>   Path to mnvr_schema.sql      (default: /etc/mnvr/mnvr_schema.sql)\n"
            "  -d <str>    PostgreSQL conninfo          (default: host=localhost dbname=mnvr user=mnvr)\n"
            "  -l <level>  Log level (0=trace .. 5=fatal, default 2=info)\n"
            "  -h          Show this help\n\n",
            prog);
}

/* =========================================================================
 * main()
 * ========================================================================= */
int main(int argc, char *argv[])
{
    print_banner();

    /* ------------------------------------------------------------------ */
    /* 0. Parse command-line arguments                                      */
    /* ------------------------------------------------------------------ */
    const char *conf_path   = "/etc/mnvr/mnvr.conf";
    const char *schema_path = "/etc/mnvr/mnvr_schema.sql";
    const char *db_path     = "host=localhost port=5432 dbname=mnvr user=mnvr password=mnvr sslmode=disable";
    int         log_level   = LOG_LEVEL_INFO;

    int opt;
    while ((opt = getopt(argc, argv, "c:s:d:l:h")) != -1) {
        switch (opt) {
            case 'c': conf_path   = optarg; break;
            case 's': schema_path = optarg; break;
            case 'd': db_path     = optarg; break;
            case 'l': log_level   = atoi(optarg); break;
            case 'h': print_usage(argv[0]); return 0;
            default:  print_usage(argv[0]); return 1;
        }
    }

    /* ------------------------------------------------------------------ */
    /* 1. GStreamer init                                                    */
    /* ------------------------------------------------------------------ */
    gst_init(&argc, &argv);

    /* ------------------------------------------------------------------ */
    /* 2. AppContext init                                                   */
    /* ------------------------------------------------------------------ */
    memset(&g_ctx, 0, sizeof(AppContext));
    pthread_mutex_init(&g_ctx.cameras_mutex, NULL);
    pthread_mutex_init(&g_ctx.shutdown_mutex, NULL);
    pthread_cond_init(&g_ctx.shutdown_cond, NULL);

    strncpy(g_ctx.config_file,       conf_path,   MNVR_MAX_PATH - 1);
    strncpy(g_ctx.db_path,           db_path,     MNVR_MAX_PATH - 1);
    strncpy(g_ctx.firmware_version,  MNVR_VERSION_STR, 31);

    /* ------------------------------------------------------------------ */
    /* 3. Logger                                                            */
    /* ------------------------------------------------------------------ */
    LoggerConfig log_cfg = {
        .log_dir       = "/var/log/mnvr",
        .min_level     = (LogLevel)log_level,
        .log_to_stderr = true,
        .log_to_syslog = false,
        .max_file_mb   = LOGGER_MAX_FILE_MB,
        .max_files     = LOGGER_MAX_FILES,
    };
    g_ctx.logger = logger_create(&log_cfg);
    logger_start(g_ctx.logger);

    LOG_INFO(&g_ctx, "MAIN", "mNVR v%s starting...", MNVR_VERSION_STR);
    LOG_INFO(&g_ctx, "MAIN", "Config: %s  Schema: %s",
             conf_path, schema_path);
    LOG_INFO(&g_ctx, "MAIN", "PostgreSQL conninfo: %s", db_path);

    /* ------------------------------------------------------------------ */
    /* 4. Signal handlers                                                   */
    /* ------------------------------------------------------------------ */
    signal(SIGINT,  handle_sigint_sigterm);
    signal(SIGTERM, handle_sigint_sigterm);
    signal(SIGHUP,  handle_sighup);
    signal(SIGPIPE, SIG_IGN);   /* ignore broken pipe from network */

    /* ------------------------------------------------------------------ */
    /* 5. Config module                                                     */
    /* ------------------------------------------------------------------ */
    g_ctx.config = config_create(conf_path, &g_ctx);
    if (!g_ctx.config || config_load(g_ctx.config) != MNVR_OK) {
        LOG_FATAL(&g_ctx, "MAIN", "Config load failed - check %s", conf_path);
        return 1;
    }
    const SystemConfig *cfg = config_get(g_ctx.config);
    snprintf(g_ctx.device_id,    MNVR_MAX_NAME, "%s", cfg->device_id);
    snprintf(g_ctx.device_name,  MNVR_MAX_NAME, "%s", cfg->device_name);
    snprintf(g_ctx.storage_base, MNVR_MAX_PATH, "%s", cfg->storage_base);
    snprintf(g_ctx.hls_base,     MNVR_MAX_PATH, "%s", cfg->hls_base);

    /* Dump all loaded config values */
    LOG_INFO(&g_ctx, "CONFIG", "=== Configuration Loaded ===");
    LOG_INFO(&g_ctx, "CONFIG", "  device_id          = %s", cfg->device_id);
    LOG_INFO(&g_ctx, "CONFIG", "  device_name        = %s", cfg->device_name);
    LOG_INFO(&g_ctx, "CONFIG", "  storage_base       = %s", cfg->storage_base);
    LOG_INFO(&g_ctx, "CONFIG", "  hls_base           = %s", cfg->hls_base);
    LOG_INFO(&g_ctx, "CONFIG", "  db_path            = %s", cfg->db_path);
    LOG_INFO(&g_ctx, "CONFIG", "  log_dir            = %s", cfg->log_dir);
    LOG_INFO(&g_ctx, "CONFIG", "  segment_duration   = %d sec", cfg->segment_duration_sec);
    LOG_INFO(&g_ctx, "CONFIG", "  segment_max_size   = %d MB", cfg->segment_max_size_mb);
    LOG_INFO(&g_ctx, "CONFIG", "  retention_days     = %d", cfg->recording_retention_days);
    LOG_INFO(&g_ctx, "CONFIG", "  enable_audio       = %s", cfg->enable_audio ? "true" : "false");
    LOG_INFO(&g_ctx, "CONFIG", "  hls_segment_sec    = %d", cfg->hls_segment_sec);
    LOG_INFO(&g_ctx, "CONFIG", "  hls_window_size    = %d", cfg->hls_window_size);
    LOG_INFO(&g_ctx, "CONFIG", "  rtsp_server_port   = %d", cfg->rtsp_server_port);
    LOG_INFO(&g_ctx, "CONFIG", "  api_port           = %d", cfg->api_port);
    LOG_INFO(&g_ctx, "CONFIG", "  enable_face_det    = %s", cfg->enable_face_detection ? "true" : "false");
    LOG_INFO(&g_ctx, "CONFIG", "  enable_motion_det  = %s", cfg->enable_motion_detection ? "true" : "false");
    LOG_INFO(&g_ctx, "CONFIG", "  motion_threshold   = %.2f", cfg->motion_threshold);
    LOG_INFO(&g_ctx, "CONFIG", "  time_sync_method   = %s", cfg->time_sync_method);
    LOG_INFO(&g_ctx, "CONFIG", "  health_poll_sec    = %d", cfg->health_poll_interval_sec);
    LOG_INFO(&g_ctx, "CONFIG", "  cpu_warn           = %.0f%%", cfg->cpu_warn_threshold);
    LOG_INFO(&g_ctx, "CONFIG", "  mem_warn           = %.0f%%", cfg->mem_warn_threshold);
    LOG_INFO(&g_ctx, "CONFIG", "  disk_warn          = %.0f%%", cfg->disk_warn_threshold);
    LOG_INFO(&g_ctx, "CONFIG", "  log_min_level      = %d", cfg->log_min_level);
    LOG_INFO(&g_ctx, "CONFIG", "=== End Configuration ===");

    /* ------------------------------------------------------------------ */
    /* 6. Database module                                                   */
    /* ------------------------------------------------------------------ */
    g_ctx.db = db_module_create(&g_ctx, db_path, schema_path);
    if (!g_ctx.db || db_module_start(g_ctx.db) != MNVR_OK) {
        LOG_FATAL(&g_ctx, "MAIN", "Database init failed");
        return 1;
    }

    /* 6b. Reload config now that DB is available — picks up VMS-managed
     *     recording_settings and hls_settings tables. */
    config_reload(g_ctx.config);
    cfg = config_get(g_ctx.config);
    snprintf(g_ctx.storage_base, MNVR_MAX_PATH, "%s", cfg->storage_base);
    snprintf(g_ctx.hls_base,     MNVR_MAX_PATH, "%s", cfg->hls_base);

    /* ------------------------------------------------------------------ */
    /* 7. Load cameras from DB                                              */
    /* ------------------------------------------------------------------ */
    if (config_load_cameras(g_ctx.config) != MNVR_OK || g_ctx.num_cameras == 0) {
        LOG_WARN(&g_ctx, "MAIN",
                 "No cameras found in DB - running in discovery-only mode");
    }
    LOG_INFO(&g_ctx, "MAIN", "Loaded %d camera(s)", g_ctx.num_cameras);

    /* ------------------------------------------------------------------ */
    /* 8. ONVIF discovery + startup probe                                  */
    /* ------------------------------------------------------------------ */
    g_ctx.onvif = onvif_module_create(&g_ctx, on_new_onvif_device, &g_ctx);
    if (g_ctx.onvif) {
        const OnvifConfig *ocfg = &g_ctx.onvif->config;
        LOG_INFO(&g_ctx, "MAIN",
                 "ONVIF module: %d camera(s) configured, discovery=%s",
                 ocfg->num_cameras,
                 ocfg->enable_discovery ? "ON" : "OFF");

        /* Dump ONVIF config */
        LOG_INFO(&g_ctx, "ONVIF", "=== ONVIF Configuration ===");
        LOG_INFO(&g_ctx, "ONVIF", "  multicast_ip       = %s", ocfg->multicast_ip);
        LOG_INFO(&g_ctx, "ONVIF", "  multicast_port     = %d", ocfg->multicast_port);
        LOG_INFO(&g_ctx, "ONVIF", "  discovery_interval = %d sec", ocfg->discovery_interval_sec);
        LOG_INFO(&g_ctx, "ONVIF", "  probe_timeout      = %d ms", ocfg->probe_timeout_ms);
        LOG_INFO(&g_ctx, "ONVIF", "  enable_discovery   = %s", ocfg->enable_discovery ? "true" : "false");
        for (int i = 0; i < MNVR_MAX_CAMERAS; i++) {
            const OnvifCameraConfig *cc = &ocfg->cameras[i];
            if (!cc->enabled) continue;
            LOG_INFO(&g_ctx, "ONVIF",
                     "  cam_%d: ip=%s port=%d onvif_user=%s "
                     "rtsp_user=%s type=%s location=%s",
                     cc->slot, cc->ip, cc->port, cc->user,
                     cc->rtsp_user[0] ? cc->rtsp_user : "(onvif)",
                     cc->camera_type[0] ? cc->camera_type : "INTERIOR",
                     cc->location[0] ? cc->location : "(none)");
        }
        LOG_INFO(&g_ctx, "ONVIF", "=== End ONVIF Configuration ===");

        if (onvif_module_start(g_ctx.onvif) == MNVR_OK)
            LOG_INFO(&g_ctx, "MAIN", "ONVIF module started (probe+discovery)");
    }

    /* ------------------------------------------------------------------ */
    /* 8b. Reload cameras — ONVIF probe may have auto-registered new ones  */
    /* ------------------------------------------------------------------ */
    if (g_ctx.num_cameras == 0) {
        int prev = g_ctx.num_cameras;
        config_load_cameras(g_ctx.config);
        if (g_ctx.num_cameras > prev) {
            LOG_INFO(&g_ctx, "MAIN",
                     "ONVIF auto-registered %d camera(s) — "
                     "reloaded from DB (total: %d)",
                     g_ctx.num_cameras - prev, g_ctx.num_cameras);
        }
    }

    /* ------------------------------------------------------------------ */
    /* 9. Health monitor                                                    */
    /* ------------------------------------------------------------------ */
    g_ctx.health = health_module_create(&g_ctx);
    if (g_ctx.health && health_module_start(g_ctx.health) == MNVR_OK)
        LOG_INFO(&g_ctx, "MAIN", "Health monitor started");

    /* ------------------------------------------------------------------ */
    /* 10. Recorder (GStreamer MP4 segments)                                */
    /* ------------------------------------------------------------------ */
    g_ctx.recorder = recorder_module_create(&g_ctx, on_segment_complete, &g_ctx);
    if (g_ctx.recorder && recorder_module_start(g_ctx.recorder) == MNVR_OK)
        LOG_INFO(&g_ctx, "MAIN", "Recorder started (%d camera(s))",
                 g_ctx.num_cameras);

    /* ------------------------------------------------------------------ */
    /* 11. HLS segmenter                                                    */
    /* ------------------------------------------------------------------ */
    g_ctx.hls = hls_module_create(&g_ctx);
    if (g_ctx.hls && hls_module_start(g_ctx.hls) == MNVR_OK)
        LOG_INFO(&g_ctx, "MAIN", "HLS module started");

    /* ------------------------------------------------------------------ */
    /* 12. Live streamer (RTSP re-stream + frame tap for AI)                */
    /* ------------------------------------------------------------------ */
    g_ctx.streamer = streamer_module_create(&g_ctx, on_frame, &g_ctx);
    if (g_ctx.streamer && streamer_module_start(g_ctx.streamer) == MNVR_OK)
        LOG_INFO(&g_ctx, "MAIN", "Streamer started");

    /* ------------------------------------------------------------------ */
    /* 12b. RTSP relay — re-exposes each camera's watermarked restream as */
    /*      rtsp://127.0.0.1:8555/cam_<id>. MediaMTX (mediamtx-sync.py)   */
    /*      should point at THIS instead of the camera's own RTSP URL,   */
    /*      so only mnvrd ever opens an RTSP session to the physical     */
    /*      camera (many cameras only allow 1-2 concurrent RTSP clients, */
    /*      and this was previously racing against MediaMTX's own pull). */
    /* ------------------------------------------------------------------ */
    g_ctx.rtsp_relay = rtsp_relay_module_create(&g_ctx, 8555);
    if (g_ctx.rtsp_relay) {
        for (int i = 0; i < g_ctx.num_cameras; i++)
            rtsp_relay_add_camera(g_ctx.rtsp_relay, g_ctx.cameras[i].camera_id);
        if (rtsp_relay_module_start(g_ctx.rtsp_relay) == MNVR_OK)
            LOG_INFO(&g_ctx, "MAIN", "RTSP relay started on :8555 (%d camera(s))",
                     g_ctx.num_cameras);
    }

    /* ------------------------------------------------------------------ */
    /* 12c. GPS telemetry (date/time + GPS/speed watermark)                */
    /*      No-op until gps_enabled=true + gps_serial_device is set in     */
    /*      mnvr.conf to a real GPS receiver — see gps_module.h.           */
    /* ------------------------------------------------------------------ */
    g_ctx.gps = gps_module_create(&g_ctx);
    if (g_ctx.gps) gps_module_start(g_ctx.gps);

    /* ------------------------------------------------------------------ */
    /* 13. AI analytics                                                     */
    /* ------------------------------------------------------------------ */
    g_ctx.ai = ai_module_create(&g_ctx, on_ai_event, &g_ctx);
    if (g_ctx.ai && ai_module_start(g_ctx.ai) == MNVR_OK)
        LOG_INFO(&g_ctx, "MAIN", "AI module started");

    /* ------------------------------------------------------------------ */
    /* 14. REST API server                                                  */
    /* ------------------------------------------------------------------ */
    g_ctx.api = api_module_create(&g_ctx);
    if (g_ctx.api && api_module_start(g_ctx.api) == MNVR_OK) {
#ifdef MNVR_WITH_API
        LOG_INFO(&g_ctx, "MAIN",
                 "API server started on port %d", cfg->api_port);
#else
        LOG_WARN(&g_ctx, "MAIN",
                 "REST API disabled (built without libmicrohttpd). "
                 "Install libmicrohttpd-dev and rebuild to enable.");
#endif
    }

    LOG_INFO(&g_ctx, "MAIN",
             "mNVR fully operational - %d camera(s). "
             "Press Ctrl+C to stop.",
             g_ctx.num_cameras);

    /* ================================================================== */
    /* MAIN WHILE LOOP  -  shutdown gate + config reload                   */
    /* ================================================================== */
    pthread_mutex_lock(&g_ctx.shutdown_mutex);
    while (!g_ctx.shutdown_requested) {

        /* Wait up to 30 seconds between wakeups */
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += 30;
        pthread_cond_timedwait(&g_ctx.shutdown_cond,
                                &g_ctx.shutdown_mutex, &ts);

        /* Config reload (triggered by SIGHUP) */
        if (g_ctx.config && g_ctx.config->dirty) {
            LOG_INFO(&g_ctx, "MAIN", "Reloading configuration (SIGHUP)...");
            config_reload(g_ctx.config);
        }

        /* Heartbeat log every 30s */
        LOG_DEBUG(&g_ctx, "MAIN",
                  "Heartbeat - cams:%d shutdown:%s",
                  g_ctx.num_cameras,
                  g_ctx.shutdown_requested ? "yes" : "no");
    }
    pthread_mutex_unlock(&g_ctx.shutdown_mutex);

    /* ================================================================== */
    /* ORDERED TEARDOWN  (reverse startup order)                           */
    /* ================================================================== */
    LOG_INFO(&g_ctx, "MAIN", "Shutdown initiated...");

    if (g_ctx.api)      { api_module_stop(g_ctx.api);      api_module_destroy(g_ctx.api);      }
    if (g_ctx.ai)       { ai_module_stop(g_ctx.ai);        ai_module_destroy(g_ctx.ai);        }
    if (g_ctx.gps)      { gps_module_destroy(g_ctx.gps); }
    if (g_ctx.rtsp_relay) { rtsp_relay_module_destroy(g_ctx.rtsp_relay); }
    if (g_ctx.streamer) { streamer_module_stop(g_ctx.streamer); streamer_module_destroy(g_ctx.streamer); }
    if (g_ctx.hls)      { hls_module_stop(g_ctx.hls);      hls_module_destroy(g_ctx.hls);      }
    if (g_ctx.recorder) { recorder_module_stop(g_ctx.recorder); recorder_module_destroy(g_ctx.recorder); }
    if (g_ctx.health)   { health_module_stop(g_ctx.health); health_module_destroy(g_ctx.health); }
    if (g_ctx.onvif)    { onvif_module_stop(g_ctx.onvif);  onvif_module_destroy(g_ctx.onvif);  }
    if (g_ctx.db)       { db_module_stop(g_ctx.db);        db_module_destroy(g_ctx.db);        }
    if (g_ctx.config)   { config_destroy(g_ctx.config); }

    /* Flush and stop logger last */
    LOG_INFO(&g_ctx, "MAIN", "mNVR shutdown complete");
    logger_flush(g_ctx.logger);
    logger_stop(g_ctx.logger);
    logger_destroy(g_ctx.logger);

    pthread_mutex_destroy(&g_ctx.cameras_mutex);
    pthread_mutex_destroy(&g_ctx.shutdown_mutex);
    pthread_cond_destroy(&g_ctx.shutdown_cond);

    gst_deinit();
    return 0;
}