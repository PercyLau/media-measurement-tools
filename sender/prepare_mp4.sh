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

VIDEO_PATH=$(jq -r '.video_input.path' "$CONFIG")
WIDTH=$(jq -r '.video_input.width' "$CONFIG")
HEIGHT=$(jq -r '.video_input.height' "$CONFIG")
SOURCE_FRAMERATE=$(jq -r '.video_input.source_framerate // .video_input.framerate' "$CONFIG")
FRAMERATE=$(jq -r '.video_input.framerate' "$CONFIG")
FORMAT=$(jq -r '.video_input.format' "$CONFIG")
FORMAT_LOWER=$(printf '%s' "$FORMAT" | tr '[:upper:]' '[:lower:]')

CODEC=$(jq -r '.encoder.codec' "$CONFIG")
BITRATE=$(jq -r '.encoder.bitrate_kbps' "$CONFIG")
SPEED_PRESET=$(jq -r '.encoder.speed_preset' "$CONFIG")
TUNE=$(jq -r '.encoder.tune' "$CONFIG")
KEY_INT_MAX=$(jq -r '.encoder.key_int_max' "$CONFIG")
BFRAMES=$(jq -r '.encoder.bframes' "$CONFIG")
THREADS=$(jq -r '.encoder.threads' "$CONFIG")

HW_ENC_ENABLED=$(jq -r '.encoder.hardware_encoder_placeholder.enabled' "$CONFIG")
HW_ENC_ELEMENT=$(jq -r '.encoder.hardware_encoder_placeholder.element' "$CONFIG")
HW_H264_ENC=$(jq -r '.encoder.hardware_encoders.h264 // empty' "$CONFIG")
HW_H265_ENC=$(jq -r '.encoder.hardware_encoders.h265 // empty' "$CONFIG")
NV_PRESET=$(jq -r '.encoder.nvcodec_defaults.preset // "low-latency-hq"' "$CONFIG")
NV_RC_MODE=$(jq -r '.encoder.nvcodec_defaults.rc_mode // "cbr"' "$CONFIG")
NV_ZERO_LATENCY=$(jq -r '.encoder.nvcodec_defaults.zerolatency // true' "$CONFIG")
SW_H264_ENC=$(jq -r '.encoder.software_h264_encoder' "$CONFIG")
SW_H265_ENC=$(jq -r '.encoder.software_h265_encoder' "$CONFIG")

PREENCODED_MP4_PATH=$(jq -r '.sender.preencoded_mp4_path' "$CONFIG")

if [[ -z "$PREENCODED_MP4_PATH" || "$PREENCODED_MP4_PATH" == "null" ]]; then
  echo "Error: sender.preencoded_mp4_path is required."
  exit 1
fi

if [[ ! -f "$VIDEO_PATH" ]]; then
  echo "Error: raw input video does not exist: $VIDEO_PATH"
  exit 1
fi

mkdir -p "$(dirname "$PREENCODED_MP4_PATH")"

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
    echo "$candidate"
    return
  fi

  echo "$sw_encoder"
}

build_hw_encoder_element() {
  local encoder_name="$1"
  echo "$encoder_name bitrate=$BITRATE gop-size=$KEY_INT_MAX preset=$NV_PRESET rc-mode=$NV_RC_MODE zerolatency=$NV_ZERO_LATENCY bframes=$BFRAMES"
}

case "$CODEC" in
  h264)
    ENCODER_NAME="$(resolve_encoder h264 "$SW_H264_ENC" "$HW_H264_ENC")"
    if [[ "$ENCODER_NAME" == "$SW_H264_ENC" ]]; then
      ENCODER_ELEMENT="$SW_H264_ENC tune=$TUNE speed-preset=$SPEED_PRESET bitrate=$BITRATE key-int-max=$KEY_INT_MAX bframes=$BFRAMES threads=$THREADS"
    else
      ENCODER_ELEMENT="$(build_hw_encoder_element "$ENCODER_NAME")"
    fi
    PARSER_ELEMENT='h264parse ! video/x-h264,stream-format=avc,alignment=au'
    ;;
  h265)
    ENCODER_NAME="$(resolve_encoder h265 "$SW_H265_ENC" "$HW_H265_ENC")"
    if [[ "$ENCODER_NAME" == "$SW_H265_ENC" ]]; then
      ENCODER_ELEMENT="$SW_H265_ENC bitrate=$BITRATE"
    else
      ENCODER_ELEMENT="$(build_hw_encoder_element "$ENCODER_NAME")"
    fi
    PARSER_ELEMENT='h265parse ! video/x-h265,stream-format=hvc1,alignment=au'
    ;;
  *)
    echo "Error: unsupported codec: $CODEC"
    exit 1
    ;;
esac

CONVERT_STAGE='videoconvert ! video/x-raw,format=NV12 !'
if [[ "$FORMAT_LOWER" == "nv12" ]]; then
  CONVERT_STAGE=''
fi

echo "=== MP4 Prepare Configuration ==="
echo "Config file     : $CONFIG"
echo "Raw input       : $VIDEO_PATH"
echo "Output MP4      : $PREENCODED_MP4_PATH"
echo "Codec           : $CODEC"
echo "Encoder name    : $ENCODER_NAME"
echo "Encoder         : $ENCODER_ELEMENT"
echo "Source FPS      : $SOURCE_FRAMERATE"
echo "Output FPS      : $FRAMERATE"
echo "==============================="

gst-launch-1.0 -e -v \
  filesrc location="$VIDEO_PATH" ! \
  rawvideoparse format="$FORMAT" width="$WIDTH" height="$HEIGHT" framerate="${SOURCE_FRAMERATE}/1" ! \
  videorate drop-only=true ! \
  video/x-raw,framerate="${FRAMERATE}/1" ! \
  $CONVERT_STAGE \
  $ENCODER_ELEMENT ! \
  $PARSER_ELEMENT ! \
  mp4mux faststart=true ! \
  filesink location="$PREENCODED_MP4_PATH"