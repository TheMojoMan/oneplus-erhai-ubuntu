# iris-vaapi: Chrome 硬件解码方案（历史开发记录）

> 本文保留开发阶段的实验、基准和排障过程，部分状态与命令已经过时。
> 当前构建、安装和限制以仓库根目录的 `README.md` 为准。

让 Chrome 在 Xiaomi Pad 5 (nabu, SM8150) 上使用 Iris 硬件解码的 VA-API 驱动。

## 背景与现状

| 项 | 状态 |
|---|---|
| 目标 | Chrome 通过 VA-API 使用 `qcom-iris` V4L2 硬件解码 H.264/HEVC/VP9 |
| 关键摩擦 | Chrome 走 VA-API 按 slice 喂数据；iris 是"整比特流" stateful 解码器（固件自解析头），不暴露 slice 级 V4L2 控件 |
| 方案 | 自写 VA-API 驱动：接收 slice → 重组成完整访问单元 → 喂 iris V4L2 队列 → NV12 映射回 VASurface |
| 依赖 | libva 2.23（已装）、libva-dev（已装）、vainfo（已装）、Chrome（已装） |

**实测基线（linux 6.14.11-nabu-iris1+）**：
- H.264 4K60: 固件瓶颈 ~49fps（12.3s/10s 媒体）
- HEVC 4K60: 快于实时（120Hz 面板 2x 速）
- VP9 4K60: ~实时

## 架构

```
Chrome (VaapiVideoDecoder)
   │  vaBeginPicture/vaRenderPicture/vaEndPicture
   │  (VAPictureParameterBufferH264 + VASliceDataBufferType)
   ▼
iris-vaapi 驱动 (iris_drv_video.so)
   │  ① 重序列化 SPS/PPS NAL (h264_params.c)
   │  ② slice 重组 → 完整 H.264 访问单元
   ▼
V4L2 解码引擎 (v4l2_dec.c)
   │  /dev/video0 OUTPUT 队列喂 AU，CAPTURE 队列收 NV12
   ▼
VASurfaceID ← NV12 DMA buffer（timestamp 匹配）
```

## 已完成的里程碑

### P0 — VA-API 驱动骨架（✅ 完成）

`src/iris_vaapi.c` 让 libva 能加载驱动并枚举解码能力：

```
$ LIBVA_DRIVER_NAME=iris LIBVA_DRIVERS_PATH=$PWD/build vainfo
Driver version: iris-vaapi P0: Qualcomm Iris SM8150 (V4L2) 0.0.1
      VAProfileH264ConstrainedBaseline:  VAEntrypointVLD
      VAProfileH264Main               :  VAEntrypointVLD
      VAProfileH264High               :  VAEntrypointVLD
      VAProfileHEVCMain               :  VAEntrypointVLD
      VAProfileVP9Profile0            :  VAEntrypointVLD
```

关键技术坑：
- libva 2.x 用版本化 init 符号 `__vaDriverInit_1_23`（无 `va_driver.h` 宏）
- **vtable 必须堆分配**：libva 的 `vaTerminate()` 会 `free(ctx->vtable)`
- `max_image_formats`/`max_subpic_formats` 必须非零，否则 init 失败
- image/subpicture 相关 vtable 入口必须非 NULL（libva 强制校验）
- vendor 字符串走 `ctx->str_vendor`，不在 vtable 里

### P1 — V4L2 解码引擎（✅ 完成并验证）

`src/v4l2_dec.c/h` 独立验证了 iris 的完整解码流程：

**实测结果**（1080p25 H.264，3 秒流）：
```
access units: 153, decoded frames: 74
decode rate: 363.5 fps (0.2 s for 74 frames)
```

**像素级正确性验证**：引擎输出与 FFmpeg 软件解码逐像素一致
（裁剪 1080→1088 对齐填充后 PNG md5 完全相同）。

复刻 FFmpeg v4l2-m2m 的关键序列（用 LD_PRELOAD ioctl 记录器反推）：
1. S_FMT OUTPUT（编码尺寸，sizeimage 给足 16MB）
2. REQBUFS OUTPUT（16）+ QUERYBUF
3. 订阅 `V4L2_EVENT_SOURCE_CHANGE`
4. **第一个输入 AU 在 STREAMON 前先 QBUF**
5. STREAMON OUTPUT
6. **CAPTURE 在 STREAMON OUTPUT 之后 S_FMT**（否则固件 buffer requirements 报 0x1004）
7. CAPTURE: REQBUFS(20) → QUERYBUF → 全部 QBUF → STREAMON
8. DRC 事件 → 若分辨率已匹配则跳过重协商（盲目 REQBUFS 会 EBUSY 并搞挂固件）

mplane 常见坑：
- QUERYBUF/DQBUF/QBUF 必须设 `b.length = 1`（平面数）
- mmap 用 `planes[0].m.mem_offset`/`planes[0].length`，不是 `b.m.offset`
- CAPTURE 高度会被对齐（1080→1088），比较分辨率要按对齐值
- OUTPUT buffer 需空闲链表管理（解码器乱序归还）

**注意**：中断会话（进程被杀/中途错误）会让固件卡死，`SESSION_INIT` 超时（-110），
需 `sudo rmmod qcom_iris && sudo modprobe qcom_iris` 恢复。

## P1 — 接入 VA-API 驱动（✅ 完成）

把引擎接进驱动，真实解码路径全部打通：

| 组件 | 状态 |
|---|---|
| `src/h264_params.c` — SPS/PPS 重序列化 | ✅ 已验证（重建 NAL 解码像素级一致）|
| `src/decode.c` — surface 管理 + slice 累积 + 异步解码 | ✅ |
| `src/iris_vaapi.c` — vtable 解码路径 | ✅ |
| 端到端 `test/test_va_decode.c` | ✅ `DECODE OK` |

关键实现要点：

1. **SPS/PPS 重序列化**：从 `VAPictureParameterBufferH264` 经 Exp-Golomb 重建
   SPS/PPS NAL。profile_idc 从 `VAProfile` 推导，level 按高度推算。
   用真实 SPS/PPS 解析→重建→ffmpeg 解码验证逐像素一致。
2. **slice 重组**：每帧所有 `VASliceDataBufferType` 拼接（保留 start code），
   前置重建的 SPS/PPS → 完整 AU。
3. **surface 映射**：v4l2 timestamp 携带 surface id（`tv_sec = surface_id`），
   iris 透传到输出帧，`assign_frame` 按 timestamp 回填 surface。
4. **异步模型**：`vaEndPicture` 只入队；`vaSyncSurface` 排水到目标 surface 帧出现。
   同步 1 帧会超时（固件需约 4 帧入队才出第一帧）。
5. **导出**：`vaExportSurfaceHandle` 用 `VIDIOC_EXPBUF` 把 CAPTURE buffer
   导出为 DRM PRIME fd，填 `VADRMPRIMESurfaceDescriptor`（NV12 两层）。

**P 帧解码（P2，✅）**：修正两个固件严格校验的字段后，P 帧逐像素正确：
1. **level 计算 bug**：`level_for_height` 收到的是宏块数而非像素，1080p 被算成
   level 3.0（承载不了 1080p），固件拒绝。已改为 `(mbs+1)*16`。
2. **PPS `num_ref_idx_l0_default_active_minus1`**：从 `VASliceParameterBufferH264`
   捕获（Chrome 填的是 slice 实际使用的参考数），重建进 PPS，否则固件
   参考解析失败（refs=0 时 62/75 帧，refs=1 时 75/75）。
3. **参数变化检测**：SPS/PPS 只在内容变化时重发（每帧重发会重置 DPB）。

**验证**（无 B 帧流，IDR + 9 个 P 帧）：
```
surf0=a6ebf5b5=ref0(IDR)   surf1=bef7950e=ref1(P)
surf3=8646c3c1=ref3(P)     surf5=d825880e=ref5(P)
```
全部与 FFmpeg 软件解码**逐像素一致**。

**已知限制**：
- B 帧流需要解码顺序处理（固件内部处理，但测试喂显示序 slice 会丢帧）。
- 单实例单引擎，不支持并发多视频流。

**EOS flush（✅ 已解决）**：stateful 固件 hold 最后一帧，之前 `vaSyncSurface`
对最后一张画面必超时。现在 `iris_decode_sync` 检测到目标是最后入队画面时
通过 `VIDIOC_DECODER_CMD(V4L2_DEC_CMD_STOP)` 发起 drain（`v4l2_dec_flush`），
固件返回空的 `V4L2_BUF_FLAG_LAST` 标记，
释放最后一张。flush 后再解码（Chrome flush/reset、loop）会经 `ensure_decoder`
自动重开会话。

```
$ ./build/test_va_decode nob.h264      # 新增：sync 最后一张 surface
synced last surface 10
DECODE OK
$ ./build/one_pic                      # 单图场景（唯一出路是 flush）
[sync] last_target=1: flushing
[flush] got ts=1000000000000 flags=0x4009
sync ret=success (no error)
```

## 真实客户端验证（ffmpeg VA-API，✅）

用 ffmpeg 的 `-hwaccel vaapi` 作为第三方 VA-API 客户端验证：

```
# 无 B 帧流: 75 帧全部解码，6.5x 超实时
$ ffmpeg -hwaccel vaapi -vaapi_device /dev/dri/renderD128 -i nob.h264 -f null -
frame= 75  ...  speed=6.54x   EXIT=0

# B 帧流: 77 帧全部解码（含 B 帧重排）
frame= 77  ...  speed=6.93x   EXIT=0

# 内容校验：前 5 帧与软件解码 md5 逐字节一致
frame0..4: vaapi = ref（全部匹配）
```

**过程中修复的关键问题**：
1. **surface id 注册时机**：ffmpeg 在 `vaCreateContext` 前创建 surface 1，此时
   decode ctx 不存在 → surface 未注册。改为 `iris_ensure_decode_ctx` 懒创建。
2. **surface 匹配从 timestamp 改为序列号映射**：不依赖客户端 surface id
   的连续性，用 seq→target surface 的 FIFO 映射。
3. **`vaCreateImage`/`vaGetImage`/`vaDeriveImage`**：实现 CPU 读回路径
   （ffmpeg 用 vaCreateImage 传输帧）。
4. **同步**：`vaEndPicture` 喂入后排空，让上一帧（固件 hold）立即就绪，
   避免逐帧 sync 的客户端死锁。

## VP9 支持（✅）

VP9 帧自包含（无 SPS/PPS），slice data 即完整帧，直接喂入固件：

```
$ ./build/test_va_vp9 /tmp/test-vp9.ivf      # VA-API 端到端
fed 422 frames / VP9 DECODE OK
$ ffmpeg -hwaccel vaapi ... -i vp9.webm       # ffmpeg VA-API
frame= 422 全部解码
# 内容校验：25 帧无 surface 复用流与软解逐字节一致（0 差异）
```

引擎与 VA-API 层均按 profile 选择 V4L2 OUTPUT 像素格式
（H264/HEVC/VP9），`V4L2_PIX_FMT_VP9` 直通喂帧。

## HEVC（✅ Main 8-bit 参数重建与 B 帧已像素级验证）

Iris 的 stateful V4L2 接口需要完整 Annex-B VPS/SPS/PPS + slice，而 Chromium
和 ffmpeg 的 VA-API HEVC 路径通常只提交 slice NAL 与已经解析好的 VA 参数。
本驱动在 Chromium 外完成转换，因此不需要编译 Chromium，也能随发行版浏览器升级。

### Chromium 字段调查与反推

上游 Chromium `media` 源码给出了 VA 参数的准确语义：

- `gpu/vaapi/h265_vaapi_video_decoder_delegate.cc` 把 parser 的 slice header、
  POC、参考图列表和 RPS flags 填进 libva buffer；`slice_data_byte_offset`
  使用移除防竞争字节后的 header 长度。
- `gpu/h265_decoder.h` 中当前短期参考集合就是
  `StCurrBefore ∪ StCurrAfter`；其余有效短期 DPB 项是 `StFoll`。
- `parsers/h265_parser.h/.cc` 保留了完整 slice 语法，但 VA picture buffer
  对 SPS 中的 `st_ref_pic_set()` 只保留集合数量，并不保留原始集合内容。

因此仅“重建 SPS”不够：若把 canonical SPS 的
`num_short_term_ref_pic_sets` 设为 0，而原 slice 仍通过索引引用原 SPS RPS，
slice header 会从该位开始整体错位。

### 当前转换方式

1. `src/hevc_params.c` 从 `VAPictureParameterBufferHEVC` 序列化 canonical
   VPS/SPS/PPS，保留 slice 实际使用的 PPS id。
2. `src/hevc_slice_rewrite.c` 解开 slice RBSP，解析到 short-term RPS，使用
   `ReferenceFrames[].pic_order_cnt/flags` 重建等价的 inline RPS，再复制其余
   slice header 和原始 CABAC payload，最后重新插入 emulation-prevention bytes。
3. IDR、dependent slice，以及原本已经与 canonical SPS 兼容的 slice 直接复制；
   多 slice 参数逐项与对应 data range 关联，不再只保留最后一项。
4. Iris 的 HEVC CAPTURE timestamp 不足以回填 VA surface；驱动按 POC 对待输出
   图片排序，并在 surface 复用时重置完成状态。EOS 使用标准
   `VIDIOC_DECODER_CMD(V4L2_DEC_CMD_STOP)`，排出最后的重排帧。
5. 客户端若提供完整且无需改写的原始 VPS/SPS/PPS，仍优先原样直通。

### 验证结果

用 x265 生成 320×240、30 帧、`1 I + 8 P + 21 B`、SPS-indexed RPS 的真实码流：

| 检查 | 结果 |
|---|---|
| slice rewrite 单元测试 | ✅ PPS id、inline RPS、header suffix、CABAC payload 全部一致 |
| 重建 Annex-B → ffmpeg 软件解码 | ✅ 30/30 帧，逐帧像素 MD5 与原码流一致 |
| ffmpeg VA-API → Iris 硬件 → CPU readback | ✅ 30/30 帧，显示顺序和像素 MD5 全部一致 |
| 1080p 原始/重建 AU 直接喂 V4L2 | ✅ 均输出 1 帧，0 corrupt |

可用 `IRIS_HEVC_DUMP=/tmp/rebuilt.h265` 保存驱动实际提交的重建码流，交给
独立软件解码器复核。

### 当前边界

- HEVC Main10/P010 已接通内核 HFI Gen1、V4L2 CAPTURE、VA surface 与 DRM PRIME
  导出路径，目前仍属于实验支持，需继续做 HDR 显示和长时间播放回归。
- 原 SPS 含 long-term reference table（`num_long_term_ref_pic_sps > 0`）时，
  因 VA buffer 不含该表内容，当前明确返回不支持，不生成可能错位的码流。
- 自定义 scaling list 通过独立 `VAIQMatrixBufferHEVC` 提交；当前 canonical SPS
  使用默认矩阵，尚未序列化自定义矩阵。
- canonical VUI 只提供保守 timing 值；它不影响已验证的解码像素，但尚未覆盖
  HDR/色彩描述等完整元数据重建。

## Chrome 集成（✅ 核心验证通过）

Chrome 151（Wayland 原生）加载 iris 驱动并**流畅解码 H.264 视频**：

```
# 页面 onloadeddata 触发，标题显示 "LOADED 1920x1080"
# 驱动日志（GPU 进程 stderr）：
[surf] id=1 size=3133440 w=1920 h=1088        # 帧池 surface 分配（DMA-heap）
[export] surf=1 type=1073741824 flags=0x5      # vaExportSurfaceHandle 成功
[begin] target=2 ... [end] done rv=0           # slice 喂入解码成功
# 持续解码：675 次 begin（约 40fps 循环播放），无错误
```

**Chrome 探测卡点的修复**（对应之前"卡在 surface 导出探测"）：

| 卡点 | 修复 |
|---|---|
| 空 surface 的 `vaSyncSurface` 超时 | surface 从未入队解码时 sync 立即成功（`src/decode.c`：`queued` 标志）|
| `vaExportSurfaceHandle` 的 `SEPARATE_LAYERS` 要求 | NV12 按 2 层、每层 1 平面返回（`src/iris_vaapi.c`）；Chrome DCHECK 每层必须单平面 |
| `drm_format_modifier` 缺失 | 填 `DRM_FORMAT_MOD_LINEAR`（Chrome 校验 modifier 一致性）|

**运行方式**：Chrome GPU 进程需继承 `LIBVA_DRIVER_NAME=iris`（用包装脚本
`export LIBVA_DRIVER_NAME=iris; google-chrome ...`），驱动装到
`/usr/lib/aarch64-linux-gnu/dri/iris_drv_video.so`，`/dev/dma_heap/system`
需 `sudo chmod 0666`（GPU 导入需要真实 DMA-BUF，memfd 回退不可用）。

## 测试工具

| 工具 | 用途 |
|---|---|
| `test/test_va.c` | 最小 libva 客户端，验证驱动加载与 profile 枚举 |
| `test/test_v4l2_dec.c` | V4L2 引擎测试：Annex-B 切 AU → 解码 → NV12 校验 |
| `test/test_h264_params.c` | SPS/PPS 解析 + 重序列化验证 |
| `test/test_va_decode.c` | 端到端 VA-API 解码 + 导出，含最后一张 sync（EOS flush） |
| `test/test_va_vp9.c` | 端到端 VP9 VA-API 解码 + 导出（IVF 逐帧喂入）|
| `test/test_hevc_au.c` | HEVC 引擎测试：整体流喂入 + EOS，统计帧数 |
| `test/ioctllog.c` | LD_PRELOAD ioctl 记录器（反推 mpv 真实序列）|

## 复现命令

```sh
make
# P0: 枚举驱动
LIBVA_DRIVER_NAME=iris LIBVA_DRIVERS_PATH=$PWD/build vainfo
# P1: 引擎解码（输出帧与软件解码逐像素一致）
./build/test_v4l2_dec /path/to/stream.h264 1920 1080 /tmp/frame.nv12
```
---

## 工作总结（截至 2026-08-22）

### 项目目标
让 Chrome 在 Xiaomi Pad 5 (nabu) 上使用 Qualcomm Iris 硬件解码 H.264。
Chrome 走 VA-API（按 slice 喂数据），iris 是"整比特流" stateful V4L2 解码器
（固件自解析 SPS/PPS），两者接口模型不兼容 → 自写 VA-API 驱动做桥接。

### 已完成

| 里程碑 | 状态 | 验证 |
|---|---|---|
| P0: VA-API 驱动骨架 | ✅ | vainfo 识别 5 个 VLD profile |
| P1: V4L2 解码引擎 | ✅ | 363 fps，与软解逐像素一致 |
| P1: SPS/PPS 重序列化 | ✅ | 重建 NAL ffmpeg 解码一致 |
| P1: 完整 VA-API 解码路径 | ✅ | 端到端 DECODE OK |
| P2: 参数变化检测 + P 帧 | ✅ | IDR+P 帧逐像素一致 |
| P2: ffmpeg VA-API 客户端 | ✅ | 75/77 帧，6.5x 超实时，逐字节一致 |
| P2: Chrome 集成 | ✅ | Wayland 下流畅解码 H.264，页面 LOADED 1920x1080 |
| P3: VP9 支持 | ✅ | VA-API + ffmpeg 全解，25 帧逐字节一致 |
| P3: HEVC 引擎级 | ✅ | 固件 422 AU → 404 帧，像素正确（需完整参数集）|
| P3: HEVC VA-API | ⚠️ | 重建参数集 ffmpeg 软件解码 10/10 帧；固件 IDR corrupt（字节级差异待消除）|

### 关键成果
**ffmpeg `-hwaccel vaapi` 作为第三方 VA-API 客户端解码成功**，且输出与
FFmpeg 软件解码**逐字节一致**。这是驱动正确性的最强证明（真实客户端，
非自定义测试）。

### Chrome 集成的卡点（已解决）
Chrome 的 `VaapiVideoDecoder` 与 stateful 解码器存在**缓冲模型不匹配**：

1. Chrome 在解码前就 `vaExportSurfaceHandle` 导出 surface（帧池预分配），
   要求每个 surface 有稳定、可导出的后备 buffer（`reference/vaapi_video_decoder.cc:930`）。
2. stateful v4l2 解码器把帧写入固件控制的 CAPTURE buffer（会被复用），
   无法预分配给具体 surface。
3. 已解决：surface 属性查询（`VASurfaceAttribMaxWidth/Height`）、
   `vaQueryConfigAttributes` 输出语义、profile 查询 lenient。
4. 已实现：每个 surface 用 `/dev/dma_heap/system` 分配稳定 DMA-BUF 后备，
   解码后拷贝进后备（`src/decode.c`）。**ffmpeg 已验证此模型可用**。
   若 `/dev/dma_heap/system` 不可访问（root only，默认权限），驱动自动
   回退到 **memfd 后备**：本地测试与 ffmpeg CPU 读回路径仍可无 root 运行
   （输出与软解逐字节一致），但该 fd 不是 DRM buffer，**无法被 GPU/EGL
   客户端（Chrome）导入**。真实 Chrome 显示互通仍需：
   `sudo chmod 0666 /dev/dma_heap/system`。
5. 已解决：空 surface 的 sync 探测（Chrome 帧池预分配后立即 sync+export）——
   surface 未入队解码时 `vaSyncSurface` 立即成功；`vaExportSurfaceHandle`
   按 `SEPARATE_LAYERS` 返回 2 层单平面 + `DRM_FORMAT_MOD_LINEAR`。

### 已知限制
- stateful 固件 hold 1 帧：最后一张画面由 EOS flush 释放（已解决，见上）。
- B 帧短序列引用未解析时丢帧（完整流正常，引擎解 74/75）。
- 单实例单引擎，不支持并发多视频流。
- Chrome 的稳定 surface 模型每帧需要一次 CAPTURE→DMA-BUF CPU 拷贝；
  H.264/HEVC/VP9 4K60 必须用 qcom-iris 的 cacheable CAPTURE：
  `options qcom_iris cached_capture=1`。本机 600 帧实测由 15.97s
  （37.6fps，明显卡顿）降至 9.61s（62.5fps）。`install-system.sh` 会将
  该选项安装到 `/etc/modprobe.d/99-iris-vaapi.conf`，模块重载或重启后生效。
  HEVC 4K60 59Mbps（1797 帧）由 46.86s（38.3fps）降至 35.02s
  （51.3fps），300 帧软/硬解 framemd5 完全一致；该高码率样本仍未达到实时。
  10-bit P010 接通后，4K59.94 VP9 Profile 2 的 300 帧拷贝带宽由
  0.8GiB/s 提升至 4.0GiB/s，吞吐由约 27fps 提升至 64fps；1200 帧热态
  仍约 62fps。4K60 HEVC Main10 为约 75--78fps。两种编码各取 30 帧与
  软件解码 framemd5 对比，逐帧完全一致。
  绕过 libva 稳定 surface 拷贝、直接使用 V4L2 MMAP CAPTURE 时，热态测试
  HEVC Main10 600 帧为 153.7fps，VP9 Profile 2 1200 帧为 119.0fps，均为
  0 corrupt；这也量化了当前 VA surface CPU 拷贝的性能上限。
- 中断会话会让固件卡死（SESSION_INIT 超时 -110），需重载模块：
  `sudo rmmod qcom_iris && sudo modprobe qcom_iris`
- surface 后备缺省用 DMA-heap（GPU 可导入）；未 chmod 时自动回退 memfd，
  本地测试/ffmpeg 仍可用，但 Chrome 显示互通需要
  `sudo chmod 0666 /dev/dma_heap/system`。

### Chrome 回退 FFmpegVideoDecoder 排查（2026-08-24）

Chrome 的 `DecoderStream` 在 VaapiVideoDecoder 首次出错时即整体回退软件解码。
已定位并修复的回退根因：

| 根因 | 现象 | 修复 |
|---|---|---|
| `/dev/dma_heap/system` root-only（每次重启复现） | surface 后备退化为 memfd，EGL 导入失败 → "Failed to create VASurface" → 秒回退 | udev 规则 `tools/99-iris-dmaheap.rules`（install-system.sh 自动安装） |
| seq→surface 映射硬上限 512 帧 | 连播 ~10–20s 后每帧映射失败且 CAPTURE 缓冲泄漏 → 固件饿死 → sync 超时回退 | `target_ring[1024]` 环带 seq 校验；未命中帧必回收 CAPTURE 缓冲 |
| `v4l2_dec_poll` 含 POLLOUT | m2m OUTPUT 几乎常写就绪 → sync 轮询瞬间烧完重试预算 → 假超时 | 仅 POLLIN\|POLLPRI |
| 导出 pitch/chroma offset 用猜测对齐 | 固件 CAPTURE 实际 stride/高度与假设不符时画面花屏/错位 | 以协商的 CAPTURE fmt 为准（`iris_surfs_*`） |
| 会话中途死亡（GPU 进程被杀/错误路径） | 固件 wedge，之后所有解码 SESSION_INIT -110 → 永久回退 | close 前 DECODER_CMD STOP + 有界 drain |
| 重建 H.264 SPS 的 VUI 非法 | `Overread VUI`、PPS 失配 | 按 Chromium packed-H264 builder 重建合法 VUI；展示重排仍由 Chromium DPB 完成 |
| 固件按显示顺序延迟返还 CAPTURE | Chrome 已输出 VA surface，但对应 CAPTURE 帧尚未产生 → 4K 卡顿、短暂旧帧/花屏 | qcom-iris 暴露标准 display-delay 控制并向 Gen1 固件请求 decode-order output；VA 驱动在会话启动前设置 delay=0 |
| Gen1 即使 decode-order 仍会成批返还 CAPTURE（实测每 3 帧） | `vaEndPicture` 已返回，但目标 surface 尚未写入；不等待隐式同步的客户端会采到旧帧或写到一半的帧 | `vaEndPicture` 给稳定 DMA-BUF 挂未完成的 reservation write fence；CAPTURE 拷贝和 cache sync 完成后再 signal。遵守 DMA-BUF 隐式同步的消费者可保持异步流水线 |
| 4K60 解码落后时 render target 先于旧 CAPTURE 返回而被复用 | 晚到的旧帧覆盖同一 backing，并误 signal 新一代帧的 fence，表现为偶发回退 1 帧 | seq/POC→surface 映射同时记录 surface generation；旧 generation 的 CAPTURE 只回收，不覆盖 backing、不解除新 fence |
| ANGLE/GL 未可靠等待 DMA-BUF 导入后追加的 reservation fence | `mediaTime` 单调递增，但 canvas 像素指纹会回到早先帧；HEVC 日志中 surface 在 CAPTURE 返回前已复用 1–2 代，持续出现 `assign STALE` | H.264/HEVC 的已导出 surface 在 `vaEndPicture` 返回前等待当前 decode-order target 和 stable copy 完成；VP9 关键帧用内部 prime AU 推出并等待，seek 后第一张 inter 帧再用不改变引用状态的 `show_existing_frame` 建立一次性完成屏障 |
| 同步等待后 4K60 只有约 37.6fps | coherent CAPTURE 被 CPU 逐帧读取，12.5MB/帧的拷贝占满一个 CPU 核 | 加载 qcom-iris 时启用 `cached_capture=1`，让 H.264/HEVC/VP9 的 vb2 缓冲在 DMA 完成后维护 cache；H.264 600 帧实测提升至 62.5fps，VP9 Profile 2 由约 27fps 提升至 64fps |
| seek 后复用同一 `VAContext` | Chromium `Reset()` 既不发 VA EOS 也不重建 context；旧 DPB/CAPTURE 可被新随机访问帧顶出，且 `ensure_decoder` 曾在重开会话时误清当前 slice。仅直接 close H.264/HEVC 会放弃旧 in-flight surface；保留 VP9 live session 则会让新关键帧/prime 与 seek 前 AU 同时存在于 VPU，新输入先顶出旧 CAPTURE，表现为目标帧后闪回旧帧；丢弃旧映射又会让已提交给合成器的 backing 显示成黑帧 | 在识别到 H.264 IDR、HEVC BLA/IDR/CRA 或 VP9 key frame 但尚未提交它时，统一先向旧 session 发 EOS，等待全部 OUTPUT/CAPTURE 和 LAST，再关闭并重开；保留单调的私有 timestamp epoch，之后才提交 seek 帧，使新旧时间段从不同时进入 VPU。VP9 关键帧另由内部 prime AU 推出并等待，seek 后第一张 inter 帧再由 `show_existing_frame` 推出并等待 |

排查工具：

```sh
# 驱动侧跟踪（GPU 进程继承 stderr）：
export IRIS_VAAPI_DEBUG=1
# >512 帧连播回归（Chrome 式流水线 sync）：
./build/test_va_stress /path/to/test-1080p-nob.h264 700
# Chrome 侧确认当前用的解码器：
# chrome://media-internals → kVideoDecoderName；chrome://gpu → Video Acceleration
# 安装/重载新内核模块后先验证 reservation fence 接口：
./build/test_surface_fence /dev/video0
```

注意：压力测试中途被杀（Ctrl-C/timeout）会 wedge 固件，先重载模块再继续调试。

### 构建与测试命令
```sh
make                                          # 构建驱动 + 测试工具
LIBVA_DRIVER_NAME=iris LIBVA_DRIVERS_PATH=$PWD/build vainfo   # P0
./build/test_v4l2_dec 流.h264 1920 1088       # P1 引擎
./build/test_va_decode 流.h264 输出.nv12      # P1 VA 路径
LIBVA_DRIVERS_PATH=$PWD/build LIBVA_DRIVER_NAME=iris \
  ffmpeg -hwaccel vaapi -vaapi_device /dev/dri/renderD128 -i 流.h264 -f null -  # P2 ffmpeg
# 安装到系统供 Chrome 使用：
sudo ./install-system.sh
# decode-order 和异步 surface fence 修复需要安装同一源码树编译的
# qcom-iris 模块；只更新 VA 驱动不会生效。关闭 Chrome 后：
sudo install -m 0644 ../nabu-linux/linux/out-iris1/drivers/media/platform/qcom/iris/qcom-iris.ko \
  /lib/modules/$(uname -r)/kernel/drivers/media/platform/qcom/iris/qcom-iris.ko
sudo depmod -a && sudo modprobe -r qcom_iris && sudo modprobe qcom_iris
# 确认 4K60 所需的 cacheable CAPTURE 已启用（应输出 Y）：
cat /sys/module/qcom_iris/parameters/cached_capture
# Chrome（GPU 进程需继承 LIBVA_DRIVER_NAME）：
export LIBVA_DRIVER_NAME=iris
google-chrome --enable-logging=stderr --v=1 file:///path/to/video.html
```

### 参考
Chromium 源码不再复制进仓库；对应上游路径和关键交互点见
`docs/chromium-integration.md`。

### git 提交历史
```
faf3c9e decode: fall back to memfd surface backing when dma_heap is root-only
92e3ae3 iris-vaapi: real client (ffmpeg vaapi) decodes H.264 end-to-end
a9464aa iris-vaapi: fix P-frame decode and add slice-param-driven PPS
d32dca0 iris-vaapi: wire full VA-API H.264 decode path
b04ea5a h264: re-serialize SPS/PPS NAL units from VAPictureParameterBufferH264
c141778 docs: summarize VA-API Chrome hardware decode project status
45cf083 iris-vaapi: VA-API driver skeleton and iris V4L2 decode engine
```
