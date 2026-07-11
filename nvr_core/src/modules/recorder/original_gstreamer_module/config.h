/**
 * @file config.h
 * @brief Configuration management for Multi-Camera NVR Recorder
 * 
 * This header defines configuration structures and functions for managing
 * recording settings, network parameters, and file options for the NVR system.
 * Supports both global and per-camera configuration with segmentation options.
 */

#ifndef CONFIG_H
#define CONFIG_H

#include <glib.h>

#define MAX_CAMERAS 16
#define MAX_NAME_LEN 64
#define MAX_PATH_LEN 512

/* Forward declaration to avoid circular dependency */
typedef struct RecorderManager RecorderManager;

/**
 * @brief Recording configuration structure
 * 
 * Contains all configurable parameters for video recording including
 * file segmentation, network settings, and output format options.
 */
typedef struct {
    /** @brief Enable automatic file segmentation */
    gboolean enable_segmentation;
    
    /** @brief Maximum file size in megabytes (0 = unlimited) */
    guint64 max_file_size_mb;
    
    /** @brief Maximum file duration in seconds */
    guint64 max_file_duration_sec;
    
    /** @brief RTSP stream latency in milliseconds */
    guint rtsp_latency_ms;
    
    /** @brief RTSP connection timeout in seconds */
    guint rtsp_timeout_sec;
    
    /** @brief Use TCP for RTSP transport (vs UDP) */
    gboolean use_tcp;
    
    /** @brief Output file extension (e.g., "mp4") */
    gchar file_extension[16];
    
    /** @brief Add timestamp to output filenames */
    gboolean add_timestamp;
    
    /** @brief Enable MP4 fragmentation for streamability */
    gboolean enable_fragments;
    
    /** @brief MP4 fragment duration in milliseconds */
    guint fragment_duration_ms;

    /** @brief Video codec: "H.264" or "H.265" (determines depay/parser elements) */
    gchar video_codec[16];
} RecordingConfig;

/**
 * @brief Initialize recording configuration with default values
 * 
 * Sets sensible defaults for all configuration parameters:
 * - Segmentation disabled by default
 * - 1-hour max duration
 * - TCP transport enabled
 * - MP4 format with fragmentation
 * - Timestamps enabled
 * 
 * @param[out] config Pointer to configuration structure to initialize
 * 
 * Called from: parse_config_file() for global config, recorder_create() for new cameras
 */
void config_init_defaults(RecordingConfig *config);

/**
 * @brief Parse configuration file and populate recorder manager
 * 
 * Reads INI-style configuration file with [global] and [camera_N] sections.
 * Supports both global defaults and per-camera overrides.
 * 
 * Configuration format:
 * - [global] section: default settings for all cameras
 * - [camera_N] sections: individual camera settings
 * - Key=value pairs for all parameters
 * 
 * @param[in] filename Path to configuration file
 * @param[in,out] mgr Recorder manager to populate with camera configurations
 * @return Number of cameras configured, -1 on error
 * 
 * Called from: main()
 */
int config_parse_file(const char *filename, RecorderManager *mgr);

#endif /* CONFIG_H */
