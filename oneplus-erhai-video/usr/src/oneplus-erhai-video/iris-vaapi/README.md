# iris-vaapi

面向 Qualcomm SM8150 Iris1 的实验性 VA-API 驱动。它把 Chrome、FFmpeg 等
VA-API 客户端提交的参数和 slice 重组成完整访问单元，再交给 `qcom-iris`
stateful V4L2 解码器。

当前版本：`0.1.0`。

> 项目仍处于实验阶段，目前只针对 Xiaomi Pad 5（`nabu`）及配套内核验证。

## 功能状态

| 格式 | VA-API profile | 输出 | 状态 |
|---|---|---|---|
| H.264 | Constrained Baseline / Main / High | NV12 | 已验证 |
| HEVC | Main / Main10 | NV12 / P010 | 已验证（8/10-bit） |
| VP9 | Profile 0 / Profile 2 | NV12 / P010 | 已验证（8/10-bit） |

HEVC Main10 与 VP9 Profile 2 已分别通过三轮 4K P010 完整码流测试，所有帧均
成功输出且没有 corrupt frame，因此按 VA-API 解码能力计为已支持。H.264、HEVC
与 VP9 支持 Chrome 所需的稳定 DMA-BUF surface、异步 fence、解码顺序输出和
片尾帧释放。实验性的 V4L2 CAPTURE 直连可通过
`IRIS_DIRECT_CAPTURE=1` 启用，默认仍使用已充分验证的稳定 surface 拷贝路径。

## 依赖

- Linux ARM64，带匹配的 `qcom-iris` 驱动；
- VA-API 1.23 ABI（已使用 libva 2.22/2.23 验证）；
- GCC、pkg-config、make；
- 可选：`libvulkan-dev`（实验性的 Turnip DMA-BUF 异步复制）；
- `/dev/video0`、DRM render node 和 `/dev/dma_heap/system`。

在 Debian/Ubuntu 系统上：

```sh
sudo apt install build-essential pkg-config libva-dev libdrm-dev vainfo \
  libvulkan-dev
```

内核侧源码和构建方法位于
[`nabu-iris`](https://github.com/CFM880/nabu-iris)。H.264/HEVC/VP9 4K 播放应启用
统一参数：

```text
options qcom_iris cached_capture=1
```

## 构建

```sh
git clone https://github.com/CFM880/iris-vaapi.git
cd iris-vaapi
make -j"$(nproc)"
LIBVA_DRIVER_NAME=iris LIBVA_DRIVERS_PATH="$PWD/build" vainfo
```

预期能看到 H.264、HEVC Main/Main10 和 VP9 Profile 0/Profile 2 的
`VAEntrypointVLD`。

驱动会按 `/dev/video0` 实际枚举的 CAPTURE 格式公布能力。若内核模块尚未更新、
未提供 P010，Main10 和 Profile 2 会被隐藏，避免客户端逐帧尝试失败后再回退软件
解码；此时应先更新并重载配套的 `nabu-iris` 模块。

无需安装即可让 FFmpeg 使用当前构建：

```sh
LIBVA_DRIVER_NAME=iris LIBVA_DRIVERS_PATH="$PWD/build" \
ffmpeg -hwaccel vaapi -vaapi_device /dev/dri/renderD128 \
  -i /path/to/video.mp4 -an -f null -
```

H.264 对未导出的 VA surface 使用异步提交以保持硬件解码流水线；已经通过
DRM PRIME 导出的 surface 继续在 `vaEndPicture` 做兼容性等待，避免旧 Adreno
显示上一帧。诊断时可设置 `IRIS_H264_SYNC_END=1`，恢复所有 H.264 surface 的
逐帧同步行为。

设置 `IRIS_VULKAN_COPY=1` 可启用实验性的 Turnip DMA-BUF copy engine：V4L2
CAPTURE buffer 通过 `VIDIOC_EXPBUF` 导出，Vulkan 异步复制到 Chrome 已导入的
稳定 surface，GPU 完成后才 signal surface fence 并回收 CAPTURE buffer。Vulkan
不可用或提交失败时会自动回退 CPU `memcpy`。当前仍默认使用 CPU 路径，先完成
Chrome 长时间播放和多标签验证后再考虑默认启用。
同一 VA display 的解码 context 共享一套 Vulkan instance/device，因此 seek、
换流和多标签不会重复初始化 Turnip。SM8150 专用启动环境还可设置
`VK_DRIVER_FILES=/usr/share/vulkan/icd.d/freedreno_icd.json`，避免加载无关 ICD。

## 系统安装

```sh
make
sudo ./install-system.sh
```

脚本从当前仓库的 `build/` 安装驱动，并安装 DMA-heap udev 规则及
`cached_capture` modprobe 配置。模块重新加载或重启后生效：

```sh
cat /sys/module/qcom_iris/parameters/cached_capture
LIBVA_DRIVER_NAME=iris vainfo
```

## Chrome

确保浏览器 GPU 进程继承驱动选择：

```sh
export LIBVA_DRIVER_NAME=iris
google-chrome --enable-features=VaapiVideoDecoder
```

在 `chrome://media-internals` 中检查 `kVideoDecoderName`，在 `chrome://gpu` 中
检查 Video Acceleration。Chrome 版本和发行版启动参数可能不同；驱动侧跟踪可用：

```sh
IRIS_VAAPI_DEBUG=1 google-chrome --enable-logging=stderr
```

## 测试

纯参数重建测试不需要硬件：

```sh
make check
```

硬件测试需要用户提供码流，不包含任何本机绝对路径：

```sh
./build/test_v4l2_dec stream.h264 3840 2160
./build/test_va_decode stream.h264
./build/test_va_stress stream.h264 700
./build/test_hevc_au stream.h265
./build/test_va_vp9 stream.ivf
./build/test_hevc_au main10.h265 3840 2160 p010
./build/test_v4l2_vp9 profile2.ivf 3840 2160 p010
./build/test_surface_fence /dev/video0
```

## 性能对比

以下为 Xiaomi Pad 5（SM8150）4K60 完整码流的最大吞吐测试。CPU 以单核满载
为 100%，RSS 为进程峰值；同一规格内交替运行 V4L2 与 VA-API。H.264 使用当前
异步提交实现，其余结果来自初始完整基准。

| 编码规格 | 位深 | 路径 | 解码速度 | CPU | 峰值 RSS |
|---|---:|---|---:|---:|---:|
| H.264 High | 8-bit | V4L2 | 167.55 fps | 30.24% | 415.84 MiB |
| H.264 High | 8-bit | VA-API | 166.95 fps | 59.28% | 544.30 MiB |
| HEVC Main | 8-bit | V4L2 | 176.30 fps | 31.71% | 411.63 MiB |
| HEVC Main | 8-bit | VA-API | 173.83 fps | 52.29% | 675.49 MiB |
| VP9 Profile 0 | 8-bit | V4L2 | 146.45 fps | 35.81% | 516.83 MiB |
| VP9 Profile 0 | 8-bit | VA-API | 144.49 fps | 44.91% | 556.79 MiB |
| HEVC Main10 | 10-bit | V4L2 P010 | 148.29 fps | 26.54% | 798.35 MiB |
| HEVC Main10 | 10-bit | VA-API | 148.55 fps | 85.70% | 937.84 MiB |
| VP9 Profile 2 | 10-bit | V4L2 P010 | 116.33 fps | 20.51% | 739.11 MiB |
| VP9 Profile 2 | 10-bit | VA-API | 113.82 fps | 42.09% | 798.59 MiB |

当前五种规格的 VA-API 与 V4L2 吞吐差均不超过 2.2%。H.264 异步优化将
VA-API 从 65.55 fps 提升到 166.95 fps；虽然最大吞吐时 CPU 占用率提高，但
完整码流总 CPU 时间从 8.66 秒降至 6.38 秒。实验性的 Vulkan copy 路径已经把
stable-surface CPU 整帧复制降为 0；H.264 4K 的 120 帧强制读回校验与 CPU
路径逐帧一致，总 CPU 时间降低约 7%，墙钟时间增加约 2%。600 帧热态 null sink
测试总 CPU 时间约从 2.69 秒降至 2.47 秒。默认 Vulkan loader 扫描全部 ICD 时
Turnip 路径令 RSS 约增加 64 MiB；限定 Freedreno ICD 后本机增量约为 15 MiB。
首次 Turnip 初始化仍有明显 CPU 成本，因此它目前主要用于验证“用 GPU 换 CPU”
的方向，还不是默认配置。

原始数据和可复现脚本位于 [`benchmark-results/`](benchmark-results/) 与
[`benchmarks/`](benchmarks/)；10-bit V4L2 测试程序会映射整个输入文件，因此
表中的原始 RSS 包含输入文件大小，CSV 中另有扣除输入映射后的修正值。

详细的实现过程、基准和历史排障记录保留在
[`docs/development-notes.md`](docs/development-notes.md)。Chromium 交互点见
[`docs/chromium-integration.md`](docs/chromium-integration.md)。

从全新系统打通内核、固件、VA-API、FFmpeg 和 Chrome 的完整步骤见
[`中文`](docs/video-decode-setup.md) / [`English`](docs/video-decode-setup.en.md)。

## 已知限制

- 10-bit P010 解码需要配套的 `nabu-iris` 内核模块；HDR 元数据、色调映射和
  最终显示效果仍取决于 Chrome、合成器和显示器链路；
- FFmpeg 的 `hevc_v4l2m2m` 与 `vp9_v4l2m2m` 前端目前不会为 10-bit 输入选择
  P010 CAPTURE，可能成功退出但输出 0 帧；VA-API 路径和仓库内显式 P010 的
  V4L2 测试程序不受此限制；
- 默认 stable-surface 路径会做一次 CAPTURE 到 DMA-BUF 的 CPU 拷贝；配套内核的
  `cached_capture=1` 会加速 H.264/HEVC/VP9 的 MMAP CAPTURE 读取；可用
  `IRIS_VULKAN_COPY=1` 改为实验性的异步 GPU 复制，但会增加 Turnip 内存开销；
- `IRIS_DIRECT_CAPTURE` 仍是实验功能，不建议作为默认发布配置；
- 非正常终止旧内核会话可能使固件超时，需要重载 `qcom_iris`；
- 需要与本仓库功能匹配的 `nabu-iris` 内核模块，单独替换用户态驱动不够。

## 许可证

本项目以 GPL-2.0-or-later 发布，详见 [COPYING](COPYING)。
