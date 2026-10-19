#!/usr/bin/env python3
"""Compare FFmpeg V4L2 M2M and VA-API decode paths on Iris.

The benchmark decodes each complete input to FFmpeg's null muxer, records the
per-process resource usage returned by wait4(2), and verifies the final frame
count reported by FFmpeg's progress protocol.
"""

from __future__ import annotations

import argparse
import csv
import json
import os
import re
import subprocess
import time
from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True)
class Sample:
    codec: str
    decoder: str
    input_path: Path
    expected_frames: int
    nominal_fps: float


def probe(path: Path) -> tuple[str, int, float]:
    output = subprocess.check_output(
        [
            "ffprobe",
            "-v",
            "error",
            "-count_frames",
            "-select_streams",
            "v:0",
            "-show_entries",
            "stream=codec_name,avg_frame_rate,nb_frames,nb_read_frames",
            "-of",
            "json",
            str(path),
        ],
        text=True,
    )
    stream = json.loads(output)["streams"][0]
    num, den = (int(value) for value in stream["avg_frame_rate"].split("/"))
    frame_count = stream.get("nb_frames") or stream.get("nb_read_frames")
    if not frame_count or frame_count == "N/A":
        raise SystemExit(f"ffprobe could not determine the frame count: {path}")
    return stream["codec_name"], int(frame_count), num / den


def ffmpeg_command(sample: Sample, build_dir: Path) -> tuple[list[str], dict[str, str]]:
    command = [
        "ffmpeg",
        "-nostdin",
        "-hide_banner",
        "-v",
        "error",
        "-nostats",
        "-stats_period",
        "3600",
        "-progress",
        "pipe:1",
    ]
    environment = os.environ.copy()

    if sample.decoder == "v4l2":
        command += ["-c:v", f"{sample.codec}_v4l2m2m"]
    else:
        environment.pop("IRIS_VULKAN_COPY", None)
        environment.update(
            LIBVA_DRIVER_NAME="iris",
            LIBVA_DRIVERS_PATH=str(build_dir.resolve()),
        )
        if sample.decoder == "vaapi-vulkan":
            environment.update(
                IRIS_VULKAN_COPY="1",
                VK_DRIVER_FILES="/usr/share/vulkan/icd.d/freedreno_icd.json",
            )
        command += [
            "-hwaccel",
            "vaapi",
            "-hwaccel_output_format",
            "vaapi",
            "-vaapi_device",
            "/dev/dri/renderD128",
        ]

    command += ["-i", str(sample.input_path), "-map", "0:v:0", "-an"]
    if sample.decoder.startswith("vaapi"):
        command += ["-pix_fmt", "vaapi"]
    command += ["-f", "null", "-"]
    return command, environment


def run_sample(
    sample: Sample,
    repetition: int,
    build_dir: Path,
    output_dir: Path,
) -> dict[str, object]:
    command, environment = ffmpeg_command(sample, build_dir)
    stem = f"{sample.codec}-{sample.decoder}-run{repetition}"
    stdout_path = output_dir / f"{stem}.stdout.log"
    stderr_path = output_dir / f"{stem}.stderr.log"

    started = time.perf_counter()
    with stdout_path.open("wb") as stdout_file, stderr_path.open("wb") as stderr_file:
        process = subprocess.Popen(
            command,
            stdout=stdout_file,
            stderr=stderr_file,
            env=environment,
        )
        _, status, usage = os.wait4(process.pid, 0)
        process.returncode = os.waitstatus_to_exitcode(status)
    wall_seconds = time.perf_counter() - started

    progress = stdout_path.read_text(errors="replace")
    frame_matches = re.findall(r"(?m)^frame=(\d+)$", progress)
    frames = int(frame_matches[-1]) if frame_matches else 0
    cpu_seconds = usage.ru_utime + usage.ru_stime

    return {
        "codec": sample.codec,
        "decoder": sample.decoder,
        "repetition": repetition,
        "exit_code": process.returncode,
        "frames": frames,
        "expected_frames": sample.expected_frames,
        "frame_count_ok": frames == sample.expected_frames,
        "wall_seconds": round(wall_seconds, 6),
        "decode_fps": round(frames / wall_seconds, 3),
        "realtime_speed_x": round(frames / wall_seconds / sample.nominal_fps, 4),
        "user_seconds": round(usage.ru_utime, 6),
        "system_seconds": round(usage.ru_stime, 6),
        "cpu_core_percent": round(cpu_seconds / wall_seconds * 100, 2),
        "max_rss_kib": usage.ru_maxrss,
        "max_rss_mib": round(usage.ru_maxrss / 1024, 3),
        "input": str(sample.input_path),
        "command": " ".join(command),
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--h264", type=Path)
    parser.add_argument("--hevc", type=Path)
    parser.add_argument("--vp9", type=Path)
    parser.add_argument("--build-dir", type=Path, default=Path("build"))
    parser.add_argument("--output-dir", type=Path, required=True,
                        help="directory for per-run stdout/stderr logs")
    parser.add_argument("--results-file", type=Path,
                        help="CSV destination (defaults to OUTPUT_DIR/results.csv)")
    parser.add_argument("--repetitions", type=int, default=3)
    parser.add_argument("--cooldown", type=float, default=1.0)
    parser.add_argument("--include-vulkan-copy", action="store_true",
                        help="also benchmark VA-API with IRIS_VULKAN_COPY=1")
    parser.add_argument("--warmup", action="store_true",
                        help="run every path once without recording it")
    args = parser.parse_args()
    args.output_dir.mkdir(parents=True, exist_ok=True)

    input_paths = [path for path in (args.h264, args.hevc, args.vp9) if path]
    if not input_paths:
        parser.error("at least one of --h264, --hevc, or --vp9 is required")

    samples: list[Sample] = []
    for path in input_paths:
        codec, expected_frames, nominal_fps = probe(path)
        if codec not in {"h264", "hevc", "vp9"}:
            raise SystemExit(f"unsupported codec {codec!r}: {path}")
        decoders = ["v4l2", "vaapi"]
        if args.include_vulkan_copy:
            decoders.append("vaapi-vulkan")
        for decoder in decoders:
            samples.append(Sample(codec, decoder, path, expected_frames, nominal_fps))

    if args.warmup:
        for sample in samples:
            print(f"warmup codec={sample.codec} decoder={sample.decoder}", flush=True)
            row = run_sample(sample, 0, args.build_dir, args.output_dir)
            if row["exit_code"] != 0 or not row["frame_count_ok"]:
                print("warmup failed")
                return 1
            time.sleep(args.cooldown)

    rows: list[dict[str, object]] = []
    for repetition in range(1, args.repetitions + 1):
        offset = (repetition - 1) % len(samples)
        ordered = samples[offset:] + samples[:offset]
        for sample in ordered:
            print(f"run={repetition} codec={sample.codec} decoder={sample.decoder}", flush=True)
            row = run_sample(sample, repetition, args.build_dir, args.output_dir)
            rows.append(row)
            print(
                f"  frames={row['frames']}/{row['expected_frames']} "
                f"fps={row['decode_fps']} cpu={row['cpu_core_percent']}% "
                f"rss={row['max_rss_mib']} MiB exit={row['exit_code']}",
                flush=True,
            )
            time.sleep(args.cooldown)

    csv_path = args.results_file or args.output_dir / "results.csv"
    csv_path.parent.mkdir(parents=True, exist_ok=True)
    with csv_path.open("w", newline="") as output:
        writer = csv.DictWriter(output, fieldnames=list(rows[0]), lineterminator="\n")
        writer.writeheader()
        writer.writerows(rows)

    failed = [row for row in rows if row["exit_code"] != 0 or not row["frame_count_ok"]]
    print(f"wrote {csv_path}")
    if failed:
        print(f"invalid runs: {len(failed)}")
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
