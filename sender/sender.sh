#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
  echo "Usage: $0 <config.json>"
  exit 1
fi

CONFIG="$1"

if ! command -v jq >/dev/null 2>&1; then
  echo "Error: jq not found. Please install jq first."
  exit 1
fi

if ! command -v gst-launch-1.0 >/dev/null 2>&1; then
  echo "Error: gst-launch-1.0 not found. Please install GStreamer tools first."
  exit 1
fi

HOST=$(jq -r '.network.host' "$CONFIG")
PORT=$(jq -r '.network.port' "$CONFIG")
MTU=$(jq -r '.network.mtu' "$CONFIG")
CODEC=$(jq -r '.encoder.codec' "$CONFIG")
TARGET_FPS=$(jq -r '.video_input.framerate' "$CONFIG")
PREENCODED_MP4_PATH=$(jq -r '.sender.preencoded_mp4_path' "$CONFIG")

if [[ -z "$PREENCODED_MP4_PATH" || "$PREENCODED_MP4_PATH" == "null" ]]; then
  echo "Error: sender.preencoded_mp4_path is required."
  echo "Prepare the asset first: ./sender/prepare_mp4.sh $CONFIG"
  exit 1
fi

if [[ ! -f "$PREENCODED_MP4_PATH" ]]; then
  echo "Error: preencoded MP4 does not exist: $PREENCODED_MP4_PATH"
  echo "Prepare the asset first: ./sender/prepare_mp4.sh $CONFIG"
  exit 1
fi

case "$CODEC" in
  h264)
    PARSER_ELEMENT='h264parse ! video/x-h264,stream-format=byte-stream,alignment=au'
    PAYLOADER_ELEMENT="rtph264pay pt=96 config-interval=1 mtu=$MTU"
    ;;
  h265)
    PARSER_ELEMENT='h265parse ! video/x-h265,stream-format=byte-stream,alignment=au'
    PAYLOADER_ELEMENT="rtph265pay pt=96 config-interval=1 mtu=$MTU"
    ;;
  *)
    echo "Error: unsupported codec: $CODEC"
    exit 1
    ;;
esac

echo "=== Sender Configuration ==="
echo "Config file     : $CONFIG"
echo "Input MP4       : $PREENCODED_MP4_PATH"
echo "Codec           : $CODEC"
echo "Target FPS      : $TARGET_FPS"
echo "Target host     : ${HOST}:${PORT}"
echo "Parser          : ${PARSER_ELEMENT}"
echo "Payloader       : ${PAYLOADER_ELEMENT}"
echo "Pacing          : realtime timestamp-paced sending"
echo "============================"

gst-launch-1.0 -v \
  filesrc location="$PREENCODED_MP4_PATH" ! \
  qtdemux name=demux demux.video_0 ! \
  queue max-size-buffers=0 max-size-bytes=0 max-size-time=0 ! \
  $PARSER_ELEMENT ! \
  $PAYLOADER_ELEMENT ! \
  udpsink host="$HOST" port="$PORT" sync=true async=false