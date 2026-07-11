/**
 * @file gps_module.h
 * @brief GPS telemetry: reads NMEA sentences from a serial GPS receiver,
 *        derives speed, and pushes lat/lon/speed into the watermark shown
 *        on both the live view and recordings.
 *
 * Two ways speed is obtained (see gps_module.c for the actual logic):
 *   1. Preferred: read directly from the GPRMC sentence's own "speed over
 *      ground" field (in knots) — this is what the GPS chip itself
 *      computed from Doppler shift, and is far more accurate than
 *      anything we could derive ourselves from consecutive fixes.
 *   2. Fallback: if a sentence has a valid position but no usable speed
 *      field, compute it ourselves via the haversine great-circle
 *      distance between this fix and the previous one, divided by the
 *      elapsed time.
 *
 * No GPS hardware exists on this system yet. This module is fully
 * functional and ready to run the moment one is connected — just set
 * gps_enabled=true and gps_serial_device to the right /dev/ttyUSBx in
 * mnvr.conf. Until then, gps_module_start() logs a warning and exits
 * cleanly (a missing serial device is not treated as fatal).
 */
#ifndef GPS_MODULE_H
#define GPS_MODULE_H

#include "mnvr_system.h"

typedef struct GpsModule GpsModule;

GpsModule *gps_module_create(AppContext *ctx);
MnvrResult gps_module_start(GpsModule *m);
void       gps_module_stop(GpsModule *m);
void       gps_module_destroy(GpsModule *m);

/* ---- Pure logic, exposed for unit testing / reuse ---------------------- */

/** Great-circle distance between two lat/lon points, in metres. */
double gps_haversine_distance_m(double lat1, double lon1,
                                 double lat2, double lon2);

/**
 * Parse a single NMEA sentence line (any trailing \r\n is ignored).
 * Handles $GPRMC / $GNRMC (position + speed + validity in one sentence).
 * Returns true if the line was a recognised, syntactically valid RMC
 * sentence (regardless of navigation-fix validity — check *valid_fix for
 * that). Returns false for any other sentence type or malformed input.
 *
 * @param[out] lat, lon        Decimal degrees (sign-adjusted for N/S, E/W)
 * @param[out] speed_knots     Speed over ground, in knots
 * @param[out] valid_fix       NMEA status field: true = 'A' (active/valid),
 *                             false = 'V' (void/no fix)
 */
bool gps_parse_gprmc(const char *line, double *lat, double *lon,
                     double *speed_knots, bool *valid_fix);

/**
 * Feed one resolved fix into the module: computes speed_kmh (from
 * speed_knots if has_speed_field, otherwise via haversine against the
 * previous fix) and broadcasts lat/lon/speed/has_fix to every camera's
 * live-view and recording watermark (streamer_update_telemetry /
 * recorder_update_telemetry for each camera in ctx->streamer / ctx->recorder).
 */
void gps_module_on_fix(GpsModule *m, double lat, double lon,
                       bool has_speed_field, double speed_knots,
                       bool valid_fix);

#endif /* GPS_MODULE_H */
