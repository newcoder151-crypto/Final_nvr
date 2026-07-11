/**
 * @file config.c
 * @brief Configuration file parsing and management implementation
 * 
 * Implements INI-style configuration file parsing for the NVR recorder.
 * Supports global defaults and per-camera overrides for all recording
 * parameters including segmentation, network settings, and file options.
 * 
 * Configuration file format:
 * - [global] section for default settings
 * - [camera_N] sections for individual cameras
 * - Key=value pairs with automatic whitespace trimming
 * - Comments start with #
 */

#include "config.h"
#include "manager.h"
#include "utils.h"
#include <stdio.h>
#include <string.h>

/**
 * @brief Initialize recording configuration with default values
 * 
 * Sets all configuration parameters to sensible defaults suitable
 * for most RTSP recording scenarios.
 * 
 * Default values:
 * - Segmentation: disabled
 * - Max duration: 3600 seconds (1 hour)
 * - RTSP latency: 200ms
 * - RTSP timeout: 10 seconds
 * - Transport: TCP
 * - Format: MP4 with fragmentation
 * - Fragment duration: 1000ms
 * - Timestamps: enabled
 * 
 * @param[out] config Configuration structure to initialize
 * 
 * Called from: recorder_create(), config_parse_file()
 */
void config_init_defaults(RecordingConfig *config) {
    config->enable_segmentation = FALSE;
    config->max_file_size_mb = 0;
    config->max_file_duration_sec = 3600;
    
    config->rtsp_latency_ms = 200;
    config->rtsp_timeout_sec = 10;
    config->use_tcp = TRUE;
    
    g_strlcpy(config->file_extension, "mp4", sizeof(config->file_extension));
    config->add_timestamp = TRUE;
    
    config->enable_fragments = TRUE;
    config->fragment_duration_ms = 1000;

    g_strlcpy(config->video_codec, "H.264", sizeof(config->video_codec));
}

/**
 * @brief Parse configuration file and populate recorder manager
 * 
 * Reads INI-style configuration file line by line. Maintains state
 * for current section ([global] or [camera_N]) and applies settings
 * appropriately to global config or specific cameras.
 * 
 * Parsing flow:
 * 1. Initialize global config with defaults
 * 2. Parse sections and key=value pairs
 * 3. For each camera section:
 *    - Add camera when all required fields present
 *    - Apply global config as defaults
 *    - Override with camera-specific settings
 * 4. Create output directories as needed
 * 
 * Required camera fields:
 * - camera_url: RTSP stream URL
 * - output_file: Output file path prefix
 * 
 * Optional camera fields:
 * - name: Camera display name
 * - All recording config parameters
 * 
 * @param[in] filename Path to configuration file
 * @param[in,out] mgr Recorder manager to populate with cameras
 * @return Number of cameras successfully configured, -1 on file error
 * 
 * Called from: main()
 */
int config_parse_file(const char *filename, RecorderManager *mgr) {
    FILE *fp = fopen(filename, "r");
    if (!fp) {
        g_printerr("Failed to open config file: %s\n", filename);
        return -1;
    }

    char line[1024];
    RecordingConfig global_config;
    config_init_defaults(&global_config);
    
    CameraRecorder *current_camera = NULL;
    gboolean in_global_section = FALSE;
    gboolean in_camera_section = FALSE;
    
    gchar *camera_name = NULL;
    gchar *camera_url = NULL;
    gchar *output_file = NULL;

    while (fgets(line, sizeof(line), fp)) {
        // Remove newline characters
        line[strcspn(line, "\r\n")] = 0;
        
        // Skip empty lines and comments
        if (line[0] == '\0' || line[0] == '#') {
            continue;
        }
        
        // Section headers
        if (strncmp(line, "[global]", 8) == 0) {
            in_global_section = TRUE;
            in_camera_section = FALSE;
            continue;
        } else if (strncmp(line, "[camera_", 8) == 0) {
            // Save previous camera if exists
            if (camera_url && output_file && !current_camera) {
                int id = manager_add_camera(mgr, camera_name ? camera_name : "Camera", 
                                           camera_url, output_file);
                if (id >= 0) {
                    current_camera = mgr->cameras[id];
                }
            }
            
            // Reset for new camera section
            g_free(camera_name);
            g_free(camera_url);
            g_free(output_file);
            camera_name = NULL;
            camera_url = NULL;
            output_file = NULL;
            current_camera = NULL;
            
            in_global_section = FALSE;
            in_camera_section = TRUE;
            continue;
        }
        
        // Parse key=value pairs
        char *equals = strchr(line, '=');
        if (!equals) continue;
        
        *equals = '\0';
        char *key = line;
        char *value = equals + 1;
        
        // Trim leading whitespace
        while (*key == ' ' || *key == '\t') key++;
        while (*value == ' ' || *value == '\t') value++;
        
        // Trim trailing whitespace from key
        char *key_end = key + strlen(key) - 1;
        while (key_end > key && (*key_end == ' ' || *key_end == '\t')) {
            *key_end = '\0';
            key_end--;
        }
        
        // Trim trailing whitespace from value
        char *value_end = value + strlen(value) - 1;
        while (value_end > value && (*value_end == ' ' || *value_end == '\t')) {
            *value_end = '\0';
            value_end--;
        }
        
        // Parse camera identification fields
        if (g_strcmp0(key, "name") == 0 && in_camera_section) {
            g_free(camera_name);
            camera_name = g_strdup(value);
        }
        else if (g_strcmp0(key, "camera_url") == 0 && in_camera_section) {
            g_free(camera_url);
            camera_url = g_strdup(value);
        }
        else if (g_strcmp0(key, "output_file") == 0 && in_camera_section) {
            g_free(output_file);
            output_file = g_strdup(value);

            // Create parent directory if needed
            ensure_output_dir_exists(output_file);
            
            // Create camera when we have both URL and output file
            if (camera_url && output_file) {
                int id = manager_add_camera(mgr, camera_name ? camera_name : "Camera",
                                           camera_url, output_file);
                if (id >= 0) {
                    current_camera = mgr->cameras[id];
                    // Apply global config as defaults
                    current_camera->config = global_config;
                    g_strlcpy(current_camera->config.file_extension, global_config.file_extension,
                             sizeof(current_camera->config.file_extension));
                }
            }
        }
        // Parse segmentation settings
        else if (g_strcmp0(key, "enable_segmentation") == 0) {
            gboolean val = (g_strcmp0(value, "true") == 0 || g_strcmp0(value, "1") == 0);
            if (in_camera_section && current_camera) {
                current_camera->config.enable_segmentation = val;
            } else if (in_global_section) {
                global_config.enable_segmentation = val;
            }
        }
        else if (g_strcmp0(key, "max_file_size_mb") == 0) {
            guint64 val = g_ascii_strtoull(value, NULL, 10);
            if (in_camera_section && current_camera) {
                current_camera->config.max_file_size_mb = val;
                if (val > 0) current_camera->config.enable_segmentation = TRUE;
            } else if (in_global_section) {
                global_config.max_file_size_mb = val;
                if (val > 0) global_config.enable_segmentation = TRUE;
            }
        }
        else if (g_strcmp0(key, "max_file_duration_sec") == 0) {
            guint64 val = g_ascii_strtoull(value, NULL, 10);
            if (in_camera_section && current_camera) {
                current_camera->config.max_file_duration_sec = val;
                if (val > 0) current_camera->config.enable_segmentation = TRUE;
            } else if (in_global_section) {
                global_config.max_file_duration_sec = val;
                if (val > 0) global_config.enable_segmentation = TRUE;
            }
        }
        // Parse network settings
        else if (g_strcmp0(key, "rtsp_latency_ms") == 0) {
            guint val = g_ascii_strtoull(value, NULL, 10);
            if (in_camera_section && current_camera) {
                current_camera->config.rtsp_latency_ms = val;
            } else if (in_global_section) {
                global_config.rtsp_latency_ms = val;
            }
        }
        else if (g_strcmp0(key, "rtsp_timeout_sec") == 0) {
            guint val = g_ascii_strtoull(value, NULL, 10);
            if (in_camera_section && current_camera) {
                current_camera->config.rtsp_timeout_sec = val;
            } else if (in_global_section) {
                global_config.rtsp_timeout_sec = val;
            }
        }
        else if (g_strcmp0(key, "use_tcp") == 0) {
            gboolean val = (g_strcmp0(value, "true") == 0 || g_strcmp0(value, "1") == 0);
            if (in_camera_section && current_camera) {
                current_camera->config.use_tcp = val;
            } else if (in_global_section) {
                global_config.use_tcp = val;
            }
        }
        // Parse file settings
        else if (g_strcmp0(key, "file_extension") == 0) {
            if (in_camera_section && current_camera) {
                g_strlcpy(current_camera->config.file_extension, value,
                         sizeof(current_camera->config.file_extension));
            } else if (in_global_section) {
                g_strlcpy(global_config.file_extension, value,
                         sizeof(global_config.file_extension));
            }
        }
        else if (g_strcmp0(key, "add_timestamp") == 0) {
            gboolean val = (g_strcmp0(value, "true") == 0 || g_strcmp0(value, "1") == 0);
            if (in_camera_section && current_camera) {
                current_camera->config.add_timestamp = val;
            } else if (in_global_section) {
                global_config.add_timestamp = val;
            }
        }
    }

    // Cleanup temporary strings
    g_free(camera_name);
    g_free(camera_url);
    g_free(output_file);
    fclose(fp);
    
    g_print("Loaded %d cameras from config file\n", mgr->num_cameras);
    return mgr->num_cameras;
}
