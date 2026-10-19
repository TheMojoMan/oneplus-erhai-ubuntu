# 让 Video Decode 工作起来

[English version](video-decode-setup.en.md)

本文说明如何在 Xiaomi Pad 5（`nabu`、SM8150、ARM64 Ubuntu）上打通完整的
硬件视频解码链路：

```text
Chrome / FFmpeg
       ↓ VA-API
iris-vaapi (iris_drv_video.so)
       ↓ stateful V4L2
qcom-iris (/dev/video0)
       ↓
SM8150 Iris1 / Venus 固件
```

最终应同时满足：

- `uname -m` 输出 `aarch64`；
- `/dev/video0` 是 `Iris Decoder`；
- `/dev/dri/renderD128` 和 `/dev/dma_heap/system` 可访问；
- `cached_capture` 与 `allow_fw_boot` 均为 `Y`；
- `vainfo` 加载 `iris-vaapi 0.1.0`，列出 H.264、HEVC Main/Main10 和 VP9
  Profile 0/Profile 2；
- FFmpeg 或 Chrome 实际选择 VA-API，而不是软件解码器。

> 这是实验性驱动。首次安装需要构建并启动自定义内核和 DTB，请先准备可用的
> 恢复启动项。不要把为其他内核构建的 `.ko` 强行装入当前系统。

## 1. 支持范围

| 项目 | 当前支持 |
|---|---|
| 设备 | Xiaomi Pad 5 (`nabu`) |
| 架构 | ARM64 / `aarch64` |
| SoC / VPU | Qualcomm SM8150 / Iris1 |
| H.264 | Constrained Baseline、Main、High，NV12 |
| HEVC | Main 8-bit/NV12；Main10/P010（实验） |
| VP9 | Profile 0/NV12；Profile 2/P010（实验） |
| 暂不支持 | AV1 |

内核和用户态驱动必须配套：

- [`nabu-iris`](https://github.com/CFM880/nabu-iris)：内核、DT、固件和模块参数；
- [`iris-vaapi`](https://github.com/CFM880/iris-vaapi)：VA-API 用户态驱动。

## 2. 首次部署内核

### 2.1 获取精确内核基线和源码覆盖层

```sh
git clone https://gitlab.postmarketos.org/soc/qualcomm-sm8150/linux.git linux
git -C linux checkout 5181e1358ddd6ea8028e841d928942373e6aebc8

git clone https://github.com/CFM880/nabu-iris.git
./nabu-iris/scripts/apply-overlay.sh ./linux
```

`apply-overlay.sh` 复制 65 个直接源码文件，不应用 patch。脚本会拒绝错误的基线或
不干净的目标工作树。

### 2.2 配置内核

沿用设备上可启动的 nabu 配置，并确认至少包含：

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

例如：

```sh
mkdir -p linux/out
cp /boot/config-"$(uname -r)" linux/out/.config
make -C linux O="$PWD/linux/out" ARCH=arm64 olddefconfig
```

### 2.3 构建和启动完整内核

首次部署必须构建并安装完整内核、模块和 nabu DTB，因为覆盖层还修改了设备树、
SM8150 video clock 和 Venus 协调代码。只复制 `qcom-iris.ko` 不能让普通发行版内核
凭空获得这些改动。

通用构建命令为：

```sh
make -C linux O="$PWD/linux/out" ARCH=arm64 -j"$(nproc)" Image dtbs modules
```

在 x86 主机交叉编译时追加：

```sh
CROSS_COMPILE=aarch64-linux-gnu-
```

内核、DTB、模块、initramfs 和引导项的安装方式取决于当前 Ubuntu/nabu 启动方案。
安装后重启，并先确认：

```sh
uname -m
uname -r
```

预期为 `aarch64` 和刚安装的 nabu Iris 内核版本。

## 3. 安装 Venus 固件

`nabu-iris` 保留了已验证固件：

```sh
sudo install -Dm0644 nabu-iris/firmware/venus.mbn \
  /usr/lib/firmware/qcom/sm8150/xiaomi/nabu/venus.mbn

sha256sum /usr/lib/firmware/qcom/sm8150/xiaomi/nabu/venus.mbn
```

预期 SHA-256：

```text
9d4af65d7ede845e900f1b29ff425b7a8e2947056e695e246e58a2091445a085
```

如果模块会进入 initramfs，再执行：

```sh
sudo update-initramfs -u
```

固件有独立的权利声明，参见 `nabu-iris/firmware/NOTICE.md`。

## 4. 启用 qcom-iris

安装推荐模块参数：

```sh
sudo install -m0644 nabu-iris/system/qcom-iris.conf \
  /etc/modprobe.d/qcom-iris.conf
```

其内容为：

```text
options qcom_iris allow_fw_boot=1 cached_capture=1
```

- `allow_fw_boot=1`：允许打开视频节点时启动 Iris 固件；
- `cached_capture=1`：为 H.264/HEVC/VP9 使用 cacheable CAPTURE，4K 播放必需。

关闭所有浏览器和解码进程后重载模块：

```sh
sudo modprobe -r qcom_iris
sudo modprobe qcom_iris
```

检查参数：

```sh
cat /sys/module/qcom_iris/parameters/allow_fw_boot
cat /sys/module/qcom_iris/parameters/cached_capture
```

两项都应输出 `Y`。

检查设备：

```sh
ls -l /dev/video0 /dev/dri/renderD128 /dev/dma_heap/system
v4l2-ctl -d /dev/video0 --all
```

关键输出应包括：

```text
Driver name : iris_driver
Card type   : Iris Decoder
Video Memory-to-Memory Multiplanar
Streaming
```

若没有 `/dev/video0`，先看：

```sh
sudo dmesg | grep -iE 'iris|venus|firmware|video-codec'
```

## 5. 构建并安装 iris-vaapi

安装依赖：

```sh
sudo apt update
sudo apt install build-essential pkg-config libva-dev libdrm-dev vainfo \
  v4l-utils ffmpeg
```

构建：

```sh
git clone https://github.com/CFM880/iris-vaapi.git
cd iris-vaapi
make -j"$(nproc)"
make check
```

先从构建目录测试，不覆盖系统文件：

```sh
LIBVA_DRIVER_NAME=iris \
LIBVA_DRIVERS_PATH="$PWD/build" \
vainfo --display drm --device /dev/dri/renderD128
```

预期：

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

安装到系统：

```sh
sudo ./install-system.sh
```

该脚本同时安装：

- `iris_drv_video.so`；
- `/dev/dma_heap/system` 的 udev 权限规则；
- H.264/HEVC/VP9 的 `cached_capture` modprobe 配置。

把当前用户加入视频和渲染设备组，然后重新登录：

```sh
sudo usermod -aG video,render "$USER"
```

重新登录后确认不需要 `sudo` 即可访问三个节点，再运行：

```sh
LIBVA_DRIVER_NAME=iris vainfo --display drm --device /dev/dri/renderD128
```

## 6. 分层验证解码

应按“V4L2 → VA-API → Chrome”的顺序验证。底层失败时直接调 Chrome只会得到模糊的
软件回退现象。

### 6.1 V4L2 直接解码

测试程序需要 Annex-B elementary stream，而不是 MP4 容器。可从 MP4 提取：

```sh
ffmpeg -i input.mp4 -map 0:v:0 -c:v copy -bsf:v h264_mp4toannexb test.h264
```

运行：

```sh
./build/test_v4l2_dec test.h264 1920 1080
```

能持续输出帧且无 timeout，说明固件、内核和 `/dev/video0` 基本正常。

### 6.2 FFmpeg VA-API

FFmpeg 可以直接读取 MP4/MKV/WebM：

```sh
IRIS_VAAPI_STATS=1 \
LIBVA_DRIVER_NAME=iris \
ffmpeg -v verbose \
  -hwaccel vaapi \
  -vaapi_device /dev/dri/renderD128 \
  -i /path/to/video.mp4 \
  -an -f null -
```

若要测试尚未安装的构建，再加：

```sh
LIBVA_DRIVERS_PATH="$PWD/build"
```

看到驱动加载、帧数持续增长且进程正常结束，说明 VA-API 路径已打通。

### 6.3 Chrome

先完全关闭所有 Chrome 窗口和后台进程，确保新 GPU 进程继承环境变量：

```sh
export LIBVA_DRIVER_NAME=iris
google-chrome --enable-features=VaapiVideoDecoder
```

需要驱动日志时：

```sh
IRIS_VAAPI_DEBUG=1 \
LIBVA_DRIVER_NAME=iris \
google-chrome --enable-features=VaapiVideoDecoder \
  --enable-logging=stderr
```

播放视频后检查：

1. `chrome://media-internals`：`kVideoDecoderName` 应是 VA-API 解码器，而不是
   `FFmpegVideoDecoder`；
2. `chrome://gpu`：Video Acceleration 应列出 H.264/HEVC/VP9；
3. 视频实际编码必须在支持范围内。AV1 会正常回退软件解码。

默认不要设置 `IRIS_DIRECT_CAPTURE`。它仍是实验路径；正常发布配置使用稳定
DMA-BUF surface 拷贝路径。

## 7. 常见问题

### `vainfo` 仍显示旧版本

系统中仍是旧的 `iris_drv_video.so`，或 Chrome 没有重启：

```sh
pkg-config --variable=driverdir libva
LIBVA_DRIVER_NAME=iris vainfo --display drm --device /dev/dri/renderD128
```

重新执行 `make && sudo ./install-system.sh`，再完全退出并重启 Chrome。

### `vainfo` 找不到 iris 驱动

```sh
find /usr/lib -name iris_drv_video.so -print
pkg-config --variable=driverdir libva
```

驱动必须位于当前 libva 返回的 driver directory。

### Chrome 立即回退 `FFmpegVideoDecoder`

依次检查：

```sh
test -r /dev/video0 && test -w /dev/video0
test -r /dev/dri/renderD128 && test -w /dev/dri/renderD128
test -r /dev/dma_heap/system && test -w /dev/dma_heap/system
cat /sys/module/qcom_iris/parameters/cached_capture
```

Chrome 需要真正的 DMA-BUF。若 `/dev/dma_heap/system` 不可访问，驱动会回退
memfd；FFmpeg CPU readback 可能仍工作，但 Chrome/EGL 无法导入该 surface。

### 4K 一卡一卡、CPU 占用高

确认 `cached_capture=Y`。未启用时，stable-surface 路径读取 coherent CAPTURE
会被大帧拷贝拖慢。修改配置后必须重载模块或重启，仅重新安装 VA 驱动无效。

### 播放结束卡住、seek 后固件超时

先关闭占用 `/dev/video0` 的程序，再恢复模块：

```sh
sudo modprobe -r qcom_iris
sudo modprobe qcom_iris
```

若 `modprobe -r` 报 busy，用下面的命令寻找占用者：

```sh
sudo fuser -v /dev/video0
```

### 固件加载失败

```sh
sha256sum /usr/lib/firmware/qcom/sm8150/xiaomi/nabu/venus.mbn
sudo dmesg | grep -iE 'iris|firmware'
```

检查文件路径、SHA-256、DTB 是否来自相同 `nabu-iris` 源码，以及
`allow_fw_boot=Y`。

### 安装新模块后 `Invalid module format`

模块与当前内核不匹配：

```sh
uname -r
modinfo -F vermagic /path/to/qcom-iris.ko
```

两者的版本和 ABI 必须匹配。不要使用 `--force` 绕过检查，应为当前内核重新构建。

## 8. 后续更新顺序

首次完整部署成功后，日常更新建议按以下顺序：

1. 更新并构建 `nabu-iris` 对应内核/模块；
2. 重启到新内核，或在 ABI 完全匹配时更新模块并重载；
3. 确认 `/dev/video0`、固件和两个模块参数；
4. 更新 `iris-vaapi`，执行 `make check` 和 `sudo ./install-system.sh`；
5. 先跑 FFmpeg VA-API，再启动 Chrome。

这样可以明确区分问题位于固件、内核 V4L2、VA-API 还是浏览器集成层。
