# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

RTP ARM Phase 1 - A minimal experiment platform for studying receiver-side frame processing rhythm, stutter events, and frame interval jitter in RTP/UDP video streaming from WSL (sender) to ARM Debian (receiver).

## Quick Start

### Setup (fresh Ubuntu/Debian machine)
```bash
./scripts/bootstrap_ubuntu_uv.sh
```

### Run sender (WSL Ubuntu)
```bash
./sender/sender.sh configs/experiment.json
```

### Run receiver with stats (ARM Debian)
```bash
./receiver/receiver_stats.sh configs/experiment.json
```

### Preview mode (debug with video output)
```bash
./receiver/receiver_stats_preview.sh configs/experiment.json
```

## Architecture

### Sender Pipeline (WSL)
```
raw YUV -> rawvideoparse -> [videorate] -> encoder -> parser -> RTP payloader -> udpsink
```
- Reads raw YUV files, encodes to H.264/H.265, sends via RTP/UDP
- Supports hardware encoder fallback (nvh264enc/nvh265enc -> x264enc/x265enc)
- Sends frames paced by buffer timestamps (not burst mode)

### Receiver Pipeline (ARM Debian)
```
udpsrc -> rtpjitterbuffer -> depay -> decoder -> queue -> appsink/fakesink
```
- Three modes via `receiver.mode`: `depay_only`, `decode_probe`, `full_stats`
- Hardware decoder support (v4l2h264dec/v4l2h265dec) with software fallback
- `full_stats` mode: captures per-frame metrics via appsink

### Key Components

| File | Purpose |
|------|---------|
| `sender/sender.sh` | Sender launcher; reads JSON, builds GStreamer pipeline |
| `receiver/receiver_stats.py` | Core receiver with metrics collection |
| `receiver/receiver_stats.sh` | Receiver launcher with optional load injection |
| `configs/experiment.json` | Single source of truth for all experiment parameters |

## Configuration

All parameters in `configs/experiment.json`:
- `network.host/port`: UDP destination
- `video_input`: path, resolution, source_framerate, output framerate
- `encoder.codec`: h264 or h265; hardware_encoder_placeholder.enabled toggles HW encoding
- `receiver.mode`: depay_only / decode_probe / full_stats
- `receiver_load`: Vulkan memory stress test for loading receiver during experiments
- `stall_thresholds_ms`: fixed_ms or frame_intervals mode for stutter detection

## Output Artifacts (full_stats mode)

Located in `output/<semantic_name>/<timestamp>_<hash8>/`:
- `receiver_metrics.csv`: Per-frame timing, stall flags, PTS jump detection
- `receiver_events.log`: Timestamped events (ERROR, WARNING, MAJOR_STALL, PTS_JUMP)
- `run_info.json`: Summary stats (p95/p99 delta, stall counts, estimated dropped frames)

## Key Metrics

- **delta_ms**: Time between frames at appsink (not display refresh time)
- **minor stall**: delta > 1.5x expected frame interval
- **major stall**: delta > 3.0x expected frame interval
- **PTS jump**: pts_gap_frames > 1.5 (indicates source-side frame drop)

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

### Vulkan stress test
```bash
cd vulkan_mem_press
make
./vk_memstress --help
```

## Platform Notes

- **Sender**: WSL Ubuntu with NVIDIA RTX (nvh264enc/nvh265enc preferred)
- **Receiver**: Orion O6 ARM Debian with CIX BSP (v4l2h264dec/v4l2h265dec)
- Receiver scripts auto-inject CIX GStreamer plugin paths (`/usr/share/cix/lib/...`)
- Python requires PyGObject with system dependencies (not pure PyPI)
