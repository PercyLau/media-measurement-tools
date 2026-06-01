#!/usr/bin/env python3
"""Hardware codec throughput benchmark.

Measures encode/decode FPS for hardware and software codecs at specified
resolution and target framerate.  Uses videotestsrc to eliminate disk I/O.

Usage:
  uv run python scripts/bench_codec.py --width 3840 --height 2160 --fps 90
  uv run python scripts/bench_codec.py -W 1920 -H 1080 -f 60 --mode decode
  uv run python scripts/bench_codec.py -W 3840 -H 2160 -f 90 --codec h265 --duration 15
"""

from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Optional

import gi

Gst: Any = None
GLib: Any = None

# ── helpers ────────────────────────────────────────────────────────────────

KNOWN_ENCODERS: dict[str, list[str]] = {
    "h264": ["nvh264enc", "vaapih264enc", "x264enc"],
    "h265": ["nvh265enc", "vaapih265enc", "x265enc"],
}

KNOWN_DECODERS: dict[str, list[str]] = {
    "h264": ["v4l2h264dec", "nvh264dec", "vaapih264dec", "avdec_h264"],
    "h265": ["v4l2h265dec", "nvh265dec", "vaapih265dec", "avdec_h265"],
}

# Software fallbacks for generating test bitstreams (decode benchmark step 1)
BITSTREAM_ENCODERS: dict[str, str] = {"h264": "x264enc", "h265": "x265enc"}


def element_available(name: str) -> bool:
    """Check whether *name* is a usable GStreamer element on this machine."""
    return shutil.which("gst-inspect-1.0") is not None and subprocess.run(
        ["gst-inspect-1.0", name],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    ).returncode == 0


def detect_codecs(codec_filter: str, mode_filter: str) -> tuple[dict[str, list[str]], dict[str, list[str]]]:
    """Return (encoders, decoders) dicts keyed by codec name (h264 / h265)."""
    codecs = []
    if codec_filter in ("h264", "both"):
        codecs.append("h264")
    if codec_filter in ("h265", "both"):
        codecs.append("h265")

    encoders: dict[str, list[str]] = {}
    decoders: dict[str, list[str]] = {}

    for c in codecs:
        if mode_filter in ("encode", "both"):
            encoders[c] = [e for e in KNOWN_ENCODERS[c] if element_available(e)]
            if not encoders[c]:
                print(f"[warn] No encoder found for {c}", file=sys.stderr)
        if mode_filter in ("decode", "both"):
            decoders[c] = [d for d in KNOWN_DECODERS[c] if element_available(d)]
            if not decoders[c]:
                print(f"[warn] No decoder found for {c}", file=sys.stderr)

    return encoders, decoders


def is_hardware_element(name: str) -> bool:
    """Heuristic: nv*, v4l2*, vaapi* are hardware; avdec_*, x26* are software."""
    hw_prefixes = ("nv", "v4l2", "vaapi", "qsv", "amf", "omx")
    return any(name.startswith(p) for p in hw_prefixes)


# ── benchmark runner ───────────────────────────────────────────────────────

@dataclass
class BenchResult:
    codec: str
    element: str
    mode: str  # "encode" or "decode"
    hardware: bool
    width: int
    height: int
    target_fps: int
    measured_fps: float
    total_frames: int
    duration_s: float
    realtime_ratio: float  # measured_fps / target_fps


@dataclass
class BenchmarkRunner:
    width: int
    height: int
    target_fps: int
    duration_s: int
    mode_filter: str  # "encode" | "decode" | "both"
    codec_filter: str  # "h264" | "h265" | "both"

    results: list[BenchResult] = field(default_factory=list)

    # internal state for appsink callbacks
    _frame_count: int = 0
    _first_frame_ns: int = 0
    _last_frame_ns: int = 0
    _loop: Any = None
    _pipeline: Any = None
    _error: Optional[str] = None

    @property
    def num_buffers(self) -> int:
        return self.target_fps * self.duration_s

    # ── pipeline helpers ──────────────────────────────────────────────────

    def _on_new_sample(self, sink: Any) -> Any:
        sample = sink.emit("pull-sample")
        if sample is None:
            return Gst.FlowReturn.ERROR
        now = time.monotonic_ns()
        if self._first_frame_ns == 0:
            self._first_frame_ns = now
        self._last_frame_ns = now
        self._frame_count += 1
        return Gst.FlowReturn.OK

    def _on_bus_message(self, _bus: Any, message: Any) -> None:
        t = message.type
        if t == Gst.MessageType.EOS:
            self._loop.quit()
        elif t == Gst.MessageType.ERROR:
            err, debug = message.parse_error()
            self._error = f"{err}: {debug or ''}"
            self._loop.quit()

    def _run_pipeline(self, desc: str, label: str) -> tuple[int, float]:
        """Run *desc*, return (total_frames, wall_seconds)."""
        self._frame_count = 0
        self._first_frame_ns = 0
        self._last_frame_ns = 0
        self._error = None

        pipeline = Gst.parse_launch(desc)
        sink = pipeline.get_by_name("benchsink")
        if sink is None:
            raise RuntimeError(f"{label}: appsink 'benchsink' not found in pipeline")

        sink.connect("new-sample", self._on_new_sample)
        bus = pipeline.get_bus()
        bus.add_signal_watch()
        bus.connect("message", self._on_bus_message)

        self._loop = GLib.MainLoop()
        pipeline.set_state(Gst.State.PLAYING)

        try:
            self._loop.run()
        except KeyboardInterrupt:
            pass
        finally:
            pipeline.set_state(Gst.State.NULL)

        if self._error:
            raise RuntimeError(f"{label}: {self._error}")

        duration_s = (self._last_frame_ns - self._first_frame_ns) / 1e9 if self._first_frame_ns else 0.0
        return self._frame_count, max(duration_s, 1e-9)

    # ── encode benchmark ──────────────────────────────────────────────────

    def _encode_desc(self, encoder: str, codec: str) -> str:
        # NVENC requires NV12; most software encoders accept I420.
        # Output the correct format directly from videotestsrc to avoid
        # videoconvert overhead (~30% at 4K) being counted as encode cost.
        if encoder.startswith("nv"):
            raw_format = "NV12"
            # Fastest settings: minimum latency, no B-frames
            encoder_props = f"{encoder} preset=p1 zerolatency=true"
        else:
            raw_format = "I420"
            encoder_props = encoder
        return (
            f"videotestsrc is-live=false num-buffers={self.num_buffers} "
            f"! video/x-raw,format={raw_format},width={self.width},height={self.height},"
            f"framerate={self.target_fps}/1 "
            f"! {encoder_props} "
            f"! appsink name=benchsink emit-signals=true sync=false max-buffers=64 drop=false"
        )

    def run_encode(self, encoder: str, codec: str) -> BenchResult:
        desc = self._encode_desc(encoder, codec)
        label = f"encode {codec}/{encoder}"
        print(f"  {label} ...", end=" ", flush=True)
        t0 = time.monotonic()
        frames, dur = self._run_pipeline(desc, label)
        wall = time.monotonic() - t0
        fps = (frames - 1) / dur if dur > 0 and frames > 1 else 0.0
        ratio = fps / self.target_fps
        tag = "OK" if ratio >= 1.0 else "BELOW"
        print(f"{frames} frames in {dur:.2f}s → {fps:.1f} fps ({ratio:.2f}x realtime) {tag}  [wall {wall:.1f}s]")

        return BenchResult(
            codec=codec,
            element=encoder,
            mode="encode",
            hardware=is_hardware_element(encoder),
            width=self.width,
            height=self.height,
            target_fps=self.target_fps,
            measured_fps=round(fps, 1),
            total_frames=frames,
            duration_s=round(dur, 3),
            realtime_ratio=round(ratio, 3),
        )

    # ── decode benchmark ──────────────────────────────────────────────────

    def _generate_bitstream(self, codec: str) -> Path:
        """Encode a test bitstream.  Prefers NVENC for speed; falls back to software
        x264enc/x265enc.  This step is not timed.  """
        tmp = Path(tempfile.gettempdir()) / f"codec_bench.{'h264' if codec == 'h264' else 'h265'}"
        tmp.unlink(missing_ok=True)

        parser = "h264parse" if codec == "h264" else "h265parse"

        # pick an encoder that can generate the bitstream quickly
        enc_candidates = []
        for c in KNOWN_ENCODERS[codec]:
            if element_available(c):
                enc_candidates.append(c)

        if not enc_candidates:
            raise RuntimeError(
                f"No encoder available for {codec} — cannot generate decode test bitstream"
            )

        # Use the first available encoder (HW preferred since KNOWN_ENCODERS lists HW first)
        gen_enc = enc_candidates[0]
        if gen_enc.startswith("nv"):
            raw_format = "NV12"
            enc_str = f"{gen_enc} preset=p1 zerolatency=true"
        else:
            raw_format = "I420"
            enc_str = gen_enc
        print(f"  generating bitstream ({gen_enc}) ...", end=" ", flush=True)
        t0 = time.monotonic()

        desc = (
            f"videotestsrc is-live=false num-buffers={self.num_buffers} "
            f"! video/x-raw,format={raw_format},width={self.width},height={self.height},"
            f"framerate={self.target_fps}/1 "
            f"! {enc_str} "
            f"! {parser} "
            f"! filesink location={tmp}"
        )
        pipeline = Gst.parse_launch(desc)
        bus = pipeline.get_bus()
        pipeline.set_state(Gst.State.PLAYING)
        msg = bus.timed_pop_filtered(Gst.CLOCK_TIME_NONE, Gst.MessageType.EOS | Gst.MessageType.ERROR)
        pipeline.set_state(Gst.State.NULL)
        if msg and msg.type == Gst.MessageType.ERROR:
            err, debug = msg.parse_error()
            raise RuntimeError(f"Bitstream generation failed: {err}: {debug or ''}")
        wall = time.monotonic() - t0
        size_mb = tmp.stat().st_size / 1e6 if tmp.exists() else 0
        print(f"{size_mb:.1f} MB in {wall:.1f}s")
        return tmp

    def _decode_desc(self, decoder: str, codec: str, src: Path) -> str:
        parser = "h264parse" if codec == "h264" else "h265parse"
        return (
            f'filesrc location={src} '
            f"! {parser} "
            f"! {decoder} "
            f"! appsink name=benchsink emit-signals=true sync=false max-buffers=64 drop=false"
        )

    def run_decode(self, decoder: str, codec: str) -> BenchResult:
        label = f"decode {codec}/{decoder}"
        print(f"  {label} ...", end=" ", flush=True)

        # Step 1: generate bitstream (not counted in measurement)
        bitstream = self._generate_bitstream(codec)

        # Step 2: decode and measure
        desc = self._decode_desc(decoder, codec, bitstream)
        t0 = time.monotonic()
        frames, dur = self._run_pipeline(desc, label)
        wall = time.monotonic() - t0
        bitstream.unlink(missing_ok=True)

        fps = (frames - 1) / dur if dur > 0 and frames > 1 else 0.0
        ratio = fps / self.target_fps
        tag = "OK" if ratio >= 1.0 else "BELOW"
        print(f"{frames} frames in {dur:.2f}s → {fps:.1f} fps ({ratio:.2f}x realtime) {tag}  [wall {wall:.1f}s]")

        return BenchResult(
            codec=codec,
            element=decoder,
            mode="decode",
            hardware=is_hardware_element(decoder),
            width=self.width,
            height=self.height,
            target_fps=self.target_fps,
            measured_fps=round(fps, 1),
            total_frames=frames,
            duration_s=round(dur, 3),
            realtime_ratio=round(ratio, 3),
        )

    # ── orchestration ─────────────────────────────────────────────────────

    def run(self) -> None:
        encoders, decoders = detect_codecs(self.codec_filter, self.mode_filter)

        print(f"\nBenchmark: {self.width}x{self.height} @ {self.target_fps} fps, "
              f"{self.duration_s}s ({self.num_buffers} frames)\n")

        if self.mode_filter in ("encode", "both"):
            print("── Encode ──")
            for c, elems in encoders.items():
                for e in elems:
                    try:
                        self.results.append(self.run_encode(e, c))
                    except Exception as exc:
                        print(f"  FAIL: {exc}")

        if self.mode_filter in ("decode", "both"):
            print("\n── Decode ──")
            for c, elems in decoders.items():
                for e in elems:
                    try:
                        self.results.append(self.run_decode(e, c))
                    except Exception as exc:
                        print(f"  FAIL: {exc}")

        self._print_summary()

    def _print_summary(self) -> None:
        if not self.results:
            print("\nNo results.", file=sys.stderr)
            return

        print(f"\n{'='*90}")
        print(f"Summary: {self.width}x{self.height} @ {self.target_fps}fps target  ({self.duration_s}s)")
        print(f"{'='*90}")
        print(f"{'Codec':<6} {'Element':<20} {'Mode':<8} {'HW':<4} {'Frames':<8} {'Dur(s)':<10} {'FPS':<10} {'xRealtime':<10}")
        print("-" * 90)

        for r in self.results:
            hw = "HW" if r.hardware else "SW"
            print(
                f"{r.codec:<6} {r.element:<20} {r.mode:<8} {hw:<4} "
                f"{r.total_frames:<8} {r.duration_s:<10.3f} {r.measured_fps:<10.1f} {r.realtime_ratio:<10.2f}"
            )

        # Also print CSV line for easy capture
        print(f"\n── CSV ──")
        print("codec,element,mode,hardware,width,height,target_fps,measured_fps,total_frames,duration_s,realtime_ratio")
        for r in self.results:
            print(
                f"{r.codec},{r.element},{r.mode},{str(r.hardware).lower()},"
                f"{r.width},{r.height},{r.target_fps},{r.measured_fps},"
                f"{r.total_frames},{r.duration_s},{r.realtime_ratio}"
            )


# ── CLI ────────────────────────────────────────────────────────────────────

def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(
        description="Hardware codec throughput benchmark",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  %(prog)s -W 3840 -H 2160 -f 90
  %(prog)s -W 1920 -H 1080 -f 60 --mode decode
  %(prog)s -W 3840 -H 2160 -f 90 --codec h265 --duration 15
  %(prog)s -W 1280 -H 720 -f 30 --mode encode --codec h264
        """,
    )
    p.add_argument("-W", "--width", type=int, required=True, help="Video width")
    p.add_argument("-H", "--height", type=int, required=True, help="Video height")
    p.add_argument("-f", "--fps", type=int, required=True, help="Target framerate")
    p.add_argument("-d", "--duration", type=int, default=10, help="Test duration in seconds (default: 10)")
    p.add_argument("--mode", choices=["encode", "decode", "both"], default="both", help="Benchmark mode (default: both)")
    p.add_argument("--codec", choices=["h264", "h265", "both"], default="both", help="Codec to test (default: both)")
    return p.parse_args()


def main() -> int:
    args = parse_args()

    gi.require_version("Gst", "1.0")
    gi.require_version("GLib", "2.0")
    global Gst, GLib
    from gi.repository import GLib as _GLib
    from gi.repository import Gst as _Gst

    Gst = _Gst
    GLib = _GLib
    Gst.init(sys.argv)

    runner = BenchmarkRunner(
        width=args.width,
        height=args.height,
        target_fps=args.fps,
        duration_s=args.duration,
        mode_filter=args.mode,
        codec_filter=args.codec,
    )
    runner.run()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
