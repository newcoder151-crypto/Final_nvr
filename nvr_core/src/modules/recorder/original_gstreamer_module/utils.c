/**
 * @file utils.c
 * @brief Utility function implementations for NVR recorder
 * 
 * Implements helper functions including directory creation for output files
 * and signal handling for graceful shutdown. Provides shared functionality
 * used across the NVR recorder system.
 */

#include "utils.h"
#include <signal.h>
#include <glib.h>

/** @brief Global manager instance for signal handler access */
static RecorderManager *g_manager = NULL;

/**
 * @brief Ensure output directory exists for recording files
 * 
 * Extracts the directory portion from an output file path and creates
 * the full directory hierarchy if it doesn't exist. Handles:
 * - Multi-level paths (e.g., "recording/camera1/stream")
 * - Whitespace in configuration (trims automatically)
 * - Current directory references (skips creation for ".")
 * 
 * Uses GLib's g_mkdir_with_parents() which creates all intermediate
 * directories with mode 0755 (rwxr-xr-x).
 * 
 * Examples:
 * - "recording/cam1/file" -> creates "recording/cam1/"
 * - "file" -> no directory created (current dir)
 * - " /path/to/file " -> creates "/path/to/" (after trimming)
 * 
 * @param[in] output_file_prefix Output file path prefix (may include directory)
 * 
 * Called from: config_parse_file() after reading output_file parameter
 */
void ensure_output_dir_exists(const char *output_file_prefix)
{
    if (!output_file_prefix || !*output_file_prefix) return;

    // Create trimmed copy to handle whitespace in config files
    gchar *tmp = g_strdup(output_file_prefix);
    g_strstrip(tmp);

    // Extract directory portion: "recording/folder1" from "recording/folder1/file"
    gchar *dir = g_path_get_dirname(tmp);

    // Skip creation for current directory references
    if (dir && g_strcmp0(dir, ".") != 0 && g_strcmp0(dir, "/") != 0) {
        if (g_mkdir_with_parents(dir, 0755) != 0) {
            g_printerr("Failed to create output directory '%s' for output_file='%s'\n",
                       dir, tmp);
        } else {
            g_print("Ensured output directory exists: %s\n", dir);
        }
    }

    g_free(dir);
    g_free(tmp);
}

/**
 * @brief Signal handler for graceful shutdown
 * 
 * Handles SIGINT (Ctrl+C) and SIGTERM signals to perform graceful
 * shutdown of all recordings. Ensures all files are properly closed
 * and pipelines cleaned up before process termination.
 * 
 * Process:
 * 1. Log signal receipt
 * 2. Call manager_stop_all() on global manager
 * 3. Allow normal program exit
 * 
 * Note: Uses global manager instance because signal handlers cannot
 * receive user data. Set via set_global_manager() in main().
 * 
 * Handled signals:
 * - SIGINT (2): Ctrl+C from terminal
 * - SIGTERM (15): Termination request from OS/service manager
 * 
 * @param[in] signum Signal number received
 * 
 * Called by: OS signal mechanism when signal received
 * Registered in: main() via signal()
 */
void signal_handler(int signum) {
    g_print("\nReceived signal %d, stopping all recordings...\n", signum);
    if (g_manager) {
        manager_stop_all(g_manager);
    }
}

/**
 * @brief Set global manager for signal handler access
 * 
 * Sets the global manager pointer used by signal_handler().
 * Required because signal handlers cannot receive context data
 * through normal function parameters.
 * 
 * Should be called once after creating the manager in main()
 * and before registering signal handlers.
 * 
 * @param[in] mgr Pointer to RecorderManager instance
 * 
 * Called from: main() after manager_create()
 */
void set_global_manager(RecorderManager *mgr) {
    g_manager = mgr;
}
