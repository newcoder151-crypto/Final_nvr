/**
 * @file manager.c
 * @brief Multi-camera recorder manager implementation
 * 
 * Implements centralized management of multiple camera recorders with
 * thread-safe operations. Provides lifecycle management for adding,
 * starting, stopping, and destroying multiple camera instances.
 */

#include "manager.h"
#include <unistd.h>

/**
 * @brief Create a new recorder manager instance
 * 
 * Allocates and initializes RecorderManager with empty camera array
 * and thread-safe mutex. Ready to accept camera additions via
 * manager_add_camera().
 * 
 * Initialization:
 * - Zero-initialized structure (all cameras NULL)
 * - Camera count set to 0
 * - Mutex initialized for thread-safe operations
 * 
 * @return Pointer to newly created RecorderManager, never NULL
 * 
 * Called from: main()
 */
RecorderManager* manager_create(void) {
    RecorderManager *mgr = g_malloc0(sizeof(RecorderManager));
    mgr->num_cameras = 0;
    pthread_mutex_init(&mgr->mutex, NULL);
    return mgr;
}

/**
 * @brief Add a camera to the manager
 * 
 * Thread-safe operation to create a new camera recorder and add it
 * to the manager's internal array. Validates capacity and creates
 * recorder with provided parameters.
 * 
 * Process:
 * 1. Validate manager and check capacity (MAX_CAMERAS)
 * 2. Acquire mutex lock for thread safety
 * 3. Create new CameraRecorder at next available index
 * 4. Increment camera count
 * 5. Release mutex lock
 * 6. Log addition
 * 
 * The created camera is not started - call manager_start_all()
 * to begin recording. Configuration must be set separately via
 * the recorder's config field.
 * 
 * @param[in,out] mgr Pointer to RecorderManager
 * @param[in] name Human-readable camera name for logging
 * @param[in] rtsp_url RTSP stream URL
 * @param[in] output_file Output file path prefix (without extension)
 * @return Camera ID (array index 0-15) on success, -1 on failure
 * 
 * Called from: config_parse_file() for each configured camera
 */
int manager_add_camera(RecorderManager *mgr, const char *name, const char *rtsp_url, const char *output_file) {
    if (!mgr || mgr->num_cameras >= MAX_CAMERAS) {
        return -1;
    }

    pthread_mutex_lock(&mgr->mutex);
    
    int id = mgr->num_cameras;
    mgr->cameras[id] = recorder_create(id, name, rtsp_url, output_file);
    mgr->num_cameras++;
    
    pthread_mutex_unlock(&mgr->mutex);
    
    g_print("Added Camera %d (%s)\n", id, name);
    return id;
}

/**
 * @brief Start recording on all configured cameras
 * 
 * Iterates through all cameras in the manager and starts each one
 * with a small delay between starts to prevent resource contention
 * during pipeline initialization.
 * 
 * Process:
 * 1. Log start operation
 * 2. For each camera in array:
 *    - Call recorder_start()
 *    - Sleep 100ms before next camera
 * 3. Log completion
 * 
 * The 100ms delay helps prevent simultaneous RTSP connection
 * attempts and GStreamer resource allocation conflicts.
 * 
 * Failed camera starts are logged but don't stop other cameras
 * from starting.
 * 
 * @param[in,out] mgr Pointer to RecorderManager
 * 
 * Called from: main() after configuration loaded
 */
void manager_start_all(RecorderManager *mgr) {
    if (!mgr) return;

    g_print("\n=== Starting %d cameras ===\n\n", mgr->num_cameras);
    
    for (int i = 0; i < mgr->num_cameras; i++) {
        if (mgr->cameras[i]) {
            recorder_start(mgr->cameras[i]);
            usleep(100000);  // 100ms delay between camera starts
        }
    }
    
    g_print("\n=== All cameras started ===\n\n");
}

/**
 * @brief Stop recording on all cameras
 * 
 * Gracefully stops all active recordings. Waits for each camera
 * to complete its current segment and properly close files.
 * 
 * Process:
 * 1. Log stop operation
 * 2. For each camera in array:
 *    - Call recorder_stop()
 *    - Wait for pipeline shutdown
 * 3. Log completion
 * 
 * Each camera's recorder_stop() handles:
 * - EOS event sending
 * - Thread joining
 * - Pipeline cleanup
 * - File closure
 * 
 * This is a blocking operation that waits for all cameras
 * to completely stop before returning.
 * 
 * @param[in,out] mgr Pointer to RecorderManager
 * 
 * Called from: signal_handler() (Ctrl+C), main() (normal exit), manager_destroy()
 */
void manager_stop_all(RecorderManager *mgr) {
    if (!mgr) return;

    g_print("\n=== Stopping %d cameras ===\n\n", mgr->num_cameras);
    
    for (int i = 0; i < mgr->num_cameras; i++) {
        if (mgr->cameras[i]) {
            recorder_stop(mgr->cameras[i]);
        }
    }
    
    g_print("\n=== All cameras stopped ===\n");
}

/**
 * @brief Destroy manager and all cameras
 * 
 * Complete cleanup of manager and all associated resources.
 * Stops all recordings, destroys all cameras, cleans up mutex,
 * and frees manager memory.
 * 
 * Cleanup sequence:
 * 1. Stop all active recordings
 * 2. Destroy each camera recorder
 * 3. Destroy mutex
 * 4. Free manager structure
 * 
 * After this call, the manager pointer is invalid and must
 * not be used. All camera pointers are also invalid.
 * 
 * @param[in,out] mgr Pointer to RecorderManager to destroy (invalidated after call)
 * 
 * Called from: main() before program exit
 */
void manager_destroy(RecorderManager *mgr) {
    if (!mgr) return;

    // Stop all recordings first
    manager_stop_all(mgr);
    
    // Destroy all camera recorders
    for (int i = 0; i < mgr->num_cameras; i++) {
        if (mgr->cameras[i]) {
            recorder_destroy(mgr->cameras[i]);
        }
    }
    
    // Cleanup mutex and manager
    pthread_mutex_destroy(&mgr->mutex);
    g_free(mgr);
}
