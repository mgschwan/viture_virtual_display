# Makefile for the V4L2 OpenGL Real-time Viewer

# --- Variables ---

# Compiler
CC = gcc

# Target executable name
TARGET = v4l2_gl
TARGET_VITURE_SDK = v4l2_gl_viture_sdk
TARGET_TEST = test_viture
TARGET_TEST_CONVERSIONS = test_conversions

# Source files (add more .c files here if your project grows)
SRCS = v4l2_gl.c viture_connection.c utility.c
SRCS_TEST = test_viture.c viture_connection.c utility.c
SRC_TEST_CONVERSIONS = test_conversions.c utility.c

# Object files (automatically generated from SRCS)
OBJS = $(SRCS:.c=.o)
OBJS_TEST = $(SRCS_TEST:.c=.o)

# Compiler flags:
# -Wall:      Enable all standard warnings
# -Wextra:    Enable extra warnings
# -g:         Generate debugging information (for gdb)
# -O2:        Optimization level 2 (for performance)
# You might use -g for development and -O2 for release.
# -std=c11 causes a segfault in the viture code
CFLAGS = -Wall -Wextra -g -O2

# Core graphics libraries
GRAPHICS_LIBS = -lglut -lGL -lGLU -lusb-1.0

# HIDAPI library
HIDAPI_LIB = -lhidapi-libusb
PTHREAD_LIB = -lpthread

# Viture SDK library (not used by test_viture directly, but viture_connection.o might have been compiled with it if it were part of its sources)
# For test_viture, we only need viture_connection.o, which itself doesn't use VITURE_LIB.
VITURE_LIB = 3rdparty/lib/libviture_one_sdk_static.a

LIBS = $(LDFLAGS) $(GRAPHICS_LIBS) $(HIDAPI_LIB) $(PTHREAD_LIB)
LIBS_TEST = $(LDFLAGS) $(HIDAPI_LIB) $(PTHREAD_LIB)
LIBS_TEST_CONVERSIONS = $(LDFLAGS)

# Standard command for removing files
RM = rm -f


# --- Rules ---

# The default goal is 'all', which builds the target executable.
# The .PHONY directive tells make that 'all' is not a file.
.PHONY: all test viture_sdk test_conversions
all: $(TARGET)

test: $(TARGET_TEST)

viture_sdk: $(TARGET_VITURE_SDK)

test_conversions: $(TARGET_TEST_CONVERSIONS)

# Rule to link the object files into the final executable.
# The executable depends on all the object files.
$(TARGET): $(filter v4l2_gl.o viture_connection.o utility.o, $(OBJS))
	@echo "==> Linking $(TARGET)..."
	$(CC) -o $(TARGET) $(filter v4l2_gl.o viture_connection.o utility.o, $(OBJS)) $(LIBS)
	@echo "==> Build complete: ./"$(TARGET)

$(TARGET_VITURE_SDK): v4l2_gl_viture_sdk.o utility.o
	@echo "==> Linking $(TARGET_VITURE_SDK)..."
	$(CC) -o $(TARGET_VITURE_SDK) v4l2_gl_viture_sdk.o utility.o $(VITURE_LIB) $(LIBS)
	@echo "==> Build complete: ./"$(TARGET_VITURE_SDK)


# Rule to link the test_viture executable
$(TARGET_TEST): $(filter test_viture.o viture_connection.o utility.o, $(OBJS_TEST))
	@echo "==> Linking $(TARGET_TEST)..."
	$(CC) -o $(TARGET_TEST) $(filter test_viture.o viture_connection.o utility.o , $(OBJS_TEST)) $(LIBS_TEST)
	@echo "==> Build complete: ./"$(TARGET_TEST)

# Rule to link the test_viture executable
$(TARGET_TEST_CONVERSIONS): test_conversions.c utility.c
	@echo "==> Linking $(TARGET_TEST_CONVERSIONS)..."
	$(CC) -g -msse4.1 -o $(TARGET_TEST_CONVERSIONS) test_conversions.c utility.c $(CFLAGS) $(LIBS_TEST_CONVERSIONS)
	@echo "==> Build complete: ./"$(TARGET_TEST_CONVERSIONS)



# Pattern rule to compile .c files into .o files.
# For any .o file, make will find the corresponding .c file
# and use this recipe to build it.
# This rule will be used for all .c files including test_viture.c and viture_connection.c
%.o: %.c
	@echo "==> Compiling $<..."
	$(CC) $(CFLAGS) -I. -c -o $@ $< 

v4l2_gl_viture_sdk.o: v4l2_gl.c
	@echo "==> Compiling v4l2_gl_viture_sdk.o..."
	$(CC) $(CFLAGS) -DUSE_VITURE -I. -c -o $@ v4l2_gl.c 

# The 'clean' rule removes all generated files.
# .PHONY tells make that 'clean' is not a file.
.PHONY: clean
clean:
	@echo "==> Cleaning up..."
	$(RM) $(TARGET) $(TARGET_TEST) $(TARGET_VITURE_SDK) $(TARGET_TEST_CONVERSIONS) $(OBJS) $(OBJS_TEST)
	@echo "==> Done."
