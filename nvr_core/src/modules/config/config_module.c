/**
 * @file config_module.c
 * @brief Configuration module implementation
 *
 * Two-phase config load:
 *   Phase 1 - Parse INI file (mnvr.conf) into SystemConfig.
 *   Phase 2 - Query PostgreSQL system_config and cameras tables
 *              to override / supplement INI values and populate
 *              ctx->cameras[].
 *
 * Reload (SIGHUP):
 *   Main loop calls config_reload() when dirty flag is set.
 *   Modules that care about config changes poll config_get() each
 *   iteration; critical modules (recorder) are restarted explicitly
 *   by main after a reload.
 */

#include "config_module.h"
#include "../../db/db_module.h"
#include "../logger/logger.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <libpq-fe.h>

/* -------------------------------------------------------------------------
 * Defaults
 * ------------------------------------------------------------------------- */
static void apply_defaults(SystemConfig *s)
{
    strncpy(s->storage_base,          "/storage/recordings",MNVR_MAX_PATH-1);
    strncpy(s->hls_base,              "/storage/hls",       MNVR_MAX_PATH-1);
    strncpy(s->db_path,               "/etc/mnvr/mnvr.db",  MNVR_MAX_PATH-1);
    strncpy(s->log_dir,               "/var/log/mnvr",      MNVR_MAX_PATH-1);
    strncpy(s->device_name,           "MNVR-001",           MNVR_MAX_NAME-1);
    strncpy(s->time_sync_method,      "PTP",                15);
    strncpy(s->ntp_server,            "pool.ntp.org",       127);

    s->recording_retention_days = 30;
    s->segment_duration_sec     = 60;   /* 15-minute MP4 segments */
    s->segment_max_size_mb      = 2048;
    s->enable_audio             = true;
    s->enable_watermark         = true;

    s->hls_segment_sec          = MNVR_HLS_SEGMENT_SEC;
    s->hls_window_size          = MNVR_HLS_MAX_SEGMENTS;
    s->hls_delete_old_segments  = true;

    s->rtsp_server_port         = 8554;
    s->webrtc_server_port       = 8080;

    s->enable_face_detection    = true;
    s->enable_motion_detection  = true;
    s->enable_rdas              = false;
    s->motion_threshold         = 0.05f;
    strncpy(s->face_model_path, "/etc/mnvr/blazeface_full.tflite",
            sizeof(s->face_model_path) - 1);
    s->face_detect_interval     = 8;

    s->api_port                 = 8443;
    s->api_tls_enabled          = true;

    s->health_poll_interval_sec = 10;
    s->cpu_warn_threshold       = 85.0f;
    s->mem_warn_threshold       = 90.0f;
    s->disk_warn_threshold      = 90.0f;

    s->log_min_level            = LOG_LEVEL_INFO;
    s->log_to_syslog            = false;
    s->ptp_domain               = 0;

    s->gps_enabled              = false;  /* flip on once hardware is wired up */
    strncpy(s->gps_serial_device, "/dev/ttyUSB0", MNVR_MAX_PATH-1);
    s->gps_baud_rate             = 9600;
}

/* -------------------------------------------------------------------------
 * INI parser (minimal, no external dependency)
 * ------------------------------------------------------------------------- */
static void trim(char *s)
{
    char *p = s;
    while (isspace((unsigned char)*p)) p++;
    memmove(s, p, strlen(p) + 1);
    char *e = s + strlen(s) - 1;
    while (e >= s && isspace((unsigned char)*e)) *e-- = '\0';
}

static void parse_ini(SystemConfig *s, const char *path)
{
    FILE *fp = fopen(path, "r");
    if (!fp) return;

    char line[1024];
    while (fgets(line, sizeof(line), fp)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (line[0] == '#' || line[0] == '[' || line[0] == '\0') continue;

        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = line;  char *val = eq + 1;
        trim(key); trim(val);

#define PARSE_STR(field, name) \
        if (strcmp(key, name) == 0) strncpy(s->field, val, sizeof(s->field)-1);
#define PARSE_INT(field, name) \
        if (strcmp(key, name) == 0) s->field = atoi(val);
#define PARSE_BOOL(field, name) \
        if (strcmp(key, name) == 0) s->field = (strcmp(val,"1")==0||strcmp(val,"true")==0);
#define PARSE_FLOAT(field, name) \
        if (strcmp(key, name) == 0) s->field = (float)atof(val);

        PARSE_STR(db_path,               "db_path")
        PARSE_STR(log_dir,               "log_dir")
        PARSE_STR(device_id,             "device_id")
        PARSE_STR(device_name,           "device_name")
        PARSE_INT(rtsp_server_port,      "rtsp_server_port")
        PARSE_INT(api_port,              "api_port")
        PARSE_BOOL(api_tls_enabled,      "api_tls_enabled")
        PARSE_STR(api_tls_cert,          "api_tls_cert")
        PARSE_STR(api_tls_key,           "api_tls_key")
        PARSE_BOOL(enable_face_detection,"enable_face_detection")
        PARSE_BOOL(enable_motion_detection,"enable_motion_detection")
        PARSE_BOOL(enable_rdas,          "enable_rdas")
        PARSE_FLOAT(motion_threshold,    "motion_threshold")
        PARSE_STR(face_model_path,       "face_model_path")
        PARSE_INT(face_detect_interval,  "face_detect_interval")
        PARSE_STR(time_sync_method,      "time_sync_method")
        PARSE_STR(ntp_server,            "ntp_server")
        PARSE_INT(ptp_domain,            "ptp_domain")
        PARSE_BOOL(log_to_syslog,        "log_to_syslog")
        PARSE_BOOL(gps_enabled,          "gps_enabled")
        PARSE_STR(gps_serial_device,     "gps_serial_device")
        PARSE_INT(gps_baud_rate,         "gps_baud_rate")
    }
    fclose(fp);
}

/* -------------------------------------------------------------------------
 * PostgreSQL config overlay
 * ------------------------------------------------------------------------- */
static void load_from_db(SystemConfig *s, const char *conninfo)
{
    PGconn *conn = PQconnectdb(conninfo);
    if (!conn || PQstatus(conn) != CONNECTION_OK) {
        if (conn) PQfinish(conn);
        return;   /* DB not available yet on first boot */
    }

    PGresult *res = PQexec(conn,
        "SELECT config_key, config_value FROM system_config");

    if (PQresultStatus(res) == PGRES_TUPLES_OK) {
        int nrows = PQntuples(res);
        for (int i = 0; i < nrows; i++) {
            const char *k = PQgetvalue(res, i, 0);
            const char *v = PQgetvalue(res, i, 1);
            if (!k || !v) continue;

            if (strcmp(k,"device_id")   == 0) strncpy(s->device_id,   v, MNVR_MAX_NAME-1);
            if (strcmp(k,"device_name") == 0) strncpy(s->device_name, v, MNVR_MAX_NAME-1);
            if (strcmp(k,"enable_face_detection")==0) s->enable_face_detection = (strcmp(v,"1")==0);
            if (strcmp(k,"rtsp_server_port")==0) s->rtsp_server_port = atoi(v);
            if (strcmp(k,"api_server_port")==0)  s->api_port = atoi(v);
            if (strcmp(k,"time_sync_method")==0) strncpy(s->time_sync_method,v,15);
            if (strcmp(k,"ntp_server")==0) strncpy(s->ntp_server,v,127);
            if (strcmp(k,"ptp_domain")==0) s->ptp_domain = atoi(v);
        }
    }
    PQclear(res);
    PQfinish(conn);
}

/* -------------------------------------------------------------------------
 * Sync final config values back to system_config DB table.
 * After config_load() resolves precedence (INI wins), this function
 * UPDATEs the DB rows so they match the running config.
 * Also logs all rows in the table for verification.
 * ------------------------------------------------------------------------- */
static void sync_config_to_db(const SystemConfig *s, const char *conninfo,
                                AppContext *ctx)
{
    PGconn *conn = PQconnectdb(conninfo);
    if (!conn || PQstatus(conn) != CONNECTION_OK) {
        if (conn) PQfinish(conn);
        return;
    }

    /* UPDATE each known config key with the final resolved value */
    char sql[512];
    const char *fmt =
        "UPDATE system_config SET config_value='%s', "
        "last_modified_at=NOW(), last_modified_by='mnvrd' "
        "WHERE config_key='%s';";

    char val_buf[64];

#define SYNC_STR(key, field) \
    snprintf(sql, sizeof(sql), fmt, (field), (key)); PQexec(conn, sql);

#define SYNC_INT(key, field) \
    snprintf(val_buf, sizeof(val_buf), "%d", (field)); \
    snprintf(sql, sizeof(sql), fmt, val_buf, (key)); PQexec(conn, sql);

#define SYNC_BOOL(key, field) \
    snprintf(sql, sizeof(sql), fmt, (field) ? "1" : "0", (key)); PQexec(conn, sql);

    SYNC_STR ("device_id",               s->device_id);
    SYNC_STR ("device_name",             s->device_name);
    SYNC_BOOL("enable_face_detection",   s->enable_face_detection);
    SYNC_INT ("rtsp_server_port",        s->rtsp_server_port);
    SYNC_INT ("api_server_port",         s->api_port);
    SYNC_STR ("time_sync_method",        s->time_sync_method);
    SYNC_STR ("ntp_server",              s->ntp_server);
    SYNC_INT ("ptp_domain",              s->ptp_domain);

#undef SYNC_STR
#undef SYNC_INT
#undef SYNC_BOOL

    /* Now read back and display the full table */
    if (ctx) {
        LOG_INFO(ctx, "CONFIG", "=== system_config table (after sync) ===");
    }
    PGresult *res = PQexec(conn,
        "SELECT config_key, config_value, config_type, is_readonly "
        "FROM system_config ORDER BY id");

    if (PQresultStatus(res) == PGRES_TUPLES_OK) {
        int nrows = PQntuples(res);
        for (int i = 0; i < nrows; i++) {
            const char *k = PQgetvalue(res, i, 0);
            const char *v = PQgetvalue(res, i, 1);
            const char *t = PQgetvalue(res, i, 2);
            const char *ro = PQgetvalue(res, i, 3);
            if (ctx) {
                LOG_INFO(ctx, "CONFIG", "  %-28s = %-30s [%s%s]",
                         k, v, t,
                         (ro && strcmp(ro, "1") == 0) ? ",RO" : "");
            }
        }
    }
    if (ctx) {
        LOG_INFO(ctx, "CONFIG", "=== end system_config ===");
    }

    PQclear(res);
    PQfinish(conn);
}

/* -------------------------------------------------------------------------
 * Load recording settings from recording_settings table (via DbModule)
 * ------------------------------------------------------------------------- */
static void rec_settings_row_cb(void *user_data, int ncols,
                                  char **vals, char **cols)
{
    (void)ncols; (void)cols;
    SystemConfig *s = (SystemConfig *)user_data;
    if (!s) return;

    /* Column order: storage_path, recording_retention_days,
                     segment_duration_sec, segment_max_size_mb,
                     max_storage_gb, enable_audio, enable_watermark */
    if (vals[0]) strncpy(s->storage_base, vals[0], MNVR_MAX_PATH - 1);
    if (vals[1]) s->recording_retention_days = atoi(vals[1]);
    if (vals[2]) s->segment_duration_sec     = atoi(vals[2]);
    if (vals[3]) s->segment_max_size_mb      = atoi(vals[3]);
    /* vals[4] = max_storage_gb — not in SystemConfig yet, skip */
    if (vals[5]) s->enable_audio     = (vals[5][0] == 't');
    if (vals[6]) s->enable_watermark = (vals[6][0] == 't');
}

static void load_recording_settings(SystemConfig *s, DbModule *db)
{
    if (!db) return;
    db_query(db,
        "SELECT storage_path, recording_retention_days, "
        "       segment_duration_sec, segment_max_size_mb, "
        "       max_storage_gb, enable_audio, enable_watermark "
        "FROM recording_settings ORDER BY id LIMIT 1",
        rec_settings_row_cb, s);
}

/* -------------------------------------------------------------------------
 * Load HLS settings from hls_settings table (via DbModule)
 * ------------------------------------------------------------------------- */
static void hls_settings_row_cb(void *user_data, int ncols,
                                  char **vals, char **cols)
{
    (void)ncols; (void)cols;
    SystemConfig *s = (SystemConfig *)user_data;
    if (!s) return;

    /* Column order: hls_base, hls_segment_sec, hls_window_size,
                     hls_delete_old_segments */
    if (vals[0]) strncpy(s->hls_base, vals[0], MNVR_MAX_PATH - 1);
    if (vals[1]) s->hls_segment_sec         = atoi(vals[1]);
    if (vals[2]) s->hls_window_size         = atoi(vals[2]);
    if (vals[3]) s->hls_delete_old_segments = (vals[3][0] == 't');
}

static void load_hls_settings(SystemConfig *s, DbModule *db)
{
    if (!db) return;
    db_query(db,
        "SELECT hls_base, hls_segment_sec, hls_window_size, "
        "       hls_delete_old_segments "
        "FROM hls_settings ORDER BY id LIMIT 1",
        hls_settings_row_cb, s);
}

/* -------------------------------------------------------------------------
 * Load health monitor settings from health_monitor_settings table
 * ------------------------------------------------------------------------- */
static void health_settings_row_cb(void *user_data, int ncols,
                                     char **vals, char **cols)
{
    (void)ncols; (void)cols;
    SystemConfig *s = (SystemConfig *)user_data;
    if (!s) return;

    /* Column order: poll_interval_sec, cpu_warn_threshold,
                     mem_warn_threshold, disk_warn_threshold */
    if (vals[0]) s->health_poll_interval_sec = atoi(vals[0]);
    if (vals[1]) s->cpu_warn_threshold       = (float)atof(vals[1]);
    if (vals[2]) s->mem_warn_threshold       = (float)atof(vals[2]);
    if (vals[3]) s->disk_warn_threshold      = (float)atof(vals[3]);
}

static void load_health_settings(SystemConfig *s, DbModule *db)
{
    if (!db) return;
    db_query(db,
        "SELECT poll_interval_sec, cpu_warn_threshold, "
        "       mem_warn_threshold, disk_warn_threshold "
        "FROM health_monitor_settings ORDER BY id LIMIT 1",
        health_settings_row_cb, s);
}

/* -------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */

ConfigModule *config_create(const char *ini_path, AppContext *ctx)
{
    ConfigModule *m = calloc(1, sizeof(ConfigModule));
    if (!m) return NULL;
    strncpy(m->ini_path, ini_path, MNVR_MAX_PATH - 1);
    m->ctx = ctx;
    pthread_mutex_init(&m->mutex, NULL);
    return m;
}

MnvrResult config_load(ConfigModule *m)
{
    if (!m) return MNVR_ERR_GENERIC;
    apply_defaults(&m->sys);

    /* Load order:
     *   1. Parse INI (needed to get db_path connection string)
     *   2. Load DB system_config (provisioning defaults)
     *   3. Parse INI again (so mnvr.conf always wins over DB values)
     *   4. Sync final values back to DB (so DB always reflects running config)
     *   5. Load recording_settings and hls_settings from dedicated tables
     *      (these are VMS-managed and always authoritative over INI/defaults)
     *
     * Precedence (highest to lowest):
     *   DB recording_settings/hls_settings  >  mnvr.conf  >  DB system_config  >  compiled defaults
     */
    parse_ini(&m->sys, m->ini_path);         /* 1st pass: get db_path */
    load_from_db(&m->sys, m->sys.db_path);   /* DB fills in provisioning values */
    parse_ini(&m->sys, m->ini_path);         /* 2nd pass: INI wins over DB */
    sync_config_to_db(&m->sys, m->sys.db_path, m->ctx);  /* write back to DB */

    /* VMS-managed tables override everything (available after DB module starts) */
    if (m->ctx && m->ctx->db) {
        load_recording_settings(&m->sys, m->ctx->db);
        load_hls_settings(&m->sys, m->ctx->db);
        load_health_settings(&m->sys, m->ctx->db);
    }

    m->dirty = false;
    return MNVR_OK;
}

MnvrResult config_reload(ConfigModule *m)
{
    pthread_mutex_lock(&m->mutex);
    MnvrResult r = config_load(m);
    pthread_mutex_unlock(&m->mutex);
    return r;
}

/* ---- Callback context for config_load_cameras ---- */
typedef struct {
    ConfigModule *m;
    AppContext   *ctx;
    int           count;
} CamLoadCtx;

/* Percent-encode characters that break an RTSP URL userinfo field.
 * '@' must always be encoded (it is the userinfo delimiter).
 * ':' is encoded in passwords to avoid port confusion. */
static void pct_encode(const char *src, char *dst, size_t dstsz,
                        bool encode_colon)
{
    size_t di = 0;
    for (const char *s = src; *s && di + 4 < dstsz; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '@' || (encode_colon && c == ':')) {
            dst[di++] = '%';
            dst[di++] = "0123456789ABCDEF"[c >> 4];
            dst[di++] = "0123456789ABCDEF"[c & 0xf];
        } else {
            dst[di++] = (char)c;
        }
    }
    dst[di] = '\0';
}

/* Build an RTSP URL with percent-encoded credentials.
 * If separate credentials are provided, any embedded user:pass@ in the
 * raw_url is stripped first, then the encoded credentials are re-inserted. */
static void build_rtsp_url(char *out, size_t outsz,
                             const char *raw_url,
                             const char *username,
                             const char *password)
{
    if ((!username || !username[0]) && (!password || !password[0])) {
        strncpy(out, raw_url, outsz - 1);
        out[outsz - 1] = '\0';
        return;
    }
    const char *scheme_end = strstr(raw_url, "://");
    if (!scheme_end) { strncpy(out, raw_url, outsz - 1); out[outsz-1]='\0'; return; }

    char scheme[16] = {0};
    size_t slen = (size_t)(scheme_end - raw_url);
    if (slen >= sizeof(scheme)) slen = sizeof(scheme) - 1;
    memcpy(scheme, raw_url, slen);

    const char *hostpart = scheme_end + 3;
    /* Strip embedded userinfo up to the LAST '@' before the path starts —
     * not just the first. A raw_url that (from before this fix existed)
     * still has doubled/corrupted credentials like
     * "user:pass@word@host:port/path" has TWO '@' before the real host;
     * stopping at the first one leaves a bogus "word@host" fragment that
     * gets treated as the hostname. The last '@' before the first '/' is
     * always the true userinfo/host boundary, regardless of how many '@'
     * characters appear in the (broken) credentials portion. */
    const char *slash = strchr(hostpart, '/');
    size_t search_len = slash ? (size_t)(slash - hostpart) : strlen(hostpart);
    const char *last_at = NULL;
    for (size_t i = 0; i < search_len; i++)
        if (hostpart[i] == '@') last_at = hostpart + i;
    if (last_at) hostpart = last_at + 1;

    char eu[192] = {0}, ep[384] = {0};
    pct_encode(username ? username : "", eu, sizeof(eu), false);
    pct_encode(password ? password : "", ep, sizeof(ep), true);

    if (ep[0])
        snprintf(out, outsz, "%s://%s:%s@%s", scheme, eu, ep, hostpart);
    else if (eu[0])
        snprintf(out, outsz, "%s://%s@%s", scheme, eu, hostpart);
    else
        snprintf(out, outsz, "%s://%s", scheme, hostpart);
}

static void cam_load_row_cb(void *user_data, int ncols,
                              char **vals, char **cols)
{
    (void)ncols; (void)cols;
    CamLoadCtx *cl = (CamLoadCtx *)user_data;
    if (!cl || cl->count >= MNVR_MAX_CAMERAS) return;

    AppContext   *ctx = cl->ctx;
    ConfigModule *m   = cl->m;

    CameraInfo *cam = &ctx->cameras[cl->count];
    memset(cam, 0, sizeof(CameraInfo));

    /* Column order matches the SELECT below:
       0  camera_id          1  camera_name       2  camera_type
       3  ip_address         4  rtsp_url           5  resolution_width
       6  resolution_height  7  target_fps         8  video_codec
       9  location_description  10 audio_supported  11 ptz_supported
      12  status             13 rec_output_dir     14 hls_output_dir
      15  hls_playlist_url
      16  rtsp_username (from cameras_config_details, may be NULL)
      17  rtsp_password (from cameras_config_details, may be NULL)
      18  rec_rtsp_url (may be NULL — falls back to rtsp_url) */

    cam->camera_id         = vals[0]  ? atoi(vals[0]) : 0;
    strncpy(cam->name,          vals[1]  ? vals[1]  : "cam",    MNVR_MAX_NAME - 1);
    strncpy(cam->ip_address,    vals[3]  ? vals[3]  : "",       63);
    cam->resolution_width  = vals[5]  ? atoi(vals[5]) : 1920;
    cam->resolution_height = vals[6]  ? atoi(vals[6]) : 1080;
    cam->target_fps        = vals[7]  ? atoi(vals[7]) : 25;
    strncpy(cam->codec,         vals[8]  ? vals[8]  : "H.264",  15);
    strncpy(cam->location_desc, vals[9]  ? vals[9]  : "",       127);
    cam->audio_enabled     = vals[10] && strcmp(vals[10], "t") == 0;
    cam->ptz_supported     = vals[11] && strcmp(vals[11], "t") == 0;
    cam->state             = CAM_STATE_ACTIVE;

    /* Credentials from cameras_config_details (cols 16 & 17) */
    strncpy(cam->rtsp_username, vals[16] ? vals[16] : "",
            sizeof(cam->rtsp_username) - 1);
    strncpy(cam->rtsp_password, vals[17] ? vals[17] : "",
            sizeof(cam->rtsp_password) - 1);

    /* Build RTSP URL with properly encoded credentials.
     * cameras_config_details credentials take precedence; any credentials
     * embedded in the cameras.rtsp_url column are replaced. */
    const char *raw_url = vals[4] ? vals[4] : "";
    if (cam->rtsp_username[0] || cam->rtsp_password[0])
        build_rtsp_url(cam->rtsp_url, sizeof(cam->rtsp_url),
                       raw_url, cam->rtsp_username, cam->rtsp_password);
    else
        strncpy(cam->rtsp_url, raw_url, MNVR_MAX_URL - 1);

    /* Recording-profile URL — same credential treatment. Falls back to the
     * live URL (already built above) when the camera has no distinct
     * recording profile (rec_rtsp_url NULL/empty/same as rtsp_url). */
    const char *raw_rec_url = (vals[18] && vals[18][0]) ? vals[18] : raw_url;
    if (cam->rtsp_username[0] || cam->rtsp_password[0])
        build_rtsp_url(cam->rec_rtsp_url, sizeof(cam->rec_rtsp_url),
                       raw_rec_url, cam->rtsp_username, cam->rtsp_password);
    else
        strncpy(cam->rec_rtsp_url, raw_rec_url, MNVR_MAX_URL - 1);

    /* rec_output_dir */
    if (vals[13] && vals[13][0])
        snprintf(cam->rec_output_dir, MNVR_MAX_PATH, "%.511s", vals[13]);
    else
        snprintf(cam->rec_output_dir, MNVR_MAX_PATH, "%.480s/cam_%d",
                 m->sys.storage_base, cam->camera_id);

    /* hls_output_dir */
    if (vals[14] && vals[14][0])
        snprintf(cam->hls_output_dir, MNVR_MAX_PATH, "%.511s", vals[14]);
    else
        snprintf(cam->hls_output_dir, MNVR_MAX_PATH, "%.480s/cam_%d",
                 m->sys.hls_base, cam->camera_id);

    /* stream_url (always computed) */
    snprintf(cam->stream_url, MNVR_MAX_URL, "rtsp://127.0.0.1:%d/cam_%d",
             m->sys.rtsp_server_port, cam->camera_id);

    cl->count++;
}

MnvrResult config_load_cameras(ConfigModule *m)
{
    if (!m || !m->ctx) return MNVR_ERR_GENERIC;
    AppContext *ctx = m->ctx;

    if (!ctx->db) {
        LOG_WARN(ctx, "CONFIG", "Cannot load cameras: DB module not available");
        return MNVR_ERR_DB;
    }

    /* LEFT JOIN cameras_config_details to pull per-camera RTSP credentials.
     * Matching on host(cd.ip_address) = c.ip_address since cameras stores
     * IP as text and cameras_config_details uses the inet type. */
    char sql[768];
    snprintf(sql, sizeof(sql),
        "SELECT c.camera_id, c.camera_name, c.camera_type, c.ip_address, c.rtsp_url, "
        "       c.resolution_width, c.resolution_height, c.target_fps, c.video_codec, "
        "       c.location_description, c.audio_supported, c.ptz_supported, c.status, "
        "       c.rec_output_dir, c.hls_output_dir, c.hls_playlist_url, "
        "       cd.rtsp_username, cd.rtsp_password, c.rec_rtsp_url "
        "FROM cameras c "
        "LEFT JOIN cameras_config_details cd ON host(cd.ip_address) = c.ip_address "
        "WHERE c.status='ACTIVE' ORDER BY c.camera_id LIMIT %d",
        MNVR_MAX_CAMERAS);

    pthread_mutex_lock(&ctx->cameras_mutex);
    ctx->num_cameras = 0;

    CamLoadCtx cl = { .m = m, .ctx = ctx, .count = 0 };
    MnvrResult r = db_query(ctx->db, sql, cam_load_row_cb, &cl);

    if (r != MNVR_OK) {
        LOG_WARN(ctx, "CONFIG", "Camera query failed (result=%d)", r);
        pthread_mutex_unlock(&ctx->cameras_mutex);
        return MNVR_ERR_DB;
    }

    ctx->num_cameras = cl.count;
    pthread_mutex_unlock(&ctx->cameras_mutex);

    LOG_INFO(ctx, "CONFIG", "Loaded %d active cameras from DB", ctx->num_cameras);
    return MNVR_OK;
}

const SystemConfig *config_get(ConfigModule *m)
{
    return m ? &m->sys : NULL;
}

bool config_get_bool(ConfigModule *m, const char *key, bool def)
{
    if (!m || !key) return def;
    const SystemConfig *s = &m->sys;

    if (strcmp(key, "enable_face_detection")  == 0) return s->enable_face_detection;
    if (strcmp(key, "enable_motion_detection")== 0) return s->enable_motion_detection;
    if (strcmp(key, "enable_rdas")            == 0) return s->enable_rdas;
    if (strcmp(key, "enable_audio")           == 0) return s->enable_audio;
    if (strcmp(key, "enable_watermark")       == 0) return s->enable_watermark;
    if (strcmp(key, "api_tls_enabled")        == 0) return s->api_tls_enabled;
    if (strcmp(key, "hls_delete_old_segments")== 0) return s->hls_delete_old_segments;
    if (strcmp(key, "log_to_syslog")          == 0) return s->log_to_syslog;
    return def;
}

int config_get_int(ConfigModule *m, const char *key, int def)
{
    if (!m || !key) return def;
    const SystemConfig *s = &m->sys;

    if (strcmp(key, "face_detect_interval")      == 0) return s->face_detect_interval;
    if (strcmp(key, "recording_retention_days")  == 0) return s->recording_retention_days;
    if (strcmp(key, "segment_duration_sec")      == 0) return s->segment_duration_sec;
    if (strcmp(key, "segment_max_size_mb")       == 0) return s->segment_max_size_mb;
    if (strcmp(key, "hls_segment_sec")           == 0) return s->hls_segment_sec;
    if (strcmp(key, "hls_window_size")           == 0) return s->hls_window_size;
    if (strcmp(key, "rtsp_server_port")          == 0) return s->rtsp_server_port;
    if (strcmp(key, "api_port")                  == 0) return s->api_port;
    if (strcmp(key, "health_poll_interval_sec")  == 0) return s->health_poll_interval_sec;
    if (strcmp(key, "ptp_domain")                == 0) return s->ptp_domain;
    return def;
}

const char *config_get_str(ConfigModule *m, const char *key, const char *def)
{
    if (!m || !key) return def;
    const SystemConfig *s = &m->sys;

    if (strcmp(key, "face_model_path")  == 0) return s->face_model_path;
    if (strcmp(key, "storage_base")     == 0) return s->storage_base;
    if (strcmp(key, "hls_base")         == 0) return s->hls_base;
    if (strcmp(key, "db_path")          == 0) return s->db_path;
    if (strcmp(key, "log_dir")          == 0) return s->log_dir;
    if (strcmp(key, "device_id")        == 0) return s->device_id;
    if (strcmp(key, "device_name")      == 0) return s->device_name;
    if (strcmp(key, "time_sync_method") == 0) return s->time_sync_method;
    if (strcmp(key, "ntp_server")       == 0) return s->ntp_server;
    if (strcmp(key, "api_tls_cert")     == 0) return s->api_tls_cert;
    if (strcmp(key, "api_tls_key")      == 0) return s->api_tls_key;
    return def;
}

void config_mark_dirty(ConfigModule *m)
{
    if (m) m->dirty = true;
}

void config_destroy(ConfigModule *m)
{
    if (!m) return;
    pthread_mutex_destroy(&m->mutex);
    free(m);
}