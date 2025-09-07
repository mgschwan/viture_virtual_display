#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <pthread.h>
#include <math.h>

#include <linux/videodev2.h>

#include <stdbool.h>

#define KGFLAGS_IMPLEMENTATION
#include "kgflags.h"

// Use GL/glut.h on macOS, GL/freeglut.h on Linux
#include <GL/freeglut.h> 

#ifdef USE_VITURE
#include "3rdparty/include/viture.h"
#else
#include "viture_connection.h" // Include our own Viture connection header
#endif

#include "utility.h"
#include "xdg_source.h" // For XDG screen capture

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif


// --- Capture Mode ---
enum CaptureMode {
    MODE_V4L2,
    MODE_XDG
};
static enum CaptureMode current_capture_mode = MODE_V4L2;


// --- V4L2 and Frame Configuration ---
// #define DEVICE_PATH      "/dev/video0" // Will be replaced by a command line flag
#define FRAME_WIDTH      1920 // Requested width
#define FRAME_HEIGHT     1080 // Requested height
#define BUFFER_COUNT     4

#define SENSITIVITY_ANGLE 2.0f // Sensitivity for head gesture tracking in degrees
#define HEAD_SHAKE_RESET_TIME 500 
#define HEAD_SHAKE_SEQUENCE_RESET_TIME 3000
#define HEAD_SHAKE_RESET_COUNT 3 // Number of shakes to reset the yaw angle

// --- Global variables for V4L2 ---
static enum v4l2_buf_type active_buffer_type;
static __u32 active_pixel_format;
static int actual_frame_width = FRAME_WIDTH;   // Initialize with requested, update with actual
static int actual_frame_height = FRAME_HEIGHT; // Initialize with requested, update with actual

struct plane_info {
    void   *start;
    size_t length;
};

struct mplane_buffer {
    struct plane_info planes[VIDEO_MAX_PLANES];
    unsigned int num_planes_in_buffer; 
};

static int fd = -1;
static struct mplane_buffer *buffers_mp = NULL; 
static unsigned int n_buffers = 0;
static unsigned int num_planes_per_buffer = 0; 


// --- Global variables for OpenGL ---
static GLuint texture_id;
static unsigned char *rgb_frames[2] = {NULL, NULL}; 
static int front_buffer_idx = 0;                   
static int back_buffer_idx = 1;                    
static volatile bool new_frame_captured = false;   
static pthread_mutex_t frame_mutex;
static pthread_t capture_thread_id = 0; // Initialize to 0
static volatile bool stop_capture_thread_flag = false;

// For XDG mode
static int xdg_prev_frame_width = 0;  // Renamed for clarity
static int xdg_prev_frame_height = 0; // Renamed for clarity
static bool texture_needs_respecification = false;
static size_t current_rgb_buffer_size = 0;


static bool glut_initialized = false;

static bool fullscreen_mode = false; 
static bool display_test_pattern = false;                
static float g_plane_orbit_distance = 1.0f;         
static float g_plane_scale = 1.0f;                  

// --- Curved Display Configuration ---
static bool g_curved_display = false;
static float g_curve_angle = 60.0f; // Arc angle in degrees


// --- V4L2 Device Path ---
static const char *v4l2_device_path_str = "/dev/video0"; // Default value

// --- Helper Functions ---

static bool use_viture_imu = false;
static volatile float viture_roll = 0.0f;
static volatile float viture_pitch = 0.0f;
static volatile float viture_yaw = 0.0f;
static float initial_roll_offset = 0.0f;  
static float initial_pitch_offset = 0.0f; 
static float initial_yaw_offset = 0.0f;  
static bool initial_offsets_set = false; 
static float average_yaw = 0.0f; // Used for head gesture tracking
static int skip_initial_imu_frames = 20;


static float makeFloat(uint8_t *data) {
    float value = 0;
    uint8_t tem[4];
    tem[0] = data[3];
    tem[1] = data[2];
    tem[2] = data[1];
    tem[3] = data[0];
    memcpy(&value, tem, 4);
    return value;
}

/* Calculates the rolling average of the yaw angle 
   if the user shakes their head quickly 3 times the yaw angle will be reset to have the screen back in front of them.
   The head shake is detected by checking the difference between the average_yaw and the current yaw.
   If it exceeds SENSITIVITY_ANGLE degrees the shake detection is started.
   The next head shae is detected by the difference between the starting yaw angle and the current yaw angle, if it exceeds SENSITIVITY_ANGLE degrees
   the head shake is counted.
   Subsequent shakes are detected by calculating the difference between the last yaw angle and the current yaw angle, if the difference exceeds SENSITIVITY_ANGLE degrees
   the head shake is counted.

   The head shake is reset after 3 shakes or after HEAD_SHAKE_RESET_TIME.
*/

static void track_reset_head_gesture(float roll, float pitch, float yaw, uint32_t ts) {
    static float last_yaw = 0.0f;
    static int shake_direction = 0; // Direction of the last shake
    static clock_t last_reset_time = 0;
    static clock_t last_shake_time = 0;
    static int shake_count = 0;

    clock_t current_time = clock() * 1000 / CLOCKS_PER_SEC; // Convert to milliseconds

    yaw += 360.0f; // Ensure yaw is positive for easier calculations
    if (current_time - last_reset_time > HEAD_SHAKE_SEQUENCE_RESET_TIME ||
        current_time - last_shake_time > HEAD_SHAKE_RESET_TIME) { 
        printf ("Resetting head gesture tracking due to timeout. Yaw reset to %f\n", yaw);
        shake_count = 0;
        shake_direction = 0; // Reset shake direction
        last_reset_time = current_time;
        last_shake_time = current_time;
    }

    if ( shake_count == 0 ) {
        last_yaw = average_yaw;
    }

    float yaw_diff = yaw - last_yaw;

    bool is_shake = false;
    switch ( shake_direction ) {
        case 0: // No direction yet
            if (fabs(yaw_diff) > SENSITIVITY_ANGLE) {
                is_shake = true;
            }
            break;
        case 1: // Last shake was positive
            if (yaw_diff < -SENSITIVITY_ANGLE) {
                is_shake = true;
            }
            break;
        case -1: // Last shake was negative
            if (yaw_diff > SENSITIVITY_ANGLE) {
                is_shake = true;
            }
            break;
    }
        
    average_yaw = (average_yaw * 0.9f) + (yaw * 0.1f); // Update the average with a simple low-pass filter

    if (is_shake) {
        printf("Head shake detected! Count: %d average yaw %f, last_yaw: %f, current_yaw: %f, ts: %ld\n", shake_count, average_yaw, last_yaw, yaw, current_time);

        last_shake_time = current_time;
        last_yaw = average_yaw;
        shake_count++;


        if (yaw_diff > 0) {
            shake_direction = 1; 
        } else {
            shake_direction = -1; 
        }
    }

    if (shake_count >= HEAD_SHAKE_RESET_COUNT ) {
        printf("Resetting head gesture tracking. Yaw reset to %f\n", yaw);
        average_yaw = yaw; // Reset the average yaw to the current yaw
        shake_count = 0; // Reset the count
        shake_direction = 0; // Reset the shake direction
        initial_offsets_set = false; // Reset the initial offsets

        last_shake_time = current_time;
        last_reset_time = current_time; // Update the last reset time
        skip_initial_imu_frames = 30;
    } 
    
}


static void app_viture_imu_data_handler(uint8_t *data, uint16_t len, uint32_t ts) {
    (void)ts; 
    if (len < 12) return;

    if (use_viture_imu && !initial_offsets_set) {
        if (skip_initial_imu_frames > 0) {
            skip_initial_imu_frames--;
            return; // Skip the first few frames to allow IMU to stabilize
        }   

        initial_roll_offset = makeFloat(data);
        initial_pitch_offset = makeFloat(data + 4); 
        initial_yaw_offset = -makeFloat(data + 8);
        initial_offsets_set = true;
        printf("V4L2_GL Viture: Initial offsets captured: Roll=%f, Pitch=%f, Yaw=%f\n", initial_roll_offset, initial_pitch_offset, initial_yaw_offset);
    }

    viture_roll = makeFloat(data);
    viture_pitch = makeFloat(data + 4);
    viture_yaw = -makeFloat(data + 8);

    track_reset_head_gesture(viture_roll, viture_pitch, viture_yaw, ts);

}

static void app_viture_mcu_event_handler(uint16_t msgid, uint8_t *data, uint16_t len, uint32_t ts)
{
    (void)ts; 
    printf("V4L2_GL MCU Event: ID=0x%04X, Len=%u, Data: ", msgid, len);
    for (uint16_t i = 0; i < len; i++) {
        printf("%02X ", data[i]);
    }
    printf("\n");
}



// --- V4L2 Initialization ---
void init_v4l2() {
    struct v4l2_capability cap;
    struct v4l2_format fmt;
    struct v4l2_requestbuffers req;
    
    printf("V4L2: Opening device: %s\n", v4l2_device_path_str);
    fd = open(v4l2_device_path_str, O_RDWR | O_NONBLOCK, 0);
    if (fd < 0) { 
        fprintf(stderr, "Cannot open device %s: %s\n", v4l2_device_path_str, strerror(errno)); 
        exit(EXIT_FAILURE); 
    }

    if (ioctl(fd, VIDIOC_QUERYCAP, &cap) < 0) { perror("VIDIOC_QUERYCAP"); exit(EXIT_FAILURE); }

    if (cap.capabilities & V4L2_CAP_VIDEO_CAPTURE_MPLANE) {
        active_buffer_type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        printf("V4L2: Device supports multi-planar video capture.\n");
    } else if (cap.capabilities & V4L2_CAP_VIDEO_CAPTURE) {
        active_buffer_type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        printf("V4L2: Device supports single-planar video capture.\n");
    } else {
        fprintf(stderr, "Device does not support video capture (single or multi-planar)\n"); exit(EXIT_FAILURE);
    }

    if (!(cap.capabilities & V4L2_CAP_STREAMING)) {
        fprintf(stderr, "Device does not support streaming\n"); exit(EXIT_FAILURE);
    }
    printf("V4L2: Device supports streaming.\n");

    memset(&fmt, 0, sizeof(fmt));
    fmt.type = active_buffer_type;
    bool format_set = false;

    if (active_buffer_type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
        fmt.fmt.pix_mp.width       = FRAME_WIDTH;
        fmt.fmt.pix_mp.height      = FRAME_HEIGHT;
        fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV24; 
        fmt.fmt.pix_mp.field       = V4L2_FIELD_NONE;
        fmt.fmt.pix_mp.num_planes  = 2; 

        if (ioctl(fd, VIDIOC_S_FMT, &fmt) == 0) {
            active_pixel_format = fmt.fmt.pix_mp.pixelformat;
            num_planes_per_buffer = fmt.fmt.pix_mp.num_planes;
            if ( ( num_planes_per_buffer == 1 || num_planes_per_buffer == 2 ) && active_pixel_format == V4L2_PIX_FMT_NV24) {
                actual_frame_width = fmt.fmt.pix_mp.width;
                actual_frame_height = fmt.fmt.pix_mp.height;
                printf("V4L2: Format set to %dx%d, pixelformat NV24, %u planes (MPLANE)\n",
                       actual_frame_width, actual_frame_height, num_planes_per_buffer);
                format_set = true;
            } else {
                 fprintf(stderr, "V4L2: Device did not accept NV24 with 1 or 2 planes as expected. Planes: %u, Format: %c%c%c%c\n",
                    num_planes_per_buffer, (active_pixel_format)&0xFF, (active_pixel_format>>8)&0xFF,
                    (active_pixel_format>>16)&0xFF, (active_pixel_format>>24)&0xFF);
            }
        } else {
            perror("VIDIOC_S_FMT (MPLANE NV24) failed");
        }
    }
    
    if (!format_set) { 
        printf("V4L2: Attempting single-plane YUYV format.\n");
        active_buffer_type = V4L2_BUF_TYPE_VIDEO_CAPTURE; 
        fmt.type = active_buffer_type; 
        fmt.fmt.pix.width       = FRAME_WIDTH;
        fmt.fmt.pix.height      = FRAME_HEIGHT;
        fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
        fmt.fmt.pix.field       = V4L2_FIELD_NONE;
        if (ioctl(fd, VIDIOC_S_FMT, &fmt) == 0) {
            active_pixel_format = fmt.fmt.pix.pixelformat;
            num_planes_per_buffer = 1; 
            actual_frame_width = fmt.fmt.pix.width;
            actual_frame_height = fmt.fmt.pix.height;
            printf("V4L2: Format set to %dx%d, pixelformat YUYV (SINGLE-PLANE)\n",
                   actual_frame_width, actual_frame_height);
            format_set = true;
        } else {
            perror("VIDIOC_S_FMT (SINGLE-PLANE YUYV) also failed.");
            exit(EXIT_FAILURE);
        }
    }
    if (!format_set) { 
        fprintf(stderr, "V4L2: Failed to set any video format.\n");
        exit(EXIT_FAILURE);
    }

    memset(&req, 0, sizeof(req));
    req.count = BUFFER_COUNT;
    req.type = active_buffer_type;
    req.memory = V4L2_MEMORY_MMAP;
    if (ioctl(fd, VIDIOC_REQBUFS, &req) < 0) { perror("VIDIOC_REQBUFS"); exit(EXIT_FAILURE); }
    n_buffers = req.count;
    printf("V4L2: %d buffers requested.\n", n_buffers);

    buffers_mp = calloc(n_buffers, sizeof(*buffers_mp)); 
    for (unsigned int i = 0; i < n_buffers; ++i) {
        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.type   = active_buffer_type;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index  = i;

        if (active_buffer_type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
            struct v4l2_plane planes_query[VIDEO_MAX_PLANES];
            memset(planes_query, 0, sizeof(planes_query));
            buf.m.planes = planes_query;
            buf.length = num_planes_per_buffer; 
        }

        if (ioctl(fd, VIDIOC_QUERYBUF, &buf) < 0) { perror("VIDIOC_QUERYBUF"); exit(EXIT_FAILURE); }

        if (active_buffer_type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
            buffers_mp[i].num_planes_in_buffer = num_planes_per_buffer;
            for (unsigned int p = 0; p < num_planes_per_buffer; ++p) {
                buffers_mp[i].planes[p].length = buf.m.planes[p].length;
                buffers_mp[i].planes[p].start = mmap(NULL, buf.m.planes[p].length,
                                                     PROT_READ | PROT_WRITE, MAP_SHARED,
                                                     fd, buf.m.planes[p].m.mem_offset);
                if (buffers_mp[i].planes[p].start == MAP_FAILED) { perror("mmap mplane"); exit(EXIT_FAILURE); }
            }
        } else { 
            buffers_mp[i].num_planes_in_buffer = 1;
            buffers_mp[i].planes[0].length = buf.length; 
            buffers_mp[i].planes[0].start = mmap(NULL, buf.length,
                                                 PROT_READ | PROT_WRITE, MAP_SHARED,
                                                 fd, buf.m.offset); 
            if (buffers_mp[i].planes[0].start == MAP_FAILED) { perror("mmap splane"); exit(EXIT_FAILURE); }
        }
    }
    printf("V4L2: Buffers and planes mapped.\n");

    for (unsigned int i = 0; i < n_buffers; ++i) {
        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.type = active_buffer_type;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;

        if (active_buffer_type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
            struct v4l2_plane planes_q[VIDEO_MAX_PLANES]; 
            memset(planes_q, 0, sizeof(planes_q));
            buf.m.planes = planes_q;
            buf.length = num_planes_per_buffer; 
        }

        if (ioctl(fd, VIDIOC_QBUF, &buf) < 0) { perror("VIDIOC_QBUF"); exit(EXIT_FAILURE); }
    }
    printf("V4L2: Buffers queued.\n");

    if (ioctl(fd, VIDIOC_STREAMON, &active_buffer_type) < 0) { perror("VIDIOC_STREAMON"); exit(EXIT_FAILURE); }
    printf("V4L2: Streaming started.\n");
}

// --- OpenGL/GLUT Functions ---

void cleanup() {
    printf("Cleaning up...\n");
    if (use_viture_imu) {
        printf("Viture: Disabling IMU and de-initializing...\n");

#ifdef USE_VITURE
        set_imu(false); 
        deinit();       
#else
        set_imu(false); 
        viture_driver_close(); 
#endif
    }

    if (fd != -1) {
        ioctl(fd, VIDIOC_STREAMOFF, &active_buffer_type);
        if (buffers_mp) {
            for (unsigned int i = 0; i < n_buffers; ++i) {
                for (unsigned int p = 0; p < buffers_mp[i].num_planes_in_buffer; ++p) { 
                     if (buffers_mp[i].planes[p].start && buffers_mp[i].planes[p].start != MAP_FAILED) {
                        munmap(buffers_mp[i].planes[p].start, buffers_mp[i].planes[p].length);
                     }
                }
            }
        }
        close(fd);
        fd = -1;
    }
    if (buffers_mp) { free(buffers_mp); buffers_mp = NULL; }
    if (rgb_frames[0]) { free(rgb_frames[0]); rgb_frames[0] = NULL; }
    if (rgb_frames[1]) { free(rgb_frames[1]); rgb_frames[1] = NULL; }
    
    // Signal capture thread to stop and wait for it (only if it was started for V4L2)
    if (current_capture_mode == MODE_V4L2 && capture_thread_id != 0) {
        printf("V4L2_GL: Signaling V4L2 capture thread to stop...\n");
        stop_capture_thread_flag = true;
        printf("V4L2_GL: Joining V4L2 capture thread...\n");
        pthread_join(capture_thread_id, NULL);
        capture_thread_id = 0; // Reset after joining
        printf("V4L2_GL: V4L2 capture thread joined.\n");
    }
    
    // Clean up XDG screencast session if it was used
    if (current_capture_mode == MODE_XDG) {
        printf("V4L2_GL: Cleaning up XDG screencast session...\n");
        cleanup_screencast_session();
    }

    pthread_mutex_destroy(&frame_mutex); 
    if (texture_id != 0) glDeleteTextures(1, &texture_id);
    printf("Cleanup complete.\n");
}

void display() {
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    gluLookAt(0.0, 0.0, 2.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0); 

    if (use_viture_imu) {
        glRotatef(viture_yaw - initial_yaw_offset, 0.0f, 1.0f, 0.0f);   
        glRotatef(viture_pitch - initial_pitch_offset, 1.0f, 0.0f, 0.0f); 
        glRotatef(viture_roll - initial_roll_offset, 0.0f, 0.0f, 1.0f);  
    } else {
        static float angle = 0.0f;
        angle += 0.2f;
        if (angle > 360.0f) angle -= 360.0f;
        glRotatef(15.0f, 1.0f, 0.0f, 0.0f); 
        glRotatef(angle, 0.0f, 1.0f, 0.0f); 
    }

    // For flat display, translate the plane away from the camera.
    // For curved display, the geometry is generated at the correct distance.
    if (!g_curved_display) {
        glTranslatef(0.0f, 0.0f, -g_plane_orbit_distance);
    }
    glScalef(g_plane_scale, g_plane_scale, g_plane_scale);

    bool generate_texture = false;
    pthread_mutex_lock(&frame_mutex);
    if (new_frame_captured) {
        int temp = front_buffer_idx;
        front_buffer_idx = back_buffer_idx;
        back_buffer_idx = temp;
        new_frame_captured = false;
        generate_texture = true; 
    }
    pthread_mutex_unlock(&frame_mutex);

    glBindTexture(GL_TEXTURE_2D, texture_id);

    if (texture_needs_respecification && glut_initialized) { // Ensure GL context is active
        printf("V4L2_GL: Re-specifying texture to %dx%d\n", actual_frame_width, actual_frame_height);
        // Update texture storage with new dimensions
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, actual_frame_width, actual_frame_height, 0,
                     GL_RGB, GL_UNSIGNED_BYTE, NULL); // Data can be NULL if immediately followed by glTexSubImage2D
        texture_needs_respecification = false;
        generate_texture = true; // Force update with new data even if new_frame_captured was false before this
    }

    if ( generate_texture ) {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, actual_frame_width, actual_frame_height, GL_RGB, GL_UNSIGNED_BYTE, rgb_frames[front_buffer_idx]);
    }

    if ( !use_viture_imu || initial_offsets_set ) { 
        float aspect_ratio = (float)actual_frame_width / (float)actual_frame_height;

        if (g_curved_display) {
            const int segments = 50;
            float radius = g_plane_orbit_distance;

            // Convert total arc angle to radians and find start/end angles
            float total_angle_rad = g_curve_angle * M_PI / 180.0f;
            float start_rad = -total_angle_rad / 2.0f;
            float angle_range_rad = total_angle_rad;

            // Arc length determines the width of the virtual screen
            float arc_length = fabsf(radius * angle_range_rad);
            float height = arc_length / aspect_ratio;

            glBegin(GL_QUAD_STRIP);
            for (int i = 0; i <= segments; ++i) {
                float ratio = (float)i / segments;
                float angle = start_rad + ratio * angle_range_rad;

                // Calculate vertex positions for a cylinder centered at the origin, concave towards +Z
                float x = radius * sin(angle);
                float z = -radius * cos(angle); // Negative cos places it behind the origin

                // Top vertex
                glTexCoord2f(ratio, 0.0f);
                glVertex3f(x, height / 2.0f, z);

                // Bottom vertex
                glTexCoord2f(ratio, 1.0f);
                glVertex3f(x, -height / 2.0f, z);
            }
            glEnd();
        } else {
            // Original flat plane rendering
            glBegin(GL_QUADS);
                glTexCoord2f(0.0f, 1.0f); glVertex3f(-aspect_ratio, -1.0f, 0.0f);
                glTexCoord2f(1.0f, 1.0f); glVertex3f( aspect_ratio, -1.0f, 0.0f);
                glTexCoord2f(1.0f, 0.0f); glVertex3f( aspect_ratio,  1.0f, 0.0f);
                glTexCoord2f(0.0f, 0.0f); glVertex3f(-aspect_ratio,  1.0f, 0.0f);
            glEnd();
        }
    }
    glutSwapBuffers();
}

void reshape(int w, int h) {
    glViewport(0, 0, w, h);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    gluPerspective(45.0, (double)w / (double)h, 1.0, 10000.0);
}

void capture_and_update() {
    struct v4l2_buffer buf;
    struct v4l2_plane planes_dq[VIDEO_MAX_PLANES]; 
    memset(&buf, 0, sizeof(buf));
    
    buf.type = active_buffer_type;
    buf.memory = V4L2_MEMORY_MMAP;

    if (active_buffer_type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
        memset(planes_dq, 0, sizeof(planes_dq));
        buf.m.planes = planes_dq;
        buf.length = num_planes_per_buffer; 
    }

    if (ioctl(fd, VIDIOC_DQBUF, &buf) == -1) {
        if (errno == EAGAIN) {
            return;
        }
        perror("VIDIOC_DQBUF");
        exit(EXIT_FAILURE);
    }
    
    if (active_buffer_type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
        if (active_pixel_format == V4L2_PIX_FMT_NV24 && num_planes_per_buffer >= 2) {
            convert_nv24_to_rgb(
                (const unsigned char *)buffers_mp[buf.index].planes[0].start,
                (const unsigned char *)buffers_mp[buf.index].planes[1].start,
                rgb_frames[back_buffer_idx], actual_frame_width, actual_frame_height);
        } else if (active_pixel_format == V4L2_PIX_FMT_NV24 && num_planes_per_buffer == 1) { 
            convert_nv24_to_rgb(
                (const unsigned char *)buffers_mp[buf.index].planes[0].start,
                (const unsigned char *)buffers_mp[buf.index].planes[0].start + actual_frame_width * actual_frame_height, 
                rgb_frames[back_buffer_idx], actual_frame_width, actual_frame_height);
        } else {
             fprintf(stderr, "Error: Unsupported MPLANE pixel format %c%c%c%c or plane count %u\n",
                    (active_pixel_format)&0xFF, (active_pixel_format>>8)&0xFF,
                    (active_pixel_format>>16)&0xFF, (active_pixel_format>>24)&0xFF,
                    num_planes_per_buffer);
            fill_frame_with_pattern(rgb_frames[back_buffer_idx], actual_frame_width, actual_frame_height);
        }
    } else { // Single-plane
        if (active_pixel_format == V4L2_PIX_FMT_YUYV) {
            convert_yuyv_to_rgb((const unsigned char *)buffers_mp[buf.index].planes[0].start, 
                                 rgb_frames[back_buffer_idx], actual_frame_width, actual_frame_height, buf.bytesused);
        } else {
             fprintf(stderr, "Error: Unsupported SINGLE-PLANE pixel format %c%c%c%c\n",
                    (active_pixel_format)&0xFF, (active_pixel_format>>8)&0xFF,
                    (active_pixel_format>>16)&0xFF, (active_pixel_format>>24)&0xFF);
            fill_frame_with_pattern(rgb_frames[back_buffer_idx], actual_frame_width, actual_frame_height);
        }
    }

    pthread_mutex_lock(&frame_mutex);
    new_frame_captured = true;
    pthread_mutex_unlock(&frame_mutex);

    if (ioctl(fd, VIDIOC_QBUF, &buf) == -1) {
        perror("VIDIOC_QBUF");
        exit(EXIT_FAILURE);
    }

}

#define TARGET_FPS 30 // This can still be used for display refresh rate

// --- Capture Thread ---
void *capture_thread_func(void *arg) {
    (void)arg; // Unused
    printf("V4L2_GL: Capture thread started.\n");
    struct timespec ts;
    ts.tv_sec = 0;
    // Use the TARGET_FPS define here
    ts.tv_nsec = (1000000000L / TARGET_FPS) / 2; // Sleep for a short duration if EAGAIN. Use long literal for 1 billion.

    while (!stop_capture_thread_flag) {
        struct v4l2_buffer buf_check; // For checking DQBUF result
        memset(&buf_check, 0, sizeof(buf_check));
        buf_check.type = active_buffer_type;
        buf_check.memory = V4L2_MEMORY_MMAP;
        if (active_buffer_type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
            struct v4l2_plane planes_temp[VIDEO_MAX_PLANES];
            memset(planes_temp, 0, sizeof(planes_temp));
            buf_check.m.planes = planes_temp;
            buf_check.length = num_planes_per_buffer;
        }

        // Non-blocking check if a buffer is ready
        // We use a temporary buffer struct for ioctl to avoid issues if capture_and_update is slow
        // and another DQBUF happens before QBUF in capture_and_update.
        // However, capture_and_update itself does DQBUF.
        // The main purpose here is to call capture_and_update when data is likely available.
        // A more robust way might be to use select() or poll() on the fd.
        // For now, we'll call capture_and_update and let it handle EAGAIN.

        capture_and_update(); // This function now handles its own EAGAIN

        // If capture_and_update returned due to EAGAIN, we can sleep a bit
        // This check is a bit indirect. A better way would be for capture_and_update to return a status.
        // For now, we assume if new_frame_captured is false after a call, it might have been EAGAIN.
        pthread_mutex_lock(&frame_mutex);
        bool frame_was_newly_captured = new_frame_captured; // Check if capture_and_update set it
        pthread_mutex_unlock(&frame_mutex);

        if (!frame_was_newly_captured) { // If no new frame was processed (e.g. EAGAIN)
             nanosleep(&ts, NULL); // Sleep briefly to avoid busy-waiting
        }
        // No explicit sleep if a frame was captured, as V4L2 DQBUF/QBUF cycle provides throttling
    }
    printf("V4L2_GL: Capture thread stopping.\n");
    return NULL;
}

// static clock_t last_redisplay_time = 0; // Moved TARGET_FPS definition earlier
static clock_t last_redisplay_time = 0;
void idle()
{
    // This function is now only responsible for triggering redisplay
    // The actual frame capture is handled by capture_thread_func for V4L2
    // For XDG, we might capture here or in display() before drawing.
    // Let's try capturing XDG frames here to decouple from display's GL context needs.

    clock_t current_time = clock();
    
    if (display_test_pattern) {
        fill_frame_with_pattern(rgb_frames[back_buffer_idx], actual_frame_width, actual_frame_height);
        pthread_mutex_lock(&frame_mutex);
        new_frame_captured = true;
        pthread_mutex_unlock(&frame_mutex);
    } else if (current_capture_mode == MODE_XDG) {
        //if ( (current_time - last_redisplay_time) * 1000 / CLOCKS_PER_SEC >= (1000 / TARGET_FPS) ) {
            XDGFrameRequest *xdg_frame = get_xdg_root_window_frame_sync();
            if (xdg_frame && xdg_frame->success && xdg_frame->data) {
                if (xdg_frame->width != xdg_prev_frame_width || xdg_frame->height != xdg_prev_frame_height)
                {
                    printf("V4L2_GL: XDG frame dimensions changed to %dx%d (from %dx%d)\n", 
                        xdg_frame->width, xdg_frame->height, xdg_prev_frame_width, xdg_prev_frame_height);
                    actual_frame_width = xdg_frame->width;
                    actual_frame_height = xdg_frame->height;
                    xdg_prev_frame_width = actual_frame_width;
                    xdg_prev_frame_height = actual_frame_height;
                    texture_needs_respecification = true;

                    // Reallocate rgb_frames if necessary
                    size_t new_size = (size_t)actual_frame_width * actual_frame_height * 3;
                    if (new_size > current_rgb_buffer_size || !rgb_frames[0] || !rgb_frames[1]) {
                        printf("V4L2_GL: Reallocating RGB buffers to %zu bytes for %dx%d\n", new_size, actual_frame_width, actual_frame_height);
                        free(rgb_frames[0]);
                        free(rgb_frames[1]);
                        rgb_frames[0] = malloc(new_size);
                        rgb_frames[1] = malloc(new_size);
                        if (!rgb_frames[0] || !rgb_frames[1]) {
                            fprintf(stderr, "FATAL: Failed to reallocate RGB frames for XDG mode!\n");
                            // Consider how to handle this - maybe exit or stop trying XDG.
                            // For now, we might crash if memcpy proceeds.
                            // Let's prevent memcpy if allocation failed.
                            if (xdg_frame) free_xdg_frame_request(xdg_frame);
                            // Skip frame processing this cycle
                            goto skip_xdg_frame_processing; 
                        }
                        memset(rgb_frames[0], 0, new_size); // Clear new buffers
                        memset(rgb_frames[1], 0, new_size);
                        current_rgb_buffer_size = new_size;
                    }
                }

                if (rgb_frames[back_buffer_idx]) { // Check if buffer is allocated
                    memcpy(rgb_frames[back_buffer_idx], xdg_frame->data, (size_t)actual_frame_width * actual_frame_height * 3);
                    pthread_mutex_lock(&frame_mutex);
                    new_frame_captured = true;
                    pthread_mutex_unlock(&frame_mutex);
                } else {
                    fprintf(stderr, "V4L2_GL: rgb_frames not allocated, cannot copy XDG frame.\n");
                }
            }

            if (xdg_frame) {
                free_xdg_frame_request(xdg_frame);
            }
        //}  /* end of FPS conditional */ 

    }

skip_xdg_frame_processing:; // Label for goto
    //if ( (current_time - last_redisplay_time) * 1000 / CLOCKS_PER_SEC >= (1000 / TARGET_FPS) ) {
        last_redisplay_time = current_time;

    glutPostRedisplay();
    nanosleep(&(struct timespec){0, 1000000000L / TARGET_FPS}, NULL); // Sleep for FPS interval, this means the exact FPS will not be reached due to processing time

    //}
}

void init_gl() {
    if (pthread_mutex_init(&frame_mutex, NULL) != 0) {
        perror("Mutex init failed");
        exit(EXIT_FAILURE);
    }

    // Allocate RGB frames based on actual dimensions.
    // actual_frame_width/height are set by init_v4l2() or by initial XDG frame check.
    current_rgb_buffer_size = (size_t)actual_frame_width * actual_frame_height * 3;
    if (current_rgb_buffer_size == 0) { // Safety if dimensions were somehow zero
        fprintf(stderr, "Warning: Frame dimensions are zero in init_gl. Defaulting to 1x1.\n");
        actual_frame_width = 1; actual_frame_height = 1;
        current_rgb_buffer_size = 3;
    }

    rgb_frames[0] = malloc(current_rgb_buffer_size);
    rgb_frames[1] = malloc(current_rgb_buffer_size);

    if (!rgb_frames[0] || !rgb_frames[1]) {
        fprintf(stderr, "Failed to allocate memory for RGB frames (%dx%d)\n", actual_frame_width, actual_frame_height);
        exit(EXIT_FAILURE);
    }
    memset(rgb_frames[0], 0, current_rgb_buffer_size);
    memset(rgb_frames[1], 0, current_rgb_buffer_size);

    glEnable(GL_DEPTH_TEST);
    glEnable(GL_TEXTURE_2D);

    glGenTextures(1, &texture_id);
    glBindTexture(GL_TEXTURE_2D, texture_id);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, actual_frame_width, actual_frame_height, 0,
                 GL_RGB, GL_UNSIGNED_BYTE, rgb_frames[front_buffer_idx]); 

    glut_initialized = true; 
}

int main(int argc, char **argv) {
    // --- Argument Parsing with kgflags ---
    kgflags_string("device", "/dev/video0", "V4L2 device path (e.g., /dev/video0).", false, &v4l2_device_path_str);
    kgflags_bool("fullscreen", false, "Enable fullscreen mode.", false, &fullscreen_mode);
    kgflags_bool("viture", false, "Enable Viture IMU.", false, &use_viture_imu);
    kgflags_bool("test-pattern", false, "Display test pattern instead of V4L2.", false, &display_test_pattern);
    bool use_xdg_mode = false;
    kgflags_bool("xdg", false, "Use XDG portal for screen capture instead of V4L2.", false, &use_xdg_mode);
    int cursor_mode = 0;
    kgflags_int("cursor-mode", 0, "Set cursor mode for XDG (1=hidden, 2=embedded, 4=direct).", false, &cursor_mode);

    double plane_distance_double = (double)g_plane_orbit_distance;
    kgflags_double("plane-distance", plane_distance_double, "Set plane orbit distance (float).", false, &plane_distance_double);

    double plane_scale_double = (double)g_plane_scale;
    kgflags_double("plane-scale", plane_scale_double, "Set plane scale (float, must be > 0).", false, &plane_scale_double);

    // Flags for curved display
    kgflags_bool("curved-display", false, "Render on a curved surface.", false, &g_curved_display);
    double curve_angle_double = (double)g_curve_angle;
    kgflags_double("curve-angle", curve_angle_double, "Set the arc angle of the curved surface in degrees.", false, &curve_angle_double);

    kgflags_set_prefix("--"); // Flags will be e.g. --fullscreen
    kgflags_set_custom_description("Usage: v4l2_gl [FLAGS]\n\nOptions:");

    if (!kgflags_parse(argc, argv)) {
        kgflags_print_errors();
        kgflags_print_usage();
        return 1;
    }

    g_plane_orbit_distance = (float)plane_distance_double;
    g_plane_scale = (float)plane_scale_double;
    g_curve_angle = (float)curve_angle_double;

    // Validate plane_scale after parsing
    if (g_plane_scale <= 0.0f) {
        fprintf(stderr, "Warning: Plane scale (--plane-scale) must be positive. Resetting to 1.0.\n");
        g_plane_scale = 1.0f;
    }

    printf("Starting V4L2-OpenGL real-time viewer with settings:\n");
    printf("  Fullscreen: %s\n", fullscreen_mode ? "enabled" : "disabled");
    printf("  Viture IMU: %s\n", use_viture_imu ? "enabled" : "disabled");
    printf("  Test Pattern: %s\n", display_test_pattern ? "enabled" : "disabled");
    printf("  V4L2 Device: %s\n", v4l2_device_path_str);
    printf("  XDG Mode: %s\n", use_xdg_mode ? "enabled" : "disabled");
    printf("  Plane Orbit Distance: %f\n", g_plane_orbit_distance);
    printf("  Plane Scale: %f\n", g_plane_scale);
    if (g_curved_display) {
        printf("  Curved Display: enabled\n");
        printf("    Radius (from plane-distance): %f\n", g_plane_orbit_distance);
        printf("    Arc Angle: %f\n", g_curve_angle);
    } else {
        printf("  Curved Display: disabled\n");
    }
    printf("\n");

    if (use_xdg_mode) {
        current_capture_mode = MODE_XDG;
        printf("V4L2_GL: XDG screen capture mode selected.\n");
    } else {
        current_capture_mode = MODE_V4L2;
        printf("V4L2_GL: V4L2 capture mode selected.\n");
    }


if (use_viture_imu) {
#ifdef USE_VITURE
    printf("Viture: Initializing with official SDK...\n");
    init(app_viture_imu_data_handler, app_viture_mcu_event_handler); 
    set_imu(true); 
    printf("Viture: IMU stream enabled via official SDK.\n");
#else
    printf("Viture: Initializing with custom driver...\n");
    if (!viture_driver_init()) { 
        fprintf(stderr, "V4L2_GL: Failed to initialize custom Viture driver.\n");
        use_viture_imu = false; 
    } else {
        //printf ("V4L2_GL: Custom Viture driver initialized successfully.\n");
        viture_set_imu_data_callback(app_viture_imu_data_handler); 
        viture_set_mcu_event_callback(app_viture_mcu_event_handler); 
        
        uint32_t imu_set_status = set_imu(true); 
        if (imu_set_status != 0) { 
            fprintf(stderr, "V4L2_GL: set_imu(true) command failed with status %u using custom driver.\n", imu_set_status);
        } else {
            printf("Viture: IMU stream enabled via custom driver.\n");
        }
    }
#endif
}

    glutInit(&argc, argv);
    glutInitDisplayMode(GLUT_DOUBLE | GLUT_RGB | GLUT_DEPTH);

    if (fullscreen_mode) {
        printf("Mode: Fullscreen\n");
        glutCreateWindow("V4L2 Real-time Display");
        glutFullScreen();
    } else {
        printf("Mode: Windowed\n");
        glutInitWindowSize(1280, 720); 
        glutCreateWindow("V4L2 Real-time Display");
    }
    
    if (current_capture_mode == MODE_V4L2 && !display_test_pattern) {
        init_v4l2(); 
    } else if (current_capture_mode == MODE_XDG) { // MODE_XDG
        printf("V4L2_GL: Initializing XDG screen capture session...\n");
        if (!init_screencast_session(cursor_mode)) {
            fprintf(stderr, "V4L2_GL: Failed to initialize XDG screencast session. Exiting.\n");
            exit(EXIT_FAILURE);
        }
    }

    init_gl();   

    glutDisplayFunc(display);
    glutReshapeFunc(reshape);
    glutIdleFunc(idle);
    atexit(cleanup);

    // Create and start the capture thread only for V4L2 mode
    if (current_capture_mode == MODE_V4L2 && !display_test_pattern) {
        printf("V4L2_GL: Creating V4L2 capture thread...\n");
        if (pthread_create(&capture_thread_id, NULL, capture_thread_func, NULL) != 0) {
            perror("Failed to create V4L2 capture thread");
            cleanup(); 
            exit(EXIT_FAILURE);
        }
        printf("V4L2_GL: V4L2 capture thread created.\n");
    } else {
        printf("V4L2_GL: XDG mode, no separate capture thread needed.\n");
    }

    printf("\n--- Starting main loop ---\n");
    glutMainLoop();
    return 0;
}
