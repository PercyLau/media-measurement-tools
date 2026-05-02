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

# 根据 codec 选择编码器与 RTP payloader。
#
# 当前默认:
#   h264 -> x264enc + rtph264pay
#   h265 -> x265enc + rtph265pay
#
# 备注:
#   如果后面切换到硬件编码，通常这里会改成:
#     ENCODER_ELEMENT="v4l2h264enc extra-params..."
#   但不同平台参数差异很大，所以这里只留最小占位。
resolve_encoder() {
  local codec="$1"
  local sw_encoder="$2"
  local codec_hw_encoder="$3"
  local candidate=""

  if [[ "$HW_ENC_ENABLED" != "true" ]]; then
    echo "$sw_encoder"
    return
  fi

  if [[ -n "$codec_hw_encoder" ]]; then
    candidate="$codec_hw_encoder"
  elif [[ -n "$HW_ENC_ELEMENT" && "$HW_ENC_ELEMENT" != "auto" && "$HW_ENC_ELEMENT" != "default" ]]; then
    candidate="$HW_ENC_ELEMENT"
  else
    case "$codec" in
      h264) candidate="nvh264enc" ;;
      h265) candidate="nvh265enc" ;;
      *) candidate="" ;;
    esac
  fi

  if [[ -n "$candidate" ]] && gst-inspect-1.0 "$candidate" >/dev/null 2>&1; then
    if encoder_runtime_supported "$codec" "$candidate"; then
      echo "$candidate"
      return
    fi

    echo "[sender.sh] Hardware encoder '${candidate}' detected but failed runtime probe; falling back to ${sw_encoder}" >&2
  fi

  echo "$sw_encoder"
}

build_hw_encoder_element() {
  local codec="$1"
  local encoder_name="$2"
  local bitrate="$3"
  local gop_size="$4"
  local bframes="$5"

  case "$codec" in
    h264|h265)
      echo "$encoder_name bitrate=$bitrate gop-size=$gop_size preset=$NV_PRESET rc-mode=$NV_RC_MODE zerolatency=$NV_ZERO_LATENCY bframes=$bframes"
      ;;
    *)
      echo "$encoder_name bitrate=$bitrate gop-size=$gop_size"
      ;;
  esac
}

encoder_runtime_supported() {
  local codec="$1"
  local encoder_name="$2"
  local parser=""
  local encoder_element=""

  case "$codec" in
    h264) parser="h264parse" ;;
    h265) parser="h265parse" ;;
    *) return 1 ;;
  esac

  encoder_element="$(build_hw_encoder_element "$codec" "$encoder_name" 1000 30 0)"
  # Use conservative probe parameters: ensure width >= plugin minimum (>=145)
  # and use a safe preset to avoid "Selected preset not supported" failures.
  local old_nv_preset="$NV_PRESET"
  NV_PRESET="default"
  encoder_element="$(build_hw_encoder_element "$codec" "$encoder_name" 1000 30 0)"
  gst-launch-1.0 -q \
    videotestsrc num-buffers=1 ! \
    video/x-raw,format=NV12,width=256,height=144,framerate=30/1 ! \
    $encoder_element ! \
    "$parser" ! \
    fakesink >/dev/null 2>&1
  NV_PRESET="$old_nv_preset"
}

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