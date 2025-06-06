# V4L2 OpenGL Real-time Viewer

**Attention** this is still in development and not fully functional yet and only compiles on X86 systems not on the OrangePi yet because of the viture library which does not work on ARM.

v4l2_gl captures video from a HDMI-in on an OrangePI Plus using the hdmirx V4L2 device, converts frames to RGB, and displays them in real-time on a textured quad in an OpenGL window. It supports Viture headset IMU integration, test patterns, and plane geometry.

## Dependencies

This requires a linux based OS and installed gcc and build tools.

To compile and run this application, ensure the following libraries are installed:

-   **OpenGL and GLUT libraries**:
    These are essential for creating the window and rendering graphics. On Debian/Ubuntu-based systems, you can install them using:
    ```
    sudo apt update
    sudo apt install freeglut3-dev
    ```

-   **libv4l2 library**:
    This library is required for interacting with V4L2 devices. On Debian/Ubuntu-based systems, install it with:
    ```
    sudo apt update
    sudo apt install libv4l-dev
    ```

## Compilation

### Using Makefile
```
make
```

## Running the viewer

Execute the compiled application from your terminal:
```
./v4l2_gl [options]
```

### Command-Line Options

The application supports the following command-line options:

-   **`-f`** or **`--fullscreen`**:
    Runs the application in fullscreen mode.
    Example: `./v4l2_gl -f`

-   **`--viture`**:
    Enables integration with Viture headset IMU for controlling the rotation of the displayed plane. The Viture SDK and device must be correctly set up.
    Example: `./v4l2_gl --viture`

-   **`-tp`** or **`--test-pattern`**:
    Displays a generated test pattern on the plane instead of the live camera feed. Useful for testing rendering and transformations.
    Example: `./v4l2_gl -tp`

-   **`-pd <distance>`** or **`--plane-distance <distance>`**:
    Sets the distance at which the plane orbits the world origin. `<distance>` is a floating-point value.
    Default: `1.0` (if not specified). A value of `0.0` places the plane at the origin.
    Example: `./v4l2_gl -pd 0.5` (plane orbits at 0.5 units from the origin)

-   **`-ps <scale>`** or **`--plane-scale <scale>`**:
    Sets a scale multiplier for the size of the plane. `<scale>` is a floating-point value.
    Default: `1.0` (original size). Values greater than 1.0 enlarge the plane, less than 1.0 shrink it. Negative or zero values are reset to 1.0.
    Example: `./v4l2_gl -ps 1.5` (plane is 50% larger)

### Combined Example

You can combine these options:
```bash
./v4l2_gl --viture -f -pd 0.8 -ps 1.2 -tp
```
This command would:
- Enable Viture IMU.
- Run in fullscreen.
- Set the plane orbit distance to 0.8 units.
- Scale the plane to 120% of its original size.
- Display the test pattern.


## TODO

* Access the Viture IMU directly through hidapi to go around the limitation of Viture SDK only being available for X86