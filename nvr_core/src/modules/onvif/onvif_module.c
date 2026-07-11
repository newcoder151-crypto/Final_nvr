/**
 * @file onvif_module.c
 * @brief Full ONVIF Profile S implementation for mNVR
 *
 * Complete Profile S client:
 *   - Config loading from PostgreSQL (onvif_settings + cameras_config_details)
 *   - WS-Discovery multicast + direct unicast probe
 *   - WS-UsernameToken Password Digest (SHA-1) with camera time sync
 *   - Device, Media, PTZ, Imaging, Events services
 *
 * All SOAP calls use a minimal hand-crafted HTTP/1.0 POST client.
 * No external SOAP library dependency.
 */

#include "onvif_module.h"
#include "../../db/db_module.h"
#include "../logger/logger.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <ctype.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/select.h>

/* GLib for SHA-1, base64, random */
#include <glib.h>

/* =========================================================================
 * Forward declarations for internal functions
 * ========================================================================= */
static MnvrResult soap_call(const char *url, const char *body,
                              char *resp_buf, size_t resp_len);
static int build_auth_envelope(char *buf, size_t buflen,
                                const char *url,
                                const char *user, const char *pass,
                                const char *soap_body);
static MnvrResult soap_call_auth(const char *url,
                                   const char *user, const char *pass,
                                   const char *soap_body,
                                   char *resp_buf, size_t resp_len);

/* =========================================================================
 * XML helpers — namespace-aware tag extraction
 * ========================================================================= */

/**
 * Extract content between <tag>...</tag> or <ns:tag>...</ns:tag>.
 * Tries the tag as-is first, then common ONVIF namespace prefixes.
 */
static bool xml_extract(const char *xml, const char *tag,
                          char *out, size_t len)
{
    if (!xml || !tag || !out || len == 0) return false;

    char close[128];
    const char *start = NULL;
    size_t skip = 0;

    /* Try with various patterns: bare, then namespaced.
     * For each, try both <tag> (no attributes) and <tag (with attributes). */
    static const char *ns_prefixes[] = {
        "", "tt", "trt", "tds", "tptz", "timg", "tev", "ter",
        "wsnt", "wsa", "wsa5", "d", "dn", "wsdd",
        "tns1", "tns2", NULL
    };

    for (int i = 0; ns_prefixes[i]; i++) {
        char open_exact[128], open_attr[128];

        if (ns_prefixes[i][0] == '\0') {
            snprintf(open_exact, sizeof(open_exact), "<%s>", tag);
            snprintf(open_attr,  sizeof(open_attr),  "<%s ", tag);
            snprintf(close, sizeof(close), "</%s>", tag);
        } else {
            snprintf(open_exact, sizeof(open_exact), "<%s:%s>",
                     ns_prefixes[i], tag);
            snprintf(open_attr,  sizeof(open_attr),  "<%s:%s ",
                     ns_prefixes[i], tag);
            snprintf(close, sizeof(close), "</%s:%s>",
                     ns_prefixes[i], tag);
        }

        /* Try exact match first (no attributes) */
        start = strstr(xml, open_exact);
        if (start) {
            skip = strlen(open_exact);
            break;
        }

        /* Try with attributes — need to find the closing '>' */
        start = strstr(xml, open_attr);
        if (start) {
            const char *gt = strchr(start + strlen(open_attr), '>');
            if (gt) {
                skip = (size_t)(gt + 1 - start);
                break;
            }
            start = NULL; /* '>' not found, keep searching */
        }
    }

    if (!start) return false;

    const char *content_start = start + skip;
    const char *end = strstr(content_start, close);
    if (!end) return false;

    size_t n = (size_t)(end - content_start);
    if (n >= len) n = len - 1;
    memcpy(out, content_start, n);
    out[n] = '\0';
    return true;
}

/**
 * Decode common XML entities in-place.
 * &amp; -> &, &lt; -> <, &gt; -> >, &quot; -> ", &apos; -> '
 */
static void xml_decode_entities(char *s)
{
    if (!s) return;
    char *r = s, *w = s;
    while (*r) {
        if (*r == '&') {
            if (strncmp(r, "&amp;", 5) == 0)  { *w++ = '&'; r += 5; }
            else if (strncmp(r, "&lt;", 4) == 0)   { *w++ = '<'; r += 4; }
            else if (strncmp(r, "&gt;", 4) == 0)   { *w++ = '>'; r += 4; }
            else if (strncmp(r, "&quot;", 6) == 0)  { *w++ = '"'; r += 6; }
            else if (strncmp(r, "&apos;", 6) == 0)  { *w++ = '\''; r += 6; }
            else { *w++ = *r++; }
        } else {
            *w++ = *r++;
        }
    }
    *w = '\0';
}

/**
 * Extract attribute value from XML tag.
 * Example: xml_extract_attr(xml, "Preset", "token", out, len)
 * finds <Preset token="value"> or <tptz:Preset token="value">
 */
static bool xml_extract_attr(const char *xml, const char *tag,
                               const char *attr, char *out, size_t len)
{
    if (!xml || !tag || !attr || !out || len == 0) return false;

    /* Search for the tag (with or without namespace prefix) */
    char patterns[4][128];
    int np = 0;
    snprintf(patterns[np++], 128, "<%s ", tag);
    snprintf(patterns[np++], 128, "<%s>", tag);

    /* Also try with common prefixes */
    static const char *pfx[] = {"tptz", "trt", "tt", "tds", NULL};
    for (int i = 0; pfx[i] && np < 4; i++) {
        snprintf(patterns[np++], 128, "<%s:%s ", pfx[i], tag);
    }

    const char *tag_start = NULL;
    for (int i = 0; i < np && !tag_start; i++)
        tag_start = strstr(xml, patterns[i]);

    if (!tag_start) return false;

    /* Find the attribute */
    const char *tag_end = strchr(tag_start, '>');
    if (!tag_end) return false;

    char attr_eq[128];
    snprintf(attr_eq, sizeof(attr_eq), "%s=\"", attr);
    const char *attr_start = strstr(tag_start, attr_eq);
    if (!attr_start || attr_start > tag_end) return false;

    attr_start += strlen(attr_eq);
    const char *attr_end = strchr(attr_start, '"');
    if (!attr_end || attr_end > tag_end + 256) return false;

    size_t n = (size_t)(attr_end - attr_start);
    if (n >= len) n = len - 1;
    memcpy(out, attr_start, n);
    out[n] = '\0';
    return true;
}

static int xml_extract_int(const char *xml, const char *tag)
{
    char buf[32] = {0};
    if (xml_extract(xml, tag, buf, sizeof(buf)))
        return atoi(buf);
    return -1;
}

static float xml_extract_float(const char *xml, const char *tag)
{
    char buf[32] = {0};
    if (xml_extract(xml, tag, buf, sizeof(buf)))
        return (float)atof(buf);
    return 0.0f;
}

/**
 * Count occurrences of a substring in xml.
 */
static int xml_count(const char *xml, const char *needle)
{
    int count = 0;
    const char *p = xml;
    size_t nlen = strlen(needle);
    while ((p = strstr(p, needle)) != NULL) {
        count++;
        p += nlen;
    }
    return count;
}

/**
 * Find the nth occurrence of a tag opening and return pointer to it.
 * Returns NULL if not found. n is 0-based.
 */
static const char *xml_find_nth(const char *xml, const char *tag_open,
                                  int n) __attribute__((unused));
static const char *xml_find_nth(const char *xml, const char *tag_open,
                                  int n)
{
    const char *p = xml;
    size_t tlen = strlen(tag_open);
    for (int i = 0; i <= n; i++) {
        p = strstr(p, tag_open);
        if (!p) return NULL;
        if (i < n) p += tlen;
    }
    return p;
}

/**
 * Extract a block between opening and closing tags.
 * Returns pointer into xml on success, writes length to *block_len.
 */
static const char *xml_extract_block(const char *xml,
                                       const char *open_tag,
                                       const char *close_tag,
                                       size_t *block_len) __attribute__((unused));
static const char *xml_extract_block(const char *xml,
                                       const char *open_tag,
                                       const char *close_tag,
                                       size_t *block_len)
{
    const char *start = strstr(xml, open_tag);
    if (!start) return NULL;

    const char *end = strstr(start, close_tag);
    if (!end) return NULL;
    end += strlen(close_tag);

    *block_len = (size_t)(end - start);
    return start;
}

/* =========================================================================
 * Configuration loading from PostgreSQL
 *
 * Reads global ONVIF settings from  onvif_settings  (single row)
 * and per-camera entries from  cameras_config_details.
 *
 * Falls back to compiled defaults when no rows exist yet (fresh
 * install before VMS has provisioned anything).
 * ========================================================================= */

/* ---- callback: onvif_settings row ---- */
typedef struct { OnvifConfig *cfg; bool found; } OsCbCtx;

static void onvif_settings_row_cb(void *user_data, int ncols,
                                    char **vals, char **cols)
{
    (void)ncols; (void)cols;
    OsCbCtx *cx = (OsCbCtx *)user_data;
    if (!cx || cx->found) return;           /* use first row only */
    cx->found = true;

    OnvifConfig *cfg = cx->cfg;
    /* Column order: multicast_ip, multicast_port, discovery_interval_sec,
                     probe_timeout_ms, enable_discovery                    */
    if (vals[0]) strncpy(cfg->multicast_ip, vals[0],
                         sizeof(cfg->multicast_ip) - 1);
    if (vals[1]) cfg->multicast_port         = atoi(vals[1]);
    if (vals[2]) cfg->discovery_interval_sec = atoi(vals[2]);
    if (vals[3]) cfg->probe_timeout_ms       = atoi(vals[3]);
    if (vals[4]) cfg->enable_discovery       = (vals[4][0] == 't');
}

/* ---- callback: cameras_config_details rows ---- */
static void cam_config_row_cb(void *user_data, int ncols,
                                char **vals, char **cols)
{
    (void)ncols; (void)cols;
    OnvifConfig *cfg = (OnvifConfig *)user_data;
    if (!cfg) return;

    /* Column order:
       0  camera_slot       1  name             2  location
       3  camera_type       4  ip_address       5  onvif_port
       6  onvif_username    7  onvif_password   8  rtsp_username
       9  rtsp_password    10  is_active                          */

    int slot = vals[0] ? atoi(vals[0]) : 0;
    if (slot < 1 || slot > MNVR_MAX_CAMERAS) return;

    bool active = vals[10] ? (vals[10][0] == 't') : true;
    if (!active) return;

    OnvifCameraConfig *cam = &cfg->cameras[slot - 1];

    /* ip_address — may come as INET "x.x.x.x" or "x.x.x.x/32" */
    if (vals[4]) {
        strncpy(cam->ip, vals[4], sizeof(cam->ip) - 1);
        /* Strip CIDR suffix if present */
        char *slash = strchr(cam->ip, '/');
        if (slash) *slash = '\0';
        cam->enabled = (cam->ip[0] != '\0');
    }

    if (vals[5]) { cam->port = atoi(vals[5]); if (cam->port <= 0) cam->port = 80; }
    if (vals[6]) strncpy(cam->user,        vals[6], sizeof(cam->user) - 1);
    if (vals[7]) strncpy(cam->pass,        vals[7], sizeof(cam->pass) - 1);
    if (vals[8]) strncpy(cam->rtsp_user,   vals[8], sizeof(cam->rtsp_user) - 1);
    if (vals[9]) strncpy(cam->rtsp_pass,   vals[9], sizeof(cam->rtsp_pass) - 1);
    if (vals[3]) strncpy(cam->camera_type, vals[3], sizeof(cam->camera_type) - 1);
    if (vals[2]) strncpy(cam->location,    vals[2], sizeof(cam->location) - 1);
}

void onvif_config_load(OnvifConfig *cfg, DbModule *db)
{
    if (!cfg) return;

    /* ---- Compiled defaults ---- */
    memset(cfg, 0, sizeof(OnvifConfig));
    strncpy(cfg->multicast_ip, "239.255.255.250", sizeof(cfg->multicast_ip) - 1);
    cfg->multicast_port         = 3702;
    cfg->discovery_interval_sec = 60;
    cfg->probe_timeout_ms       = 3000;
    cfg->enable_discovery       = true;

    for (int i = 0; i < MNVR_MAX_CAMERAS; i++) {
        cfg->cameras[i].slot    = i + 1;
        cfg->cameras[i].port    = 80;
        cfg->cameras[i].enabled = false;
    }

    if (!db) return;   /* no DB yet — run with defaults only */

    /* ---- Global ONVIF settings ---- */
    OsCbCtx os_ctx = { .cfg = cfg, .found = false };
    db_query(db,
        "SELECT multicast_ip, multicast_port, discovery_interval_sec,"
        "       probe_timeout_ms, enable_discovery "
        "FROM onvif_settings ORDER BY id LIMIT 1",
        onvif_settings_row_cb, &os_ctx);

    /* ---- Per-camera config ---- */
    db_query(db,
        "SELECT camera_slot, name, location, camera_type,"
        "       host(ip_address), onvif_port,"
        "       onvif_username, onvif_password,"
        "       rtsp_username, rtsp_password,"
        "       is_active "
        "FROM cameras_config_details "
        "WHERE is_active = TRUE "
        "ORDER BY camera_slot",
        cam_config_row_cb, cfg);

    /* ---- Derive xaddr and count enabled cameras ---- */
    cfg->num_cameras = 0;
    for (int i = 0; i < MNVR_MAX_CAMERAS; i++) {
        OnvifCameraConfig *cam = &cfg->cameras[i];
        if (cam->enabled) {
            if (cam->port == 80)
                snprintf(cam->xaddr, sizeof(cam->xaddr),
                         "http://%s/onvif/device_service", cam->ip);
            else
                snprintf(cam->xaddr, sizeof(cam->xaddr),
                         "http://%s:%d/onvif/device_service",
                         cam->ip, cam->port);
            cfg->num_cameras++;
        }
    }
}

const OnvifCameraConfig *onvif_config_get_camera(const OnvifConfig *cfg,
                                                   int slot)
{
    if (!cfg || slot < 1 || slot > MNVR_MAX_CAMERAS) return NULL;
    const OnvifCameraConfig *cam = &cfg->cameras[slot - 1];
    return cam->enabled ? cam : NULL;
}

/* =========================================================================
 * WS-UsernameToken Password Digest
 * Digest = Base64(SHA1(Nonce_raw + Created_utf8 + Password_utf8))
 * ========================================================================= */
static int build_wsse_header(char *buf, size_t buflen,
                              const char *user, const char *pass,
                              const char *created_utc)
{
    guint8 nonce_raw[20];
    for (int i = 0; i < 20; i++)
        nonce_raw[i] = (guint8)(g_random_int() & 0xFF);

    GChecksum *cs = g_checksum_new(G_CHECKSUM_SHA1);
    g_checksum_update(cs, nonce_raw, 16);  /* use first 16 bytes as nonce */
    g_checksum_update(cs, (const guchar *)created_utc, strlen(created_utc));
    g_checksum_update(cs, (const guchar *)pass, strlen(pass));

    guint8 digest_raw[20];
    gsize dlen = sizeof(digest_raw);
    g_checksum_get_digest(cs, digest_raw, &dlen);
    g_checksum_free(cs);

    gchar *digest_b64 = g_base64_encode(digest_raw, dlen);
    gchar *nonce_b64  = g_base64_encode(nonce_raw, 16);

    int n = snprintf(buf, buflen,
        "<wsse:Security xmlns:wsse=\"http://docs.oasis-open.org/wss/2004/01/"
        "oasis-200401-wss-wssecurity-secext-1.0.xsd\" "
        "xmlns:wsu=\"http://docs.oasis-open.org/wss/2004/01/"
        "oasis-200401-wss-wssecurity-utility-1.0.xsd\">"
        "<wsse:UsernameToken>"
        "<wsse:Username>%s</wsse:Username>"
        "<wsse:Password Type=\"http://docs.oasis-open.org/wss/2004/01/"
        "oasis-200401-wss-username-token-profile-1.0#PasswordDigest\">"
        "%s</wsse:Password>"
        "<wsse:Nonce EncodingType=\"http://docs.oasis-open.org/wss/2004/01/"
        "oasis-200401-wss-soap-message-security-1.0#Base64Binary\">"
        "%s</wsse:Nonce>"
        "<wsu:Created>%s</wsu:Created>"
        "</wsse:UsernameToken>"
        "</wsse:Security>",
        user, digest_b64, nonce_b64, created_utc);

    g_free(digest_b64);
    g_free(nonce_b64);
    return n;
}

/* =========================================================================
 * Minimal HTTP POST SOAP transport (blocking, TCP)
 * ========================================================================= */
static MnvrResult soap_call(const char *url, const char *body,
                              char *resp_buf, size_t resp_len)
{
    if (!url || !body || !resp_buf || resp_len == 0)
        return MNVR_ERR_GENERIC;

    char host[128] = {0};
    int port = 80;
    const char *p = strstr(url, "://");
    if (!p) return MNVR_ERR_NETWORK;
    p += 3;
    const char *slash = strchr(p, '/');
    const char *colon = strchr(p, ':');
    if (colon && (!slash || colon < slash)) {
        port = atoi(colon + 1);
        size_t hlen = (size_t)(colon - p);
        if (hlen >= sizeof(host)) hlen = sizeof(host) - 1;
        memcpy(host, p, hlen);
    } else {
        size_t hlen = slash ? (size_t)(slash - p) : strlen(p);
        if (hlen >= sizeof(host)) hlen = sizeof(host) - 1;
        memcpy(host, p, hlen);
    }

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((uint16_t)port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) <= 0)
        return MNVR_ERR_NETWORK;

    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return MNVR_ERR_NETWORK;

    struct timeval tv = {8, 0};  /* 8s timeout for ONVIF */
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    if (connect(s, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(s);
        return MNVR_ERR_NETWORK;
    }

    char *req = malloc(strlen(body) + 512);
    if (!req) { close(s); return MNVR_ERR_NOMEM; }

    int req_len = sprintf(req,
        "POST %s HTTP/1.0\r\n"
        "Host: %s:%d\r\n"
        "Content-Type: application/soap+xml; charset=utf-8\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n%s",
        slash ? slash : "/onvif/device_service",
        host, port, strlen(body), body);

    ssize_t sent = send(s, req, (size_t)req_len, 0);
    free(req);
    if (sent <= 0) { close(s); return MNVR_ERR_NETWORK; }

    size_t total = 0;
    while (total < resp_len - 1) {
        ssize_t n = recv(s, resp_buf + total, resp_len - 1 - total, 0);
        if (n <= 0) break;
        total += (size_t)n;
    }
    close(s);

    if (total == 0) return MNVR_ERR_NETWORK;
    resp_buf[total] = '\0';

    /* Skip HTTP headers — find \r\n\r\n */
    char *xml_start = strstr(resp_buf, "\r\n\r\n");
    if (xml_start) {
        xml_start += 4;
        size_t xml_len = total - (size_t)(xml_start - resp_buf);
        memmove(resp_buf, xml_start, xml_len);
        resp_buf[xml_len] = '\0';
    }

    return MNVR_OK;
}

/* =========================================================================
 * Get camera time (unauthenticated) for digest computation
 * ========================================================================= */
MnvrResult onvif_get_system_date_time(const char *xaddr,
                                        char *created_utc, size_t len)
{
    const char *body =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\">"
        "<s:Body><GetSystemDateAndTime "
        "xmlns=\"http://www.onvif.org/ver10/device/wsdl\"/>"
        "</s:Body></s:Envelope>";

    char resp[4096] = {0};
    MnvrResult r = soap_call(xaddr, body, resp, sizeof(resp));
    if (r != MNVR_OK) return r;

    if (strstr(resp, "UTCDateTime")) {
        /* Extract the UTCDateTime block first, then parse fields from it.
         * This avoids accidentally reading LocalDateTime fields (which
         * appear in the same response and may have a different timezone). */
        char utc_block[512] = {0};
        if (xml_extract(resp, "UTCDateTime", utc_block, sizeof(utc_block))) {
            int yr = xml_extract_int(utc_block, "Year");
            int mo = xml_extract_int(utc_block, "Month");
            int dy = xml_extract_int(utc_block, "Day");
            int hr = xml_extract_int(utc_block, "Hour");
            int mn = xml_extract_int(utc_block, "Minute");
            int sc = xml_extract_int(utc_block, "Second");

            if (yr > 0 && mo > 0 && dy > 0) {
                snprintf(created_utc, len,
                         "%04d-%02d-%02dT%02d:%02d:%02dZ",
                         yr, mo, dy, hr, mn, sc);
                return MNVR_OK;
            }
        }
    }

    /* Fallback to local time */
    time_t now = time(NULL);
    struct tm *t = gmtime(&now);
    strftime(created_utc, len, "%Y-%m-%dT%H:%M:%SZ", t);
    return MNVR_OK;
}

/* =========================================================================
 * Build authenticated SOAP envelope (fetches camera time first)
 * ========================================================================= */
static int build_auth_envelope(char *buf, size_t buflen,
                                const char *url,
                                const char *user, const char *pass,
                                const char *soap_body)
{
    char wsse_header[2048] = {0};

    if (user && user[0] && pass && pass[0]) {
        char created[32] = {0};
        onvif_get_system_date_time(url, created, sizeof(created));
        build_wsse_header(wsse_header, sizeof(wsse_header),
                          user, pass, created);
    }

    return snprintf(buf, buflen,
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<s:Envelope "
        "xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\" "
        "xmlns:tds=\"http://www.onvif.org/ver10/device/wsdl\" "
        "xmlns:trt=\"http://www.onvif.org/ver10/media/wsdl\" "
        "xmlns:tt=\"http://www.onvif.org/ver10/schema\" "
        "xmlns:tptz=\"http://www.onvif.org/ver20/ptz/wsdl\" "
        "xmlns:timg=\"http://www.onvif.org/ver20/imaging/wsdl\" "
        "xmlns:tev=\"http://www.onvif.org/ver10/events/wsdl\" "
        "xmlns:wsnt=\"http://docs.oasis-open.org/wsn/b-2\" "
        "xmlns:wsa=\"http://www.w3.org/2005/08/addressing\">"
        "<s:Header>%s</s:Header>"
        "<s:Body>%s</s:Body>"
        "</s:Envelope>",
        wsse_header, soap_body);
}

/**
 * Convenience: build auth envelope + soap_call in one shot.
 */
static MnvrResult soap_call_auth(const char *url,
                                   const char *user, const char *pass,
                                   const char *soap_body,
                                   char *resp_buf, size_t resp_len)
{
    char *envelope = malloc(ONVIF_SOAP_BUF_SIZE);
    if (!envelope) return MNVR_ERR_NOMEM;

    build_auth_envelope(envelope, ONVIF_SOAP_BUF_SIZE,
                        url, user, pass, soap_body);

    MnvrResult r = soap_call(url, envelope, resp_buf, resp_len);
    free(envelope);

    if (r == MNVR_OK && strstr(resp_buf, "NotAuthorized"))
        return MNVR_ERR_GENERIC;
    if (r == MNVR_OK && strstr(resp_buf, "Sender"))
        if (strstr(resp_buf, "NotAuthorized"))
            return MNVR_ERR_GENERIC;

    return r;
}

/* =========================================================================
 * WS-Discovery probe message (with configurable multicast address)
 * ========================================================================= */
static int send_discovery_probe(OnvifModule *om,
                                 OnvifDevice *devices, int max)
{
    const OnvifConfig *cfg = &om->config;

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) return 0;

    int reuse = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in local = {0};
    local.sin_family      = AF_INET;
    local.sin_addr.s_addr = INADDR_ANY;
    if (bind(sock, (struct sockaddr *)&local, sizeof(local)) < 0) {
        close(sock);
        return 0;
    }

    struct sockaddr_in dest = {0};
    dest.sin_family = AF_INET;
    dest.sin_port   = htons((uint16_t)cfg->multicast_port);
    inet_pton(AF_INET, cfg->multicast_ip, &dest.sin_addr);

    /* Generate unique message ID */
    char msg_id[64];
    snprintf(msg_id, sizeof(msg_id), "uuid:mnvr-probe-%08x",
             (unsigned)g_random_int());

    char probe[1024];
    snprintf(probe, sizeof(probe),
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<e:Envelope xmlns:e=\"http://www.w3.org/2003/05/soap-envelope\""
        " xmlns:w=\"http://schemas.xmlsoap.org/ws/2004/08/addressing\""
        " xmlns:d=\"http://schemas.xmlsoap.org/ws/2005/04/discovery\""
        " xmlns:dn=\"http://www.onvif.org/ver10/network/wsdl\">"
        "<e:Header>"
        "<w:MessageID>%s</w:MessageID>"
        "<w:To>urn:schemas-xmlsoap-org:ws:2005:04:discovery</w:To>"
        "<w:Action>http://schemas.xmlsoap.org/ws/2005/04/discovery/Probe"
        "</w:Action>"
        "</e:Header>"
        "<e:Body>"
        "<d:Probe><d:Types>dn:NetworkVideoTransmitter</d:Types></d:Probe>"
        "</e:Body>"
        "</e:Envelope>",
        msg_id);

    sendto(sock, probe, strlen(probe), 0,
           (struct sockaddr *)&dest, sizeof(dest));

    int count = 0;
    struct timeval tv = {
        cfg->probe_timeout_ms / 1000,
        (cfg->probe_timeout_ms % 1000) * 1000
    };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    char buf[8192];
    struct sockaddr_in src;
    socklen_t src_len = sizeof(src);

    while (count < max) {
        ssize_t n = recvfrom(sock, buf, sizeof(buf) - 1, 0,
                             (struct sockaddr *)&src, &src_len);
        if (n <= 0) break;
        buf[n] = '\0';

        if (!strstr(buf, "ProbeMatch")) continue;

        OnvifDevice *d = &devices[count];
        memset(d, 0, sizeof(OnvifDevice));

        /* Try multiple namespace prefixes for XAddrs */
        if (!xml_extract(buf, "XAddrs", d->xaddrs, sizeof(d->xaddrs)))
            xml_extract(buf, "d:XAddrs", d->xaddrs, sizeof(d->xaddrs));
        if (!d->xaddrs[0])
            xml_extract(buf, "wsdd:XAddrs", d->xaddrs, sizeof(d->xaddrs));

        xml_extract(buf, "Address", d->device_uuid, sizeof(d->device_uuid));

        inet_ntop(AF_INET, &src.sin_addr, d->ip_address, sizeof(d->ip_address));
        d->discovered_at = time(NULL);

        LOG_INFO(om->ctx, "ONVIF", "Discovered device: %s @ %s",
                 d->device_uuid, d->ip_address);
        count++;
    }

    close(sock);
    return count;
}

/* =========================================================================
 * Discovery thread
 *
 * Each cycle:
 *   1. Re-read cameras_config_details from PostgreSQL to detect
 *      cameras added/removed by the VMS at runtime.
 *   2. Probe any newly-added cameras via direct SOAP.
 *   3. Run multicast WS-Discovery (if enabled) for unknown devices.
 * ========================================================================= */
static void *discovery_thread_fn(void *arg)
{
    OnvifModule *om = (OnvifModule *)arg;
    OnvifConfig *cfg = &om->config;

    LOG_INFO(om->ctx, "ONVIF",
             "Discovery thread started (interval %ds, multicast %s:%d)",
             cfg->discovery_interval_sec,
             cfg->multicast_ip, cfg->multicast_port);

    while (om->running) {

        /* -----------------------------------------------------------
         * 1. Reload cameras_config_details from DB — detect hot-adds
         * ----------------------------------------------------------- */
        if (om->ctx && om->ctx->db) {
            OnvifConfig fresh;
            onvif_config_load(&fresh, om->ctx->db);

            /* Probe cameras that are in 'fresh' but not in current config */
            for (int i = 0; i < MNVR_MAX_CAMERAS; i++) {
                const OnvifCameraConfig *new_cam = &fresh.cameras[i];
                if (!new_cam->enabled) continue;

                /* Is this camera already in the current config? */
                const OnvifCameraConfig *old_cam = &cfg->cameras[i];
                if (old_cam->enabled &&
                    strcmp(old_cam->ip, new_cam->ip) == 0 &&
                    old_cam->port == new_cam->port)
                    continue;  /* unchanged slot — skip */

                /* Also check if this IP was already discovered WITH a valid
                 * stream URI.  Multicast discovery may have found the device
                 * but without credentials the probe yields "(none)" — in that
                 * case we must re-probe using the DB credentials. */
                bool already_discovered = false;
                pthread_mutex_lock(&om->mutex);
                for (int j = 0; j < om->num_discovered; j++) {
                    if (strcmp(om->discovered[j].ip_address,
                              new_cam->ip) == 0 &&
                        om->discovered[j].stream_uri[0] != '\0') {
                        already_discovered = true;
                        break;
                    }
                }
                pthread_mutex_unlock(&om->mutex);
                if (already_discovered) {
                    /* Still update the config slot so credentials are current */
                    cfg->cameras[i] = fresh.cameras[i];
                    continue;
                }

                LOG_INFO(om->ctx, "ONVIF",
                         "New camera detected in DB — slot %d: %s:%d user=%s",
                         new_cam->slot, new_cam->ip, new_cam->port,
                         new_cam->user);

                OnvifDevice dev;
                MnvrResult r = onvif_probe_direct(new_cam->ip, new_cam->port,
                                                   new_cam->user, new_cam->pass,
                                                   &dev);
                if (r == MNVR_OK) {
                    dev.config_slot = new_cam->slot;
                    LOG_INFO(om->ctx, "ONVIF",
                             "  -> %s %s (FW: %s, SN: %s)",
                             dev.manufacturer, dev.model,
                             dev.firmware, dev.serial_number);
                    LOG_INFO(om->ctx, "ONVIF",
                             "  -> Stream: %s", dev.stream_uri);

                    /* Update config slot BEFORE firing callback so that
                     * on_new_onvif_device() can look up RTSP credentials
                     * via onvif_config_get_camera(). */
                    pthread_mutex_lock(&om->mutex);
                    cfg->cameras[i] = fresh.cameras[i];
                    cfg->num_cameras = 0;
                    for (int k = 0; k < MNVR_MAX_CAMERAS; k++)
                        if (cfg->cameras[k].enabled) cfg->num_cameras++;
                    if (om->num_discovered < MNVR_MAX_CAMERAS * 2)
                        om->discovered[om->num_discovered++] = dev;
                    pthread_mutex_unlock(&om->mutex);

                    if (om->on_new_device)
                        om->on_new_device(&dev, om->cb_user_data);
                } else {
                    LOG_WARN(om->ctx, "ONVIF",
                             "  -> FAILED to probe new camera %s (result=%d)",
                             new_cam->ip, r);
                }
            }

            /* Update config to fresh state (also picks up global setting changes) */
            pthread_mutex_lock(&om->mutex);
            *cfg = fresh;
            pthread_mutex_unlock(&om->mutex);
        }

        /* -----------------------------------------------------------
         * 2. Multicast WS-Discovery for unknown devices
         * ----------------------------------------------------------- */
        if (cfg->enable_discovery) {
            OnvifDevice found[MNVR_MAX_CAMERAS * 2];
            int n = send_discovery_probe(om, found, MNVR_MAX_CAMERAS * 2);
            LOG_DEBUG(om->ctx, "ONVIF",
                      "Discovery probe returned %d device(s)", n);

            pthread_mutex_lock(&om->mutex);
            for (int i = 0; i < n; i++) {
                bool known = false;
                for (int j = 0; j < om->num_discovered; j++) {
                    if (strcmp(om->discovered[j].ip_address,
                              found[i].ip_address) == 0) {
                        known = true;
                        break;
                    }
                }
                if (!known && om->num_discovered < MNVR_MAX_CAMERAS * 2) {
                    om->discovered[om->num_discovered++] = found[i];
                    if (om->on_new_device)
                        om->on_new_device(&found[i], om->cb_user_data);
                }
            }
            pthread_mutex_unlock(&om->mutex);
        }

        /* Interruptible sleep */
        for (int s = 0; s < cfg->discovery_interval_sec && om->running; s++)
            sleep(1);
    }

    LOG_INFO(om->ctx, "ONVIF", "Discovery thread stopped");
    return NULL;
}

/* =========================================================================
 * Device service: GetDeviceInformation
 * ========================================================================= */
MnvrResult onvif_get_device_info(const char *xaddr,
                                  const char *user, const char *pass,
                                  OnvifDevice *out)
{
    const char *soap_body =
        "<GetDeviceInformation "
        "xmlns=\"http://www.onvif.org/ver10/device/wsdl\"/>";

    char resp[ONVIF_RESP_BUF_SIZE] = {0};
    MnvrResult r = soap_call_auth(xaddr, user, pass, soap_body,
                                   resp, sizeof(resp));
    if (r != MNVR_OK) return r;

    xml_extract(resp, "Manufacturer",    out->manufacturer,
                sizeof(out->manufacturer));
    xml_extract(resp, "Model",           out->model,
                sizeof(out->model));
    xml_extract(resp, "FirmwareVersion", out->firmware,
                sizeof(out->firmware));
    xml_extract(resp, "SerialNumber",    out->serial_number,
                sizeof(out->serial_number));
    xml_extract(resp, "HardwareId",      out->hardware_id,
                sizeof(out->hardware_id));

    return MNVR_OK;
}

/* =========================================================================
 * Device service: GetCapabilities
 * ========================================================================= */
MnvrResult onvif_get_capabilities(const char *xaddr,
                                    const char *user, const char *pass,
                                    OnvifDevice *dev)
{
    const char *soap_body =
        "<GetCapabilities xmlns=\"http://www.onvif.org/ver10/device/wsdl\">"
        "<Category>All</Category>"
        "</GetCapabilities>";

    char resp[ONVIF_RESP_BUF_SIZE] = {0};
    MnvrResult r = soap_call_auth(xaddr, user, pass, soap_body,
                                   resp, sizeof(resp));
    if (r != MNVR_OK) return r;

    /* Debug: dump first 3000 chars of GetCapabilities response */
    {
        int dlen = (int)strlen(resp);
        fprintf(stderr, "  [ONVIF-DBG] GetCapabilities response (%d bytes):\n%.3000s\n"
                "  [ONVIF-DBG] --- end caps dump ---\n", dlen, resp);
    }

    /* Extract service URLs from capabilities */
    char media_block[4096] = {0};
    if (xml_extract(resp, "Media", media_block, sizeof(media_block))) {
        xml_extract(media_block, "XAddr", dev->media_service_url,
                    sizeof(dev->media_service_url));
    }

    char ptz_block[4096] = {0};
    if (xml_extract(resp, "PTZ", ptz_block, sizeof(ptz_block))) {
        xml_extract(ptz_block, "XAddr", dev->ptz_service_url,
                    sizeof(dev->ptz_service_url));
        dev->ptz_supported = true;
    }

    char imaging_block[4096] = {0};
    if (xml_extract(resp, "Imaging", imaging_block, sizeof(imaging_block))) {
        xml_extract(imaging_block, "XAddr", dev->imaging_service_url,
                    sizeof(dev->imaging_service_url));
        dev->imaging_supported = true;
    }

    char events_block[4096] = {0};
    if (xml_extract(resp, "Events", events_block, sizeof(events_block))) {
        xml_extract(events_block, "XAddr", dev->events_service_url,
                    sizeof(dev->events_service_url));
        dev->events_supported = true;
    }

    /* Check for audio support */
    if (strstr(resp, "AudioSources") || strstr(resp, "AudioOutput"))
        dev->audio_supported = true;

    /* Debug: log extracted service URLs */
    fprintf(stderr, "  [ONVIF-DBG] Extracted service URLs:\n"
            "    Media:   '%s'\n"
            "    PTZ:     '%s'\n"
            "    Imaging: '%s'\n"
            "    Events:  '%s'\n",
            dev->media_service_url,
            dev->ptz_service_url,
            dev->imaging_service_url,
            dev->events_service_url);

    return MNVR_OK;
}

/* =========================================================================
 * Device service: SystemReboot
 * ========================================================================= */
MnvrResult onvif_reboot_device(const char *xaddr,
                                const char *user, const char *pass)
{
    const char *soap_body =
        "<SystemReboot xmlns=\"http://www.onvif.org/ver10/device/wsdl\"/>";

    char resp[4096] = {0};
    return soap_call_auth(xaddr, user, pass, soap_body,
                          resp, sizeof(resp));
}

/* =========================================================================
 * Media service: GetProfiles
 * ========================================================================= */
MnvrResult onvif_get_profiles(const char *xaddr,
                               const char *user, const char *pass,
                               OnvifProfile *profiles, int *count,
                               int max_profiles)
{
    const char *soap_body =
        "<GetProfiles xmlns=\"http://www.onvif.org/ver10/media/wsdl\"/>";

    char *resp = malloc(ONVIF_RESP_BUF_SIZE);
    if (!resp) return MNVR_ERR_NOMEM;

    MnvrResult r = soap_call_auth(xaddr, user, pass, soap_body,
                                   resp, ONVIF_RESP_BUF_SIZE);
    if (r != MNVR_OK) { free(resp); return r; }

    /* Debug: dump first 2000 chars of GetProfiles response */
    {
        int dlen = (int)strlen(resp);
        fprintf(stderr, "  [ONVIF-DBG] GetProfiles response (%d bytes):\n%.2000s\n"
                "  [ONVIF-DBG] --- end dump ---\n", dlen, resp);
    }

    *count = 0;

    /* Parse profiles from the response.
     * Camera response uses namespace-prefixed tags:
     *   <trt:Profiles token="Profile000" fixed="true">
     *     ...
     *   </trt:Profiles>
     *
     * We search for opening "Profiles " (with space = has attributes)
     * and find the closing tag by scanning for ":Profiles>" or "Profiles>"
     */
    const char *pos = resp;
    while (*count < max_profiles) {
        /* Find next opening Profiles tag with attributes */
        const char *p = strstr(pos, "Profiles ");
        if (!p) break;

        /* Back up to find the '<' */
        const char *lt = p;
        while (lt > resp && *lt != '<') lt--;
        if (*lt != '<') { pos = p + 1; continue; }

        /* Check this is an opening tag (not closing) */
        if (*(lt + 1) == '/') { pos = p + 1; continue; }

        /* Verify this is actually a Profiles element, not GetProfilesResponse */
        {
            /* The tag name (between < and space) should end with "Profiles" */
            const char *tag_end = p;  /* p points to "Profiles ..." */
            /* Check that 'P' in Profiles is right after '<' or after ':' */
            const char *before_p = p - 1;
            while (before_p > lt && *before_p != '<' && *before_p != ':')
                before_p--;
            if (*before_p == '<' || *before_p == ':') {
                /* Good — this is <Profiles or <ns:Profiles */
            } else {
                pos = p + 1; continue;
            }

            /* Extra check: make sure this isn't "GetProfilesResponse" or similar */
            if (p > lt + 1) {
                char ch = *(p - 1);
                if (ch != '<' && ch != ':') {
                    /* Character before "Profiles" is not < or : — skip */
                    pos = p + 1;
                    continue;
                }
            }
        }

        OnvifProfile *prof = &profiles[*count];
        memset(prof, 0, sizeof(OnvifProfile));

        /* Extract token attribute */
        xml_extract_attr(lt, "Profiles", "token",
                         prof->token, sizeof(prof->token));

        /* Find the closing </...Profiles> tag.
         * It could be </Profiles>, </trt:Profiles>, etc.
         * Search for ":Profiles>" first (namespaced), then "/Profiles>" (bare) */
        const char *block_end = NULL;
        const char *search = p + 9; /* skip past "Profiles " */

        /* Try namespaced close: look for string ending in ":Profiles>" */
        const char *ns_close = strstr(search, ":Profiles>");
        /* Try bare close: look for "/Profiles>" */
        const char *bare_close = strstr(search, "/Profiles>");

        if (ns_close) {
            /* Verify this is actually a closing tag by checking for '</' before */
            const char *tag_lt = ns_close;
            while (tag_lt > search && *tag_lt != '<') tag_lt--;
            if (*tag_lt == '<' && *(tag_lt + 1) == '/') {
                block_end = ns_close + 10; /* skip past ":Profiles>" */
            }
        }

        if (!block_end && bare_close) {
            block_end = bare_close + 10; /* skip past "/Profiles>" */
        }

        if (!block_end) { pos = p + 1; continue; }

        /* Work within this profile block */
        size_t blen = (size_t)(block_end - lt);
        char *block = malloc(blen + 1);
        if (!block) { pos = p + 1; continue; }
        memcpy(block, lt, blen);
        block[blen] = '\0';

        xml_extract(block, "Name", prof->name, sizeof(prof->name));

        /* Video encoder configuration */
        char vec_block[2048] = {0};
        if (xml_extract(block, "VideoEncoderConfiguration",
                        vec_block, sizeof(vec_block))) {
            xml_extract_attr(block, "VideoEncoderConfiguration", "token",
                             prof->video_encoder_token,
                             sizeof(prof->video_encoder_token));
            xml_extract(vec_block, "Encoding", prof->encoding,
                        sizeof(prof->encoding));
            prof->width  = xml_extract_int(vec_block, "Width");
            prof->height = xml_extract_int(vec_block, "Height");
            prof->fps    = xml_extract_int(vec_block, "FrameRateLimit");
            if (prof->fps <= 0)
                prof->fps = xml_extract_int(vec_block, "FrameRate");
            prof->bitrate_kbps = xml_extract_int(vec_block, "BitrateLimit");
            if (prof->bitrate_kbps <= 0)
                prof->bitrate_kbps = xml_extract_int(vec_block, "Bitrate");
            prof->gov_length = xml_extract_int(vec_block, "GovLength");
            xml_extract(vec_block, "Quality", prof->quality,
                        sizeof(prof->quality));
        }

        /* Video source */
        xml_extract_attr(block, "VideoSourceConfiguration", "token",
                         prof->video_source_token,
                         sizeof(prof->video_source_token));

        /* PTZ configuration */
        if (strstr(block, "PTZConfiguration")) {
            prof->has_ptz = true;
            xml_extract_attr(block, "PTZConfiguration", "token",
                             prof->ptz_config_token,
                             sizeof(prof->ptz_config_token));
        }

        /* Audio */
        if (strstr(block, "AudioEncoderConfiguration")) {
            prof->has_audio = true;
            xml_extract_attr(block, "AudioEncoderConfiguration", "token",
                             prof->audio_encoder_token,
                             sizeof(prof->audio_encoder_token));
        }

        free(block);
        (*count)++;
        pos = block_end;
    }

    free(resp);
    return MNVR_OK;
}

/* =========================================================================
 * Media service: GetStreamUri
 * ========================================================================= */
MnvrResult onvif_get_stream_uri(const char *xaddr,
                                 const char *user, const char *pass,
                                 const char *profile_token,
                                 char *uri_out, size_t uri_len)
{
    char soap_body[512];
    snprintf(soap_body, sizeof(soap_body),
        "<GetStreamUri xmlns=\"http://www.onvif.org/ver10/media/wsdl\">"
        "<StreamSetup>"
        "<Stream xmlns=\"http://www.onvif.org/ver10/schema\">"
        "RTP-Unicast</Stream>"
        "<Transport xmlns=\"http://www.onvif.org/ver10/schema\">"
        "<Protocol>RTSP</Protocol></Transport>"
        "</StreamSetup>"
        "<ProfileToken>%s</ProfileToken>"
        "</GetStreamUri>",
        profile_token ? profile_token : "Profile_1");

    char resp[ONVIF_RESP_BUF_SIZE] = {0};
    MnvrResult r = soap_call_auth(xaddr, user, pass, soap_body,
                                   resp, sizeof(resp));
    if (r != MNVR_OK) return r;

    /* Debug: dump GetStreamUri response */
    fprintf(stderr, "  [ONVIF-DBG] GetStreamUri response (%d bytes):\n%.2000s\n"
            "  [ONVIF-DBG] --- end stream uri dump ---\n", (int)strlen(resp), resp);

    if (!xml_extract(resp, "Uri", uri_out, uri_len))
        return MNVR_ERR_GENERIC;

    /* Decode XML entities: &amp; -> &, etc. */
    xml_decode_entities(uri_out);

    return MNVR_OK;
}

/* =========================================================================
 * Media service: GetSnapshotUri
 * ========================================================================= */
MnvrResult onvif_get_snapshot_uri(const char *xaddr,
                                    const char *user, const char *pass,
                                    const char *profile_token,
                                    char *uri_out, size_t uri_len)
{
    char soap_body[256];
    snprintf(soap_body, sizeof(soap_body),
        "<GetSnapshotUri xmlns=\"http://www.onvif.org/ver10/media/wsdl\">"
        "<ProfileToken>%s</ProfileToken>"
        "</GetSnapshotUri>",
        profile_token ? profile_token : "Profile_1");

    char resp[ONVIF_RESP_BUF_SIZE] = {0};
    MnvrResult r = soap_call_auth(xaddr, user, pass, soap_body,
                                   resp, sizeof(resp));
    if (r != MNVR_OK) return r;

    if (!xml_extract(resp, "Uri", uri_out, uri_len))
        return MNVR_ERR_GENERIC;

    xml_decode_entities(uri_out);

    return MNVR_OK;
}

/* =========================================================================
 * Media service: GetVideoSources
 * ========================================================================= */
MnvrResult onvif_get_video_sources(const char *xaddr,
                                     const char *user, const char *pass,
                                     OnvifVideoSource *srcs, int *count,
                                     int max_srcs)
{
    const char *soap_body =
        "<GetVideoSources xmlns=\"http://www.onvif.org/ver10/media/wsdl\"/>";

    char resp[ONVIF_RESP_BUF_SIZE] = {0};
    MnvrResult r = soap_call_auth(xaddr, user, pass, soap_body,
                                   resp, sizeof(resp));
    if (r != MNVR_OK) return r;

    *count = 0;
    const char *pos = resp;
    while (*count < max_srcs) {
        const char *vs = strstr(pos, "VideoSources ");
        const char *vs2 = strstr(pos, "VideoSources>");
        if (!vs && !vs2) break;
        if (!vs) vs = vs2;
        else if (vs2 && vs2 < vs) vs = vs2;

        /* Back up to '<' */
        const char *lt = vs;
        while (lt > resp && *lt != '<') lt--;
        if (*(lt + 1) == '/') { pos = vs + 1; continue; }

        OnvifVideoSource *src = &srcs[*count];
        memset(src, 0, sizeof(OnvifVideoSource));

        xml_extract_attr(vs, "VideoSources", "token",
                         src->token, sizeof(src->token));

        /* Find block end */
        const char *end = strstr(vs, "/VideoSources>");
        if (!end) break;
        size_t blen = (size_t)(end - lt) + 14;
        char *block = malloc(blen + 1);
        if (!block) break;
        memcpy(block, lt, blen);
        block[blen] = '\0';

        src->width     = xml_extract_int(block, "Width");
        src->height    = xml_extract_int(block, "Height");
        src->framerate = xml_extract_float(block, "FrameRateRange");
        if (src->framerate <= 0)
            src->framerate = (float)xml_extract_int(block, "Max");

        free(block);
        (*count)++;
        pos = end + 14;
    }

    return MNVR_OK;
}

/* =========================================================================
 * Media service: GetVideoEncoderConfigurations
 * ========================================================================= */
MnvrResult onvif_get_video_encoder_configs(const char *xaddr,
                                             const char *user, const char *pass,
                                             OnvifVideoEncoderConfig *cfgs,
                                             int *count, int max_cfgs)
{
    const char *soap_body =
        "<GetVideoEncoderConfigurations "
        "xmlns=\"http://www.onvif.org/ver10/media/wsdl\"/>";

    char *resp = malloc(ONVIF_RESP_BUF_SIZE);
    if (!resp) return MNVR_ERR_NOMEM;

    MnvrResult r = soap_call_auth(xaddr, user, pass, soap_body,
                                   resp, ONVIF_RESP_BUF_SIZE);
    if (r != MNVR_OK) { free(resp); return r; }

    *count = 0;
    const char *pos = resp;
    while (*count < max_cfgs) {
        const char *vs = strstr(pos, "Configurations ");
        if (!vs) vs = strstr(pos, "Configurations>");
        if (!vs) break;

        const char *lt = vs;
        while (lt > resp && *lt != '<') lt--;
        if (*(lt + 1) == '/') { pos = vs + 1; continue; }

        /* Verify this is VideoEncoder, not Audio */
        if (!strstr(lt, "VideoEncoder")) { pos = vs + 1; continue; }

        OnvifVideoEncoderConfig *cfg = &cfgs[*count];
        memset(cfg, 0, sizeof(OnvifVideoEncoderConfig));

        xml_extract_attr(lt, "Configurations", "token",
                         cfg->token, sizeof(cfg->token));

        const char *end = strstr(vs, "/Configurations>");
        if (!end) break;
        size_t blen = (size_t)(end - lt) + 16;
        char *block = malloc(blen + 1);
        if (!block) break;
        memcpy(block, lt, blen);
        block[blen] = '\0';

        xml_extract(block, "Name", cfg->name, sizeof(cfg->name));
        xml_extract(block, "Encoding", cfg->encoding,
                    sizeof(cfg->encoding));
        cfg->width       = xml_extract_int(block, "Width");
        cfg->height      = xml_extract_int(block, "Height");
        cfg->fps         = xml_extract_int(block, "FrameRateLimit");
        cfg->bitrate_kbps = xml_extract_int(block, "BitrateLimit");
        cfg->gov_length  = xml_extract_int(block, "GovLength");
        cfg->quality     = xml_extract_float(block, "Quality");

        char h264_block[1024] = {0};
        if (xml_extract(block, "H264", h264_block, sizeof(h264_block)))
            xml_extract(h264_block, "H264Profile", cfg->profile,
                        sizeof(cfg->profile));

        free(block);
        (*count)++;
        pos = end + 16;
    }

    free(resp);
    return MNVR_OK;
}

/* =========================================================================
 * Media service: GetVideoEncoderConfiguration (single)
 * ========================================================================= */
MnvrResult onvif_get_video_encoder_config(const char *xaddr,
                                            const char *user, const char *pass,
                                            const char *token,
                                            OnvifVideoEncoderConfig *cfg)
{
    char soap_body[256];
    snprintf(soap_body, sizeof(soap_body),
        "<GetVideoEncoderConfiguration "
        "xmlns=\"http://www.onvif.org/ver10/media/wsdl\">"
        "<ConfigurationToken>%s</ConfigurationToken>"
        "</GetVideoEncoderConfiguration>",
        token);

    char resp[ONVIF_RESP_BUF_SIZE] = {0};
    MnvrResult r = soap_call_auth(xaddr, user, pass, soap_body,
                                   resp, sizeof(resp));
    if (r != MNVR_OK) return r;

    memset(cfg, 0, sizeof(OnvifVideoEncoderConfig));
    strncpy(cfg->token, token, sizeof(cfg->token) - 1);

    xml_extract(resp, "Name", cfg->name, sizeof(cfg->name));
    xml_extract(resp, "Encoding", cfg->encoding, sizeof(cfg->encoding));
    cfg->width       = xml_extract_int(resp, "Width");
    cfg->height      = xml_extract_int(resp, "Height");
    cfg->fps         = xml_extract_int(resp, "FrameRateLimit");
    cfg->bitrate_kbps = xml_extract_int(resp, "BitrateLimit");
    cfg->gov_length  = xml_extract_int(resp, "GovLength");
    cfg->quality     = xml_extract_float(resp, "Quality");

    return MNVR_OK;
}

/* =========================================================================
 * Media service: SetVideoEncoderConfiguration
 * ========================================================================= */
MnvrResult onvif_set_video_encoder_config(const char *xaddr,
                                            const char *user, const char *pass,
                                            const OnvifVideoEncoderConfig *cfg)
{
    char soap_body[2048];
    snprintf(soap_body, sizeof(soap_body),
        "<SetVideoEncoderConfiguration "
        "xmlns=\"http://www.onvif.org/ver10/media/wsdl\">"
        "<Configuration token=\"%s\">"
        "<tt:Name>%s</tt:Name>"
        "<tt:UseCount>1</tt:UseCount>"
        "<tt:Encoding>%s</tt:Encoding>"
        "<tt:Resolution>"
        "<tt:Width>%d</tt:Width>"
        "<tt:Height>%d</tt:Height>"
        "</tt:Resolution>"
        "<tt:Quality>%.1f</tt:Quality>"
        "<tt:RateControl>"
        "<tt:FrameRateLimit>%d</tt:FrameRateLimit>"
        "<tt:EncodingInterval>1</tt:EncodingInterval>"
        "<tt:BitrateLimit>%d</tt:BitrateLimit>"
        "</tt:RateControl>"
        "<tt:H264>"
        "<tt:GovLength>%d</tt:GovLength>"
        "<tt:H264Profile>%s</tt:H264Profile>"
        "</tt:H264>"
        "<tt:Multicast>"
        "<tt:Address><tt:Type>IPv4</tt:Type>"
        "<tt:IPv4Address>0.0.0.0</tt:IPv4Address></tt:Address>"
        "<tt:Port>0</tt:Port><tt:TTL>0</tt:TTL>"
        "<tt:AutoStart>false</tt:AutoStart>"
        "</tt:Multicast>"
        "<tt:SessionTimeout>PT60S</tt:SessionTimeout>"
        "</Configuration>"
        "<ForcePersistence>true</ForcePersistence>"
        "</SetVideoEncoderConfiguration>",
        cfg->token, cfg->name[0] ? cfg->name : "VideoEncoder",
        cfg->encoding[0] ? cfg->encoding : "H264",
        cfg->width > 0 ? cfg->width : 1920,
        cfg->height > 0 ? cfg->height : 1080,
        cfg->quality > 0 ? cfg->quality : 4.0f,
        cfg->fps > 0 ? cfg->fps : 25,
        cfg->bitrate_kbps > 0 ? cfg->bitrate_kbps : 4096,
        cfg->gov_length > 0 ? cfg->gov_length : 50,
        cfg->profile[0] ? cfg->profile : "Main");

    char resp[ONVIF_RESP_BUF_SIZE] = {0};
    return soap_call_auth(xaddr, user, pass, soap_body,
                          resp, sizeof(resp));
}

/* =========================================================================
 * Media service: GetAudioSources
 * ========================================================================= */
MnvrResult onvif_get_audio_sources(const char *xaddr,
                                     const char *user, const char *pass,
                                     int *count)
{
    const char *soap_body =
        "<GetAudioSources xmlns=\"http://www.onvif.org/ver10/media/wsdl\"/>";

    char resp[ONVIF_RESP_BUF_SIZE] = {0};
    MnvrResult r = soap_call_auth(xaddr, user, pass, soap_body,
                                   resp, sizeof(resp));
    if (r != MNVR_OK) return r;

    *count = xml_count(resp, "AudioSources ");
    if (*count == 0)
        *count = xml_count(resp, "AudioSources>");
    /* Subtract closing tags counted */
    int closing = xml_count(resp, "/AudioSources>");
    *count = (*count > closing) ? *count - closing : 0;

    return MNVR_OK;
}

/* =========================================================================
 * PTZ service: ContinuousMove
 * ========================================================================= */
MnvrResult onvif_ptz_continuous_move(const char *xaddr,
                                      const char *user, const char *pass,
                                      const char *profile_token,
                                      float pan_speed, float tilt_speed,
                                      float zoom_speed)
{
    char soap_body[512];
    snprintf(soap_body, sizeof(soap_body),
        "<ContinuousMove xmlns=\"http://www.onvif.org/ver20/ptz/wsdl\">"
        "<ProfileToken>%s</ProfileToken>"
        "<Velocity>"
        "<tt:PanTilt x=\"%.3f\" y=\"%.3f\"/>"
        "<tt:Zoom x=\"%.3f\"/>"
        "</Velocity>"
        "</ContinuousMove>",
        profile_token, pan_speed, tilt_speed, zoom_speed);

    char resp[4096] = {0};
    return soap_call_auth(xaddr, user, pass, soap_body,
                          resp, sizeof(resp));
}

/* =========================================================================
 * PTZ service: RelativeMove
 * ========================================================================= */
MnvrResult onvif_ptz_relative_move(const char *xaddr,
                                     const char *user, const char *pass,
                                     const char *profile_token,
                                     float pan, float tilt, float zoom)
{
    char soap_body[512];
    snprintf(soap_body, sizeof(soap_body),
        "<RelativeMove xmlns=\"http://www.onvif.org/ver20/ptz/wsdl\">"
        "<ProfileToken>%s</ProfileToken>"
        "<Translation>"
        "<tt:PanTilt x=\"%.3f\" y=\"%.3f\"/>"
        "<tt:Zoom x=\"%.3f\"/>"
        "</Translation>"
        "</RelativeMove>",
        profile_token, pan, tilt, zoom);

    char resp[4096] = {0};
    return soap_call_auth(xaddr, user, pass, soap_body,
                          resp, sizeof(resp));
}

/* =========================================================================
 * PTZ service: AbsoluteMove
 * ========================================================================= */
MnvrResult onvif_ptz_absolute_move(const char *xaddr,
                                     const char *user, const char *pass,
                                     const char *profile_token,
                                     float pan, float tilt, float zoom)
{
    char soap_body[512];
    snprintf(soap_body, sizeof(soap_body),
        "<AbsoluteMove xmlns=\"http://www.onvif.org/ver20/ptz/wsdl\">"
        "<ProfileToken>%s</ProfileToken>"
        "<Position>"
        "<tt:PanTilt x=\"%.3f\" y=\"%.3f\"/>"
        "<tt:Zoom x=\"%.3f\"/>"
        "</Position>"
        "</AbsoluteMove>",
        profile_token, pan, tilt, zoom);

    char resp[4096] = {0};
    return soap_call_auth(xaddr, user, pass, soap_body,
                          resp, sizeof(resp));
}

/* =========================================================================
 * PTZ service: Stop
 * ========================================================================= */
MnvrResult onvif_ptz_stop(const char *xaddr,
                            const char *user, const char *pass,
                            const char *profile_token,
                            bool stop_pan_tilt, bool stop_zoom)
{
    char soap_body[256];
    snprintf(soap_body, sizeof(soap_body),
        "<Stop xmlns=\"http://www.onvif.org/ver20/ptz/wsdl\">"
        "<ProfileToken>%s</ProfileToken>"
        "<PanTilt>%s</PanTilt>"
        "<Zoom>%s</Zoom>"
        "</Stop>",
        profile_token,
        stop_pan_tilt ? "true" : "false",
        stop_zoom ? "true" : "false");

    char resp[4096] = {0};
    return soap_call_auth(xaddr, user, pass, soap_body,
                          resp, sizeof(resp));
}

/* =========================================================================
 * PTZ service: GetPresets
 * ========================================================================= */
MnvrResult onvif_ptz_get_presets(const char *xaddr,
                                  const char *user, const char *pass,
                                  const char *profile_token,
                                  OnvifPreset *presets, int *count,
                                  int max_presets)
{
    char soap_body[256];
    snprintf(soap_body, sizeof(soap_body),
        "<GetPresets xmlns=\"http://www.onvif.org/ver20/ptz/wsdl\">"
        "<ProfileToken>%s</ProfileToken>"
        "</GetPresets>",
        profile_token);

    char *resp = malloc(ONVIF_RESP_BUF_SIZE);
    if (!resp) return MNVR_ERR_NOMEM;

    MnvrResult r = soap_call_auth(xaddr, user, pass, soap_body,
                                   resp, ONVIF_RESP_BUF_SIZE);
    if (r != MNVR_OK) { free(resp); return r; }

    *count = 0;
    const char *pos = resp;
    while (*count < max_presets) {
        const char *p = strstr(pos, "Preset ");
        const char *p2 = strstr(pos, "Preset>");
        if (!p && !p2) break;
        if (!p) p = p2;
        else if (p2 && p2 < p) p = p2;

        const char *lt = p;
        while (lt > resp && *lt != '<') lt--;
        if (*(lt + 1) == '/') { pos = p + 1; continue; }

        OnvifPreset *pr = &presets[*count];
        memset(pr, 0, sizeof(OnvifPreset));

        xml_extract_attr(lt, "Preset", "token", pr->token,
                         sizeof(pr->token));

        const char *end = strstr(p, "/Preset>");
        if (!end) {
            /* Self-closing preset */
            end = strchr(p, '>');
            if (!end) break;
            end++;
        } else {
            /* Extract name from block */
            size_t blen = (size_t)(end - lt) + 8;
            char block[512] = {0};
            if (blen < sizeof(block)) {
                memcpy(block, lt, blen);
                xml_extract(block, "Name", pr->name, sizeof(pr->name));

                char pos_block[256] = {0};
                if (xml_extract(block, "Position", pos_block,
                                sizeof(pos_block)) ||
                    xml_extract(block, "PTZPosition", pos_block,
                                sizeof(pos_block))) {
                    char pt[64] = {0};
                    if (xml_extract_attr(pos_block, "PanTilt", "x",
                                         pt, sizeof(pt)))
                        pr->x = (float)atof(pt);
                    if (xml_extract_attr(pos_block, "PanTilt", "y",
                                         pt, sizeof(pt)))
                        pr->y = (float)atof(pt);
                    if (xml_extract_attr(pos_block, "Zoom", "x",
                                         pt, sizeof(pt)))
                        pr->z = (float)atof(pt);
                }
            }
            end += 8;
        }

        (*count)++;
        pos = end;
    }

    free(resp);
    return MNVR_OK;
}

/* =========================================================================
 * PTZ service: GotoPreset
 * ========================================================================= */
MnvrResult onvif_ptz_goto_preset(const char *xaddr,
                                   const char *user, const char *pass,
                                   const char *profile_token,
                                   const char *preset_token)
{
    char soap_body[256];
    snprintf(soap_body, sizeof(soap_body),
        "<GotoPreset xmlns=\"http://www.onvif.org/ver20/ptz/wsdl\">"
        "<ProfileToken>%s</ProfileToken>"
        "<PresetToken>%s</PresetToken>"
        "</GotoPreset>",
        profile_token, preset_token);

    char resp[4096] = {0};
    return soap_call_auth(xaddr, user, pass, soap_body,
                          resp, sizeof(resp));
}

/* =========================================================================
 * PTZ service: SetPreset
 * ========================================================================= */
MnvrResult onvif_ptz_set_preset(const char *xaddr,
                                  const char *user, const char *pass,
                                  const char *profile_token,
                                  const char *preset_name,
                                  char *preset_token_out, size_t token_len)
{
    char soap_body[256];
    snprintf(soap_body, sizeof(soap_body),
        "<SetPreset xmlns=\"http://www.onvif.org/ver20/ptz/wsdl\">"
        "<ProfileToken>%s</ProfileToken>"
        "<PresetName>%s</PresetName>"
        "</SetPreset>",
        profile_token, preset_name);

    char resp[4096] = {0};
    MnvrResult r = soap_call_auth(xaddr, user, pass, soap_body,
                                   resp, sizeof(resp));
    if (r != MNVR_OK) return r;

    if (preset_token_out)
        xml_extract(resp, "PresetToken", preset_token_out, token_len);

    return MNVR_OK;
}

/* =========================================================================
 * PTZ service: RemovePreset
 * ========================================================================= */
MnvrResult onvif_ptz_remove_preset(const char *xaddr,
                                     const char *user, const char *pass,
                                     const char *profile_token,
                                     const char *preset_token)
{
    char soap_body[256];
    snprintf(soap_body, sizeof(soap_body),
        "<RemovePreset xmlns=\"http://www.onvif.org/ver20/ptz/wsdl\">"
        "<ProfileToken>%s</ProfileToken>"
        "<PresetToken>%s</PresetToken>"
        "</RemovePreset>",
        profile_token, preset_token);

    char resp[4096] = {0};
    return soap_call_auth(xaddr, user, pass, soap_body,
                          resp, sizeof(resp));
}

/* =========================================================================
 * PTZ service: GetStatus
 * ========================================================================= */
MnvrResult onvif_ptz_get_status(const char *xaddr,
                                  const char *user, const char *pass,
                                  const char *profile_token,
                                  OnvifPtzStatus *status)
{
    char soap_body[256];
    snprintf(soap_body, sizeof(soap_body),
        "<GetStatus xmlns=\"http://www.onvif.org/ver20/ptz/wsdl\">"
        "<ProfileToken>%s</ProfileToken>"
        "</GetStatus>",
        profile_token);

    char resp[ONVIF_RESP_BUF_SIZE] = {0};
    MnvrResult r = soap_call_auth(xaddr, user, pass, soap_body,
                                   resp, sizeof(resp));
    if (r != MNVR_OK) return r;

    memset(status, 0, sizeof(OnvifPtzStatus));

    char pos_block[512] = {0};
    if (xml_extract(resp, "Position", pos_block, sizeof(pos_block))) {
        char val[32] = {0};
        if (xml_extract_attr(pos_block, "PanTilt", "x", val, sizeof(val)))
            status->pan = (float)atof(val);
        if (xml_extract_attr(pos_block, "PanTilt", "y", val, sizeof(val)))
            status->tilt = (float)atof(val);
        if (xml_extract_attr(pos_block, "Zoom", "x", val, sizeof(val)))
            status->zoom = (float)atof(val);
    }

    char move_status[64] = {0};
    if (xml_extract(resp, "MoveStatus", move_status, sizeof(move_status))) {
        status->moving = (strstr(move_status, "MOVING") != NULL);
    }

    xml_extract(resp, "Error", status->error, sizeof(status->error));

    return MNVR_OK;
}

/* =========================================================================
 * PTZ service: GotoHomePosition
 * ========================================================================= */
MnvrResult onvif_ptz_goto_home(const char *xaddr,
                                 const char *user, const char *pass,
                                 const char *profile_token)
{
    char soap_body[256];
    snprintf(soap_body, sizeof(soap_body),
        "<GotoHomePosition xmlns=\"http://www.onvif.org/ver20/ptz/wsdl\">"
        "<ProfileToken>%s</ProfileToken>"
        "</GotoHomePosition>",
        profile_token);

    char resp[4096] = {0};
    return soap_call_auth(xaddr, user, pass, soap_body,
                          resp, sizeof(resp));
}

/* =========================================================================
 * PTZ service: SetHomePosition
 * ========================================================================= */
MnvrResult onvif_ptz_set_home(const char *xaddr,
                                const char *user, const char *pass,
                                const char *profile_token)
{
    char soap_body[256];
    snprintf(soap_body, sizeof(soap_body),
        "<SetHomePosition xmlns=\"http://www.onvif.org/ver20/ptz/wsdl\">"
        "<ProfileToken>%s</ProfileToken>"
        "</SetHomePosition>",
        profile_token);

    char resp[4096] = {0};
    return soap_call_auth(xaddr, user, pass, soap_body,
                          resp, sizeof(resp));
}

/* =========================================================================
 * Imaging service: GetImagingSettings
 * ========================================================================= */
MnvrResult onvif_imaging_get_settings(const char *xaddr,
                                        const char *user, const char *pass,
                                        const char *video_source_token,
                                        OnvifImagingSettings *settings)
{
    char soap_body[256];
    snprintf(soap_body, sizeof(soap_body),
        "<GetImagingSettings xmlns=\"http://www.onvif.org/ver20/imaging/wsdl\">"
        "<VideoSourceToken>%s</VideoSourceToken>"
        "</GetImagingSettings>",
        video_source_token);

    char resp[ONVIF_RESP_BUF_SIZE] = {0};
    MnvrResult r = soap_call_auth(xaddr, user, pass, soap_body,
                                   resp, sizeof(resp));
    if (r != MNVR_OK) return r;

    memset(settings, 0, sizeof(OnvifImagingSettings));

    settings->brightness = xml_extract_float(resp, "Brightness");
    settings->contrast   = xml_extract_float(resp, "ColorSaturation");
    settings->saturation = xml_extract_float(resp, "ColorSaturation");
    settings->sharpness  = xml_extract_float(resp, "Sharpness");

    xml_extract(resp, "IrCutFilter", settings->ir_cut_filter,
                sizeof(settings->ir_cut_filter));

    char exp_block[512] = {0};
    if (xml_extract(resp, "Exposure", exp_block, sizeof(exp_block))) {
        xml_extract(exp_block, "Mode", settings->exposure_mode,
                    sizeof(settings->exposure_mode));
        settings->exposure_time = xml_extract_float(exp_block,
                                                      "ExposureTime");
        settings->gain = xml_extract_float(exp_block, "Gain");
    }

    char wb_block[512] = {0};
    if (xml_extract(resp, "WhiteBalance", wb_block, sizeof(wb_block))) {
        xml_extract(wb_block, "Mode", settings->wb_mode,
                    sizeof(settings->wb_mode));
        settings->wb_cr_gain = xml_extract_float(wb_block, "CrGain");
        settings->wb_cb_gain = xml_extract_float(wb_block, "CbGain");
    }

    char bl_block[128] = {0};
    if (xml_extract(resp, "BacklightCompensation", bl_block,
                    sizeof(bl_block))) {
        char mode[16] = {0};
        xml_extract(bl_block, "Mode", mode, sizeof(mode));
        settings->backlight_comp = (strcmp(mode, "ON") == 0);
    }

    char wdr_block[128] = {0};
    if (xml_extract(resp, "WideDynamicRange", wdr_block,
                    sizeof(wdr_block))) {
        char mode[16] = {0};
        xml_extract(wdr_block, "Mode", mode, sizeof(mode));
        settings->wdr_enabled = (strcmp(mode, "ON") == 0);
        settings->wdr_level   = xml_extract_float(wdr_block, "Level");
    }

    return MNVR_OK;
}

/* =========================================================================
 * Imaging service: SetImagingSettings
 * ========================================================================= */
MnvrResult onvif_imaging_set_settings(const char *xaddr,
                                        const char *user, const char *pass,
                                        const char *video_source_token,
                                        const OnvifImagingSettings *settings)
{
    char soap_body[2048];
    snprintf(soap_body, sizeof(soap_body),
        "<SetImagingSettings xmlns=\"http://www.onvif.org/ver20/imaging/wsdl\">"
        "<VideoSourceToken>%s</VideoSourceToken>"
        "<ImagingSettings>"
        "<tt:Brightness>%.1f</tt:Brightness>"
        "<tt:ColorSaturation>%.1f</tt:ColorSaturation>"
        "<tt:Contrast>%.1f</tt:Contrast>"
        "<tt:Sharpness>%.1f</tt:Sharpness>"
        "<tt:IrCutFilter>%s</tt:IrCutFilter>"
        "<tt:Exposure>"
        "<tt:Mode>%s</tt:Mode>"
        "<tt:ExposureTime>%.1f</tt:ExposureTime>"
        "<tt:Gain>%.1f</tt:Gain>"
        "</tt:Exposure>"
        "<tt:WhiteBalance>"
        "<tt:Mode>%s</tt:Mode>"
        "<tt:CrGain>%.1f</tt:CrGain>"
        "<tt:CbGain>%.1f</tt:CbGain>"
        "</tt:WhiteBalance>"
        "<tt:BacklightCompensation>"
        "<tt:Mode>%s</tt:Mode>"
        "</tt:BacklightCompensation>"
        "<tt:WideDynamicRange>"
        "<tt:Mode>%s</tt:Mode>"
        "<tt:Level>%.1f</tt:Level>"
        "</tt:WideDynamicRange>"
        "</ImagingSettings>"
        "<ForcePersistence>true</ForcePersistence>"
        "</SetImagingSettings>",
        video_source_token,
        settings->brightness, settings->saturation,
        settings->contrast, settings->sharpness,
        settings->ir_cut_filter[0] ? settings->ir_cut_filter : "AUTO",
        settings->exposure_mode[0] ? settings->exposure_mode : "AUTO",
        settings->exposure_time, settings->gain,
        settings->wb_mode[0] ? settings->wb_mode : "AUTO",
        settings->wb_cr_gain, settings->wb_cb_gain,
        settings->backlight_comp ? "ON" : "OFF",
        settings->wdr_enabled ? "ON" : "OFF",
        settings->wdr_level);

    char resp[ONVIF_RESP_BUF_SIZE] = {0};
    return soap_call_auth(xaddr, user, pass, soap_body,
                          resp, sizeof(resp));
}

/* =========================================================================
 * Events service: CreatePullPointSubscription
 * ========================================================================= */
MnvrResult onvif_events_subscribe(const char *xaddr,
                                    const char *user, const char *pass,
                                    int timeout_sec,
                                    OnvifSubscription *sub)
{
    char soap_body[512];
    snprintf(soap_body, sizeof(soap_body),
        "<CreatePullPointSubscription "
        "xmlns=\"http://www.onvif.org/ver10/events/wsdl\">"
        "<InitialTerminationTime>PT%dS</InitialTerminationTime>"
        "</CreatePullPointSubscription>",
        timeout_sec > 0 ? timeout_sec : 300);

    char resp[ONVIF_RESP_BUF_SIZE] = {0};
    MnvrResult r = soap_call_auth(xaddr, user, pass, soap_body,
                                   resp, sizeof(resp));
    if (r != MNVR_OK) return r;

    memset(sub, 0, sizeof(OnvifSubscription));

    /* The subscription reference is in the Address element */
    xml_extract(resp, "Address", sub->subscription_ref,
                sizeof(sub->subscription_ref));
    xml_extract(resp, "CurrentTime", sub->current_time,
                sizeof(sub->current_time));
    xml_extract(resp, "TerminationTime", sub->termination_time,
                sizeof(sub->termination_time));

    return sub->subscription_ref[0] ? MNVR_OK : MNVR_ERR_GENERIC;
}

/* =========================================================================
 * Events service: PullMessages
 * ========================================================================= */
MnvrResult onvif_events_pull(const char *subscription_ref,
                               const char *user, const char *pass,
                               int timeout_sec, int max_messages,
                               OnvifEventMessage *msgs, int *count)
{
    char soap_body[512];
    snprintf(soap_body, sizeof(soap_body),
        "<PullMessages xmlns=\"http://www.onvif.org/ver10/events/wsdl\">"
        "<Timeout>PT%dS</Timeout>"
        "<MessageLimit>%d</MessageLimit>"
        "</PullMessages>",
        timeout_sec > 0 ? timeout_sec : 10,
        max_messages > 0 ? max_messages : ONVIF_MAX_EVENT_MSGS);

    char *resp = malloc(ONVIF_RESP_BUF_SIZE);
    if (!resp) return MNVR_ERR_NOMEM;

    MnvrResult r = soap_call_auth(subscription_ref, user, pass,
                                   soap_body, resp, ONVIF_RESP_BUF_SIZE);
    if (r != MNVR_OK) { free(resp); return r; }

    *count = 0;
    const char *pos = resp;
    while (*count < max_messages && *count < ONVIF_MAX_EVENT_MSGS) {
        const char *msg = strstr(pos, "NotificationMessage");
        if (!msg) break;

        const char *lt = msg;
        while (lt > resp && *lt != '<') lt--;
        if (*(lt + 1) == '/') { pos = msg + 1; continue; }

        OnvifEventMessage *m = &msgs[*count];
        memset(m, 0, sizeof(OnvifEventMessage));

        const char *end = strstr(msg, "/NotificationMessage>");
        if (!end) break;

        size_t blen = (size_t)(end - lt) + 21;
        char *block = malloc(blen + 1);
        if (!block) break;
        memcpy(block, lt, blen);
        block[blen] = '\0';

        xml_extract(block, "Topic", m->topic, sizeof(m->topic));

        char source_block[256] = {0};
        if (xml_extract(block, "Source", source_block,
                        sizeof(source_block))) {
            xml_extract_attr(source_block, "SimpleItem", "Value",
                             m->source, sizeof(m->source));
        }

        char data_block[256] = {0};
        if (xml_extract(block, "Data", data_block,
                        sizeof(data_block))) {
            xml_extract_attr(data_block, "SimpleItem", "Name",
                             m->data_name, sizeof(m->data_name));
            xml_extract_attr(data_block, "SimpleItem", "Value",
                             m->data_value, sizeof(m->data_value));
        }

        /* Timestamp from Message element */
        xml_extract_attr(block, "Message", "UtcTime",
                         m->timestamp, sizeof(m->timestamp));

        free(block);
        (*count)++;
        pos = end + 21;
    }

    free(resp);
    return MNVR_OK;
}

/* =========================================================================
 * Events service: Unsubscribe
 * ========================================================================= */
MnvrResult onvif_events_unsubscribe(const char *subscription_ref,
                                      const char *user, const char *pass)
{
    const char *soap_body =
        "<Unsubscribe xmlns=\"http://docs.oasis-open.org/wsn/b-2\"/>";

    char resp[4096] = {0};
    return soap_call_auth(subscription_ref, user, pass, soap_body,
                          resp, sizeof(resp));
}

/* =========================================================================
 * Direct probe — query a single known IP (full capability scan)
 * ========================================================================= */
MnvrResult onvif_probe_direct(const char *ip_address, int port,
                               const char *user, const char *pass,
                               OnvifDevice *out)
{
    if (!ip_address || !out) return MNVR_ERR_GENERIC;

    char xaddr[MNVR_MAX_URL];
    if (port <= 0 || port == 80)
        snprintf(xaddr, sizeof(xaddr),
                 "http://%s/onvif/device_service", ip_address);
    else
        snprintf(xaddr, sizeof(xaddr),
                 "http://%s:%d/onvif/device_service", ip_address, port);

    memset(out, 0, sizeof(OnvifDevice));
    strncpy(out->ip_address, ip_address, sizeof(out->ip_address) - 1);
    strncpy(out->xaddrs, xaddr, sizeof(out->xaddrs) - 1);
    out->discovered_at = time(NULL);

    /* 1. Get capabilities (fills service URLs, PTZ/imaging/events flags) */
    MnvrResult cap_r = onvif_get_capabilities(xaddr, user, pass, out);

    /* 2. Get device info (manufacturer, model, serial, firmware) */
    onvif_get_device_info(xaddr, user, pass, out);

    /* 3. Get profiles and stream URI from media service */
    const char *media_url = out->media_service_url[0]
                             ? out->media_service_url : xaddr;

    OnvifProfile profiles[ONVIF_MAX_PROFILES];
    int num_profiles = 0;
    MnvrResult prof_r = onvif_get_profiles(media_url, user, pass,
                                            profiles, &num_profiles,
                                            ONVIF_MAX_PROFILES);

    if (prof_r == MNVR_OK && num_profiles > 0) {
        /* Pick a "live" profile and a "rec" profile by name/token pattern
         * (case-insensitive substring match on "LIVE" / "REC" — matches
         * this fleet's BELLPRFLIVE / BELLPRFREC naming). Falls back to
         * profiles[0] for live and, if no distinct REC profile exists,
         * the same profile for both — preserving old single-profile
         * camera behavior. */
        int live_idx = -1, rec_idx = -1;
        for (int pi = 0; pi < num_profiles; pi++) {
            char utok[64], uname[64];
            snprintf(utok, sizeof(utok), "%s", profiles[pi].token);
            snprintf(uname, sizeof(uname), "%s", profiles[pi].name);
            for (char *c = utok; *c; c++)  *c = (char)toupper((unsigned char)*c);
            for (char *c = uname; *c; c++) *c = (char)toupper((unsigned char)*c);

            if (live_idx < 0 && (strstr(utok, "LIVE") || strstr(uname, "LIVE")))
                live_idx = pi;
            if (rec_idx < 0 && (strstr(utok, "REC") || strstr(uname, "REC")))
                rec_idx = pi;
        }
        if (live_idx < 0) live_idx = 0;
        if (rec_idx  < 0) rec_idx  = (num_profiles > 1 && live_idx != 1) ? 1 : live_idx;

        /* Store the LIVE profile's details for auto-registration (unchanged
         * field semantics — out->profile_* / out->stream_uri always meant
         * "the profile used for live view/AI" even before this change). */
        out->num_profiles    = num_profiles;
        out->profile_width   = profiles[live_idx].width;
        out->profile_height  = profiles[live_idx].height;
        out->profile_fps     = profiles[live_idx].fps;
        strncpy(out->profile_encoding, profiles[live_idx].encoding,
                sizeof(out->profile_encoding) - 1);

        MnvrResult uri_r = onvif_get_stream_uri(media_url, user, pass,
                             profiles[live_idx].token,
                             out->stream_uri, sizeof(out->stream_uri));

        /* Recording profile's stream URI — separate RTSP session/profile
         * from live, so recorder.c and streamer_module.c never compete for
         * the same connection slot on the camera. */
        MnvrResult rec_uri_r = onvif_get_stream_uri(media_url, user, pass,
                             profiles[rec_idx].token,
                             out->rec_stream_uri, sizeof(out->rec_stream_uri));
        if (rec_uri_r != MNVR_OK || !out->rec_stream_uri[0]) {
            /* Fall back to the live URI rather than leaving recording with
             * nothing to connect to. */
            strncpy(out->rec_stream_uri, out->stream_uri,
                    sizeof(out->rec_stream_uri) - 1);
        }

        /* Debug: log profile details */
        for (int pi = 0; pi < num_profiles; pi++) {
            fprintf(stderr,
                "  [ONVIF-DBG] Profile[%d]: token='%s' name='%s' "
                "%dx%d %s %dfps vec='%s'%s%s\n",
                pi, profiles[pi].token, profiles[pi].name,
                profiles[pi].width, profiles[pi].height,
                profiles[pi].encoding, profiles[pi].fps,
                profiles[pi].video_encoder_token,
                pi == live_idx ? " [LIVE]" : "",
                pi == rec_idx  ? " [REC]"  : "");
        }

        if (uri_r != MNVR_OK || !out->stream_uri[0]) {
            fprintf(stderr,
                "  [ONVIF-DBG] GetStreamUri result=%d "
                "media_url='%s' profile_token='%s' "
                "stream_uri='%s'\n",
                uri_r, media_url, profiles[live_idx].token,
                out->stream_uri);
        }

        /* Try snapshot URI */
        onvif_get_snapshot_uri(media_url, user, pass,
                               profiles[live_idx].token,
                               out->snapshot_uri,
                               sizeof(out->snapshot_uri));
    } else {
        fprintf(stderr,
            "  [ONVIF-DBG] GetProfiles FAILED: result=%d "
            "num_profiles=%d media_url='%s' "
            "cap_result=%d media_svc='%s'\n",
            prof_r, num_profiles, media_url,
            cap_r, out->media_service_url);
    }

    /* 4. Check audio */
    int audio_count = 0;
    onvif_get_audio_sources(media_url, user, pass, &audio_count);
    if (audio_count > 0) out->audio_supported = true;

    return MNVR_OK;
}

/* =========================================================================
 * Startup probe — probe all configured cameras
 * ========================================================================= */
int onvif_probe_all_configured(OnvifModule *om)
{
    if (!om) return 0;

    int probed = 0;
    const OnvifConfig *cfg = &om->config;

    LOG_INFO(om->ctx, "ONVIF",
             "Probing %d configured camera(s) via direct SOAP...",
             cfg->num_cameras);

    for (int i = 0; i < MNVR_MAX_CAMERAS; i++) {
        const OnvifCameraConfig *cam = &cfg->cameras[i];
        if (!cam->enabled) continue;

        LOG_INFO(om->ctx, "ONVIF",
                 "Probing camera slot %d: %s:%d user=%s",
                 cam->slot, cam->ip, cam->port, cam->user);

        OnvifDevice dev;
        MnvrResult r = onvif_probe_direct(cam->ip, cam->port,
                                           cam->user, cam->pass, &dev);
        if (r == MNVR_OK) {
            dev.config_slot = cam->slot; /* so callback can find config */
            LOG_INFO(om->ctx, "ONVIF",
                     "  -> %s %s (FW: %s, SN: %s)",
                     dev.manufacturer, dev.model,
                     dev.firmware, dev.serial_number);
            LOG_INFO(om->ctx, "ONVIF",
                     "  -> Stream: %s", dev.stream_uri);
            LOG_INFO(om->ctx, "ONVIF",
                     "  -> PTZ: %s  Audio: %s  Imaging: %s  Events: %s",
                     dev.ptz_supported ? "YES" : "NO",
                     dev.audio_supported ? "YES" : "NO",
                     dev.imaging_supported ? "YES" : "NO",
                     dev.events_supported ? "YES" : "NO");

            /* Add to discovered list */
            pthread_mutex_lock(&om->mutex);
            bool known = false;
            for (int j = 0; j < om->num_discovered; j++) {
                if (strcmp(om->discovered[j].ip_address,
                           dev.ip_address) == 0) {
                    om->discovered[j] = dev;  /* update existing */
                    known = true;
                    break;
                }
            }
            if (!known && om->num_discovered < MNVR_MAX_CAMERAS * 2) {
                om->discovered[om->num_discovered++] = dev;
            }
            pthread_mutex_unlock(&om->mutex);

            /* Notify callback */
            if (om->on_new_device)
                om->on_new_device(&dev, om->cb_user_data);

            probed++;
        } else {
            LOG_WARN(om->ctx, "ONVIF",
                     "  -> FAILED to probe %s (result=%d)",
                     cam->ip, r);
        }
    }

    LOG_INFO(om->ctx, "ONVIF",
             "Startup probe complete: %d/%d cameras responded",
             probed, cfg->num_cameras);

    return probed;
}

/* =========================================================================
 * Module lifecycle
 * ========================================================================= */

OnvifModule *onvif_module_create(AppContext *ctx,
                                  OnNewDeviceFound cb, void *user_data)
{
    OnvifModule *om = calloc(1, sizeof(OnvifModule));
    if (!om) return NULL;

    om->ctx            = ctx;
    om->on_new_device  = cb;
    om->cb_user_data   = user_data;

    pthread_mutex_init(&om->mutex, NULL);

    /* Load ONVIF config from PostgreSQL */
    if (ctx && ctx->db)
        onvif_config_load(&om->config, ctx->db);

    return om;
}

MnvrResult onvif_module_start(OnvifModule *om)
{
    if (!om) return MNVR_ERR_GENERIC;

    om->running = true;

    /* Probe all configured cameras on startup (blocking, before discovery thread) */
    if (om->config.num_cameras > 0)
        onvif_probe_all_configured(om);

    /* Start discovery thread */
    if (pthread_create(&om->discovery_thread, NULL,
                       discovery_thread_fn, om) != 0)
        return MNVR_ERR_GENERIC;

    return MNVR_OK;
}

void onvif_module_stop(OnvifModule *om)
{
    if (!om) return;
    om->running = false;
    pthread_join(om->discovery_thread, NULL);

    /* Unsubscribe from any active event subscriptions */
    for (int i = 0; i < MNVR_MAX_CAMERAS; i++) {
        if (om->sub_active[i] && om->subscriptions[i].subscription_ref[0]) {
            const OnvifCameraConfig *cam = &om->config.cameras[i];
            onvif_events_unsubscribe(
                om->subscriptions[i].subscription_ref,
                cam->user, cam->pass);
            om->sub_active[i] = false;
        }
    }
}

void onvif_module_destroy(OnvifModule *om)
{
    if (!om) return;
    onvif_module_stop(om);
    pthread_mutex_destroy(&om->mutex);
    free(om);
}

void onvif_module_set_event_callback(OnvifModule *om,
                                      OnOnvifEvent cb, void *user_data)
{
    if (!om) return;
    om->on_event = cb;
    om->event_user_data = user_data;
}

int onvif_get_discovered(OnvifModule *om, OnvifDevice *out, int max_count)
{
    if (!om) return 0;
    pthread_mutex_lock(&om->mutex);
    int n = om->num_discovered < max_count ? om->num_discovered : max_count;
    memcpy(out, om->discovered, (size_t)n * sizeof(OnvifDevice));
    pthread_mutex_unlock(&om->mutex);
    return n;
}