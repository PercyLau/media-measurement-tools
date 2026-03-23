#!/usr/bin/env bash
set -euo pipefail

# 用法:
#   ./receiver_stats.sh configs/experiment.json
#
# 功能:
#   1. 启动 receiver_stats.py
#   2. 按配置可选启动接收端压力程序
#   3. receiver 退出时自动清理压力进程
#
# 说明:
#   正式实验不显示画面，只做统计。
#
# 依赖:
#   - python3
#   - jq
#
# 备注:
#   压力程序当前按外部可执行文件处理:
#       ./vulkan_mem_press/vk_memstress
#   参数通过 JSON 中 receiver_load.args 传入。

if [[ $# -ne 1 ]]; then
  echo "Usage: $0 <config.json>"
  exit 1
fi

CONFIG="$1"

if ! command -v python3 >/dev/null 2>&1; then
  echo "Error: python3 not found."
  exit 1
fi

if ! command -v jq >/dev/null 2>&1; then
  echo "Error: jq not found. Please install jq first."
  exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

RECEIVER_PY="${SCRIPT_DIR}/receiver_stats.py"

LOAD_ENABLED="$(jq -r '.receiver_load.enabled // false' "$CONFIG")"
LOAD_STARTUP_DELAY="$(jq -r '.receiver_load.startup_delay_sec // 0' "$CONFIG")"
LOAD_WORKDIR_RAW="$(jq -r '.receiver_load.workdir // "."' "$CONFIG")"
LOAD_BINARY_RAW="$(jq -r '.receiver_load.binary // ""' "$CONFIG")"
LOAD_LOG_STDOUT="$(jq -r '.receiver_load.log_stdout // true' "$CONFIG")"

# 允许 workdir / binary 用相对路径，相对于项目根目录解析。
if [[ "$LOAD_WORKDIR_RAW" = /* ]]; then
  LOAD_WORKDIR="$LOAD_WORKDIR_RAW"
else
  LOAD_WORKDIR="${PROJECT_ROOT}/${LOAD_WORKDIR_RAW}"
fi

if [[ -n "$LOAD_BINARY_RAW" ]]; then
  if [[ "$LOAD_BINARY_RAW" = /* ]]; then
    LOAD_BINARY="$LOAD_BINARY_RAW"
  else
    LOAD_BINARY="${PROJECT_ROOT}/${LOAD_BINARY_RAW}"
  fi
else
  LOAD_BINARY=""
fi

# 从 JSON 数组中读取参数。
# mapfile + jq @sh 的组合容易引入额外转义，这里用 while read 更稳。
LOAD_ARGS=()
while IFS= read -r arg; do
  LOAD_ARGS+=("$arg")
done < <(jq -r '.receiver_load.args[]? // empty' "$CONFIG")

RECEIVER_PID=""
LOAD_PID=""
LOAD_LOG_FILE=""
RECEIVER_WAIT_DONE="false"

cleanup() {
  set +e

  if [[ -n "${LOAD_PID}" ]]; then
    if kill -0 "${LOAD_PID}" 2>/dev/null; then
      echo "[receiver_stats.sh] Stopping load process PID=${LOAD_PID}"
      kill "${LOAD_PID}" 2>/dev/null || true
      wait "${LOAD_PID}" 2>/dev/null || true
    fi
  fi

  if [[ -n "${RECEIVER_PID}" ]]; then
    if [[ "${RECEIVER_WAIT_DONE}" != "true" ]] && kill -0 "${RECEIVER_PID}" 2>/dev/null; then
      echo "[receiver_stats.sh] Stopping receiver process PID=${RECEIVER_PID}"
      kill "${RECEIVER_PID}" 2>/dev/null || true
      wait "${RECEIVER_PID}" 2>/dev/null || true
    fi
  fi
}

trap cleanup EXIT INT TERM

echo "[receiver_stats.sh] Project root : ${PROJECT_ROOT}"
echo "[receiver_stats.sh] Config       : ${CONFIG}"
echo "[receiver_stats.sh] Launching receiver_stats.py ..."

python3 "${RECEIVER_PY}" --config "${CONFIG}" &
RECEIVER_PID=$!

echo "[receiver_stats.sh] Receiver PID : ${RECEIVER_PID}"

if [[ "${LOAD_ENABLED}" == "true" ]]; then
  if [[ -z "${LOAD_BINARY}" ]]; then
    echo "[receiver_stats.sh] Error: receiver_load.enabled=true but binary is empty."
    exit 1
  fi

  if [[ ! -x "${LOAD_BINARY}" ]]; then
    echo "[receiver_stats.sh] Error: load binary not executable: ${LOAD_BINARY}"
    exit 1
  fi

  if [[ ! -d "${LOAD_WORKDIR}" ]]; then
    echo "[receiver_stats.sh] Error: load workdir not found: ${LOAD_WORKDIR}"
    exit 1
  fi

  echo "[receiver_stats.sh] Waiting ${LOAD_STARTUP_DELAY}s before starting load ..."
  sleep "${LOAD_STARTUP_DELAY}"

  RUN_TS="$(date +%Y%m%dT%H%M%S)"
  if [[ "${LOAD_LOG_STDOUT}" == "true" ]]; then
    mkdir -p "${PROJECT_ROOT}/output/load_launcher_logs"
    LOAD_LOG_FILE="${PROJECT_ROOT}/output/load_launcher_logs/load_${RUN_TS}.log"
    echo "[receiver_stats.sh] Load log     : ${LOAD_LOG_FILE}"
  fi

  echo "[receiver_stats.sh] Load workdir : ${LOAD_WORKDIR}"
  echo "[receiver_stats.sh] Load binary  : ${LOAD_BINARY}"
  echo "[receiver_stats.sh] Load args    : ${LOAD_ARGS[*]:-<none>}"

  (
    cd "${LOAD_WORKDIR}"
    if [[ "${LOAD_LOG_STDOUT}" == "true" ]]; then
      "${LOAD_BINARY}" "${LOAD_ARGS[@]}" >"${LOAD_LOG_FILE}" 2>&1
    else
      "${LOAD_BINARY}" "${LOAD_ARGS[@]}"
    fi
  ) &
  LOAD_PID=$!

  echo "[receiver_stats.sh] Load PID     : ${LOAD_PID}"
else
  echo "[receiver_stats.sh] receiver_load.enabled=false, load will not be started."
fi

# 等 receiver 结束。
set +e
wait "${RECEIVER_PID}"
RECEIVER_EXIT_CODE=$?
set -e
RECEIVER_WAIT_DONE="true"

# receiver 结束后，如负载还在运行，主动结束。
if [[ -n "${LOAD_PID}" ]]; then
  if kill -0 "${LOAD_PID}" 2>/dev/null; then
    echo "[receiver_stats.sh] Receiver finished; stopping load PID=${LOAD_PID}"
    kill "${LOAD_PID}" 2>/dev/null || true
    wait "${LOAD_PID}" 2>/dev/null || true
  fi
fi

exit "${RECEIVER_EXIT_CODE}"
