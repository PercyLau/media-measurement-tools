# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

RTP ARM Phase 1 - A minimal experiment platform for studying receiver-side frame processing rhythm, stutter events, and frame interval jitter in RTP/UDP video streaming from WSL (sender) to ARM Debian (receiver).

## Quick Start

### Setup (fresh Ubuntu/Debian machine)
```bash
./scripts/bootstrap_ubuntu_uv.sh
```

### Prepare MP4 assets (sender side, one-time per config)
```bash
./sender/prepare_mp4.sh configs/experiment.json
```

### Run sender (WSL Ubuntu)
```bash
./sender/sender.sh configs/experiment.json
```

### Run sender local probe (verify MP4 path before sending)
```bash
uv run python sender/sender_stats.py --config configs/experiment.json
```

### Run receiver with stats (ARM Debian)
```bash
./receiver/receiver_stats.sh configs/experiment.json
```

### Preview mode (debug with video output)
```bash
./receiver/receiver_stats_preview.sh configs/experiment.json
```

### Vendor plugins (temporary, for experiments)
```bash
source scripts/activate_with_vendor.sh
python receiver/receiver_stats.py --config configs/experiment.json
```

## Architecture

### Sender Pipeline (WSL)

Two-stage workflow:

**Prepare stage** (offline, one-time):
```
raw YUV -> rawvideoparse -> [videorate] -> encoder -> parser -> mp4mux -> MP4 file
```

**Runtime stage** (per experiment):
```
MP4 file -> qtdemux -> parser -> RTP payloader -> udpsink
```

- `sender/prepare_mp4.sh`: Offline encoder from raw YUV to MP4. Supports 8-bit (i420/nv12) and 10-bit (i420_10le/p010_10le) formats.
- `sender/sender.sh`: Reads pre-encoded MP4, demuxes + parses + RTP payloads to UDP. Sends frames paced by buffer timestamps (not burst mode).
- `sender/sender_stats.py`: Local probe that measures MP4 demux -> parse path via appsink. Outputs `sender_metrics.csv` to verify sender runtime path achieves target framerate.
- Hardware encoder fallback: nvh264enc/nvh265enc -> x264enc/x265enc (auto-probed at runtime).
- `sender.preencoded_mp4_path` can be `"auto"` to auto-generate output path from input filename and encoding parameters.

### Receiver Pipeline (ARM Debian)
```
udpsrc -> rtpjitterbuffer -> depay -> decoder -> queue -> appsink/fakesink
```

- Five modes via `receiver.mode`:
  - `depay_only`: udpsrc -> rtpjitterbuffer -> depay -> fakesink (network/RTP debug)
  - `decode_probe`: adds decoder to pipeline (decoder debug)
  - `full_stats`: adds queue + appsink for per-frame metrics
  - `local_mp4_full_stats`: filesrc -> qtdemux -> parser -> decoder -> appsink (local MP4 decode对照 test)
- Hardware decoder (v4l2h264dec/v4l2h265dec) with automatic software fallback (avdec_h264/avdec_h265) on runtime error.
- `receiver/gstreamer_env.sh`: Sets up CIX vendor plugin paths when `receiver.use_vendor_plugins=true`.
- Receiver has automatic decoder fallback: if hardware decoder errors at runtime, pipeline is rebuilt with software decoder.

### Key Components

| File | Purpose |
|------|---------|
| `sender/sender.sh` | Sender launcher; reads JSON, builds GStreamer pipeline |
| `sender/sender_stats.py` | Sender-side MP4 demux probe with per-sample metrics |
| `sender/prepare_mp4.sh` | Offline raw YUV -> MP4 encoder |
| `receiver/receiver_stats.py` | Core receiver with per-frame metrics collection (~1000 lines) |
| `receiver/receiver_stats.sh` | Receiver launcher with optional Vulkan load injection |
| `receiver/receiver_stats_preview.sh` | Preview mode with autovideosink for visual debugging |
| `receiver/gstreamer_env.sh` | CIX vendor plugin path bootstrap |
| `scripts/detect_and_configure_hw.py` | Auto-detects NVENC/V4L2/VAAPI hardware codecs |
| `scripts/bench_codec.py` | Hardware codec throughput benchmark (encode/decode FPS) |
| `scripts/activate_with_vendor.sh` | Activates venv + exports CIX LD_LIBRARY_PATH |
| `scripts/deactivate_vendor.sh` | Reverts vendor plugin environment variables |
| `scripts/bootstrap_ubuntu_uv.sh` | Installs system deps, uv, runs uv sync |
| `configs/experiment.json` | Single source of truth for all experiment parameters |
| `vulkan_mem_press/vk_memstress.cpp` | GPU memory bandwidth stress tool (~900 lines, Vulkan compute) |
| `cpu_load/cpu_stress.sh` | CPU stress test wrapper (stress-ng, modes: matrix/stream/cache/cpu/all) |
| `llm_load/llm_stress.sh` | LLM inference load wrapper (ollama, supports phi4/lfm2/etc.) |
| `llm_load/benchmark_models.sh` | Ollama model benchmarking tool (3 models, CSV output) |

### Vulkan Stress Tool

Build with `make` (or `cmake . && make`). Two targets: `vk_memstress` (full stress tool) and `vk_compute_min` (minimal compute test). Two shader variants:
- `memstress_alu.comp`: ALU pressure shader (bitwise ops, atomicAdd sink)
- `memstress_bw.comp`: Streaming copy bandwidth test (separate src/dst buffers)

Modes: `rd` (read-only), `wr` (write-only), `rdwr` (read-write). Includes watchdog that auto-reduces `--chunk-iters` on DEVICE_LOST. See `vulkan_mem_press/pressure_tuning.md` for 10-tier parameter guide.

## Configuration

All parameters in `configs/experiment.json`:
- `network.host/port`: UDP destination, MTU, jitterbuffer latency
- `video_input`: path, width/height, source_framerate (raw), framerate (output), format (i420/nv12/i420_10le), bit_depth (8/10)
- `encoder.codec`: h264 or h265; `hardware_encoder_placeholder.enabled` toggles HW encoding
- `sender.preencoded_mp4_path`: path to pre-encoded MP4, or `"auto"` for auto-generated path
- `receiver.mode`: depay_only / decode_probe / full_stats / local_mp4_full_stats
- `receiver.use_vendor_plugins`: whether to load CIX vendor plugin paths (default: false)
- `receiver_load`: Vulkan GPU memory stress test args and enable/disable
- `receiver_load_cpu`: CPU stress test (stress-ng wrapper) args and enable/disable
- `receiver_load_llm`: LLM inference load (ollama wrapper) args and enable/disable
- `stall_thresholds_ms`: fixed_ms or frame_intervals mode for stutter detection
- `receiver.csv_flush_interval`: batch CSV flush period (default: per-frame flush disabled)

### Stall threshold modes

- `frame_intervals` (recommended): minor=1.5x, major=3.0x of theoretical frame interval. Auto-scales with framerate.
- `fixed_ms`: uses hardcoded ms values (legacy, not recommended for cross-framerate comparison).

## Output Artifacts (full_stats mode)

Located in `output/<semantic_name>/<timestamp>_<hash8>/`:
- `receiver_metrics.csv`: Per-frame timing, stall flags, PTS jump detection
- `receiver_events.log`: Timestamped events (ERROR, WARNING, MAJOR_STALL, PTS_JUMP)
- `run_info.json`: Summary stats (p95/p99 delta, stall counts, estimated late frames)
- `resolved_config.json`: Resolved configuration after hardware probes

### CSV columns
```
frame_idx, pts_ns, recv_monotonic_ns, delta_ms, pts_delta_ms, pts_gap_frames, is_pts_jump, estimated_late_frames, is_stall_minor, is_stall_major
```

## Key Metrics

- **delta_ms**: Time between frames at appsink (not display refresh time)
- **minor stall**: delta > threshold (1.5x expected frame interval in frame_intervals mode)
- **major stall**: delta > threshold (3.0x expected frame interval in frame_intervals mode)
- **PTS jump**: pts_gap_frames > 1.5 (heuristic for playback-side frame lateness)
- **estimated_late_frames**: round(pts_gap_frames) - 1 (approximate late/missing frames)
- **p95/p99 delta**: Distribution percentiles from run_info.json

See `docs/metrics.md` for full definitions, and `docs/llm_load_test.md` for LLM load test methodology and known pitfalls.

## Common Tasks

### Check GStreamer elements
```bash
gst-inspect-1.0 nvh264enc    # NVIDIA encoder
gst-inspect-1.0 v4l2h264dec  # V4L2 decoder (Orion O6)
gst-inspect-1.0 rtph264depay # RTP depayloader
```

### Debug receiver pipeline stages
1. Set `receiver.mode = depay_only` - verify network/RTP/depay
2. Set `receiver.mode = decode_probe` - add decoder to pipeline
3. Set `receiver.mode = full_stats` - full metrics collection
4. Use `local_mp4_full_stats` to isolate whether PTS jumps/late frames are from network path or decoder

### Local MP4 decode comparison test
Used to determine if PTS jumps/late frames come from live RTP reception or decoder itself:
```bash
# 1. Copy the same MP4 used by sender to receiver machine
# 2. Set receiver.mode = local_mp4_full_stats in experiment.json
# 3. Run receiver and compare metrics with RTP live receive results
```
See `docs/local_mp4_decode_test.md` for detailed procedure.

### Hardware codec auto-detection
```bash
uv run python scripts/detect_and_configure_hw.py --dry-run   # preview changes
uv run python scripts/detect_and_configure_hw.py             # apply to experiment.json
```

### Codec throughput benchmark
```bash
# Test all available codecs at 4K 90fps
uv run python scripts/bench_codec.py -W 3840 -H 2160 -f 90

# Decode-only, H.265 only
uv run python scripts/bench_codec.py -W 3840 -H 2160 -f 90 --mode decode --codec h265
```

### Vulkan stress test
```bash
cd vulkan_mem_press
make
./vk_memstress --help
```

### Vendor plugins
```bash
# Temporary (per-shell)
source scripts/activate_with_vendor.sh

# System-wide (permanent, requires root)
echo "/usr/share/cix/lib" | sudo tee /etc/ld.so.conf.d/cix.conf
sudo ldconfig
```

## Troubleshooting

### Empty receiver_metrics.csv
When `receiver_load.enabled=false`, the launcher runs `receiver_stats.py` in foreground. If CSV is still empty, check:
- Running in WSL bash (not PowerShell)
- `receiver_events.log` for ERROR/WARNING entries
- GStreamer pipeline actually received frames

### gst-plugin-scanner errors about missing .so files
Ensure `/usr/share/cix/lib` is on `LD_LIBRARY_PATH` or registered via ldconfig. Use `source scripts/activate_with_vendor.sh` for experiments.

### Sender not achieving target framerate
Run `sender/sender_stats.py` to measure the MP4 demux path locally. Check `samples_per_s` in `run_info.json`.

## Platform Notes

- **Sender (WSL)**: WSL Ubuntu 26.04 LTS with NVIDIA RTX (nvh264enc/nvh265enc preferred, x264enc/x265enc fallback)
  - NVENC requires **NV12** input format (not I420) — use `videoconvert` before the encoder
  - GStreamer 1.28.2+
- **Sender (Windows native)**: `sender_stats.py` runs natively on Windows with GStreamer official installer
  - Requires GStreamer Windows installer (winget or MSI) with devel files
  - Requires VS Build Tools (MSVC v143) for PyGObject compilation from source
  - PyGObject 3.50.x only (3.52+ needs girepository-2.0 not in GStreamer Windows installer)
  - `sender_stats.py` auto-adds GStreamer DLL path via `os.add_dll_directory()`; override with `GSTREAMER_BIN_DIR` env var
  - Shell scripts (`sender.sh`, `prepare_mp4.sh`) remain Linux/WSL-only
- **Receiver**: Orion O6 ARM Debian with CIX BSP (v4l2h264dec/v4l2h265dec, avdec fallback)
- Python requires PyGObject (not pure PyPI) — needs gobject-introspection, libgirepository, libcairo2-dev system packages (Linux) or GStreamer installer + MSVC (Windows)
- Shell scripts target Linux/WSL bash; `sender_stats.py` is cross-platform (Linux + Windows)
- `.gitignore` excludes `videos/`, `output/`, `.venv/`, and vulkan_mem_press binaries
- Python 3.13 is default (`.python-version` = 3.13, `requires-python = >=3.10,<3.15`). PyGObject 3.50.x does not compile on Python 3.14
- On Orion O6 ARM Debian (older Python), override with `UV_PYTHON=3.10 uv sync`
