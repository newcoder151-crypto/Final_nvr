/**
 * @file utils.h
 * @brief Utility functions for NVR recorder
 * 
 * Provides helper functions for directory management and signal handling
 * used across the NVR recorder system.
 */

#ifndef UTILS_H
#define UTILS_H

#include "manager.h"

/**
 * @brief Ensure output directory exists for recording files
 * 
 * Extracts directory path from output file prefix and creates
 * directory hierarchy if it doesn't exist. Handles paths with
 * multiple levels (e.g., "recording/camera1/stream").
 * 
 * Uses g_mkdir_with_parents() for recursive directory creation.
 * Trims whitespace from paths to handle configuration file issues.
 * 
 * @param[in] output_file_prefix Output file path prefix (may include directory)
 * 
 * Called from: config_parse_file() after reading output_file parameter
 */
void ensure_output_dir_exists(const char *output_file_prefix);

/**
 * @brief Signal handler for graceful shutdown
 * 
 * Handles SIGINT (Ctrl+C) and SIGTERM signals to gracefully stop
 * all recordings and clean up resources before exit.
 * 
 * Calls manager_stop_all() on global manager instance to ensure
 * all files are properly closed and pipelines cleaned up.
 * 
 * @param[in] signum Signal number received (SIGINT=2, SIGTERM=15)
 * 
 * Called by: OS signal mechanism
 * Registered in: main() via signal()
 */
void signal_handler(int signum);

/**
 * @brief Set global manager for signal handler access
 * 
 * Sets the global manager pointer used by signal_handler().
 * Required because signal handlers cannot receive context data.
 * 
 * @param[in] mgr Pointer to RecorderManager instance
 * 
 * Called from: main() after creating manager
 */
void set_global_manager(RecorderManager *mgr);

#endif /* UTILS_H */
