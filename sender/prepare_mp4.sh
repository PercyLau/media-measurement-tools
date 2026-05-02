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
BIT_DEPTH=$(jq -r '.video_input.bit_depth // empty' "$CONFIG")

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

if [[ ! -f "$VIDEO_PATH" ]]; then
  echo "Error: raw input video does not exist: $VIDEO_PATH"
  exit 1
fi

VIDEO_SIZE_BYTES=$(stat -c '%s' "$VIDEO_PATH")

VIDEO_BASENAME=$(basename "$VIDEO_PATH")
VIDEO_STEM="${VIDEO_BASENAME%.*}"

if [[ "$VIDEO_BASENAME" =~ _([0-9]+)x([0-9]+)_([0-9]+)fps_420_([0-9]+)bit_ ]]; then
  FILE_WIDTH="${BASH_REMATCH[1]}"
  FILE_HEIGHT="${BASH_REMATCH[2]}"
  FILE_FPS="${BASH_REMATCH[3]}"
  FILE_BIT_DEPTH="${BASH_REMATCH[4]}"

  if [[ "$WIDTH" != "$FILE_WIDTH" || "$HEIGHT" != "$FILE_HEIGHT" ]]; then
    echo "Error: config video_input.width/height (${WIDTH}x${HEIGHT}) do not match file name hint (${FILE_WIDTH}x${FILE_HEIGHT})."
    exit 1
  fi

  if [[ "$SOURCE_FRAMERATE" != "$FILE_FPS" ]]; then
    echo "Error: config video_input.source_framerate (${SOURCE_FRAMERATE}) does not match file name hint (${FILE_FPS})."
    exit 1
  fi

  if [[ -z "$BIT_DEPTH" ]]; then
    BIT_DEPTH="$FILE_BIT_DEPTH"
  elif [[ "$BIT_DEPTH" != "$FILE_BIT_DEPTH" ]]; then
    echo "Error: config video_input.bit_depth (${BIT_DEPTH}) does not match file name hint (${FILE_BIT_DEPTH})."
    exit 1
  fi
fi

if [[ -z "$BIT_DEPTH" ]]; then
  echo "Error: video_input.bit_depth is required when the file name does not encode bit depth."
  exit 1
fi

if [[ "$BIT_DEPTH" != "8" && "$BIT_DEPTH" != "10" ]]; then
  echo "Error: unsupported video_input.bit_depth: $BIT_DEPTH"
  exit 1
fi

sanitize_name() {
  printf '%s' "$1" | tr '[:upper:]' '[:lower:]' | sed 's/[^a-z0-9._-]/_/g; s/__\+/_/g; s/^_//; s/_$//'
}

build_default_mp4_path() {
  local input_stem
  input_stem="$(sanitize_name "$VIDEO_STEM")"
  printf 'prepared/%s_%sx%s_%sfps_%sfps_%s_%skbps_%sbit.mp4' \
    "$input_stem" "$WIDTH" "$HEIGHT" "$SOURCE_FRAMERATE" "$FRAMERATE" "$CODEC" "$BITRATE" "$BIT_DEPTH"
}

if [[ -z "$PREENCODED_MP4_PATH" || "$PREENCODED_MP4_PATH" == "null" || "$PREENCODED_MP4_PATH" == "auto" ]]; then
  PREENCODED_MP4_PATH="$(build_default_mp4_path)"
fi

mkdir -p "$(dirname "$PREENCODED_MP4_PATH")"

to_rawvideoparse_format() {
  local format_name="$1"
  case "$format_name" in
    i420_10le) echo 'i420-10le' ;;
    i420_10be) echo 'i420-10be' ;;
    i422_10le) echo 'i422-10le' ;;
    i422_10be) echo 'i422-10be' ;;
    y444_10le) echo 'y444-10le' ;;
    y444_10be) echo 'y444-10be' ;;
    p010_10le) echo 'p010-10le' ;;
    p010_10be) echo 'p010-10be' ;;
    nv12_10le32) echo 'nv12-10le32' ;;
    nv12_10le40) echo 'nv12-10le40' ;;
    nv12_10be_8l128) echo 'nv12-10be-8l128' ;;
    *) echo "$format_name" ;;
  esac
}

RAWVIDEOPARSE_FORMAT="$(to_rawvideoparse_format "$FORMAT_LOWER")"

FRAME_SIZE_BYTES=""
case "$FORMAT_LOWER" in
  i420|yv12|nv12)
    FRAME_SIZE_BYTES=$((WIDTH * HEIGHT * 3 / 2))
    ;;
  i420_10le|p010_10le)
    FRAME_SIZE_BYTES=$((WIDTH * HEIGHT * 3))
    ;;
esac

if [[ -n "$FRAME_SIZE_BYTES" ]]; then
  if (( VIDEO_SIZE_BYTES < FRAME_SIZE_BYTES )); then
    echo "Error: raw input is smaller than one frame for format=$FORMAT_LOWER width=$WIDTH height=$HEIGHT."
    echo "File size bytes : $VIDEO_SIZE_BYTES"
    echo "Frame size bytes: $FRAME_SIZE_BYTES"
    exit 1
  fi

  if (( VIDEO_SIZE_BYTES % FRAME_SIZE_BYTES != 0 )); then
    echo "Warning: raw input size is not an integer multiple of one frame."
    echo "File size bytes : $VIDEO_SIZE_BYTES"
    echo "Frame size bytes: $FRAME_SIZE_BYTES"
  fi
fi

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

TARGET_RAW_FORMAT=""
case "$CODEC:$BIT_DEPTH:$FORMAT_LOWER" in
  h264:8:i420|h264:8:yv12)
    TARGET_RAW_FORMAT='I420'
    ;;
  h264:8:nv12)
    TARGET_RAW_FORMAT='NV12'
    ;;
  h264:10:i420_10le)
    TARGET_RAW_FORMAT='I420_10LE'
    ;;
  h265:8:i420|h265:8:yv12|h265:8:nv12)
    TARGET_RAW_FORMAT='I420'
    ;;
  h265:10:i420_10le|h265:10:p010_10le)
    TARGET_RAW_FORMAT='I420_10LE'
    ;;
  h264:10:p010_10le)
    echo 'Error: 10-bit p010_10le input is not supported for H.264 in this workflow. Use h265 + i420_10le/p010_10le instead.'
    exit 1
    ;;
  *)
    echo "Error: unsupported raw format / bit depth / codec combination: format=$FORMAT_LOWER bit_depth=$BIT_DEPTH codec=$CODEC"
    echo 'Supported examples: 8-bit i420/nv12 with h264 or h265, 10-bit i420_10le with h264 or h265, 10-bit p010_10le with h265.'
    exit 1
    ;;
esac

CONVERT_STAGE="videoconvert ! video/x-raw,format=${TARGET_RAW_FORMAT} !"
if [[ "${FORMAT_LOWER^^}" == "$TARGET_RAW_FORMAT" ]]; then
  CONVERT_STAGE=''
fi

echo "=== MP4 Prepare Configuration ==="
echo "Config file     : $CONFIG"
echo "Raw input       : $VIDEO_PATH"
echo "Raw bytes       : $VIDEO_SIZE_BYTES"
echo "Output MP4      : $PREENCODED_MP4_PATH"
echo "Codec           : $CODEC"
echo "Encoder name    : $ENCODER_NAME"
echo "Encoder         : $ENCODER_ELEMENT"
echo "Raw format      : $FORMAT"
echo "Parser format   : $RAWVIDEOPARSE_FORMAT"
echo "Bit depth       : $BIT_DEPTH"
echo "Target raw fmt  : $TARGET_RAW_FORMAT"
echo "Source FPS      : $SOURCE_FRAMERATE"
echo "Output FPS      : $FRAMERATE"
echo "==============================="

gst-launch-1.0 -e -v \
  filesrc location="$VIDEO_PATH" ! \
  rawvideoparse format="$RAWVIDEOPARSE_FORMAT" width="$WIDTH" height="$HEIGHT" framerate="${SOURCE_FRAMERATE}/1" ! \
  videorate drop-only=true ! \
  video/x-raw,framerate="${FRAMERATE}/1" ! \
  $CONVERT_STAGE \
  $ENCODER_ELEMENT ! \
  $PARSER_ELEMENT ! \
  mp4mux faststart=true ! \
  filesink location="$PREENCODED_MP4_PATH"