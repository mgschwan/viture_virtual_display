#ifndef XDG_SOURCE_H
#define XDG_SOURCE_H

#include <glib.h> // For gboolean

// Define the structure that holds the frame data.
// This is the same structure defined in xdg_source.c.
// If it were more complex or private, xdg_source.c might expose an opaque pointer.
typedef struct {
    unsigned char *data;    // RGB or RGBA pixel data
    int width;
    int height;
    int stride;             // Bytes per row in the data buffer
    // Internal fields used by xdg_source.c, not typically for external use directly
    // but part of the struct returned by get_xdg_root_window_frame_sync()
    gboolean completed; 
    gboolean success;
    GMainLoop *loop_for_sync_call; 
} XDGFrameRequest; // Consider renaming to XDGFrame for public API clarity if preferred

/**
 * @brief Captures the current root window (entire screen) using XDG ScreenCast Portal.
 * 
 * This function operates by establishing a ScreenCast session with the XDG Desktop Portal.
 * It uses the org.freedesktop.portal.ScreenCast interface to create a streaming session
 * that provides real-time screen content via PipeWire. The function initializes the
 * screencast session on first call and reuses it for subsequent calls, providing
 * continuous frame data without user interaction.
 * 
 * The caller is responsible for calling free_xdg_frame_request() on the returned pointer
 * when the frame data is no longer needed. Call cleanup_screencast_session() when
 * the application is shutting down to properly clean up the screencast session.
 * 
 * @return XDGFrameRequest* A pointer to an XDGFrameRequest structure containing the
 *         pixel data and metadata if successful, or NULL on failure (e.g., portal error,
 *         timeout, PipeWire connection error).
 *         If non-NULL, frame_request->data will point to the allocated pixel buffer.
 *         frame_request->success will be TRUE if data was populated.
 */
XDGFrameRequest* get_xdg_root_window_frame_sync(void);

/**
 * @brief Frees the resources associated with an XDGFrameRequest structure.
 * 
 * This includes the pixel data buffer (frame_req->data) and the structure itself.
 * Should be called on any XDGFrameRequest pointer obtained from 
 * get_xdg_root_window_frame_sync(), whether it was successful or not (if non-NULL).
 * Note: This function will not free the global screencast session.
 * 
 * @param frame_req Pointer to the XDGFrameRequest structure to free.
 */
void free_xdg_frame_request(XDGFrameRequest* frame_req);

/**
 * @brief Cleans up the global screencast session and associated resources.
 * 
 * This function should be called when the application is shutting down to properly
 * clean up the screencast session, close PipeWire connections, and free all
 * associated resources. After calling this function, subsequent calls to
 * get_xdg_root_window_frame_sync() will reinitialize the screencast session.
 */
void cleanup_screencast_session(void);
XDGFrameRequest* init_screencast_session(int cursor_mode);

#endif // XDG_SOURCE_H
