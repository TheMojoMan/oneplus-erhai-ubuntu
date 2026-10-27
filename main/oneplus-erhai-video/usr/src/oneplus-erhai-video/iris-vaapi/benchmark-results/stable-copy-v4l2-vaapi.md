# Stable-surface copy benchmark

Source commit: `f1ee9f8` (`decode: add Vulkan stable-surface copy`)

Each path was warmed up once, then measured over three complete-input runs.
Run order was rotated between repetitions. The output was FFmpeg's null sink,
so these numbers measure decode and stable-surface handling rather than display.
CPU percentage is one CPU core; 100% means one fully occupied core.

| Codec | Path | Decode fps | CPU/core | CPU time (s) | Comparable RSS (MiB) |
|---|---|---:|---:|---:|---:|
| H.264 High 8-bit | direct V4L2 | 165.43 | 32.57% | 3.54 | 414.82 |
| H.264 High 8-bit | VA-API CPU copy | 164.97 | 60.78% | 6.62 | 543.21 |
| H.264 High 8-bit | VA-API Vulkan copy | 164.50 | 60.62% | 6.62 | 557.95 |
| HEVC Main10 | direct V4L2 | 146.51 | 28.68% | 3.52 | 587.47* |
| HEVC Main10 | VA-API CPU copy | 145.36 | 88.29% | 10.92 | 936.99 |
| HEVC Main10 | VA-API Vulkan copy | 145.85 | 87.52% | 10.78 | 951.14 |
| VP9 Profile 2 | direct V4L2 | 113.94 | 22.74% | 2.40 | 696.10* |
| VP9 Profile 2 | VA-API CPU copy | 110.43 | 45.49% | 4.94 | 797.97 |
| VP9 Profile 2 | VA-API Vulkan copy | 110.35 | 39.44% | 4.29 | 811.25 |

`*` The direct P010 harness memory-maps the complete input file. Its comparable
RSS subtracts that mapped file size. The raw maximum RSS was 798.35 MiB for
HEVC Main10 and 739.80 MiB for VP9 Profile 2.

H.264 uses FFmpeg for both paths. Because FFmpeg's V4L2 M2M frontend does not
select P010 CAPTURE for these streams, the two 10-bit direct-V4L2 rows use the
repository harness while the VA-API rows use FFmpeg. Their CPU/RSS gap therefore
includes frontend and process-structure differences as well as VA-API overhead.

## Vulkan copy versus VA-API CPU copy

| Codec | Throughput | CPU/core | Maximum RSS |
|---|---:|---:|---:|
| H.264 High 8-bit | -0.29% | -0.26% | +14.74 MiB |
| HEVC Main10 | +0.34% | -0.88% | +14.15 MiB |
| VP9 Profile 2 | -0.08% | -13.30% | +13.28 MiB |

Vulkan stable-surface copy is throughput-neutral in this steady-state test.
It materially reduces VP9 Profile 2 CPU use, but does not reduce H.264 or HEVC
Main10 CPU use. The Vulkan device/import cache adds about 13--15 MiB RSS.
Direct V4L2 remains the lowest-CPU path because VA-API still has FFmpeg,
surface-management, synchronization, and compatibility-layer overhead beyond
the full-frame copy itself.

## Validation

- All 27 measured runs exited successfully and produced the expected frame
  count: 1,797 H.264, 1,797 HEVC Main10, and 1,200 VP9 Profile 2 frames.
- All six measured direct P010 runs reported zero corrupt frames.
- All nine `vaapi-vulkan` logs confirmed `Vulkan DMA-BUF copy enabled` on
  Turnip Adreno 640.
- This is a steady-state benchmark. One unmeasured warm-up per path excludes
  first-process Vulkan/Mesa initialization cost.

Raw samples are in `h264-v4l2-vaapi-stable-copy.csv` and
`p010-v4l2-vaapi-stable-copy.csv`. Per-run logs are under the ignored
`benchmark-results/logs/` directory.
