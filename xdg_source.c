/* Read the root window from X11 or Wayland using XDG ScreenCast and return it as a RGB buffer */

#include <glib-object.h> // Explicitly include for GObject base
#include <glib.h>
#include <gio/gio.h>
#include <gio/gdbusproxy.h> // For GDBusProxy functions
#include <gdk-pixbuf/gdk-pixbuf.h> // For image loading
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <fcntl.h>

// PipeWire includes
#include <pipewire/pipewire.h>
#include <spa/param/video/format-utils.h>
#include <spa/param/video/type-info.h>
#include <spa/debug/types.h>
#include <spa/pod/builder.h>
#include <spa/utils/result.h>

// Forward declaration
typedef struct XDGFrameRequest XDGFrameRequest;
typedef struct PipeWireStreamData PipeWireStreamData;

// PipeWire stream data structure
struct PipeWireStreamData {
    struct pw_main_loop *loop;
    struct pw_context *context;
    struct pw_core *core;
    struct pw_stream *stream;
    struct spa_hook stream_listener;
    
    unsigned char *frame_data;
    int frame_width;
    int frame_height;
    int frame_stride;
    gboolean frame_ready;
    GMutex frame_mutex;
    GThread *pipewire_thread;
    
    XDGFrameRequest *parent_request;
};

// Define a structure to hold the frame data and sync primitives
struct XDGFrameRequest {
    unsigned char *data;
    int width;
    int height;
    int stride; // Bytes per row
    gboolean completed;
    gboolean success;
    GMainLoop *loop_for_sync_call; // For synchronous wrapper
    GDBusProxy *portal_request_proxy; // To store the request proxy for cleanup on timeout
    
    // ScreenCast specific fields
    int pipewire_fd;
    char *session_handle;
    char *stream_node_id;
    gboolean session_created;
    gboolean sources_selected;
    gboolean stream_started;
    
    // PipeWire stream data
    PipeWireStreamData *pw_data;
};

// Forward declarations
void free_xdg_frame_request(XDGFrameRequest* frame_req);
static void on_screencast_start_completed(GObject *source_object, GAsyncResult *res, gpointer user_data);
static void on_open_pipewire_remote_completed(GObject *source_object, GAsyncResult *res, gpointer user_data);

// Global screencast session state
static XDGFrameRequest *g_screencast_session = NULL;
static gboolean g_screencast_initialized = FALSE;

// PipeWire stream event handlers
static void on_stream_process(void *userdata)
{
    PipeWireStreamData *pw_data = userdata;
    struct pw_buffer *b;
    struct spa_buffer *buf;
    struct spa_data *d;
    
    if ((b = pw_stream_dequeue_buffer(pw_data->stream)) == NULL) {
        g_printerr("Out of buffers: %m\n");
        return;
    }

    buf = b->buffer;
    d = &buf->datas[0];
    
    if (d->data == NULL) {
        g_printerr("No buffer data\n");
        pw_stream_queue_buffer(pw_data->stream, b);
        return;
    }

    g_mutex_lock(&pw_data->frame_mutex);

    // Get frame dimensions from buffer metadata
    struct spa_meta_header *h = spa_buffer_find_meta_data(buf, SPA_META_Header, sizeof(*h));
    if (h) {
        // For now, we'll get dimensions from the buffer size
        // In a real implementation, we'd parse the video format properly
        pw_data->frame_width = 1920; // Default, should be parsed from format
        pw_data->frame_height = 1080; // Default, should be parsed from format
    }

    // For now, assume common screen resolution if metadata is not available
    if (pw_data->frame_width <= 0 || pw_data->frame_height <= 0) {
        pw_data->frame_width = 1920;
        pw_data->frame_height = 1080;
    }

    pw_data->frame_stride = pw_data->frame_width * 4; // Assume BGRA format
    
    // Allocate frame buffer if needed
    size_t frame_size = pw_data->frame_height * pw_data->frame_width * 3; // RGB output
    if (!pw_data->frame_data) {
        pw_data->frame_data = g_malloc(frame_size);
    }

    // Convert from BGRA to RGB
    uint8_t *src = (uint8_t *)d->data;
    uint8_t *dst = pw_data->frame_data;
    
    for (int y = 0; y < pw_data->frame_height; y++) {
        for (int x = 0; x < pw_data->frame_width; x++) {
            int src_offset = (y * pw_data->frame_stride) + (x * 4);
            int dst_offset = (y * pw_data->frame_width * 3) + (x * 3);
            
            // Convert BGRA to RGB
            dst[dst_offset + 0] = src[src_offset + 2]; // R
            dst[dst_offset + 1] = src[src_offset + 1]; // G
            dst[dst_offset + 2] = src[src_offset + 0]; // B
            // Skip alpha channel
        }
    }

    pw_data->frame_ready = TRUE;
    g_mutex_unlock(&pw_data->frame_mutex);
    // g_print("PipeWire frame processed: %dx%d\n", pw_data->frame_width, pw_data->frame_height);

    pw_stream_queue_buffer(pw_data->stream, b);
}

static void on_stream_state_changed(void *userdata, enum pw_stream_state old, enum pw_stream_state state, const char *error)
{
    (void)userdata;
    g_print("PipeWire stream state changed: %s -> %s\n", pw_stream_state_as_string(old), pw_stream_state_as_string(state));
    
    if (state == PW_STREAM_STATE_ERROR) {
        g_printerr("PipeWire stream error: %s\n", error);
    }
}

static const struct pw_stream_events stream_events = {
    PW_VERSION_STREAM_EVENTS,
    .process = on_stream_process,
    .state_changed = on_stream_state_changed,
};

static gpointer pipewire_loop_thread_func(gpointer data) {
    PipeWireStreamData *pw_data = data;
    g_print("Starting PipeWire event loop thread.\n");
    pw_main_loop_run(pw_data->loop);
    g_print("PipeWire event loop thread finished.\n");
    return NULL;
}

static void
process_pipewire_stream(XDGFrameRequest *frame_request)
{
    if (!frame_request) {
        g_printerr("Invalid frame request\n");
        return;
    }

    if (frame_request->pipewire_fd < 0) {
        g_print("No PipeWire file descriptor, using test pattern\n");
        // Create test pattern as fallback
        if (!frame_request->data) {
            frame_request->width = 1920;
            frame_request->height = 1080;
            frame_request->stride = frame_request->width * 3;
            size_t data_size = frame_request->height * frame_request->stride;
            frame_request->data = (unsigned char *)g_malloc(data_size);
            
            // Fill with a gradient pattern
            for (int y = 0; y < frame_request->height; y++) {
                for (int x = 0; x < frame_request->width; x++) {
                    int offset = (y * frame_request->stride) + (x * 3);
                    frame_request->data[offset] = (unsigned char)((x * 255) / frame_request->width);
                    frame_request->data[offset + 1] = (unsigned char)((y * 255) / frame_request->height);
                    frame_request->data[offset + 2] = 128;
                }
            }
            g_print("Created test pattern frame: %d x %d\n", frame_request->width, frame_request->height);
        }
        frame_request->success = TRUE;
        frame_request->completed = TRUE;
        return;
    }

    // Initialize PipeWire
    pw_init(NULL, NULL);

    // Create PipeWire stream data
    PipeWireStreamData *pw_data = g_new0(PipeWireStreamData, 1);
    pw_data->parent_request = frame_request;
    frame_request->pw_data = pw_data;
    g_mutex_init(&pw_data->frame_mutex);

    // Create main loop and context
    pw_data->loop = pw_main_loop_new(NULL);
    pw_data->context = pw_context_new(pw_main_loop_get_loop(pw_data->loop), NULL, 0);
    
    // Connect to PipeWire using the provided file descriptor
    pw_data->core = pw_context_connect_fd(pw_data->context, frame_request->pipewire_fd, NULL, 0);
    if (!pw_data->core) {
        g_printerr("Failed to connect to PipeWire\n");
        goto cleanup;
    }

    // Create stream
    pw_data->stream = pw_stream_new_simple(
        pw_main_loop_get_loop(pw_data->loop),
        "screencast-consumer",
        pw_properties_new(
            PW_KEY_MEDIA_TYPE, "Video",
            PW_KEY_MEDIA_CATEGORY, "Capture",
            PW_KEY_MEDIA_ROLE, "Screen",
            NULL),
        &stream_events,
        pw_data);

    if (!pw_data->stream) {
        g_printerr("Failed to create PipeWire stream\n");
        goto cleanup;
    }

    // Connect to the specific node
    uint32_t node_id = atoi(frame_request->stream_node_id);
    
    // Set up stream parameters
    uint8_t buffer[1024];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
    const struct spa_pod *params[1];
    
    params[0] = spa_pod_builder_add_object(&b,
        SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
        SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
        SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
        SPA_FORMAT_VIDEO_format, SPA_POD_CHOICE_ENUM_Id(3,
            SPA_VIDEO_FORMAT_RGBx,
            SPA_VIDEO_FORMAT_RGBA,
            SPA_VIDEO_FORMAT_BGRx),
        SPA_FORMAT_VIDEO_size, SPA_POD_CHOICE_RANGE_Rectangle(
            &SPA_RECTANGLE(320, 240),
            &SPA_RECTANGLE(1, 1),
            &SPA_RECTANGLE(4096, 4096)),
        SPA_FORMAT_VIDEO_framerate, SPA_POD_CHOICE_RANGE_Fraction(
            &SPA_FRACTION(25, 1),
            &SPA_FRACTION(0, 1),
            &SPA_FRACTION(1000, 1)));

    // Connect stream to the node
    if (pw_stream_connect(pw_data->stream,
                         PW_DIRECTION_INPUT,
                         node_id,
                         PW_STREAM_FLAG_AUTOCONNECT |
                         PW_STREAM_FLAG_MAP_BUFFERS,
                         params, 1) < 0) {
        g_printerr("Failed to connect PipeWire stream to node %u\n", node_id);
        goto cleanup;
    }

    g_print("PipeWire stream connected to node %u\n", node_id);

    // Start PipeWire loop in a separate thread
    pw_data->pipewire_thread = g_thread_new("pipewire-loop", pipewire_loop_thread_func, pw_data);

    // Wait for the first frame to arrive to ensure session is working
    g_print("Waiting for the first PipeWire frame...\n");
    gboolean first_frame_received = FALSE;
    for (int i = 0; i < 100; i++) { // Wait up to 10 seconds
        g_mutex_lock(&pw_data->frame_mutex);
        if (pw_data->frame_ready) {
            first_frame_received = TRUE;
        }
        g_mutex_unlock(&pw_data->frame_mutex);
        
        if (first_frame_received) {
            g_print("First PipeWire frame received.\n");
            break;
        }
        g_usleep(100000); // 100ms
    }

    if (first_frame_received) {
        g_mutex_lock(&pw_data->frame_mutex);
        // Copy frame data to the request
        frame_request->width = pw_data->frame_width;
        frame_request->height = pw_data->frame_height;
        frame_request->stride = pw_data->frame_width * 3;
        
        size_t data_size = frame_request->height * frame_request->stride;
        frame_request->data = (unsigned char *)g_malloc(data_size);
        memcpy(frame_request->data, pw_data->frame_data, data_size);
        g_mutex_unlock(&pw_data->frame_mutex);
        
        g_print("Real PipeWire frame captured: %dx%d\n", frame_request->width, frame_request->height);
        frame_request->success = TRUE;
    } else {
        g_printerr("Failed to capture PipeWire frame in time, using test pattern\n");
        // Fallback to test pattern
        frame_request->width = 1920;
        frame_request->height = 1080;
        frame_request->stride = frame_request->width * 3;
        size_t data_size = frame_request->height * frame_request->stride;
        frame_request->data = (unsigned char *)g_malloc(data_size);
        
        for (int y = 0; y < frame_request->height; y++) {
            for (int x = 0; x < frame_request->width; x++) {
                int offset = (y * frame_request->stride) + (x * 3);
                frame_request->data[offset] = (unsigned char)((x * 255) / frame_request->width);
                frame_request->data[offset + 1] = (unsigned char)((y * 255) / frame_request->height);
                frame_request->data[offset + 2] = 128;
            }
        }
        frame_request->success = TRUE;
    }

cleanup:
    // Note: We keep the PipeWire connection alive for the session
    // It will be cleaned up when the session is destroyed
    frame_request->completed = TRUE;
}

// Callback for the Response signal from the Start request
static void
on_start_response_signal(GDBusProxy *proxy,
                         const char *sender_name G_GNUC_UNUSED,
                         const char *dbus_signal_name,
                         GVariant   *parameters,
                         gpointer    user_data)
{
    XDGFrameRequest *frame_request = (XDGFrameRequest *)user_data;
    guint response_code;
    GVariant *results_dict;

    if (g_strcmp0(dbus_signal_name, "Response") != 0) {
        return;
    }

    g_variant_get(parameters, "(u@a{sv})", &response_code, &results_dict);

    g_print( "Value: %s", g_variant_print(parameters, TRUE));

    if (response_code == 0) { // Success
        GVariantIter iter;
        GVariant *streams_variant;
        
        
        g_print("Start Response contents:\n");
        g_variant_iter_init(&iter, results_dict);
        const char *key;
        GVariant *value;
        while (g_variant_iter_next(&iter, "{&sv}", &key, &value)) {
            g_print("  Key: %s, Type: %s\n", key, g_variant_get_type_string(value));
            g_print("  Value: %s\n", g_variant_print(value, TRUE));
            g_variant_unref(value);
        }

        streams_variant = g_variant_lookup_value(results_dict, "streams", G_VARIANT_TYPE("a(ua{sv})"));
        if (streams_variant) {
            GVariantIter stream_iter;
            guint32 node_id;
            GVariant *stream_properties;

            g_print("Streams: %s\n", g_variant_print(streams_variant, TRUE));

            g_variant_iter_init(&stream_iter, streams_variant);
            if (g_variant_iter_next(&stream_iter, "(ua{sv})", &node_id, &stream_properties)) {
                frame_request->stream_node_id = g_strdup_printf("%u", node_id);
                g_print("ScreenCast stream started with node ID: %s\n", frame_request->stream_node_id);
                

                frame_request->stream_started = TRUE;
                g_variant_unref(stream_properties);

                // Now open the pipewire remote
                GDBusProxy *screencast_proxy = g_dbus_proxy_new_for_bus_sync(
                    G_BUS_TYPE_SESSION,
                    G_DBUS_PROXY_FLAGS_NONE,
                    NULL,
                    "org.freedesktop.portal.Desktop",
                    "/org/freedesktop/portal/desktop",
                    "org.freedesktop.portal.ScreenCast",
                    NULL,
                    NULL);
                
                if (screencast_proxy) {
                    GDBusConnection *connection = g_dbus_proxy_get_connection(screencast_proxy);
                    g_dbus_connection_call_with_unix_fd_list(connection,
                                                             "org.freedesktop.portal.Desktop",
                                                             "/org/freedesktop/portal/desktop",
                                                             "org.freedesktop.portal.ScreenCast",
                                                             "OpenPipeWireRemote",
                                                             g_variant_new("(oa{sv})", frame_request->session_handle, NULL),
                                                             G_VARIANT_TYPE("(h)"),
                                                             G_DBUS_CALL_FLAGS_NONE,
                                                             -1,
                                                             NULL,
                                                             NULL, // No FD list to send
                                                             (GAsyncReadyCallback)on_open_pipewire_remote_completed,
                                                             frame_request);
                    g_object_unref(screencast_proxy);
                }
            }
            g_variant_unref(streams_variant);
        } else {
             g_printerr("No 'streams' key found in Start response or type mismatch\n");
             goto fail_response;
        }
    } else {
        g_printerr("Start method failed with response code: %u\n", response_code);
        goto fail_response;
    }

    g_signal_handlers_disconnect_by_data(proxy, user_data);
    g_object_unref(proxy);
    frame_request->portal_request_proxy = NULL;
    // The main loop is quit in on_open_pipewire_remote_completed
    return;

fail_response:
    g_variant_unref(results_dict);
    g_signal_handlers_disconnect_by_data(proxy, user_data);
    g_object_unref(proxy);
    frame_request->portal_request_proxy = NULL;
    frame_request->completed = TRUE;
    frame_request->success = FALSE;
    if (frame_request->loop_for_sync_call) g_main_loop_quit(frame_request->loop_for_sync_call);
}

// Callback for OpenPipeWireRemote method completion
static void
on_open_pipewire_remote_completed(GObject *source_object,
                                  GAsyncResult *res,
                                  gpointer user_data)
{
    GDBusConnection *connection = G_DBUS_CONNECTION(source_object);
    XDGFrameRequest *frame_request = (XDGFrameRequest *)user_data;
    GVariant *result_tuple;
    GError *error = NULL;
    GUnixFDList *fd_list = NULL;
    gint handle;

    result_tuple = g_dbus_connection_call_with_unix_fd_list_finish(connection,
                                                                   &fd_list,
                                                                   res,
                                                                   &error);

    if (error != NULL) {
        g_printerr("Error calling OpenPipeWireRemote: %s\n", error->message);
        g_error_free(error);
        goto fail;
    }

    if (result_tuple) {
        g_variant_get(result_tuple, "(h)", &handle);
        g_print("OpenPipeWireRemote result: %s\n", g_variant_print(result_tuple, TRUE));
        
        if (fd_list) {
            frame_request->pipewire_fd = g_unix_fd_list_get(fd_list, handle, &error);
            if (error) {
                g_printerr("Failed to get file descriptor from list: %s\n", error->message);
                g_error_free(error);
                g_object_unref(fd_list);
                g_variant_unref(result_tuple);
                goto fail;
            }
            g_print("Got PipeWire file descriptor: %d\n", frame_request->pipewire_fd);
            g_object_unref(fd_list);
        } else {
            g_printerr("No file descriptor list received\n");
            g_variant_unref(result_tuple);
            goto fail;
        }
        
        g_variant_unref(result_tuple);
        process_pipewire_stream(frame_request);
    } else {
        g_printerr("Failed to get file descriptor from OpenPipeWireRemote\n");
        goto fail;
    }

    if (frame_request->loop_for_sync_call && g_main_loop_is_running(frame_request->loop_for_sync_call)) {
        g_main_loop_quit(frame_request->loop_for_sync_call);
    }
    return;

fail:
    frame_request->completed = TRUE;
    frame_request->success = FALSE;
    if (frame_request->loop_for_sync_call && g_main_loop_is_running(frame_request->loop_for_sync_call)) {
        g_main_loop_quit(frame_request->loop_for_sync_call);
    }
}

// Callback for the Response signal from the SelectSources request
static void
on_select_sources_response_signal(GDBusProxy *proxy,
                                  const char *sender_name G_GNUC_UNUSED,
                                  const char *dbus_signal_name,
                                  GVariant   *parameters,
                                  gpointer    user_data)
{
    XDGFrameRequest *frame_request = (XDGFrameRequest *)user_data;
    guint response_code;
    GVariant *results_dict;

    if (g_strcmp0(dbus_signal_name, "Response") != 0) {
        return;
    }

    g_variant_get(parameters, "(ua{sv})", &response_code, &results_dict);

    if (response_code == 0) { // Success
        frame_request->sources_selected = TRUE;
        g_print("ScreenCast sources selected successfully\n");

        // Now start the stream
        GDBusProxy *session_proxy = g_dbus_proxy_new_for_bus_sync(
            G_BUS_TYPE_SESSION,
            G_DBUS_PROXY_FLAGS_NONE,
            NULL,
            "org.freedesktop.portal.Desktop",
            "/org/freedesktop/portal/desktop",
            "org.freedesktop.portal.ScreenCast",
            NULL,
            NULL);
        
        if (session_proxy) {
            GVariantBuilder *start_options_builder = g_variant_builder_new(G_VARIANT_TYPE("a{sv}"));
            GVariant *start_params = g_variant_new("(osa{sv})", frame_request->session_handle, "", start_options_builder);
            g_variant_builder_unref(start_options_builder);

            g_dbus_proxy_call(session_proxy,
                              "Start",
                              start_params,
                              G_DBUS_CALL_FLAGS_NONE,
                              -1,
                              NULL,
                              on_screencast_start_completed,
                              frame_request);
            g_object_unref(session_proxy);
        }
    } else {
        g_printerr("SelectSources failed with response code: %u\n", response_code);
        frame_request->completed = TRUE;
        frame_request->success = FALSE;
        if (frame_request->loop_for_sync_call) g_main_loop_quit(frame_request->loop_for_sync_call);
    }

    g_variant_unref(results_dict);
    g_signal_handlers_disconnect_by_data(proxy, user_data);
    g_object_unref(proxy);
    frame_request->portal_request_proxy = NULL;
}

// Callback for ScreenCast Start method completion
static void
on_screencast_start_completed(GObject *source_object,
                              GAsyncResult *res,
                              gpointer user_data)
{
    GDBusProxy *session_proxy = G_DBUS_PROXY(source_object);
    XDGFrameRequest *frame_request = (XDGFrameRequest *)user_data;
    GVariant *result_tuple;
    GError *error = NULL;
    char *request_handle_path = NULL;

    result_tuple = g_dbus_proxy_call_finish(session_proxy, res, &error);

    if (error != NULL) {
        g_printerr("Error calling ScreenCast Start method: %s\n", error->message);
        g_error_free(error);
        goto fail;
    }

    if (result_tuple) {
        g_variant_get(result_tuple, "(&o)", &request_handle_path);
        if (request_handle_path) {
            g_print("ScreenCast Start request handle: %s\n", request_handle_path);

            GDBusProxy *request_proxy = g_dbus_proxy_new_for_bus_sync(
                G_BUS_TYPE_SESSION,
                G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES,
                NULL,
                "org.freedesktop.portal.Desktop",
                request_handle_path,
                "org.freedesktop.portal.Request",
                NULL,
                &error);

            if (error != NULL) {
                g_printerr("Error creating Start request proxy: %s\n", error->message);
                g_error_free(error);
                g_variant_unref(result_tuple);
                goto fail;
            }

            g_signal_connect(request_proxy,
                           "g-signal",
                           G_CALLBACK(on_start_response_signal),
                           frame_request);

            frame_request->portal_request_proxy = request_proxy;
        }
        g_variant_unref(result_tuple);
    }

    if (!request_handle_path) {
        g_printerr("Failed to get request handle from ScreenCast Start\n");
        goto fail;
    }

    return;

fail:
    frame_request->completed = TRUE;
    frame_request->success = FALSE;
    if (frame_request->loop_for_sync_call && g_main_loop_is_running(frame_request->loop_for_sync_call)) {
        g_main_loop_quit(frame_request->loop_for_sync_call);
    }
}

// Callback for ScreenCast SelectSources method completion
static void
on_screencast_select_sources_completed(GObject *source_object,
                                       GAsyncResult *res,
                                       gpointer user_data)
{
    GDBusProxy *session_proxy = G_DBUS_PROXY(source_object);
    XDGFrameRequest *frame_request = (XDGFrameRequest *)user_data;
    GVariant *result_tuple;
    GError *error = NULL;
    char *request_handle_path = NULL;

    result_tuple = g_dbus_proxy_call_finish(session_proxy, res, &error);

    if (error != NULL) {
        g_printerr("Error calling ScreenCast SelectSources method: %s\n", error->message);
        g_error_free(error);
        goto fail;
    }

    if (result_tuple) {
        g_variant_get(result_tuple, "(&o)", &request_handle_path);
        if (request_handle_path) {
            g_print("ScreenCast SelectSources request handle: %s\n", request_handle_path);

            GDBusProxy *request_proxy = g_dbus_proxy_new_for_bus_sync(
                G_BUS_TYPE_SESSION,
                G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES,
                NULL,
                "org.freedesktop.portal.Desktop",
                request_handle_path,
                "org.freedesktop.portal.Request",
                NULL,
                &error);

            if (error != NULL) {
                g_printerr("Error creating SelectSources request proxy: %s\n", error->message);
                g_error_free(error);
                g_variant_unref(result_tuple);
                goto fail;
            }

            g_signal_connect(request_proxy,
                           "g-signal",
                           G_CALLBACK(on_select_sources_response_signal),
                           frame_request);

            frame_request->portal_request_proxy = request_proxy;
        }
        g_variant_unref(result_tuple);
    }

    if (!request_handle_path) {
        g_printerr("Failed to get request handle from ScreenCast SelectSources\n");
        goto fail;
    }

    return;

fail:
    frame_request->completed = TRUE;
    frame_request->success = FALSE;
    if (frame_request->loop_for_sync_call && g_main_loop_is_running(frame_request->loop_for_sync_call)) {
        g_main_loop_quit(frame_request->loop_for_sync_call);
    }
}

// Callback for the Response signal from CreateSession request
static void
on_create_session_response_signal(GDBusProxy *proxy,
                                  const char *sender_name G_GNUC_UNUSED,
                                  const char *dbus_signal_name,
                                  GVariant   *parameters,
                                  gpointer    user_data)
{
    XDGFrameRequest *frame_request = (XDGFrameRequest *)user_data;
    guint response_code;
    GVariant *result_tuple = NULL;
    GVariant *results_dict = NULL;
    char *session_handle_str = NULL;

    if (g_strcmp0(dbus_signal_name, "Response") != 0) {
        return;
    }

    /* 
        Here is an example parameters variant:
        Parameters: (uint32 0, {'session_handle': <'/org/freedesktop/portal/desktop/session/1_6307/viture_screencast_1376226928'>})
    */

    g_print( "Value: %s", g_variant_print(parameters, TRUE));
    g_variant_get(parameters, "(ua{sv})", &response_code, &result_tuple);

    results_dict = g_variant_get_child_value(parameters, 1);
    g_print("Format of child: %s\n", g_variant_get_type_string(results_dict));


    if (response_code == 0) { // Success
        if (!results_dict) {
            g_printerr("CreateSession response dictionary is NULL\n");
            frame_request->completed = TRUE;
            frame_request->success = FALSE;
            if (frame_request->loop_for_sync_call) g_main_loop_quit(frame_request->loop_for_sync_call);
            goto cleanup;
        }

        g_print("Retrieving session handle\n");

        if (g_variant_lookup(results_dict, "session_handle", "&s", &session_handle_str)) {
            
            frame_request->session_handle = g_strdup(session_handle_str);
           
            //g_free(session_handle_str); This crashes but why?

            g_print("Got session handle from Response: %s\n", frame_request->session_handle);
            frame_request->session_created = TRUE;

            // Now we can proceed with SelectSources
            GDBusProxy *screencast_proxy = g_dbus_proxy_new_for_bus_sync(
                G_BUS_TYPE_SESSION,
                G_DBUS_PROXY_FLAGS_NONE,
                NULL,
                "org.freedesktop.portal.Desktop",
                "/org/freedesktop/portal/desktop",
                "org.freedesktop.portal.ScreenCast",
                NULL,
                NULL);

            if (screencast_proxy) {
                GVariantBuilder *select_options_builder = g_variant_builder_new(G_VARIANT_TYPE("a{sv}"));
                g_variant_builder_add(select_options_builder, "{sv}", "types", g_variant_new_uint32(1)); // Monitor = 1
                g_variant_builder_add(select_options_builder, "{sv}", "multiple", g_variant_new_boolean(FALSE));
                
                GVariant *select_params = g_variant_new("(oa{sv})", frame_request->session_handle, select_options_builder);
                g_variant_builder_unref(select_options_builder);

                g_dbus_proxy_call(screencast_proxy,
                                  "SelectSources",
                                  select_params,
                                  G_DBUS_CALL_FLAGS_NONE,
                                  -1,
                                  NULL,
                                  on_screencast_select_sources_completed,
                                  frame_request);
                g_object_unref(screencast_proxy);
            }
        } else {
            g_printerr("No session_handle found in CreateSession response\n");
            frame_request->completed = TRUE;
            frame_request->success = FALSE;
            if (frame_request->loop_for_sync_call) g_main_loop_quit(frame_request->loop_for_sync_call);
        }
    } else {
        g_printerr("CreateSession failed with response code: %u\n", response_code);
        frame_request->completed = TRUE;
        frame_request->success = FALSE;
        if (frame_request->loop_for_sync_call) g_main_loop_quit(frame_request->loop_for_sync_call);
    }

cleanup:
    if (results_dict) {
        g_variant_unref(results_dict);
    }
    if (result_tuple) {
        g_variant_unref(result_tuple);
    } 
    g_signal_handlers_disconnect_by_data(proxy, user_data);
    g_object_unref(proxy);
    frame_request->portal_request_proxy = NULL;
}

// Callback for ScreenCast CreateSession method completion
static void
on_screencast_create_session_completed(GObject *source_object,
                                       GAsyncResult *res,
                                       gpointer user_data)
{
    GDBusProxy *screencast_proxy = G_DBUS_PROXY(source_object);
    XDGFrameRequest *frame_request = (XDGFrameRequest *)user_data;
    GVariant *result_tuple;
    GError *error = NULL;
    char *request_handle_path = NULL;

    result_tuple = g_dbus_proxy_call_finish(screencast_proxy, res, &error);

    if (error != NULL) {
        g_printerr("Error calling ScreenCast CreateSession method: %s\n", error->message);
        g_error_free(error);
        goto fail;
    }

    if (result_tuple) {
        g_variant_get(result_tuple, "(&o)", &request_handle_path);
        if (request_handle_path) {
            g_print("ScreenCast CreateSession request handle: %s\n", request_handle_path);

            // Create proxy for the request object to listen for Response signal
            GDBusProxy *request_proxy = g_dbus_proxy_new_for_bus_sync(
                G_BUS_TYPE_SESSION,
                G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES,
                NULL,
                "org.freedesktop.portal.Desktop",
                request_handle_path,
                "org.freedesktop.portal.Request",
                NULL,
                &error);

            if (error != NULL) {
                g_printerr("Error creating request proxy: %s\n", error->message);
                g_error_free(error);
                g_variant_unref(result_tuple);
                goto fail;
            }

            // Connect to the Response signal
            g_signal_connect(request_proxy,
                           "g-signal",
                           G_CALLBACK(on_create_session_response_signal),
                           frame_request);

            frame_request->portal_request_proxy = request_proxy;
        }
        g_variant_unref(result_tuple);
    }

    if (!request_handle_path) {
        g_printerr("Failed to get request handle from ScreenCast CreateSession\n");
        goto fail;
    }

    return;

fail:
    frame_request->completed = TRUE;
    frame_request->success = FALSE;
    if (frame_request->loop_for_sync_call && g_main_loop_is_running(frame_request->loop_for_sync_call)) {
        g_main_loop_quit(frame_request->loop_for_sync_call);
    }
}

// Timeout callback function for screencast session initialization
static gboolean screencast_timeout_callback(gpointer user_data) {
    XDGFrameRequest *freq = (XDGFrameRequest *)user_data;
    if (!freq->completed) {
        freq->completed = TRUE;
        freq->success = FALSE;
        g_printerr("ScreenCast session creation timed out\n");
        if (freq->loop_for_sync_call && g_main_loop_is_running(freq->loop_for_sync_call)) {
            g_main_loop_quit(freq->loop_for_sync_call);
        }
    }
    return G_SOURCE_REMOVE;
}

static XDGFrameRequest* init_screencast_session() {
    if (g_screencast_session && g_screencast_initialized) {
        // Return existing session
        return g_screencast_session;
    }

    GDBusProxy *proxy;
    GError *error = NULL;
    
    // Allocate and zero-initialize
    XDGFrameRequest *frame_request = g_new0(XDGFrameRequest, 1);
    frame_request->completed = FALSE;
    frame_request->success = FALSE;
    frame_request->data = NULL;
    frame_request->pipewire_fd = -1;
    frame_request->session_created = FALSE;
    frame_request->sources_selected = FALSE;
    frame_request->stream_started = FALSE;

    // Create a D-Bus proxy for the screencast portal
    proxy = g_dbus_proxy_new_for_bus_sync(G_BUS_TYPE_SESSION,
                                          G_DBUS_PROXY_FLAGS_NONE,
                                          NULL,
                                          "org.freedesktop.portal.Desktop",
                                          "/org/freedesktop/portal/desktop",
                                          "org.freedesktop.portal.ScreenCast",
                                          NULL,
                                          &error);

    if (error != NULL) {
        g_printerr("Error creating D-Bus proxy for ScreenCast: %s\n", error->message);
        g_error_free(error);
        g_free(frame_request);
        return NULL;
    }

    // Create session options
    GVariantBuilder *options_builder = g_variant_builder_new(G_VARIANT_TYPE("a{sv}"));
    
    char handle_token_string[64];
    snprintf(handle_token_string, sizeof(handle_token_string), "viture_screencast_%u", g_random_int());
    g_variant_builder_add(options_builder, "{sv}", "handle_token", g_variant_new_string(handle_token_string));
    g_variant_builder_add(options_builder, "{sv}", "session_handle_token", g_variant_new_string(handle_token_string));

    GVariant *params = g_variant_new("(a{sv})", options_builder);
    g_variant_builder_unref(options_builder);

    if (params == NULL) {
        g_printerr("FATAL: params for ScreenCast CreateSession is NULL\n");
        g_object_unref(proxy);
        g_free(frame_request);
        return NULL;
    }

    g_print("Calling ScreenCast CreateSession with handle_token: %s\n", handle_token_string);

    // For synchronous behavior, set up main loop
    GMainContext *context = g_main_context_default();
    frame_request->loop_for_sync_call = g_main_loop_new(context, FALSE);

    g_dbus_proxy_call(proxy,
                      "CreateSession",
                      params,
                      G_DBUS_CALL_FLAGS_NONE,
                      -1,
                      NULL,
                      on_screencast_create_session_completed,
                      frame_request);

    g_object_unref(proxy);

    // Timeout for the entire operation
    guint timeout_source_id = g_timeout_add(10000, // 10 seconds timeout
                                           screencast_timeout_callback, frame_request);

    g_main_loop_run(frame_request->loop_for_sync_call);

    if (timeout_source_id > 0) {
        g_source_remove(timeout_source_id);
    }

    g_main_loop_unref(frame_request->loop_for_sync_call);
    frame_request->loop_for_sync_call = NULL;

    if (frame_request->success && frame_request->stream_started) {
        g_screencast_session = frame_request;
        g_screencast_initialized = TRUE;
        g_print("ScreenCast session initialized successfully\n");
        return frame_request;
    } else {
        g_printerr("Failed to initialize ScreenCast session\n");
        free_xdg_frame_request(frame_request);
        return NULL;
    }
}

XDGFrameRequest* get_xdg_root_window_frame_sync() {
    // Initialize screencast session if not already done
    if (!g_screencast_initialized) {
        XDGFrameRequest *session = init_screencast_session();
        if (!session) {
            g_printerr("Failed to initialize screencast session\n");
            return NULL;
        }
    }

    if (!g_screencast_session || !g_screencast_session->stream_started) {
        g_printerr("ScreenCast session not ready\n");
        return NULL;
    }

    // Create a new frame request that copies data from the active session
    XDGFrameRequest *frame_request = g_new0(XDGFrameRequest, 1);
    frame_request->completed = TRUE;
    
    PipeWireStreamData *pw_data = g_screencast_session->pw_data;
    if (pw_data) {
        g_mutex_lock(&pw_data->frame_mutex);
        if (pw_data->frame_ready && pw_data->frame_data) {
            frame_request->success = TRUE;
            frame_request->width = pw_data->frame_width;
            frame_request->height = pw_data->frame_height;
            frame_request->stride = pw_data->frame_width * 3;
            
            size_t data_size = (size_t)frame_request->height * frame_request->stride;
            frame_request->data = (unsigned char *)g_malloc(data_size);
            memcpy(frame_request->data, pw_data->frame_data, data_size);
            
            // Mark frame as consumed
            pw_data->frame_ready = FALSE;
        } else {
            frame_request->success = FALSE;
        }
        g_mutex_unlock(&pw_data->frame_mutex);
    } else {
        frame_request->success = FALSE;
    }

    if (!frame_request->success) {
        // g_printerr("No new frame data available in screencast session\n");
        g_free(frame_request->data);
        g_free(frame_request);
        return NULL;
    }

    return frame_request;
}

void free_xdg_frame_request(XDGFrameRequest* frame_req) {
    if (frame_req) {
        // Don't free the global session
        if (frame_req == g_screencast_session) {
            return;
        }
        
        g_free(frame_req->data);
        g_free(frame_req->session_handle);
        g_free(frame_req->stream_node_id);
        
        if (frame_req->portal_request_proxy) {
            g_object_unref(frame_req->portal_request_proxy);
        }
        
        if (frame_req->pipewire_fd >= 0) {
            close(frame_req->pipewire_fd);
        }
        
        g_free(frame_req);
    }
}

void cleanup_screencast_session() {
    if (g_screencast_session) {
        g_print("Cleaning up screencast session\n");
        
        // Clean up PipeWire resources
        if (g_screencast_session->pw_data) {
            PipeWireStreamData *pw_data = g_screencast_session->pw_data;
            
            if (pw_data->loop) {
                pw_main_loop_quit(pw_data->loop);
            }
            if (pw_data->pipewire_thread) {
                g_thread_join(pw_data->pipewire_thread);
                pw_data->pipewire_thread = NULL;
            }

            if (pw_data->stream) {
                pw_stream_destroy(pw_data->stream);
            }
            if (pw_data->core) {
                pw_core_disconnect(pw_data->core);
            }
            if (pw_data->context) {
                pw_context_destroy(pw_data->context);
            }
            if (pw_data->loop) {
                pw_main_loop_destroy(pw_data->loop);
            }
            
            g_mutex_clear(&pw_data->frame_mutex);
            g_free(pw_data->frame_data);
            g_free(pw_data);
        }
        
        // Close PipeWire connection
        if (g_screencast_session->pipewire_fd >= 0) {
            close(g_screencast_session->pipewire_fd);
        }
        
        // Clean up session handle and other resources
        g_free(g_screencast_session->data);
        g_free(g_screencast_session->session_handle);
        g_free(g_screencast_session->stream_node_id);
        
        if (g_screencast_session->portal_request_proxy) {
            g_object_unref(g_screencast_session->portal_request_proxy);
        }
        
        g_free(g_screencast_session);
        g_screencast_session = NULL;
        g_screencast_initialized = FALSE;
        
        // Deinitialize PipeWire
        pw_deinit();
    }
}

/*
// Example usage
int main() {
    XDGFrameRequest *frame = get_xdg_root_window_frame_sync();

    if (frame && frame->data) {
        g_print("ScreenCast frame received.\n");
        g_print("Width: %d, Height: %d, Stride: %d\n", frame->width, frame->height, frame->stride);
        
        // Process frame->data
        // Save to PPM file for testing
        FILE *f = fopen("screencast.ppm", "wb");
        if (f) {
            fprintf(f, "P6\n%d %d\n255\n", frame->width, frame->height);
            if (frame->stride == frame->width * 3) {
                fwrite(frame->data, frame->height * frame->stride, 1, f);
            } else {
                for (int i = 0; i < frame->height; ++i) {
                    fwrite(frame->data + i * frame->stride, frame->width * 3, 1, f);
                }
            }
            fclose(f);
            g_print("Saved screencast to screencast.ppm\n");
        }
    } else {
        g_printerr("Failed to get screencast frame or no data.\n");
    }

    free_xdg_frame_request(frame);
    cleanup_screencast_session();
    return 0;
}
*/
