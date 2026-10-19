# Getting Video Decode Working

[Chinese version](video-decode-setup.md)

This guide explains how to bring up the complete hardware video decode stack on an
ARM64 Ubuntu installation running on the Xiaomi Pad 5 (`nabu`, SM8150):

```text
Chrome / FFmpeg
       ↓ VA-API
iris-vaapi (iris_drv_video.so)
       ↓ stateful V4L2
qcom-iris (/dev/video0)
       ↓
SM8150 Iris1 / Venus firmware
```

A working installation satisfies all of the following conditions:

- `uname -m` prints `aarch64`;
- `/dev/video0` identifies itself as `Iris Decoder`;
- `/dev/dri/renderD128` and `/dev/dma_heap/system` are accessible;
- both `cached_capture` and `allow_fw_boot` are `Y`;
- `vainfo` loads `iris-vaapi 0.1.0` and lists H.264, HEVC Main/Main10, and VP9
  Profile 0/Profile 2;
- FFmpeg or Chrome actually selects VA-API instead of a software decoder.

> This is an experimental driver. The initial installation requires a custom kernel and
> DTB. Prepare a known-good recovery boot entry before replacing them. Never force-load a
> `.ko` built for a different kernel.

## 1. Supported configuration

| Item | Current support |
|---|---|
| Device | Xiaomi Pad 5 (`nabu`) |
| Architecture | ARM64 / `aarch64` |
| SoC / VPU | Qualcomm SM8150 / Iris1 |
| H.264 | Constrained Baseline, Main, High; NV12 output |
| HEVC | Main 8-bit/NV12; experimental Main10/P010 |
| VP9 | Profile 0/NV12; experimental Profile 2/P010 |
| Not supported yet | AV1 |

The kernel and userspace driver must be used together:

- [`nabu-iris`](https://github.com/CFM880/nabu-iris) provides the kernel, DT changes,
  firmware, and module configuration;
- [`iris-vaapi`](https://github.com/CFM880/iris-vaapi) provides the VA-API userspace
  driver.

## 2. Initial kernel deployment

### 2.1 Get the exact kernel base and source overlay

```sh
git clone https://gitlab.postmarketos.org/soc/qualcomm-sm8150/linux.git linux
git -C linux checkout 5181e1358ddd6ea8028e841d928942373e6aebc8

git clone https://github.com/CFM880/nabu-iris.git
./nabu-iris/scripts/apply-overlay.sh ./linux
```

`apply-overlay.sh` copies 65 kernel source files directly. It does not apply patches and
refuses to overwrite a dirty tree or a tree at the wrong base commit.

### 2.2 Configure the kernel

Start with a known-bootable nabu configuration and verify that it contains at least:

```text
CONFIG_MEDIA_SUPPORT=m
CONFIG_VIDEO_DEV=m
CONFIG_VIDEO_QCOM_IRIS=m
CONFIG_VIDEOBUF2_CORE=m
CONFIG_VIDEOBUF2_V4L2=m
CONFIG_VIDEOBUF2_DMA_CONTIG=m
CONFIG_DMA_SHARED_BUFFER=y
CONFIG_DMABUF_HEAPS=y
CONFIG_DMABUF_HEAPS_SYSTEM=y
```

For example:

```sh
mkdir -p linux/out
cp /boot/config-"$(uname -r)" linux/out/.config
make -C linux O="$PWD/linux/out" ARCH=arm64 olddefconfig
```

### 2.3 Build and boot the complete kernel

The first deployment must include the complete kernel, modules, and nabu DTB. The overlay
also changes the device tree, SM8150 video clocks, and Venus coordination code. Copying
only `qcom-iris.ko` into an otherwise stock distribution kernel is not sufficient.

A generic build command is:

```sh
make -C linux O="$PWD/linux/out" ARCH=arm64 -j"$(nproc)" Image dtbs modules
```

When cross-compiling on an x86 host, add:

```sh
CROSS_COMPILE=aarch64-linux-gnu-
```

Installing the kernel, DTB, modules, initramfs, and boot entry depends on the Ubuntu/nabu
boot setup in use. After installation, reboot and verify:

```sh
uname -m
uname -r
```

The first command must print `aarch64`; the second must identify the newly installed nabu
Iris kernel.

## 3. Install the Venus firmware

`nabu-iris` contains the firmware used by the validated system:

```sh
sudo install -Dm0644 nabu-iris/firmware/venus.mbn \
  /usr/lib/firmware/qcom/sm8150/xiaomi/nabu/venus.mbn

sha256sum /usr/lib/firmware/qcom/sm8150/xiaomi/nabu/venus.mbn
```

Expected SHA-256:

```text
9d4af65d7ede845e900f1b29ff425b7a8e2947056e695e246e58a2091445a085
```

If the module is included in the initramfs, update it as well:

```sh
sudo update-initramfs -u
```

The firmware has separate rights and redistribution considerations. See
`nabu-iris/firmware/NOTICE.md`.

## 4. Enable qcom-iris

Install the recommended module configuration:

```sh
sudo install -m0644 nabu-iris/system/qcom-iris.conf \
  /etc/modprobe.d/qcom-iris.conf
```

Its contents are:

```text
options qcom_iris allow_fw_boot=1 cached_capture=1
```

- `allow_fw_boot=1` permits the Iris firmware to start when the video node is opened;
- `cached_capture=1` enables cacheable H.264/HEVC/VP9 CAPTURE buffers and is required for
  practical 4K performance.

Close every browser and decoder using the device, then reload the module:

```sh
sudo modprobe -r qcom_iris
sudo modprobe qcom_iris
```

Verify both parameters:

```sh
cat /sys/module/qcom_iris/parameters/allow_fw_boot
cat /sys/module/qcom_iris/parameters/cached_capture
```

Both commands should print `Y`.

Check the device nodes and V4L2 capabilities:

```sh
ls -l /dev/video0 /dev/dri/renderD128 /dev/dma_heap/system
v4l2-ctl -d /dev/video0 --all
```

The important fields are:

```text
Driver name : iris_driver
Card type   : Iris Decoder
Video Memory-to-Memory Multiplanar
Streaming
```

If `/dev/video0` is missing, inspect the kernel log first:

```sh
sudo dmesg | grep -iE 'iris|venus|firmware|video-codec'
```

## 5. Build and install iris-vaapi

Install the build and diagnostic dependencies:

```sh
sudo apt update
sudo apt install build-essential pkg-config libva-dev libdrm-dev vainfo \
  v4l-utils ffmpeg
```

Build the driver and run the hardware-independent tests:

```sh
git clone https://github.com/CFM880/iris-vaapi.git
cd iris-vaapi
make -j"$(nproc)"
make check
```

Test the build directory before replacing any system file:

```sh
LIBVA_DRIVER_NAME=iris \
LIBVA_DRIVERS_PATH="$PWD/build" \
vainfo --display drm --device /dev/dri/renderD128
```

Expected profiles:

```text
Driver version: iris-vaapi: Qualcomm Iris SM8150 (V4L2) 0.1.0
VAProfileH264ConstrainedBaseline: VAEntrypointVLD
VAProfileH264Main:                VAEntrypointVLD
VAProfileH264High:                VAEntrypointVLD
VAProfileHEVCMain:                VAEntrypointVLD
VAProfileHEVCMain10:              VAEntrypointVLD
VAProfileVP9Profile0:             VAEntrypointVLD
VAProfileVP9Profile2:             VAEntrypointVLD
```

Install it system-wide:

```sh
sudo ./install-system.sh
```

The script installs:

- `iris_drv_video.so`;
- a udev rule granting access to `/dev/dma_heap/system`;
- the `cached_capture` modprobe configuration for H.264, HEVC, and VP9.

Add the current user to the video-device groups and log in again:

```sh
sudo usermod -aG video,render "$USER"
```

After logging in again, confirm that all three device nodes are accessible without
`sudo`, then run:

```sh
LIBVA_DRIVER_NAME=iris vainfo --display drm --device /dev/dri/renderD128
```

## 6. Validate one layer at a time

Validate in the order V4L2 → VA-API → Chrome. Starting with Chrome hides most lower-level
errors behind a generic software-decoder fallback.

### 6.1 Direct V4L2 decode

The direct test expects an Annex-B elementary stream, not an MP4 container. Extract one
from an H.264 MP4 file with:

```sh
ffmpeg -i input.mp4 -map 0:v:0 -c:v copy -bsf:v h264_mp4toannexb test.h264
```

Run the V4L2 test:

```sh
./build/test_v4l2_dec test.h264 1920 1080
```

Continuous frame output without a timeout means that the firmware, kernel driver, and
`/dev/video0` path are basically working.

### 6.2 FFmpeg through VA-API

FFmpeg can read MP4, MKV, or WebM containers directly:

```sh
IRIS_VAAPI_STATS=1 \
LIBVA_DRIVER_NAME=iris \
ffmpeg -v verbose \
  -hwaccel vaapi \
  -vaapi_device /dev/dri/renderD128 \
  -i /path/to/video.mp4 \
  -an -f null -
```

To test an uninstalled build, also set:

```sh
LIBVA_DRIVERS_PATH="$PWD/build"
```

The VA driver should load, the frame counter should advance, and FFmpeg should exit
normally at end of stream.

### 6.3 Chrome

Completely close all Chrome windows and background processes first so that the new GPU
process inherits the environment:

```sh
export LIBVA_DRIVER_NAME=iris
google-chrome --enable-features=VaapiVideoDecoder
```

To enable driver-side diagnostics:

```sh
IRIS_VAAPI_DEBUG=1 \
LIBVA_DRIVER_NAME=iris \
google-chrome --enable-features=VaapiVideoDecoder \
  --enable-logging=stderr
```

While a video is playing, check:

1. `chrome://media-internals`: `kVideoDecoderName` should identify a VA-API decoder,
   not `FFmpegVideoDecoder`;
2. `chrome://gpu`: Video Acceleration should list H.264, HEVC, and VP9;
3. the actual stream codec must be supported. AV1 correctly falls back to software.

Do not set `IRIS_DIRECT_CAPTURE` for the normal configuration. Direct CAPTURE remains
experimental; the default stable DMA-BUF surface copy path is the supported setup.

## 7. Troubleshooting

### `vainfo` still reports an old driver

The installed `iris_drv_video.so` is stale, or Chrome has not restarted:

```sh
pkg-config --variable=driverdir libva
LIBVA_DRIVER_NAME=iris vainfo --display drm --device /dev/dri/renderD128
```

Run `make && sudo ./install-system.sh` again, then exit Chrome completely and restart it.

### `vainfo` cannot find the iris driver

```sh
find /usr/lib -name iris_drv_video.so -print
pkg-config --variable=driverdir libva
```

The driver must be installed in the driver directory reported by the active libva.

### Chrome immediately falls back to `FFmpegVideoDecoder`

Check every required device node:

```sh
test -r /dev/video0 && test -w /dev/video0
test -r /dev/dri/renderD128 && test -w /dev/dri/renderD128
test -r /dev/dma_heap/system && test -w /dev/dma_heap/system
cat /sys/module/qcom_iris/parameters/cached_capture
```

Chrome requires a real DMA-BUF. If `/dev/dma_heap/system` is inaccessible, iris-vaapi
falls back to memfd. FFmpeg CPU readback may still work, but Chrome/EGL cannot import that
surface.

### 4K video stutters and CPU usage is high

Confirm that `cached_capture=Y`. Without it, reading coherent CAPTURE memory for the
stable-surface copy path is too slow for large frames. Reloading the module or rebooting
is required after changing this option; reinstalling only iris-vaapi is not enough.

### Playback hangs at end of stream or firmware times out after seeking

Close programs holding `/dev/video0`, then recover the module:

```sh
sudo modprobe -r qcom_iris
sudo modprobe qcom_iris
```

If module removal reports that it is busy, identify the process with:

```sh
sudo fuser -v /dev/video0
```

### Firmware loading fails

```sh
sha256sum /usr/lib/firmware/qcom/sm8150/xiaomi/nabu/venus.mbn
sudo dmesg | grep -iE 'iris|firmware'
```

Check the path, SHA-256, matching `nabu-iris` DTB, and `allow_fw_boot=Y`.

### A new module fails with `Invalid module format`

The module does not match the running kernel:

```sh
uname -r
modinfo -F vermagic /path/to/qcom-iris.ko
```

The kernel release and ABI must match. Do not bypass this with a force option; rebuild the
module for the running kernel.

## 8. Recommended update order

After the first complete deployment works, use this order for later updates:

1. update and build the matching `nabu-iris` kernel or module;
2. boot the new kernel, or replace and reload only the module when its ABI exactly matches;
3. verify `/dev/video0`, the firmware, and both module parameters;
4. update `iris-vaapi`, run `make check`, and run `sudo ./install-system.sh`;
5. validate FFmpeg VA-API before starting Chrome.

Following the layers in this order makes it clear whether a failure belongs to the
firmware, kernel V4L2 driver, VA-API driver, or browser integration.
