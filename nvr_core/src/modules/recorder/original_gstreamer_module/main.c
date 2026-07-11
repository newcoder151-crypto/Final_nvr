/**
 * @file main.c
 * @brief Main entry point for Professional Multi-Camera NVR Recorder
 * 
 * Initializes GStreamer, loads configuration, sets up signal handlers,
 * and manages the recording lifecycle. Coordinates all modules to provide
 * a complete multi-camera NVR recording solution.
 * 
 * Program flow:
 * 1. Initialize GStreamer
 * 2. Validate command-line arguments
 * 3. Set up signal handlers (SIGINT, SIGTERM)
 * 4. Create recorder manager
 * 5. Parse configuration file
 * 6. Start all cameras
 * 7. Wait for completion or signal
 * 8. Clean shutdown
 * 
 * Features:
 * - Multi-camera simultaneous recording
 * - RTSP stream capture
 * - File segmentation (size/duration based)
 * - MP4 output with optional fragmentation
 * - Graceful shutdown on Ctrl+C
 * - Configuration-driven setup
 */

#include <gst/gst.h>
#include <stdio.h>
#include <signal.h>
#include "config.h"
#include "manager.h"
#include "utils.h"

/**
 * @brief Print usage information
 * 
 * Displays program usage, command-line syntax, and example
 * configuration file format to help users get started.
 * 
 * @param[in] program_name Name of executable (argv[0])
 */
static void print_usage(const char *program_name) {
    g_print("Professional Multi-Camera NVR Recorder\n\n");
    g_print("Usage: %s <config_file.conf>\n\n", program_name);
    g_print("Example config:\n");
    g_print("  [global]\n");
    g_print("  enable_segmentation=true\n");
    g_print("  max_file_duration_sec=3600\n");
    g_print("  max_file_size_mb=2048\n\n");
    g_print("  [camera_1]\n");
    g_print("  name=Front Door\n");
    g_print("  camera_url=rtsp://admin:pass@192.168.1.100/stream\n");
    g_print("  output_file=/recordings/front_door\n\n");
}

/**
 * @brief Main entry point
 * 
 * Orchestrates the complete lifecycle of the NVR recorder application.
 * 
 * Execution sequence:
 * 1. Initialize GStreamer subsystem
 * 2. Validate command-line arguments
 * 3. Register signal handlers for graceful shutdown
 * 4. Create recorder manager
 * 5. Parse configuration file and add cameras
 * 6. Validate at least one camera configured
 * 7. Start all camera recordings
 * 8. Wait for all recording threads to complete
 * 9. Clean up manager and resources
 * 
 * Signal handling:
 * - SIGINT (Ctrl+C): Graceful shutdown
 * - SIGTERM: Graceful shutdown
 * 
 * Exit codes:
 * - 0: Successful completion
 * - -1: Error (invalid args, config parse error, no cameras)
 * 
 * @param[in] argc Argument count
 * @param[in] argv Argument vector (argv[1] = config file path)
 * @return 0 on success, -1 on error
 */
int main(int argc, char *argv[]) {
    RecorderManager *manager;

    // Initialize GStreamer
    gst_init(&argc, &argv);

    // Validate command-line arguments
    if (argc < 2) {
        print_usage(argv[0]);
        return -1;
    }

    // Set up signal handlers for graceful shutdown
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // Create recorder manager
    manager = manager_create();
    set_global_manager(manager);

    // Parse configuration file
    if (config_parse_file(argv[1], manager) < 0) {
        g_printerr("Failed to parse config file: %s\n", argv[1]);
        manager_destroy(manager);
        return -1;
    }

    // Validate at least one camera configured
    if (manager->num_cameras == 0) {
        g_printerr("No cameras configured\n");
        manager_destroy(manager);
        return -1;
    }

    // Start all cameras
    manager_start_all(manager);

    g_print("Recording %d cameras... Press Ctrl+C to stop\n\n", manager->num_cameras);
    
    // Wait for all recording threads to complete
    for (int i = 0; i < manager->num_cameras; i++) {
        if (manager->cameras[i] && manager->cameras[i]->is_recording) {
            pthread_join(manager->cameras[i]->thread, NULL);
        }
    }

    // Clean up
    manager_destroy(manager);
    set_global_manager(NULL);

    g_print("\nAll recordings completed\n");
    return 0;
}
