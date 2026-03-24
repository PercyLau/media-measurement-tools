#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
  echo "Usage: $0 <config.json>"
  exit 1
fi

CONFIG="$1"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Make Orion O6 / CIX GStreamer plugins visible even in fresh shells / venvs.
# shellcheck source=/dev/null
source "${SCRIPT_DIR}/gstreamer_env.sh"

PORT=$(jq -r '.network.port' "$CONFIG")
PT=$(jq -r '.network.rtp_payload_type' "$CONFIG")
CLOCK_RATE=$(jq -r '.network.clock_rate' "$CONFIG")
LATENCY=$(jq -r '.network.jitterbuffer_latency_ms' "$CONFIG")
CODEC=$(jq -r '.encoder.codec' "$CONFIG")
HW_DEC_ENABLED=$(jq -r '.receiver.hardware_decoder_placeholder.enabled // false' "$CONFIG")
HW_DEC_FALLBACK=$(jq -r '.receiver.hardware_decoder_placeholder.element // ""' "$CONFIG")
HW_H264_DEC=$(jq -r '.receiver.hardware_decoders.h264 // empty' "$CONFIG")
HW_H265_DEC=$(jq -r '.receiver.hardware_decoders.h265 // empty' "$CONFIG")
SW_H264_DEC=$(jq -r '.receiver.software_h264_decoder // "avdec_h264"' "$CONFIG")
SW_H265_DEC=$(jq -r '.receiver.software_h265_decoder // "avdec_h265"' "$CONFIG")

resolve_decoder() {
  local codec="$1"
  local sw_decoder="$2"
  local codec_hw_decoder="$3"
  local candidate=""

  if [[ "$HW_DEC_ENABLED" != "true" ]]; then
    echo "$sw_decoder"
    return
  fi

  if [[ -n "$codec_hw_decoder" ]]; then
    candidate="$codec_hw_decoder"
  elif [[ -n "$HW_DEC_FALLBACK" && "$HW_DEC_FALLBACK" != "auto" && "$HW_DEC_FALLBACK" != "default" ]]; then
    candidate="$HW_DEC_FALLBACK"
  else
    case "$codec" in
      h264) candidate="v4l2h264dec" ;;
      h265) candidate="v4l2h265dec" ;;
      *) candidate="" ;;
    esac
  fi

  if [[ -n "$candidate" ]] && gst-inspect-1.0 "$candidate" >/dev/null 2>&1; then
    echo "$candidate"
    return
  fi

  echo "$sw_decoder"
}

case "$CODEC" in
  h264)
    DEPAY="rtph264depay"
    PARSER="h264parse ! video/x-h264,stream-format=byte-stream,alignment=au"
    DECODER="$(resolve_decoder h264 "$SW_H264_DEC" "$HW_H264_DEC")"
    ENCODING_NAME="H264"
    ;;
  h265)
    DEPAY="rtph265depay"
    PARSER="h265parse ! video/x-h265,stream-format=byte-stream,alignment=au"
    DECODER="$(resolve_decoder h265 "$SW_H265_DEC" "$HW_H265_DEC")"
    ENCODING_NAME="H265"
    ;;
  *)
    echo "Unsupported codec: $CODEC"
    exit 1
    ;;
esac

echo "[receiver_stats_preview.sh] Decoder: ${DECODER}"
echo "[receiver_stats_preview.sh] GST_PLUGIN_PATH_1_0=${GST_PLUGIN_PATH_1_0:-}"
echo "[receiver_stats_preview.sh] GST_PLUGIN_SCANNER =${GST_PLUGIN_SCANNER:-}"

gst-launch-1.0 -v \
  udpsrc port="$PORT" caps="application/x-rtp,media=video,encoding-name=${ENCODING_NAME},payload=${PT},clock-rate=${CLOCK_RATE}" ! \
  rtpjitterbuffer latency="$LATENCY" ! \
  $DEPAY ! \
  $PARSER ! \
  $DECODER ! \
  videoconvert ! \
  autovideosink sync=true
