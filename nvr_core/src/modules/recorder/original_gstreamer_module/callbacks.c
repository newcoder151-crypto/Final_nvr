/**
 * @file callbacks.c
 * @brief GStreamer callback implementations for NVR recorder
 * 
 * Implements callback functions for GStreamer pipeline events including
 * dynamic pad linking, bus message handling, and segment filename generation.
 * These callbacks handle runtime pipeline behavior and error conditions.
 */

#include "callbacks.h"
#include "recorder.h"
#include <time.h>

/**
 * @brief Callback for dynamic pad creation on rtspsrc
 * 
 * Handles rtspsrc's "pad-added" signal when a new RTP stream pad is created.
 * Filters for RTP H.264 streams and links to depayloader.
 * 
 * Process:
 * 1. Get capabilities of new pad
 * 2. Check if it's an RTP stream (application/x-rtp)
 * 3. Verify depayloader sink pad is not already linked
 * 4. Attempt pad linking
 * 
 * @param[in] element Source element (rtspsrc) - unused but required by signature
 * @param[in] pad Newly created GStreamer pad to potentially link
 * @param[in] data User data containing depayloader element pointer
 * 
 * Called by: GStreamer when rtspsrc creates new pad after stream detection
 * Connected in: recorder_start() via g_signal_connect()
 */
void on_pad_added(GstElement *element, GstPad *pad, gpointer data) {
    (void)element;  // Unused parameter
    GstPad *sink_pad = NULL;
    GstElement *depay = GST_ELEMENT(data);
    GstPadLinkReturn ret;
    GstCaps *new_pad_caps = NULL;
    GstStructure *new_pad_struct = NULL;
    const gchar *new_pad_type = NULL;
    const gchar *media_type = NULL;

    // Get capabilities of new pad
    new_pad_caps = gst_pad_get_current_caps(pad);
    if (!new_pad_caps) {
        new_pad_caps = gst_pad_query_caps(pad, NULL);
    }
    if (!new_pad_caps) goto exit;

    new_pad_struct = gst_caps_get_structure(new_pad_caps, 0);
    new_pad_type = gst_structure_get_name(new_pad_struct);

    // Only link RTP pads (ignore RTCP)
    if (!g_str_has_prefix(new_pad_type, "application/x-rtp")) {
        goto exit;
    }

    // Only link video pads - skip audio and other media types
    media_type = gst_structure_get_string(new_pad_struct, "media");
    if (!media_type || g_strcmp0(media_type, "video") != 0) {
        goto exit;
    }

    // Get sink pad from depayloader
    sink_pad = gst_element_get_static_pad(depay, "sink");
    if (gst_pad_is_linked(sink_pad)) {
        goto exit;
    }

    // Attempt to link pads
    ret = gst_pad_link(pad, sink_pad);
    if (GST_PAD_LINK_FAILED(ret)) {
        g_print("Pad link failed (ret=%d)\n", ret);
    }

exit:
    if (new_pad_caps != NULL)
        gst_caps_unref(new_pad_caps);
    if (sink_pad != NULL)
        gst_object_unref(sink_pad);
}

/**
 * @brief GStreamer bus message callback
 * 
 * Processes messages from GStreamer pipeline bus including:
 * - EOS: End of stream, triggers main loop exit
 * - ERROR: Logs error details and triggers shutdown
 * - STATE_CHANGED: Logs pipeline state transitions for debugging
 * 
 * Error messages include both human-readable text and debug information
 * from GStreamer for troubleshooting pipeline issues.
 * 
 * @param[in] bus GStreamer bus object - unused but required by signature
 * @param[in] msg GStreamer message to process
 * @param[in] data User data containing CameraRecorder pointer for context
 * @return TRUE to continue receiving messages, FALSE to stop
 * 
 * Called by: GStreamer bus watch mechanism
 * Connected in: recorder_start() via gst_bus_add_watch()
 */
gboolean bus_call(GstBus *bus, GstMessage *msg, gpointer data) {
    (void)bus;  // Unused parameter
    CameraRecorder *rec = (CameraRecorder *)data;

    switch (GST_MESSAGE_TYPE(msg)) {
        case GST_MESSAGE_EOS:
            g_print("[%s] End of stream\n", rec->camera_name);
            if (rec->loop)
                g_main_loop_quit(rec->loop);
            break;

        case GST_MESSAGE_ERROR: {
            gchar *debug;
            GError *error;
            gst_message_parse_error(msg, &error, &debug);
            g_printerr("[%s] ERROR: %s\n", rec->camera_name, error->message);
            if (debug)
                g_printerr("[%s] Debug: %s\n", rec->camera_name, debug);
            g_free(debug);
            g_error_free(error);
            if (rec->loop)
                g_main_loop_quit(rec->loop);
            break;
        }

        case GST_MESSAGE_STATE_CHANGED: {
            // Only log state changes for the pipeline itself, not sub-elements
            if (GST_MESSAGE_SRC(msg) == GST_OBJECT(rec->pipeline)) {
                GstState old_state, new_state;
                gst_message_parse_state_changed(msg, &old_state, &new_state, NULL);
                g_print("[%s] State: %s -> %s\n",
                       rec->camera_name,
                       gst_element_state_get_name(old_state),
                       gst_element_state_get_name(new_state));
            }
            break;
        }

        default:
            break;
    }
    return TRUE;
}

/**
 * @brief Format location callback for splitmuxsink
 * 
 * Generates filename for each segment created by splitmuxsink.
 * Creates descriptive filenames with optional timestamps and
 * sequential segment numbering.
 * 
 * Filename patterns:
 * - With timestamp: {prefix}_{YYYYMMDD}_{HHMMSS}_seg{NNNNN}.{ext}
 * - Without timestamp: {prefix}_seg{NNNNN}.{ext}
 * 
 * Process:
 * 1. Get current local time
 * 2. Format filename based on timestamp setting
 * 3. Strip any trailing whitespace
 * 4. Log segment creation
 * 5. Return allocated filename string
 * 
 * @param[in] splitmux Splitmuxsink element - unused but required by signature
 * @param[in] fragment_id Sequential segment number (0-based)
 * @param[in] user_data CameraRecorder pointer containing output file config
 * @return Newly allocated filename string (caller must free with g_free)
 * 
 * Called by: GStreamer splitmuxsink when starting new segment
 * Connected in: recorder_start() via g_signal_connect()
 */
gchar* format_location_callback(GstElement *splitmux, guint fragment_id, gpointer user_data) {
    (void)splitmux;  // Unused parameter
    CameraRecorder *rec = (CameraRecorder *)user_data;
    gchar *location;
    
    // Get current time for timestamp
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    
    // Format filename based on timestamp setting
    if (rec->config.add_timestamp) {
        location = g_strdup_printf("%s_%04d%02d%02d_%02d%02d%02d_seg%05d.%s",
                                   rec->output_file,
                                   t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
                                   t->tm_hour, t->tm_min, t->tm_sec,
                                   fragment_id,
                                   rec->config.file_extension);
    } else {
        location = g_strdup_printf("%s_seg%05d.%s",
                                   rec->output_file,
                                   fragment_id,
                                   rec->config.file_extension);
    }
    
    // Clean up any whitespace issues
    g_strstrip(location);
    
    g_print("[%s] Starting segment %d: %s\n", rec->camera_name, fragment_id, location);
    
    return location;
}
