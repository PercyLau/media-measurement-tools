#!/usr/bin/env python3
"""Sender-side local probe for preencoded MP4 assets.

This probe measures the sender runtime path after moving encoding offline:
filesrc -> qtdemux -> parse -> appsink
"""

from __future__ import annotations

import argparse
import copy
import csv
import hashlib
import json
import math
import os
import signal
import socket
import sys
import time
from datetime import datetime
from pathlib import Path
from typing import Any, Optional

import gi

Gst = None
GLib = None


def sanitize_name(value: str) -> str:
    allowed: list[str] = []
    for ch in value.lower():
        allowed.append(ch if ch.isalnum() or ch in ("-", "_", ".") else "_")
    sanitized = "".join(allowed).strip("._")
    return sanitized or "unknown"


def resolve_preencoded_mp4_path(config: dict[str, Any]) -> str:
    sender_cfg = config.get("sender", {})
    configured_path = str(sender_cfg.get("preencoded_mp4_path", "")).strip()
    if configured_path and configured_path.lower() != "auto":
        return configured_path

    video_input = config["video_input"]
    encoder = config["encoder"]
    video_stem = Path(str(video_input["path"])).stem or "video"
    return (
        f"prepared/{sanitize_name(video_stem)}_"
        f"{int(video_input['width'])}x{int(video_input['height'])}_"
        f"{int(video_input.get('source_framerate', video_input['framerate']))}fps_"
        f"{int(video_input['framerate'])}fps_"
        f"{str(encoder['codec']).lower()}_"
        f"{int(encoder['bitrate_kbps'])}kbps_"
        f"{int(video_input.get('bit_depth', 8))}bit.mp4"
    )


class SenderStatsApp:
    def __init__(self, config: dict[str, Any]) -> None:
        self.config = config
        network = config["network"]
        video_input = config["video_input"]
        encoder = config["encoder"]
        thresholds = config.get("stall_thresholds_ms", {})
        sender_cfg = config.get("sender", {})
        receiver_cfg = config.get("receiver", {})

        self.experiment_name = str(config.get("experiment_name", "experiment"))
        self.host = str(network["host"])
        self.port = int(network["port"])
        self.mtu = int(network["mtu"])
        self.codec = str(encoder["codec"]).lower()
        self.bitrate_kbps = int(encoder["bitrate_kbps"])
        self.width = int(video_input["width"])
        self.height = int(video_input["height"])
        self.source_framerate = int(video_input.get("source_framerate", video_input["framerate"]))
        self.framerate = int(video_input["framerate"])
        self.pixel_format = str(video_input["format"])
        self.mp4_path = resolve_preencoded_mp4_path(config)
        if not self.mp4_path:
            raise ValueError("sender.preencoded_mp4_path is required.")

        self.expected_frame_interval_ns = 0.0
        self.expected_frame_interval_ms = 0.0
        if self.framerate > 0:
            self.expected_frame_interval_ns = 1_000_000_000.0 / self.framerate
            self.expected_frame_interval_ms = self.expected_frame_interval_ns / 1_000_000.0

        self.stall_threshold_mode = str(thresholds.get("mode", "frame_intervals")).lower()
        self.minor_threshold_frames = float(thresholds.get("minor_frame_intervals", 1.5))
        self.major_threshold_frames = float(thresholds.get("major_frame_intervals", 3.0))
        if self.stall_threshold_mode == "frame_intervals":
            self.minor_threshold_ms = self.expected_frame_interval_ms * self.minor_threshold_frames
            self.major_threshold_ms = self.expected_frame_interval_ms * self.major_threshold_frames
        elif self.stall_threshold_mode == "fixed_ms":
            self.minor_threshold_ms = float(thresholds["minor"])
            self.major_threshold_ms = float(thresholds["major"])
        else:
            raise ValueError(
                f"Unsupported stall_thresholds_ms.mode: {self.stall_threshold_mode}. "
                "Expected 'fixed_ms' or 'frame_intervals'."
            )

        self.pts_jump_threshold_frames = float(
            sender_cfg.get(
                "pts_jump_threshold_frames",
                receiver_cfg.get("pts_jump_threshold_frames", 1.5),
            )
        )
        self.csv_flush_interval = int(sender_cfg.get("csv_flush_interval", 60))
        self.probe_max_buffers = int(sender_cfg.get("probe_max_buffers", 256))
        self.output_root = Path(sender_cfg.get("output_root", receiver_cfg.get("output_root", "output")))
        self.save_resolved_config = bool(sender_cfg.get("save_resolved_config", True))
        self.save_run_info = bool(sender_cfg.get("save_run_info", True))

        self.prev_recv_monotonic_ns: Optional[int] = None
        self.prev_pts_ns: Optional[int] = None
        self.first_sample_monotonic_ns: Optional[int] = None
        self.last_sample_monotonic_ns: Optional[int] = None
        self.sample_idx = 0
        self.sample_count = 0
        self.delta_samples_ms: list[float] = []
        self.pts_jump_count = 0
        self.output_gap_frames_total = 0
        self.max_output_gap_frames_per_jump = 0
        self.stall_minor_count = 0
        self.stall_major_count = 0
        self.total_bytes = 0

        self.loop: Any = None
        self.pipeline: Any = None
        self.appsink: Any = None
        self.stop_requested = False

        self.csv_fp = None
        self.csv_writer = None
        self.event_fp = None
        self.csv_rows_since_flush = 0

        self.run_start_monotonic_ns = time.monotonic_ns()
        self.run_start_wall_time = datetime.now().astimezone().isoformat(timespec="seconds")
        self.semantic_name = self.build_semantic_name()
        self.config_hash8 = self.build_run_hash()
        self.timestamp_str = datetime.now().strftime("%Y%m%dT%H%M%S")
        self.run_dir = self.output_root / self.semantic_name / f"{self.timestamp_str}_{self.config_hash8}"
        self.output_csv = self.run_dir / "sender_metrics.csv"
        self.output_events = self.run_dir / "sender_events.log"
        self.resolved_config_path = self.run_dir / "resolved_config.json"
        self.run_info_path = self.run_dir / "run_info.json"

    def build_semantic_name(self) -> str:
        video_stem = sanitize_name(Path(self.mp4_path).stem or "video")
        parts = [
            video_stem,
            f"{self.width}x{self.height}",
            f"{self.framerate}fps",
            sanitize_name(self.codec),
            f"{self.bitrate_kbps}kbps",
            "sender_mp4_probe",
        ]
        if self.source_framerate != self.framerate:
            parts.append(f"src{self.source_framerate}fps")
        return "_".join(parts)

    def build_hash_payload(self) -> dict[str, Any]:
        return {
            "experiment_name": self.experiment_name,
            "sender": {
                "preencoded_mp4_path_basename": Path(self.mp4_path).name,
                "probe_max_buffers": self.probe_max_buffers,
                "pts_jump_threshold_frames": self.pts_jump_threshold_frames,
            },
            "video_input": {
                "width": self.width,
                "height": self.height,
                "source_framerate": self.source_framerate,
                "output_framerate": self.framerate,
                "format": self.pixel_format,
            },
            "encoder": {
                "codec": self.codec,
                "bitrate_kbps": self.bitrate_kbps,
            },
        }

    def build_run_hash(self) -> str:
        canonical = json.dumps(self.build_hash_payload(), sort_keys=True, separators=(",", ":"), ensure_ascii=False)
        return hashlib.sha256(canonical.encode("utf-8")).hexdigest()[:8]

    def log_event(self, message: str) -> None:
        ts = time.monotonic_ns()
        line = f"[{ts}] {message}"
        print(line)
        if self.event_fp is not None:
            self.event_fp.write(line + "\n")
            self.event_fp.flush()

    @staticmethod
    def percentile(values: list[float], percentile_value: float) -> float:
        if not values:
            return 0.0
        sorted_values = sorted(values)
        if len(sorted_values) == 1:
            return sorted_values[0]
        rank = (len(sorted_values) - 1) * (percentile_value / 100.0)
        low = int(math.floor(rank))
        high = int(math.ceil(rank))
        if low == high:
            return sorted_values[low]
        fraction = rank - low
        return sorted_values[low] * (1.0 - fraction) + sorted_values[high] * fraction

    def build_pipeline_description(self) -> str:
        parser = "h264parse" if self.codec == "h264" else "h265parse"
        return (
            f'filesrc location="{self.mp4_path}" ! qtdemux name=demux '
            f'demux.video_0 ! queue max-size-buffers=0 max-size-bytes=0 max-size-time=0 ! '
            f'{parser} ! appsink name=probesink emit-signals=true sync=false '
            f'max-buffers={self.probe_max_buffers} drop=false'
        )

    def build_summary(self) -> dict[str, Any]:
        duration_s = 0.0
        if self.first_sample_monotonic_ns is not None and self.last_sample_monotonic_ns is not None:
            duration_s = max(0.0, (self.last_sample_monotonic_ns - self.first_sample_monotonic_ns) / 1_000_000_000.0)

        throughput_kbps = 0.0
        if duration_s > 0:
            throughput_kbps = (self.total_bytes * 8.0) / duration_s / 1000.0

        return {
            "total_samples": self.sample_count,
            "observed_intervals": len(self.delta_samples_ms),
            "minor_stalls": self.stall_minor_count,
            "major_stalls": self.stall_major_count,
            "max_delta_ms": round(max(self.delta_samples_ms, default=0.0), 3),
            "p95_delta_ms": round(self.percentile(self.delta_samples_ms, 95.0), 3),
            "p99_delta_ms": round(self.percentile(self.delta_samples_ms, 99.0), 3),
            "pts_jump_count": self.pts_jump_count,
            "estimated_output_gap_frames_total": self.output_gap_frames_total,
            "max_output_gap_frames_per_jump": self.max_output_gap_frames_per_jump,
            "duration_s": round(duration_s, 3),
            "samples_per_s": round((self.sample_count / duration_s) if duration_s > 0 else 0.0, 3),
            "bytes_total": self.total_bytes,
            "throughput_kbps": round(throughput_kbps, 3),
        }

    def ensure_output_dirs(self) -> None:
        self.run_dir.mkdir(parents=True, exist_ok=True)

    def write_resolved_config(self) -> None:
        if not self.save_resolved_config:
            return
        resolved = copy.deepcopy(self.config)
        resolved.setdefault("sender", {})
        resolved["sender"]["probe_max_buffers"] = self.probe_max_buffers
        resolved["sender"]["pts_jump_threshold_frames"] = self.pts_jump_threshold_frames
        resolved["_resolved"] = {
            "semantic_name": self.semantic_name,
            "config_hash8": self.config_hash8,
            "timestamp": self.timestamp_str,
            "run_dir": str(self.run_dir),
            "probe_kind": "sender_mp4_probe",
            "preencoded_mp4_path": self.mp4_path,
        }
        with self.resolved_config_path.open("w", encoding="utf-8") as f:
            json.dump(resolved, f, indent=2, ensure_ascii=False)

    def write_run_info_file(self, final: bool = False) -> None:
        if not self.save_run_info:
            return
        info = {
            "experiment_name": self.experiment_name,
            "run_start_wall_time": self.run_start_wall_time,
            "run_start_monotonic_ns": self.run_start_monotonic_ns,
            "timestamp": self.timestamp_str,
            "semantic_name": self.semantic_name,
            "config_hash8": self.config_hash8,
            "hostname": socket.gethostname(),
            "pid": os.getpid(),
            "run_dir": str(self.run_dir),
            "probe_kind": "sender_mp4_probe",
            "preencoded_mp4_path": self.mp4_path,
            "paths": {
                "sender_metrics_csv": str(self.output_csv),
                "sender_events_log": str(self.output_events),
                "resolved_config_json": str(self.resolved_config_path),
                "run_info_json": str(self.run_info_path),
            },
            "summary": self.build_summary(),
            "finalized": final,
        }
        with self.run_info_path.open("w", encoding="utf-8") as f:
            json.dump(info, f, indent=2, ensure_ascii=False)

    def open_outputs(self) -> None:
        self.ensure_output_dirs()
        self.write_resolved_config()
        self.write_run_info_file(final=False)
        self.event_fp = self.output_events.open("w", encoding="utf-8")
        self.csv_fp = self.output_csv.open("w", newline="", encoding="utf-8")
        self.csv_writer = csv.writer(self.csv_fp)
        self.csv_writer.writerow([
            "sample_idx",
            "pts_ns",
            "recv_monotonic_ns",
            "delta_ms",
            "pts_delta_ms",
            "pts_gap_frames",
            "is_pts_jump",
            "estimated_output_gap_frames",
            "sample_bytes",
            "is_stall_minor",
            "is_stall_major",
        ])
        self.csv_fp.flush()
        self.csv_rows_since_flush = 0

    def close_outputs(self) -> None:
        if self.csv_fp is not None:
            self.csv_fp.flush()
            self.csv_fp.close()
            self.csv_fp = None
        if self.event_fp is not None:
            self.event_fp.close()
            self.event_fp = None

    def on_new_sample(self, sink: Any) -> Any:
        sample = sink.emit("pull-sample")
        if sample is None:
            return Gst.FlowReturn.ERROR
        buf = sample.get_buffer()
        if buf is None:
            self.log_event("Received sample without buffer.")
            return Gst.FlowReturn.ERROR

        recv_ns = time.monotonic_ns()
        if self.first_sample_monotonic_ns is None:
            self.first_sample_monotonic_ns = recv_ns
        self.last_sample_monotonic_ns = recv_ns

        pts_ns = int(buf.pts) if buf.pts != Gst.CLOCK_TIME_NONE else -1
        sample_bytes = int(buf.get_size())
        self.total_bytes += sample_bytes

        delta_ms = 0.0
        delta_ms_text = "0.000"
        if self.prev_recv_monotonic_ns is not None:
            delta_ms = (recv_ns - self.prev_recv_monotonic_ns) / 1_000_000.0
            delta_ms_text = f"{delta_ms:.3f}"
            self.delta_samples_ms.append(delta_ms)

        is_minor = int(delta_ms > self.minor_threshold_ms)
        is_major = int(delta_ms > self.major_threshold_ms)
        if is_minor:
            self.stall_minor_count += 1
        if is_major:
            self.stall_major_count += 1
            self.log_event(f"MAJOR_STALL sample={self.sample_idx} delta_ms={delta_ms:.3f}")

        pts_delta_ms_text = ""
        pts_gap_frames_text = ""
        is_pts_jump = 0
        estimated_output_gap_frames = 0
        if pts_ns >= 0 and self.prev_pts_ns is not None and self.expected_frame_interval_ns > 0:
            pts_delta_ns = pts_ns - self.prev_pts_ns
            pts_delta_ms = pts_delta_ns / 1_000_000.0
            pts_gap_frames = pts_delta_ns / self.expected_frame_interval_ns
            pts_delta_ms_text = f"{pts_delta_ms:.3f}"
            pts_gap_frames_text = f"{pts_gap_frames:.3f}"
            if pts_gap_frames > self.pts_jump_threshold_frames:
                estimated_frame_steps = int(math.floor(pts_gap_frames + 0.5))
                estimated_output_gap_frames = max(0, estimated_frame_steps - 1)
                if estimated_output_gap_frames > 0:
                    is_pts_jump = 1
                    self.pts_jump_count += 1
                    self.output_gap_frames_total += estimated_output_gap_frames
                    self.max_output_gap_frames_per_jump = max(self.max_output_gap_frames_per_jump, estimated_output_gap_frames)
                    self.log_event(
                        "PTS_JUMP "
                        f"sample={self.sample_idx} "
                        f"pts_delta_ms={pts_delta_ms:.3f} "
                        f"gap_frames={pts_gap_frames:.3f} "
                        f"estimated_output_gap_frames={estimated_output_gap_frames}"
                    )

        self.csv_writer.writerow([
            self.sample_idx,
            pts_ns,
            recv_ns,
            delta_ms_text,
            pts_delta_ms_text,
            pts_gap_frames_text,
            is_pts_jump,
            estimated_output_gap_frames,
            sample_bytes,
            is_minor,
            is_major,
        ])
        self.csv_rows_since_flush += 1
        if self.csv_rows_since_flush >= max(1, self.csv_flush_interval):
            self.csv_fp.flush()
            self.csv_rows_since_flush = 0

        self.prev_recv_monotonic_ns = recv_ns
        if pts_ns >= 0:
            self.prev_pts_ns = pts_ns
        self.sample_idx += 1
        self.sample_count += 1
        return Gst.FlowReturn.OK

    def on_bus_message(self, _bus: Any, message: Any) -> None:
        if message.type == Gst.MessageType.EOS:
            self.log_event("Sender MP4 probe reached EOS.")
            self.stop()
            return
        if message.type == Gst.MessageType.ERROR:
            err, debug = message.parse_error()
            self.log_event(f"ERROR {err}: {debug or 'no debug info'}")
            self.stop()

    def open_pipeline(self) -> None:
        desc = self.build_pipeline_description()
        self.log_event(f"Input MP4: {self.mp4_path}")
        self.log_event(f"Pipeline: {desc}")
        self.pipeline = Gst.parse_launch(desc)
        self.appsink = self.pipeline.get_by_name("probesink")
        if self.appsink is None:
            raise RuntimeError("Failed to resolve sender probe appsink.")
        self.appsink.connect("new-sample", self.on_new_sample)
        bus = self.pipeline.get_bus()
        bus.add_signal_watch()
        bus.connect("message", self.on_bus_message)

    def start(self) -> None:
        self.open_outputs()
        self.open_pipeline()
        self.loop = GLib.MainLoop()
        self.pipeline.set_state(Gst.State.PLAYING)
        self.log_event("Sender MP4 probe pipeline started.")
        self.loop.run()

    def stop(self) -> None:
        if self.stop_requested:
            return
        self.stop_requested = True
        if self.pipeline is not None:
            self.pipeline.set_state(Gst.State.NULL)
        if self.loop is not None and self.loop.is_running():
            self.loop.quit()

    def finalize(self) -> None:
        summary = self.build_summary()
        self.log_event("=== Summary ===")
        self.log_event(f"Total samples          : {summary['total_samples']}")
        self.log_event(f"Observed intervals     : {summary['observed_intervals']}")
        self.log_event(f"Minor stalls           : {summary['minor_stalls']}")
        self.log_event(f"Major stalls           : {summary['major_stalls']}")
        self.log_event(f"PTS jump count         : {summary['pts_jump_count']}")
        self.log_event(f"Output gap frames total: {summary['estimated_output_gap_frames_total']}")
        self.log_event(f"Duration (s)           : {summary['duration_s']}")
        self.log_event(f"Samples per second     : {summary['samples_per_s']}")
        self.log_event(f"Throughput kbps        : {summary['throughput_kbps']}")
        self.write_run_info_file(final=True)
        self.close_outputs()


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Sender MP4 local throughput probe")
    parser.add_argument("--config", required=True, help="Path to experiment JSON config")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    config_path = Path(args.config)
    if not config_path.is_file():
        print(f"Config file not found: {config_path}", file=sys.stderr)
        return 1
    with config_path.open("r", encoding="utf-8") as f:
        config = json.load(f)
    mp4_path = Path(resolve_preencoded_mp4_path(config))
    if not mp4_path.is_file():
        print(f"Preencoded MP4 not found: {mp4_path}", file=sys.stderr)
        return 1

    gi.require_version("Gst", "1.0")
    gi.require_version("GLib", "2.0")
    global Gst, GLib
    from gi.repository import GLib as _GLib  # type: ignore
    from gi.repository import Gst as _Gst  # type: ignore

    Gst = _Gst
    GLib = _GLib
    Gst.init(None)

    app = SenderStatsApp(config)

    def handle_signal(_signum: int, _frame: Any) -> None:
        app.log_event("Signal received, stopping sender MP4 probe.")
        app.stop()

    signal.signal(signal.SIGINT, handle_signal)
    signal.signal(signal.SIGTERM, handle_signal)

    try:
        app.start()
    except Exception as exc:
        print(f"Sender MP4 probe failed: {exc}", file=sys.stderr)
        return 1
    finally:
        app.finalize()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())