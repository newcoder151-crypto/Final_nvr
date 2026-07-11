/**
 * @file callbacks.h
 * @brief GStreamer pipeline callbacks for NVR recorder
 * 
 * Defines callback functions for GStreamer events including pad linking,
 * bus messages, and file segmentation. Handles runtime pipeline events
 * and error conditions.
 */

#ifndef CALLBACKS_H
#define CALLBACKS_H

#include <gst/gst.h>

/**
 * @brief Callback for dynamic pad creation on rtspsrc
 * 
 * Called when rtspsrc element creates a new pad for RTP stream.
 * Links the dynamic RTP pad to the depayloader's static sink pad.
 * Only links application/x-rtp pads, ignoring RTCP pads.
 * 
 * @param[in] element Source element (rtspsrc) creating the pad
 * @param[in] pad Newly created GStreamer pad
 * @param[in] data User data (GstElement* to depayloader)
 * 
 * Called by: GStreamer when rtspsrc detects stream and creates pad
 * Connected in: recorder_start() via g_signal_connect()
 */
void on_pad_added(GstElement *element, GstPad *pad, gpointer data);

/**
 * @brief GStreamer bus message callback
 * 
 * Handles pipeline bus messages including:
 * - EOS (End of Stream): Quits main loop
 * - ERROR: Logs error details and quits loop
 * - STATE_CHANGED: Logs state transitions for debugging
 * 
 * @param[in] bus GStreamer bus object
 * @param[in] msg GStreamer message received
 * @param[in] data User data (CameraRecorder* for context)
 * @return TRUE to continue receiving messages
 * 
 * Called by: GStreamer bus watch
 * Connected in: recorder_start() via gst_bus_add_watch()
 */
gboolean bus_call(GstBus *bus, GstMessage *msg, gpointer data);

/**
 * @brief Format location callback for splitmuxsink
 * 
 * Called by splitmuxsink to generate filename for each segment.
 * Creates filenames with optional timestamps and sequential numbering.
 * 
 * Filename format (with timestamp):
 *   {prefix}_{YYYYMMDD}_{HHMMSS}_seg{NNNNN}.{ext}
 * 
 * Filename format (without timestamp):
 *   {prefix}_seg{NNNNN}.{ext}
 * 
 * @param[in] splitmux Splitmuxsink element requesting filename
 * @param[in] fragment_id Sequential fragment/segment number
 * @param[in] user_data User data (CameraRecorder* for context)
 * @return Newly allocated filename string (caller must free with g_free)
 * 
 * Called by: GStreamer splitmuxsink when starting new segment
 * Connected in: recorder_start() via g_signal_connect()
 */
gchar* format_location_callback(GstElement *splitmux, guint fragment_id, gpointer user_data);

#endif /* CALLBACKS_H */
