# Third-Party Libraries

The Viture SDK shared libraries cannot be bundled in this repository due to
licensing restrictions. You must download them separately from the Viture
developer portal and place them in the correct directories before building the
`viture_sdk` target.

## Download

Go to the [Viture Developer Portal](https://www.viture.com/developer) and
download the **Linux SDK** package for your architecture.

## Required directory structure

After unpacking, the files must be arranged as follows:

```
3rdparty/
└── release/
    ├── include/                  # SDK header files
    │   ├── viture_glasses_provider.h
    │   ├── viture_device.h
    │   ├── viture_device_carina.h
    │   ├── viture_protocol_public.h
    │   ├── viture_macros_public.h
    │   └── viture_version.h
    ├── x86_64/                   # Shared libraries for x86_64
    │   ├── libglasses.so
    │   ├── libcarina_vio.so
    │   └── libopencv_*.so*       # OpenCV libs bundled with the SDK
    └── aarch64/                  # Shared libraries for aarch64 (ARM)
        ├── libglasses.so
        └── libcarina_vio.so
```

## Setup steps

1. Download the SDK archive from the developer portal.
2. Extract the archive.
3. Copy the header files into `3rdparty/release/include/`.
4. Copy the shared libraries for your architecture into the appropriate directory:
   - `3rdparty/release/x86_64/` for x86_64
   - `3rdparty/release/aarch64/` for aarch64 / ARM64

## Building

Once the libraries are in place, build with:

```bash
make viture_sdk
```

## Running

Use the provided launcher script, which sets `LD_LIBRARY_PATH` so that the
bundled shared libraries (including transitive OpenCV dependencies) are found
at runtime:

```bash
./run_viture_sdk.sh [options]
```
