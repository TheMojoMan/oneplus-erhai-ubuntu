# Decode benchmark results

Result filenames describe the comparison and intentionally do not contain a
date. Future runs should update an existing comparison file or add a new,
descriptive CSV file. Run timestamps remain inside the CSV/report metadata.

Tracked files:

- `h264-hevc8-v4l2-vaapi.csv`: H.264 High and HEVC Main 8-bit raw runs.
- `vp9-profile0-v4l2-vaapi.csv`: VP9 Profile 0 raw runs.
- `p010-v4l2-vaapi.csv`: HEVC Main10 and VP9 Profile 2 P010 raw runs.
- `h264-vaapi-async-v4l2.csv`: H.264 results after enabling asynchronous
  submission for decode-only VA surfaces.
- `h264-v4l2-vaapi-stable-copy.csv`: H.264 direct V4L2, VA-API CPU-copy, and
  VA-API Vulkan-copy raw samples after stable-surface copy support.
- `p010-v4l2-vaapi-stable-copy.csv`: HEVC Main10 and VP9 Profile 2 direct
  V4L2, VA-API CPU-copy, and VA-API Vulkan-copy raw samples.
- `stable-copy-v4l2-vaapi.md`: steady-state averages, relative differences,
  methodology, and validation for the stable-surface copy comparison.
- `v4l2-vaapi-summary.sql`: reproducible aggregation of the initial baseline.
- `v4l2-vaapi-report.json`: canonical initial-baseline report input.
- `v4l2-vaapi-report.html`: generated initial-baseline portable report.

The initial baseline report predates asynchronous H.264 submission. For the
current H.264 result, use `h264-vaapi-async-v4l2.csv`; it supersedes only the
H.264 rows, while the HEVC and VP9 rows in the initial report remain current.

Per-run logs and generated elementary streams live in the ignored `logs/` and
`streams/` directories so repeated comparisons do not pollute Git history.

Example output routing:

```sh
python3 benchmarks/compare_decode_paths.py \
  --h264 /path/to/h264.mp4 \
  --build-dir build \
  --output-dir benchmark-results/logs/h264-async \
  --results-file benchmark-results/h264-vaapi-async-v4l2.csv

python3 benchmarks/compare_decode_paths.py \
  --vp9 /path/to/vp9-profile0.webm \
  --build-dir build \
  --output-dir benchmark-results/logs/vp9-profile0 \
  --results-file benchmark-results/vp9-profile0-v4l2-vaapi.csv \
  --repetitions 5

python3 benchmarks/compare_10bit_paths.py \
  --hevc benchmark-results/streams/hevc-main10-4k60.h265 \
  --vp9 benchmark-results/streams/vp9-profile2-4k60-1200.ivf \
  --build-dir build \
  --output-dir benchmark-results/logs/p010 \
  --results-file benchmark-results/p010-v4l2-vaapi.csv

# Stable-surface CPU-copy versus Vulkan-copy comparison. The warm-up runs are
# written only to per-run logs and excluded from the result CSV.
python3 benchmarks/compare_decode_paths.py \
  --h264 /path/to/h264.mp4 \
  --build-dir build \
  --output-dir benchmark-results/logs/h264-v4l2-vaapi-stable-copy \
  --results-file benchmark-results/h264-v4l2-vaapi-stable-copy.csv \
  --include-vulkan-copy \
  --warmup

python3 benchmarks/compare_10bit_paths.py \
  --hevc benchmark-results/streams/hevc-main10-4k60.h265 \
  --vp9 benchmark-results/streams/vp9-profile2-4k60-1200.ivf \
  --build-dir build \
  --output-dir benchmark-results/logs/p010-v4l2-vaapi-stable-copy \
  --results-file benchmark-results/p010-v4l2-vaapi-stable-copy.csv \
  --include-vulkan-copy \
  --warmup
```
