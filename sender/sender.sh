#!/usr/bin/env bash
set -euo pipefail

# 用法:
#   ./sender.sh configs/experiment.json
#
# 说明:
#   该脚本读取 JSON 配置，拼出发送端 GStreamer pipeline，并直接执行。
#
# 当前默认路线:
#   raw YUV -> software encoder -> RTP payloader -> UDP sink
#
# 当前默认发送模式:
#   按 buffer 时间戳平滑发送，而不是“尽快推送”
#
# 当前默认测试:
#   H.264 软件编码 (x264enc)
#
# 预留扩展:
#   1) 切到 H.265:
#      - 将 encoder.codec 改为 "h265"
#      - payloader 会自动切到 rtph265pay
#      - 编码器默认会尝试使用 software_h265_encoder (例如 x265enc)
#
#   2) 切到硬件编码:
#      - 将 encoder.hardware_encoder_placeholder.enabled 设为 true
#      - 并把 element 改成你的平台实际可用的编码器
#      - 例如部分 ARM / SoC / GPU 平台可能是:
#          v4l2h264enc
#          vaapih264enc
#          nvh264enc
#          mpph264enc
#      - 这里只留占位，当前未测试，不保证各平台参数兼容
#
# 注意:
#   shell 里解析 JSON 依赖 jq:
#       sudo apt install -y jq

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

VIDEO_PATH=$(jq -r '.video_input.path' "$CONFIG")
WIDTH=$(jq -r '.video_input.width' "$CONFIG")
HEIGHT=$(jq -r '.video_input.height' "$CONFIG")
SOURCE_FRAMERATE=$(jq -r '.video_input.source_framerate // .video_input.framerate' "$CONFIG")
FRAMERATE=$(jq -r '.video_input.framerate' "$CONFIG")
FORMAT=$(jq -r '.video_input.format' "$CONFIG")

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

if [[ ! -f "$VIDEO_PATH" ]]; then
  echo "Error: input video file does not exist: $VIDEO_PATH"
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

  gst-launch-1.0 -q \
    videotestsrc num-buffers=1 ! \
    video/x-raw,format=NV12,width=128,height=72,framerate=30/1 ! \
    $encoder_element ! \
    "$parser" ! \
    fakesink >/dev/null 2>&1
}

case "$CODEC" in
  h264)
    ENCODER_NAME="$(resolve_encoder h264 "$SW_H264_ENC" "$HW_H264_ENC")"
    if [[ "$ENCODER_NAME" == "$SW_H264_ENC" ]]; then
      ENCODER_ELEMENT="$SW_H264_ENC tune=$TUNE speed-preset=$SPEED_PRESET bitrate=$BITRATE key-int-max=$KEY_INT_MAX bframes=$BFRAMES threads=$THREADS"
    else
      ENCODER_ELEMENT="$(build_hw_encoder_element h264 "$ENCODER_NAME" "$BITRATE" "$KEY_INT_MAX" "$BFRAMES")"
    fi
    PARSER_ELEMENT="h264parse"
    PAYLOADER_ELEMENT="rtph264pay pt=96 config-interval=1 mtu=$MTU"
    ;;
  h265)
    ENCODER_NAME="$(resolve_encoder h265 "$SW_H265_ENC" "$HW_H265_ENC")"
    if [[ "$ENCODER_NAME" == "$SW_H265_ENC" ]]; then
      # x265enc 的具体参数风格和 x264enc 不完全一致。
      # 这里先用最简形式作为占位。
      # 后续若真切到 H.265，建议再按该元素的 gst-inspect 输出核对参数。
      ENCODER_ELEMENT="$SW_H265_ENC bitrate=$BITRATE"
    else
      ENCODER_ELEMENT="$(build_hw_encoder_element h265 "$ENCODER_NAME" "$BITRATE" "$KEY_INT_MAX" "$BFRAMES")"
    fi
    PARSER_ELEMENT="h265parse"
    PAYLOADER_ELEMENT="rtph265pay pt=96 config-interval=1 mtu=$MTU"
    ;;
  *)
    echo "Error: unsupported codec: $CODEC"
    exit 1
    ;;
esac

# 打印配置，便于留日志和复现实验。
echo "=== Sender Configuration ==="
echo "Config file : $CONFIG"
echo "Video file  : $VIDEO_PATH"
echo "Resolution  : ${WIDTH}x${HEIGHT}"
echo "Source FPS  : ${SOURCE_FRAMERATE}"
echo "Output FPS  : ${FRAMERATE}"
echo "Format      : ${FORMAT}"
echo "Codec       : ${CODEC}"
echo "Target host : ${HOST}:${PORT}"
echo "Encoder name: ${ENCODER_NAME}"
echo "Encoder     : ${ENCODER_ELEMENT}"
if [[ "${ENCODER_NAME}" == nvh264enc || "${ENCODER_NAME}" == nvh265enc ]]; then
  echo "NV preset   : ${NV_PRESET}"
  echo "NV rc-mode  : ${NV_RC_MODE}"
  echo "NV zerolat. : ${NV_ZERO_LATENCY}"
fi
echo "Parser      : ${PARSER_ELEMENT}"
echo "Payloader   : ${PAYLOADER_ELEMENT}"
if [[ "${SOURCE_FRAMERATE}" != "${FRAMERATE}" ]]; then
  echo "Rate adjust : videorate drop-only=true (${SOURCE_FRAMERATE} -> ${FRAMERATE})"
fi
echo "Pacing      : realtime timestamp-paced sending"
echo "============================"

# 说明:
#   filesrc + rawvideoparse:
#     将裸 YUV 解析成 raw video buffers
#
#   videorate:
#     当 source_framerate 与输出 framerate 不同时，
#     用于在编码前做抽帧，避免仅通过修改 caps 导致视频变慢一倍
#
#   encoder:
#     目前默认使用软件编码
#
#   parser:
#     让码流更规范，便于后续 RTP 打包
#
#   payloader:
#     RTP 打包
#
#   udpsink:
#     按时间戳节奏将 RTP 包发到接收端
#
# 说明 sync/async:
#   这里使用 sync=true:
#     让 sender 按 pipeline 时钟与 buffer 时间戳平滑发送，
#     避免 filesrc 模式下“尽快推送”造成的 burst 流量与接收端假性掉帧。
#
#   async=false:
#     保持启动行为简单，避免等待异步 preroll。
#
#   对不同帧率/分辨率的适应方式:
#     - 原始帧率由 rawvideoparse 提供时间戳
#     - 若 source_framerate != framerate，则由 videorate 做抽帧
#     - 最终由 udpsink 按输出 buffer 时间戳节奏发送
if [[ "${SOURCE_FRAMERATE}" != "${FRAMERATE}" ]]; then
  gst-launch-1.0 -v \
    filesrc location="$VIDEO_PATH" ! \
    rawvideoparse format="$FORMAT" width="$WIDTH" height="$HEIGHT" framerate="${SOURCE_FRAMERATE}/1" ! \
    videorate drop-only=true ! \
    video/x-raw,framerate="${FRAMERATE}/1" ! \
    $ENCODER_ELEMENT ! \
    $PARSER_ELEMENT ! \
    $PAYLOADER_ELEMENT ! \
    udpsink host="$HOST" port="$PORT" sync=true async=false
else
  gst-launch-1.0 -v \
    filesrc location="$VIDEO_PATH" ! \
    rawvideoparse format="$FORMAT" width="$WIDTH" height="$HEIGHT" framerate="${FRAMERATE}/1" ! \
    $ENCODER_ELEMENT ! \
    $PARSER_ELEMENT ! \
    $PAYLOADER_ELEMENT ! \
    udpsink host="$HOST" port="$PORT" sync=true async=false
fi
