/**
 * @file recorder.h
 * @brief Individual camera recorder management
 * 
 * Defines structures and functions for managing individual camera recorders.
 * Each recorder handles one RTSP stream with its own GStreamer pipeline,
 * configuration, and recording thread.
 */

#ifndef RECORDER_H
#define RECORDER_H

#include <gst/gst.h>
#include <pthread.h>
#include "config.h"

/**
 * @brief Camera recorder structure
 * 
 * Represents a single camera recording session with its own GStreamer pipeline,
 * configuration, and control thread. Manages RTSP stream capture and file output.
 */
typedef struct {
    /** @brief Unique camera identifier (0-based index) */
    int camera_id;
    
    /** @brief Human-readable camera name for logging */
    gchar camera_name[MAX_NAME_LEN];
    
    /** @brief GStreamer pipeline for this camera */
    GstElement *pipeline;

    /* --- Source: the camera's own RTSP, but its dedicated RECORDING
     * profile (e.g. "BELLPRFREC"), separate from the profile
     * streamer_module.c connects to for live view ("BELLPRFLIVE"). Most
     * cameras allow one concurrent RTSP session PER PROFILE, so two
     * different profiles don't compete for the same connection slot the
     * way two connections to the *same* profile would. Since this reads
     * the camera directly (not an already-watermarked relay), this
     * pipeline does its own decode -> overlay -> re-encode below. */
    GstElement *rtspsrc;

    /** @brief RTP H.264 depayloader */
    GstElement *depay;
    
    /** @brief H.264 parser element */
    GstElement *parser;
    
    /** @brief Queue element for buffering */
    GstElement *queue;
    
    /** @brief Splitmux sink for file segmentation */
    GstElement *splitmux;
    
    /** @brief MP4 muxer element */
    GstElement *muxer;
    
    /** @brief File sink element (non-segmented mode) */
    GstElement *filesink;

    /* --- Watermark chain: decode -> overlay(s) -> re-encode -> re-parse.
     * Inserted between parser and queue/muxer so recordings carry the
     * same burned-in date/time + GPS/speed overlay the live view shows. */
    GstElement *decoder;         /**< avdec_h264 / avdec_h265 */
    GstElement *videoconvert;
    GstElement *clock_overlay;   /**< clockoverlay: date/time */
    GstElement *gps_overlay;     /**< textoverlay: GPS + speed */
    GstElement *overlay_convert;
    GstElement *encoder;         /**< x264enc / x265enc, re-encodes after overlay */
    GstElement *out_parser;      /**< h264parse/h265parse, muxer needs a parsed stream */

    /** @brief Monotonic timestamp (us) of the last overlay text refresh */
    gint64 gps_overlay_last_update_us;
    /** @brief Live GPS/speed telemetry shown in the watermark (see recorder_update_telemetry) */
    volatile gdouble  gps_lat;
    volatile gdouble  gps_lon;
    volatile gdouble  gps_speed_kmh;
    volatile gboolean gps_has_fix;

    /** @brief GLib main loop for this recorder */
    GMainLoop *loop;

    /* Consecutive failure count, drives exponential retry backoff — see
     * recorder_thread(). Resets to 0 after a connection stays up a while. */
    int retry_count;
    
    /** @brief Recording thread handle */
    pthread_t thread;
    
    /** @brief RTSP camera URL (location only, no embedded credentials) */
    gchar camera_url[MAX_PATH_LEN];

    /** @brief RTSP username (set via user-id property, supports special chars) */
    gchar rtsp_username[64];

    /** @brief RTSP password (set via user-pw property, supports @, :, etc.) */
    gchar rtsp_password[128];

    /** @brief Output file path (without extension) */
    gchar output_file[MAX_PATH_LEN];
    
    /** @brief Current recording status */
    gboolean is_recording;

    /** @brief Stop flag for graceful shutdown */
    gboolean should_stop;

    /** @brief Set after first pthread_create; prevents double-thread on retry */
    gboolean _in_retry;
    
    /** @brief Recording configuration for this camera */
    RecordingConfig config;
} CameraRecorder;

/**
 * @brief Create a new camera recorder instance
 * 
 * Allocates and initializes a CameraRecorder structure. rtsp_url should be
 * the camera's dedicated RECORDING profile URL (falls back to the same
 * URL as live view if the camera has no distinct one — see
 * config_module.c's rec_rtsp_url handling). Does not create the pipeline
 * or start recording; call recorder_start() for that.
 * 
 * @param[in] id Unique camera identifier
 * @param[in] name Human-readable camera name (NULL for default)
 * @param[in] rtsp_url Recording-profile RTSP stream URL
 * @param[in] output_file Output file path prefix (without extension)
 * @return Pointer to newly created CameraRecorder, NULL on failure
 * 
 * Called from: manager_add_camera()
 */
CameraRecorder* recorder_create(int id, const char *name, const char *rtsp_url, const char *output_file);

/**
 * @brief Start recording for a camera
 * 
 * Creates GStreamer pipeline, configures elements based on recorder config,
 * and starts recording thread. Supports both segmented and continuous recording.
 * 
 * Source: the camera's dedicated recording profile, connected directly via
 * RTSP — a different profile from the one streamer_module.c uses for live
 * view, so the two don't compete for the same connection slot on the
 * camera (most cameras allow one concurrent session per profile).
 *
 * Pipeline topology (segmented):
 * rtspsrc -> depay -> parser -> [decode -> overlay -> re-encode] -> queue -> splitmuxsink
 * 
 * Pipeline topology (continuous):
 * rtspsrc -> depay -> parser -> [decode -> overlay -> re-encode] -> mp4mux -> filesink
 * 
 * @param[in,out] rec Pointer to CameraRecorder to start
 * @return 0 on success, -1 on failure
 * 
 * Called from: manager_start_all()
 */
int recorder_start(CameraRecorder *rec);

/**
 * @brief Update the GPS/speed telemetry burned into the recording watermark.
 * Call whenever the GPS/telemetry module has a new fix. Thread-safe (plain
 * writes; the encoder-side probe reads the latest value once per second).
 */
void recorder_update_telemetry(CameraRecorder *rec, gdouble lat, gdouble lon,
                                gdouble speed_kmh, gboolean has_fix);

/**
 * @brief Stop recording for a camera
 * 
 * Gracefully stops recording by sending EOS event, stopping pipeline,
 * joining thread, and cleaning up resources. Waits for clean shutdown.
 * 
 * @param[in,out] rec Pointer to CameraRecorder to stop
 * 
 * Called from: manager_stop_all(), recorder_destroy()
 */
void recorder_stop(CameraRecorder *rec);

/**
 * @brief Destroy camera recorder and free resources
 * 
 * Stops recording if active and frees all allocated memory.
 * After this call, the recorder pointer is invalid.
 * 
 * @param[in,out] rec Pointer to CameraRecorder to destroy
 * 
 * Called from: manager_destroy()
 */
void recorder_destroy(CameraRecorder *rec);

/**
 * @brief Recording thread function
 * 
 * Runs GLib main loop for this camera's pipeline. Executes in separate
 * pthread for concurrent multi-camera recording.
 * 
 * @param[in] arg Pointer to CameraRecorder (cast from void*)
 * @return NULL (pthread return value)
 * 
 * Called from: pthread_create() in recorder_start()
 */
void* recorder_thread(void *arg);

#endif /* RECORDER_H */
