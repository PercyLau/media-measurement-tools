#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
  echo "Usage: $0 <config.json>"
  exit 1
fi

CONFIG="$1"

PORT=$(jq -r '.network.port' "$CONFIG")
PT=$(jq -r '.network.rtp_payload_type' "$CONFIG")
CLOCK_RATE=$(jq -r '.network.clock_rate' "$CONFIG")
LATENCY=$(jq -r '.network.jitterbuffer_latency_ms' "$CONFIG")
CODEC=$(jq -r '.encoder.codec' "$CONFIG")

case "$CODEC" in
  h264)
    DEPAY="rtph264depay"
    DECODER="avdec_h264"
    ENCODING_NAME="H264"
    ;;
  h265)
    DEPAY="rtph265depay"
    DECODER="avdec_h265"
    ENCODING_NAME="H265"
    ;;
  *)
    echo "Unsupported codec: $CODEC"
    exit 1
    ;;
esac

gst-launch-1.0 -v \
  udpsrc port="$PORT" caps="application/x-rtp,media=video,encoding-name=${ENCODING_NAME},payload=${PT},clock-rate=${CLOCK_RATE}" ! \
  rtpjitterbuffer latency="$LATENCY" ! \
  $DEPAY ! \
  $DECODER ! \
  videoconvert ! \
  autovideosink sync=true