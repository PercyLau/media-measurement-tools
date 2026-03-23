#!/usr/bin/env python3
"""
接收端统计程序。

功能：
    - 接收 RTP/UDP 视频流
    - 解码后通过 appsink 逐帧取样
    - 记录逐帧到达应用侧的本地单调时钟时间
    - 计算 delta_ms / stall flags
    - 每次运行自动创建独立输出目录
    - 输出：
        * receiver_metrics.csv
        * receiver_events.log
        * resolved_config.json
        * run_info.json

输出目录结构：
    output/
      └─ <semantic_name>/
         └─ <timestamp>_<hash8>/
            ├─ receiver_metrics.csv
            ├─ receiver_events.log
            ├─ resolved_config.json
            └─ run_info.json

说明：
    这里统计的是“应用侧取到解码后帧”的时间，不是最终屏幕实际显示时刻。
"""

from __future__ import annotations

import argparse
import csv
import copy
import hashlib
import json
import os
import signal
import socket
import sys
import time
from datetime import datetime
from pathlib import Path
from typing import Any, Dict, Optional

import gi

gi.require_version("Gst", "1.0")
from gi.repository import Gst, GLib  # type: ignore


class ReceiverStatsApp:
    def __init__(self, config: Dict[str, Any]) -> None:
        self.config = config

        network = config["network"]
        receiver = config["receiver"]
        thresholds = config["stall_thresholds_ms"]
        encoder = config["encoder"]
        video_input = config["video_input"]

        self.experiment_name: str = str(config.get("experiment_name", "experiment"))

        self.port: int = int(network["port"])
        self.payload_type: int = int(network["rtp_payload_type"])
        self.clock_rate: int = int(network["clock_rate"])
        self.jitter_latency: int = int(network["jitterbuffer_latency_ms"])

        self.codec: str = str(encoder["codec"]).lower()

        self.video_path: str = str(video_input["path"])
        self.width: int = int(video_input["width"])
        self.height: int = int(video_input["height"])
        self.framerate: int = int(video_input["framerate"])
        self.pixel_format: str = str(video_input["format"])

        self.bitrate_kbps: int = int(encoder["bitrate_kbps"])
        self.key_int_max: int = int(encoder["key_int_max"])
        self.bframes: int = int(encoder["bframes"])

        self.output_root = Path(receiver.get("output_root", "output"))
        self.save_resolved_config: bool = bool(receiver.get("save_resolved_config", True))
        self.save_run_info: bool = bool(receiver.get("save_run_info", True))

        self.minor_threshold_ms: float = float(thresholds["minor"])
        self.major_threshold_ms: float = float(thresholds["major"])

        self.prev_recv_monotonic_ns: Optional[int] = None
        self.frame_idx: int = 0
        self.stall_minor_count: int = 0
        self.stall_major_count: int = 0
        self.sample_count: int = 0

        self.loop: Optional[GLib.MainLoop] = None
        self.pipeline: Optional[Gst.Pipeline] = None
        self.appsink: Optional[Gst.Element] = None

        self.csv_fp = None
        self.csv_writer = None
        self.event_fp = None

        self.run_start_monotonic_ns: int = time.monotonic_ns()
        self.run_start_wall_time: str = datetime.now().astimezone().isoformat(timespec="seconds")

        self.semantic_name: str = self.build_semantic_name()
        self.config_hash8: str = self.build_run_hash()
        self.timestamp_str: str = datetime.now().strftime("%Y%m%dT%H%M%S")
        self.run_dir: Path = self.build_run_dir()

        self.output_csv: Path = self.run_dir / "receiver_metrics.csv"
        self.output_events: Path = self.run_dir / "receiver_events.log"
        self.resolved_config_path: Path = self.run_dir / "resolved_config.json"
        self.run_info_path: Path = self.run_dir / "run_info.json"
        self.stop_requested: bool = False

    @staticmethod
    def sanitize_name(value: str) -> str:
        allowed = []
        for ch in value:
            if ch.isalnum() or ch in ("-", "_", "."):
                allowed.append(ch)
            else:
                allowed.append("_")
        sanitized = "".join(allowed).strip("._")
        return sanitized or "unknown"

    def build_semantic_name(self) -> str:
        video_stem = Path(self.video_path).stem or "video"
        video_stem = self.sanitize_name(video_stem)

        parts = [
            video_stem,
            f"{self.width}x{self.height}",
            f"{self.framerate}fps",
            self.sanitize_name(self.pixel_format),
            self.sanitize_name(self.codec),
            f"{self.bitrate_kbps}kbps",
        ]

        receiver_load = self.config.get("receiver_load", {})
        if receiver_load.get("enabled", False):
            load_binary = Path(str(receiver_load.get("binary", "load"))).stem
            parts.append(f"load_{self.sanitize_name(load_binary)}")

        return "_".join(parts)

    def build_hash_payload(self) -> Dict[str, Any]:
        receiver_load = self.config.get("receiver_load", {})

        return {
            "experiment_name": self.experiment_name,
            "video_input": {
                "path_basename": Path(self.video_path).name,
                "width": self.width,
                "height": self.height,
                "framerate": self.framerate,
                "format": self.pixel_format,
            },
            "encoder": {
                "codec": self.codec,
                "bitrate_kbps": self.bitrate_kbps,
                "key_int_max": self.key_int_max,
                "bframes": self.bframes,
            },
            "network": {
                "port": self.port,
                "rtp_payload_type": self.payload_type,
                "clock_rate": self.clock_rate,
                "jitterbuffer_latency_ms": self.jitter_latency,
            },
            "stall_thresholds_ms": {
                "minor": self.minor_threshold_ms,
                "major": self.major_threshold_ms,
            },
            "receiver_load": {
                "enabled": receiver_load.get("enabled", False),
                "startup_delay_sec": receiver_load.get("startup_delay_sec", 0),
                "workdir": receiver_load.get("workdir", "."),
                "binary": receiver_load.get("binary", ""),
                "args": receiver_load.get("args", []),
            },
        }
    
    def build_run_hash(self) -> str:
        payload = self.build_hash_payload()
        canonical = json.dumps(payload, sort_keys=True, separators=(",", ":"), ensure_ascii=False)
        return hashlib.sha256(canonical.encode("utf-8")).hexdigest()[:8]

    def build_run_dir(self) -> Path:
        return self.output_root / self.semantic_name / f"{self.timestamp_str}_{self.config_hash8}"

    def log_event(self, message: str) -> None:
        ts = time.monotonic_ns()
        line = f"[{ts}] {message}"
        print(line)
        if self.event_fp is not None:
            self.event_fp.write(line + "\n")
            self.event_fp.flush()

    def ensure_output_dirs(self) -> None:
        self.run_dir.mkdir(parents=True, exist_ok=True)

    def write_resolved_config(self) -> None:
        if not self.save_resolved_config:
            return

        resolved = copy.deepcopy(self.config)
        resolved["_resolved"] = {
            "semantic_name": self.semantic_name,
            "config_hash8": self.config_hash8,
            "timestamp": self.timestamp_str,
            "run_dir": str(self.run_dir),
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
            "paths": {
                "receiver_metrics_csv": str(self.output_csv),
                "receiver_events_log": str(self.output_events),
                "resolved_config_json": str(self.resolved_config_path),
                "run_info_json": str(self.run_info_path),
            },
            "summary": {
                "total_samples": self.sample_count,
                "minor_stalls": self.stall_minor_count,
                "major_stalls": self.stall_major_count,
            },
            "finalized": final,
        }

        with self.run_info_path.open("w", encoding="utf-8") as f:
            json.dump(info, f, indent=2, ensure_ascii=False)

    def open_outputs(self) -> None:
        self.ensure_output_dirs()
        self.write_resolved_config()
        self.write_run_info_file(final=False)

        self.csv_fp = self.output_csv.open("w", newline="", encoding="utf-8")
        self.csv_writer = csv.writer(self.csv_fp)
        self.csv_writer.writerow(
            [
                "frame_idx",
                "pts_ns",
                "recv_monotonic_ns",
                "delta_ms",
                "is_stall_minor",
                "is_stall_major",
            ]
        )
        self.csv_fp.flush()

        self.event_fp = self.output_events.open("w", encoding="utf-8")

    def close_outputs(self) -> None:
        if self.csv_fp is not None:
            self.csv_fp.close()
            self.csv_fp = None
        if self.event_fp is not None:
            self.event_fp.close()
            self.event_fp = None

    def build_pipeline_description(self) -> str:
        receiver = self.config["receiver"]

        hw_dec_enabled = bool(receiver["hardware_decoder_placeholder"]["enabled"])
        hw_dec_element = str(receiver["hardware_decoder_placeholder"]["element"])

        sw_h264_dec = str(receiver["software_h264_decoder"])
        sw_h265_dec = str(receiver["software_h265_decoder"])

        if self.codec == "h264":
            depay = "rtph264depay"
            decoder = hw_dec_element if hw_dec_enabled else sw_h264_dec
            encoding_name = "H264"
        elif self.codec == "h265":
            depay = "rtph265depay"
            decoder = hw_dec_element if hw_dec_enabled else sw_h265_dec
            encoding_name = "H265"
        else:
            raise ValueError(f"Unsupported codec: {self.codec}")

        desc = f"""
            udpsrc port={self.port} caps="application/x-rtp,media=video,encoding-name={encoding_name},payload={self.payload_type},clock-rate={self.clock_rate}" !
            rtpjitterbuffer latency={self.jitter_latency} !
            {depay} !
            {decoder} !
            appsink name=mysink emit-signals=true sync=false max-buffers=8 drop=true
        """
        return " ".join(desc.split())

    def on_new_sample(self, sink: Gst.Element) -> Gst.FlowReturn:
        sample = sink.emit("pull-sample")
        if sample is None:
            return Gst.FlowReturn.ERROR

        buf = sample.get_buffer()
        if buf is None:
            self.log_event("Received sample without buffer.")
            return Gst.FlowReturn.ERROR

        recv_ns = time.monotonic_ns()
        pts_ns = int(buf.pts) if buf.pts != Gst.CLOCK_TIME_NONE else -1

        delta_ms = 0.0
        if self.prev_recv_monotonic_ns is not None:
            delta_ms = (recv_ns - self.prev_recv_monotonic_ns) / 1_000_000.0

        is_minor = int(delta_ms > self.minor_threshold_ms)
        is_major = int(delta_ms > self.major_threshold_ms)

        if is_minor:
            self.stall_minor_count += 1
        if is_major:
            self.stall_major_count += 1

        if self.csv_writer is not None:
            self.csv_writer.writerow(
                [
                    self.frame_idx,
                    pts_ns,
                    recv_ns,
                    f"{delta_ms:.3f}",
                    is_minor,
                    is_major,
                ]
            )
            self.csv_fp.flush()

        self.prev_recv_monotonic_ns = recv_ns
        self.frame_idx += 1
        self.sample_count += 1

        return Gst.FlowReturn.OK

    def on_bus_message(self, bus: Gst.Bus, message: Gst.Message) -> None:
        mtype = message.type

        if mtype == Gst.MessageType.ERROR:
            err, debug = message.parse_error()
            self.log_event(f"ERROR: {err}; debug={debug}")
            if self.loop is not None:
                self.loop.quit()

        elif mtype == Gst.MessageType.EOS:
            self.log_event("EOS received.")
            if self.loop is not None:
                self.loop.quit()

        elif mtype == Gst.MessageType.WARNING:
            warn, debug = message.parse_warning()
            self.log_event(f"WARNING: {warn}; debug={debug}")

        elif mtype == Gst.MessageType.QOS:
            self.log_event("QOS message received.")

    def request_stop(self, reason: str) -> None:
        if self.stop_requested:
            return

        self.stop_requested = True
        self.log_event(f"Stop requested: {reason}")
        if self.loop is not None:
            self.loop.quit()

    def on_termination_signal(self, signum: int, _frame: object) -> None:
        try:
            signame = signal.Signals(signum).name
        except ValueError:
            signame = str(signum)
        self.request_stop(f"signal {signame}")

    def write_summary(self) -> None:
        self.log_event("=== Summary ===")
        self.log_event(f"Run directory      : {self.run_dir}")
        self.log_event(f"Semantic name      : {self.semantic_name}")
        self.log_event(f"Config hash        : {self.config_hash8}")
        self.log_event(f"Total samples      : {self.sample_count}")
        self.log_event(f"Minor stalls (> {self.minor_threshold_ms} ms): {self.stall_minor_count}")
        self.log_event(f"Major stalls (> {self.major_threshold_ms} ms): {self.stall_major_count}")
        self.log_event("================")

    def run(self) -> int:
        Gst.init(None)
        self.open_outputs()
        signal.signal(signal.SIGINT, self.on_termination_signal)
        signal.signal(signal.SIGTERM, self.on_termination_signal)

        try:
            pipeline_desc = self.build_pipeline_description()

            print(f"Run directory: {self.run_dir}")
            print(f"Metrics CSV : {self.output_csv}")
            print(f"Events log  : {self.output_events}")

            self.log_event(f"Pipeline: {pipeline_desc}")

            pipeline = Gst.parse_launch(pipeline_desc)
            if not isinstance(pipeline, Gst.Pipeline):
                self.log_event("Failed to create Gst.Pipeline.")
                return 1

            self.pipeline = pipeline
            self.appsink = pipeline.get_by_name("mysink")
            if self.appsink is None:
                self.log_event("Failed to find appsink named 'mysink'.")
                return 1

            self.appsink.connect("new-sample", self.on_new_sample)

            bus = pipeline.get_bus()
            if bus is None:
                self.log_event("Failed to get bus from pipeline.")
                return 1
            bus.add_signal_watch()
            bus.connect("message", self.on_bus_message)

            self.loop = GLib.MainLoop()

            ret = pipeline.set_state(Gst.State.PLAYING)
            if ret == Gst.StateChangeReturn.FAILURE:
                self.log_event("Failed to set pipeline to PLAYING.")
                return 1

            self.log_event("Receiver stats pipeline started.")
            self.loop.run()

            pipeline.set_state(Gst.State.NULL)
            self.write_summary()
            self.write_run_info_file(final=True)
            return 0

        finally:
            if self.pipeline is not None:
                self.pipeline.set_state(Gst.State.NULL)
            self.write_run_info_file(final=True)
            self.close_outputs()


def load_config(path: str) -> Dict[str, Any]:
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)


def main() -> int:
    parser = argparse.ArgumentParser(description="Receiver stats collector")
    parser.add_argument("--config", required=True, help="Path to JSON config")
    args = parser.parse_args()

    config = load_config(args.config)
    app = ReceiverStatsApp(config)
    return app.run()

if __name__ == "__main__":
    sys.exit(main())
