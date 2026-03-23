# Makefile for the V4L2 OpenGL Real-time Viewer

# --- Variables ---

# Compiler
CC = gcc
CXX = g++

# Target executable name
TARGET = v4l2_gl
TARGET_VITURE_SDK = v4l2_gl_viture_sdk

# Source files (add more .c files here if your project grows)
SRCS = v4l2_gl.c viture_connection.c utility.c xdg_source.c system_utils.c

# Object files (automatically generated from SRCS)
OBJS = $(SRCS:.c=.o)

# --- Architecture specific flags ---
ARCH := $(shell uname -m)

ifeq ($(ARCH),x86_64)
    ARCH_CFLAGS = -DARCH_X86_64
    SIMD_LIB = 3rdparty/lib/libSimd_x86.a
    VITURE_LIB_DIR = 3rdparty/release/x86_64
else ifeq ($(ARCH),aarch64)
    ARCH_CFLAGS = -DARCH_ARM64
    SIMD_LIB =
    VITURE_LIB_DIR = 3rdparty/release/aarch64
else
    $(warning "Unsupported architecture: $(ARCH)")
    SIMD_LIB =
    VITURE_LIB_DIR =
endif

# Compiler flags:
# -Wall:      Enable all standard warnings
# -Wextra:    Enable extra warnings
# -g:         Generate debugging information (for gdb)
# -O2:        Optimization level 2 (for performance)
# You might use -g for development and -O2 for release.
# -std=c11 causes a segfault in the viture code
GLIB_CFLAGS = $(shell pkg-config --cflags glib-2.0 gio-2.0 gdk-pixbuf-2.0 gio-unix-2.0)
PIPEWIRE_CFLAGS = $(shell pkg-config --cflags libpipewire-0.3)
CFLAGS = -Wall -Wextra -g -O2 $(GLIB_CFLAGS) $(PIPEWIRE_CFLAGS) $(ARCH_CFLAGS)

# Core graphics libraries
GRAPHICS_LIBS = -lglut -lGL -lGLU -lusb-1.0

# HIDAPI library
HIDAPI_LIB = -lhidapi-libusb
PTHREAD_LIB = -lpthread

# Viture SDK library (shared, architecture-specific)
VITURE_INCLUDES = -I3rdparty/release/include
VITURE_LIB = -L$(VITURE_LIB_DIR) -lglasses -Wl,--disable-new-dtags,-rpath,'$$ORIGIN/$(VITURE_LIB_DIR)' -Wl,-rpath-link,$(VITURE_LIB_DIR)

GLIB_LIBS = $(shell pkg-config --libs glib-2.0 gio-2.0 gdk-pixbuf-2.0 gio-unix-2.0) -lm
PIPEWIRE_LIBS = $(shell pkg-config --libs libpipewire-0.3)
LIBS = $(GRAPHICS_LIBS) $(HIDAPI_LIB) $(PTHREAD_LIB) $(GLIB_LIBS) $(PIPEWIRE_LIBS)

# Standard command for removing files
RM = rm -f

# --- Rules ---

# The default goal is 'all', which builds the target executable.
# The .PHONY directive tells make that 'all' is not a file.
.PHONY: all test viture_sdk
all: $(TARGET)

viture_sdk: $(TARGET_VITURE_SDK)

# Rule to link the object files into the final executable.
# The executable depends on all the object files.
$(TARGET): $(OBJS)
	@echo "==> Linking $(TARGET)..."
	$(CC) -o $(TARGET) $(OBJS) $(SIMD_LIB) $(LIBS) -lstdc++
	@echo "==> Build complete: ./"$(TARGET)

$(TARGET_VITURE_SDK): v4l2_gl_viture_sdk.o utility.o xdg_source.o system_utils.o
	@echo "==> Linking $(TARGET_VITURE_SDK)..."
	$(CXX) -o $(TARGET_VITURE_SDK) v4l2_gl_viture_sdk.o utility.o xdg_source.o system_utils.o $(SIMD_LIB) $(LIBS) $(VITURE_LIB)
	@echo "==> Build complete: ./"$(TARGET_VITURE_SDK)

# Pattern rule to compile .c files into .o files.
# For any .o file, make will find the corresponding .c file
# and use this recipe to build it.
# This rule will be used for all .c files including test_viture.c and viture_connection.c
%.o: %.c
	@echo "==> Compiling $<..."
	$(CC) $(CFLAGS) -I. -c -o $@ $< 

v4l2_gl_viture_sdk.o: v4l2_gl.c
	@echo "==> Compiling v4l2_gl_viture_sdk.o..."
	$(CXX) $(CFLAGS) -DUSE_VITURE $(VITURE_INCLUDES) -I. -c -o $@ v4l2_gl.c

# The 'clean' rule removes all generated files.
# .PHONY tells make that 'clean' is not a file.
.PHONY: clean
clean:
	@echo "==> Cleaning up..."
	$(RM) $(TARGET) $(TARGET_VITURE_SDK) $(OBJS) v4l2_gl_viture_sdk.o
	@echo "==> Done."
