#!/bin/bash
# Launcher for v4l2_gl_viture_sdk
# Sets LD_LIBRARY_PATH to include the bundled SDK libs so that all
# transitive OpenCV/Carina dependencies are resolved from the 3rdparty tree.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

ARCH="$(uname -m)"
case "$ARCH" in
    x86_64)  LIB_DIR="$SCRIPT_DIR/3rdparty/release/x86_64" ;;
    aarch64) LIB_DIR="$SCRIPT_DIR/3rdparty/release/aarch64" ;;
    *)
        echo "Unsupported architecture: $ARCH" >&2
        exit 1
        ;;
esac

export LD_LIBRARY_PATH="$LIB_DIR${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
exec "$SCRIPT_DIR/v4l2_gl_viture_sdk" "$@"
