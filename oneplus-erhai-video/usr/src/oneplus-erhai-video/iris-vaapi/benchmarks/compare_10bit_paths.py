#!/usr/bin/env python3
"""Benchmark the repository P010 V4L2 harness against FFmpeg VA-API."""

from __future__ import annotations

import argparse
import csv
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


def command_for(sample: Sample, build_dir: Path) -> tuple[list[str], dict[str, str]]:
    environment = os.environ.copy()
    if sample.decoder == "v4l2":
        binary = "test_hevc_au" if sample.codec == "hevc-main10" else "test_v4l2_vp9"
        return [
            str((build_dir / binary).resolve()),
            str(sample.input_path),
            "3840",
            "2160",
            "p010",
        ], environment

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
    return [
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
        "-hwaccel",
        "vaapi",
        "-hwaccel_output_format",
        "vaapi",
        "-vaapi_device",
        "/dev/dri/renderD128",
        "-i",
        str(sample.input_path),
        "-map",
        "0:v:0",
        "-an",
        "-pix_fmt",
        "vaapi",
        "-f",
        "null",
        "-",
    ], environment


def run_sample(sample: Sample, repetition: int, build_dir: Path, output_dir: Path) -> dict[str, object]:
    command, environment = command_for(sample, build_dir)
    stem = f"{sample.codec}-{sample.decoder}-run{repetition}"
    stdout_path = output_dir / f"{stem}.stdout.log"
    stderr_path = output_dir / f"{stem}.stderr.log"

    started = time.perf_counter()
    with stdout_path.open("wb") as stdout_file, stderr_path.open("wb") as stderr_file:
        process = subprocess.Popen(command, stdout=stdout_file, stderr=stderr_file, env=environment)
        _, status, usage = os.wait4(process.pid, 0)
        process.returncode = os.waitstatus_to_exitcode(status)
    wall_seconds = time.perf_counter() - started

    stdout = stdout_path.read_text(errors="replace")
    if sample.decoder == "v4l2":
        match = re.search(r"decoded (\d+) frames \((\d+) corrupt\), ([\d.]+) fps", stdout)
        frames = int(match.group(1)) if match else 0
        corrupt_frames = int(match.group(2)) if match else -1
        decoder_loop_fps = float(match.group(3)) if match else 0.0
    else:
        matches = re.findall(r"(?m)^frame=(\d+)$", stdout)
        frames = int(matches[-1]) if matches else 0
        corrupt_frames = 0
        decoder_loop_fps = 0.0

    cpu_seconds = usage.ru_utime + usage.ru_stime
    input_file_mib = sample.input_path.stat().st_size / 1024 / 1024
    max_rss_mib = usage.ru_maxrss / 1024
    adjusted_rss = max_rss_mib - input_file_mib if sample.decoder == "v4l2" else max_rss_mib
    return {
        "codec": sample.codec,
        "decoder": sample.decoder,
        "repetition": repetition,
        "exit_code": process.returncode,
        "frames": frames,
        "expected_frames": sample.expected_frames,
        "corrupt_frames": corrupt_frames,
        "frame_count_ok": frames == sample.expected_frames and corrupt_frames == 0,
        "wall_seconds": round(wall_seconds, 6),
        "decode_fps": round(frames / wall_seconds, 3),
        "decoder_loop_fps": decoder_loop_fps,
        "realtime_speed_x": round(frames / wall_seconds / sample.nominal_fps, 4),
        "user_seconds": round(usage.ru_utime, 6),
        "system_seconds": round(usage.ru_stime, 6),
        "cpu_core_percent": round(cpu_seconds / wall_seconds * 100, 2),
        "max_rss_mib": round(max_rss_mib, 3),
        "input_file_mib": round(input_file_mib, 3),
        "input_adjusted_rss_mib": round(adjusted_rss, 3),
        "input": str(sample.input_path),
        "command": " ".join(command),
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--hevc", type=Path, required=True)
    parser.add_argument("--vp9", type=Path, required=True)
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

    decoders = ["v4l2", "vaapi"]
    if args.include_vulkan_copy:
        decoders.append("vaapi-vulkan")

    samples = [
        Sample("hevc-main10", decoder, args.hevc, 1797, 60.0)
        for decoder in decoders
    ] + [
        Sample("vp9-profile2", decoder, args.vp9, 1200, 19001 / 317)
        for decoder in decoders
    ]

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
                f"  frames={row['frames']}/{row['expected_frames']} fps={row['decode_fps']} "
                f"cpu={row['cpu_core_percent']}% rss={row['max_rss_mib']} MiB "
                f"adjusted_rss={row['input_adjusted_rss_mib']} MiB exit={row['exit_code']}",
                flush=True,
            )
            time.sleep(args.cooldown)

    csv_path = args.results_file or args.output_dir / "results.csv"
    csv_path.parent.mkdir(parents=True, exist_ok=True)
    with csv_path.open("w", newline="") as output:
        writer = csv.DictWriter(output, fieldnames=list(rows[0]), lineterminator="\n")
        writer.writeheader()
        writer.writerows(rows)
    print(f"wrote {csv_path}")

    return 1 if any(row["exit_code"] != 0 or not row["frame_count_ok"] for row in rows) else 0


if __name__ == "__main__":
    raise SystemExit(main())
