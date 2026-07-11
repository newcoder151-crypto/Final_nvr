/**
 * @file gps_module.c
 * @brief See gps_module.h for the design notes.
 */
#include "gps_module.h"
#include "../logger/logger.h"
#include "../config/config_module.h"
#include "streamer_module.h"
#include "recorder_module.h"
#include "recorder.h"   /* CameraRecorder + recorder_update_telemetry() */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <termios.h>
#include <errno.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

struct GpsModule {
    AppContext     *ctx;
    pthread_t       thread;
    volatile bool   running;
    int             fd;              /* serial port fd, -1 when closed */

    /* Previous fix, for the haversine speed fallback */
    bool            have_prev;
    double          prev_lat, prev_lon;
    struct timespec prev_ts;
};

/* =========================================================================
 * Pure logic: haversine distance + NMEA GPRMC parsing
 * ========================================================================= */

double gps_haversine_distance_m(double lat1, double lon1,
                                 double lat2, double lon2)
{
    const double R = 6371000.0; /* mean Earth radius, metres */
    double dlat = (lat2 - lat1) * M_PI / 180.0;
    double dlon = (lon2 - lon1) * M_PI / 180.0;
    double a = sin(dlat / 2) * sin(dlat / 2) +
               cos(lat1 * M_PI / 180.0) * cos(lat2 * M_PI / 180.0) *
               sin(dlon / 2) * sin(dlon / 2);
    double c = 2 * atan2(sqrt(a), sqrt(1 - a));
    return R * c;
}

/* Convert NMEA's "ddmm.mmmm" (or "dddmm.mmmm" for longitude) into decimal
 * degrees. hemisphere is 'N'/'S'/'E'/'W'. */
static double nmea_coord_to_decimal(const char *field, char hemisphere)
{
    if (!field || !*field) return 0.0;
    double raw = atof(field);
    int deg = (int)(raw / 100.0);
    double min = raw - (deg * 100.0);
    double decimal = deg + (min / 60.0);
    if (hemisphere == 'S' || hemisphere == 'W') decimal = -decimal;
    return decimal;
}

bool gps_parse_gprmc(const char *line, double *lat, double *lon,
                     double *speed_knots, bool *valid_fix)
{
    if (!line || !lat || !lon || !speed_knots || !valid_fix) return false;
    if (strncmp(line, "$GPRMC", 6) != 0 && strncmp(line, "$GNRMC", 6) != 0)
        return false;

    /* $GPRMC,time,status,lat,NS,lon,EW,speed_knots,track,date,... */
    char buf[256];
    strncpy(buf, line, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    /* Strip checksum/CR/LF if present */
    char *star = strchr(buf, '*');
    if (star) *star = '\0';

    char *fields[13] = {0};
    int nfields = 0;
    char *tok = strtok(buf, ",");
    while (tok && nfields < 13) {
        fields[nfields++] = tok;
        tok = strtok(NULL, ",");
    }
    if (nfields < 8) return false; /* not enough fields to be a real RMC */

    /* fields[0]=$GPRMC fields[1]=time fields[2]=status fields[3]=lat
     * fields[4]=N/S fields[5]=lon fields[6]=E/W fields[7]=speed(knots) */
    *valid_fix   = (fields[2][0] == 'A');
    *lat         = nmea_coord_to_decimal(fields[3], fields[4][0]);
    *lon         = nmea_coord_to_decimal(fields[5], fields[6][0]);
    *speed_knots = atof(fields[7]);
    return true;
}

/* =========================================================================
 * Broadcast a resolved fix to every camera's live-view + recording overlay
 * ========================================================================= */

void gps_module_on_fix(GpsModule *m, double lat, double lon,
                       bool has_speed_field, double speed_knots,
                       bool valid_fix)
{
    if (!m || !m->ctx) return;
    AppContext *ctx = m->ctx;

    double speed_kmh = 0.0;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    if (valid_fix) {
        if (has_speed_field) {
            /* Preferred: the GPS chip's own Doppler-derived speed. */
            speed_kmh = speed_knots * 1.852;
        } else if (m->have_prev) {
            /* Fallback: derive speed from displacement / elapsed time. */
            double dt_sec = (now.tv_sec - m->prev_ts.tv_sec) +
                            (now.tv_nsec - m->prev_ts.tv_nsec) / 1e9;
            if (dt_sec > 0.05) { /* ignore absurdly small/duplicate intervals */
                double dist_m = gps_haversine_distance_m(m->prev_lat, m->prev_lon, lat, lon);
                speed_kmh = (dist_m / dt_sec) * 3.6; /* m/s -> km/h */
            }
        }
        m->prev_lat = lat;
        m->prev_lon = lon;
        m->prev_ts  = now;
        m->have_prev = true;
    }

    /* Broadcast to every camera's live-view watermark. */
    if (ctx->streamer) {
        pthread_mutex_lock(&ctx->streamer->mutex);
        for (int i = 0; i < ctx->streamer->num_cams; i++) {
            streamer_update_telemetry(&ctx->streamer->cams[i], lat, lon,
                                       speed_kmh, valid_fix);
        }
        pthread_mutex_unlock(&ctx->streamer->mutex);
    }

    /* Broadcast to every camera's recording watermark — recorder.c has its
     * own separate overlay again now that it reads the camera's dedicated
     * recording profile directly, rather than streamer_module.c's already-
     * watermarked restream. */
    if (ctx->recorder) {
        pthread_mutex_lock(&ctx->recorder->mutex);
        for (int i = 0; i < ctx->recorder->num_cams; i++) {
            CameraRecorder *rec = (CameraRecorder *)ctx->recorder->cams[i]._orig_rec;
            if (rec) recorder_update_telemetry(rec, lat, lon, speed_kmh, valid_fix);
        }
        pthread_mutex_unlock(&ctx->recorder->mutex);
    }
}

/* =========================================================================
 * Serial reader thread
 * ========================================================================= */

static speed_t baud_to_termios(int baud)
{
    switch (baud) {
        case 4800:   return B4800;
        case 9600:   return B9600;
        case 19200:  return B19200;
        case 38400:  return B38400;
        case 57600:  return B57600;
        case 115200: return B115200;
        default:     return B9600;
    }
}

static int open_serial(const char *device, int baud, AppContext *ctx)
{
    int fd = open(device, O_RDONLY | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        LOG_WARN(ctx, "GPS", "Could not open %s: %s (GPS hardware not connected yet?)",
                 device, strerror(errno));
        return -1;
    }

    struct termios tty;
    memset(&tty, 0, sizeof(tty));
    if (tcgetattr(fd, &tty) != 0) {
        LOG_WARN(ctx, "GPS", "tcgetattr(%s) failed: %s", device, strerror(errno));
        close(fd);
        return -1;
    }

    speed_t spd = baud_to_termios(baud);
    cfsetispeed(&tty, spd);
    cfsetospeed(&tty, spd);

    tty.c_cflag |= (CLOCAL | CREAD);
    tty.c_cflag &= ~PARENB;
    tty.c_cflag &= ~CSTOPB;
    tty.c_cflag &= ~CSIZE;
    tty.c_cflag |= CS8;
    tty.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG);
    tty.c_iflag &= ~(IXON | IXOFF | IXANY | ICRNL);
    tty.c_oflag &= ~OPOST;
    tty.c_cc[VMIN]  = 0;
    tty.c_cc[VTIME] = 5; /* 0.5s read timeout */

    if (tcsetattr(fd, TCSANOW, &tty) != 0) {
        LOG_WARN(ctx, "GPS", "tcsetattr(%s) failed: %s", device, strerror(errno));
        close(fd);
        return -1;
    }

    /* Clear the non-blocking flag now that the port is configured — the
     * VTIME above gives us a bounded blocking read instead. */
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);

    return fd;
}

static void *gps_thread_fn(void *arg)
{
    GpsModule *m = (GpsModule *)arg;
    AppContext *ctx = m->ctx;
    const SystemConfig *cfg = config_get(ctx->config);

    char linebuf[256];
    size_t linelen = 0;

    while (m->running) {
        if (m->fd < 0) {
            m->fd = open_serial(cfg->gps_serial_device, cfg->gps_baud_rate, ctx);
            if (m->fd < 0) {
                /* Retry periodically in case the receiver is plugged in later. */
                sleep(10);
                continue;
            }
            LOG_INFO(ctx, "GPS", "Opened %s @ %d baud", cfg->gps_serial_device,
                      cfg->gps_baud_rate);
        }

        char c;
        ssize_t n = read(m->fd, &c, 1);
        if (n <= 0) {
            if (n < 0 && errno != EAGAIN && errno != EINTR) {
                LOG_WARN(ctx, "GPS", "Serial read error: %s — reconnecting", strerror(errno));
                close(m->fd);
                m->fd = -1;
            }
            continue;
        }

        if (c == '\n' || c == '\r') {
            if (linelen > 0) {
                linebuf[linelen] = '\0';
                double lat, lon, speed_knots;
                bool valid_fix;
                if (gps_parse_gprmc(linebuf, &lat, &lon, &speed_knots, &valid_fix)) {
                    gps_module_on_fix(m, lat, lon, /*has_speed_field=*/true,
                                       speed_knots, valid_fix);
                }
            }
            linelen = 0;
        } else if (linelen < sizeof(linebuf) - 1) {
            linebuf[linelen++] = c;
        } else {
            linelen = 0; /* line too long / garbage — resync */
        }
    }

    if (m->fd >= 0) close(m->fd);
    return NULL;
}

/* =========================================================================
 * Lifecycle
 * ========================================================================= */

GpsModule *gps_module_create(AppContext *ctx)
{
    GpsModule *m = (GpsModule *)calloc(1, sizeof(GpsModule));
    if (!m) return NULL;
    m->ctx = ctx;
    m->fd  = -1;
    return m;
}

MnvrResult gps_module_start(GpsModule *m)
{
    if (!m || !m->ctx) return MNVR_ERR_GENERIC;
    const SystemConfig *cfg = config_get(m->ctx->config);

    if (!cfg->gps_enabled) {
        LOG_INFO(m->ctx, "GPS",
                 "GPS disabled (gps_enabled=false in mnvr.conf) — "
                 "watermark will show GPS: NO FIX until enabled");
        return MNVR_OK; /* not an error: hardware simply isn't installed yet */
    }

    m->running = true;
    if (pthread_create(&m->thread, NULL, gps_thread_fn, m) != 0) {
        LOG_FATAL(m->ctx, "GPS", "Failed to start GPS reader thread");
        m->running = false;
        return MNVR_ERR_GENERIC;
    }
    return MNVR_OK;
}

void gps_module_stop(GpsModule *m)
{
    if (!m || !m->running) return;
    m->running = false;
    pthread_join(m->thread, NULL);
}

void gps_module_destroy(GpsModule *m)
{
    if (!m) return;
    if (m->running) gps_module_stop(m);
    free(m);
}
