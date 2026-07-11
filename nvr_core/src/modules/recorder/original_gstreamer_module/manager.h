/**
 * @file manager.h
 * @brief Multi-camera recorder management
 * 
 * Provides centralized management of multiple camera recorders with
 * thread-safe operations for adding cameras and controlling recording
 * sessions across all cameras simultaneously.
 */

#ifndef MANAGER_H
#define MANAGER_H

#include <pthread.h>
#include "recorder.h"

/**
 * @brief Multi-camera recorder manager structure
 * 
 * Manages array of camera recorders with thread-safe operations.
 * Provides centralized control for starting/stopping all cameras.
 */
typedef struct RecorderManager {
    /** @brief Array of camera recorder pointers */
    CameraRecorder *cameras[MAX_CAMERAS];
    
    /** @brief Current number of configured cameras */
    int num_cameras;
    
    /** @brief Mutex for thread-safe camera operations */
    pthread_mutex_t mutex;
} RecorderManager;

/**
 * @brief Create a new recorder manager instance
 * 
 * Allocates and initializes RecorderManager with empty camera array
 * and initialized mutex for thread safety.
 * 
 * @return Pointer to newly created RecorderManager, NULL on failure
 * 
 * Called from: main()
 */
RecorderManager* manager_create(void);

/**
 * @brief Add a camera to the manager
 * 
 * Thread-safe operation to add a new camera recorder to the manager.
 * Creates CameraRecorder instance and adds to internal array.
 * Does not start recording - call manager_start_all() to begin.
 * 
 * @param[in,out] mgr Pointer to RecorderManager
 * @param[in] name Camera name for identification
 * @param[in] rtsp_url RTSP stream URL
 * @param[in] output_file Output file path prefix
 * @return Camera ID (array index) on success, -1 on failure
 * 
 * Called from: config_parse_file()
 */
int manager_add_camera(RecorderManager *mgr, const char *name, const char *rtsp_url, const char *output_file);

/**
 * @brief Start recording on all configured cameras
 * 
 * Iterates through all cameras and starts recording with small delay
 * between each to prevent resource contention during startup.
 * 
 * @param[in,out] mgr Pointer to RecorderManager
 * 
 * Called from: main()
 */
void manager_start_all(RecorderManager *mgr);

/**
 * @brief Stop recording on all cameras
 * 
 * Gracefully stops all active recordings. Waits for each camera
 * to finish current segment and clean up resources.
 * 
 * @param[in,out] mgr Pointer to RecorderManager
 * 
 * Called from: signal_handler(), main(), manager_destroy()
 */
void manager_stop_all(RecorderManager *mgr);

/**
 * @brief Destroy manager and all cameras
 * 
 * Stops all recordings, destroys all camera recorders, cleans up
 * mutex, and frees manager memory. Manager pointer is invalid after this.
 * 
 * @param[in,out] mgr Pointer to RecorderManager to destroy
 * 
 * Called from: main()
 */
void manager_destroy(RecorderManager *mgr);

#endif /* MANAGER_H */
