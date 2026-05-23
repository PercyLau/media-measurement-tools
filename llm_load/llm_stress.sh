#!/usr/bin/env bash
#
# LLM 负载测试工具包装器
# 使用 ollama 运行实际大模型推理，模拟真实 LLM 工作负载
#
# 用法:
#   ./llm_stress.sh --model phi4:14b --seconds 60
#   ./llm_stress.sh --model lfm2:24b-a2b --seconds 30 --threads 4
#
# 参数:
#   --model MODEL     ollama 模型名 (默认: phi4:14b)
#   --seconds N       持续时间秒 (默认: 60)
#   --threads N       ollama 线程数 (默认: 0 = 自动)
#   --prompt TEXT     自定义 prompt (默认: 技术类问题)
#   --verbose         输出详细日志
#   --help            显示帮助
#

set -euo pipefail

MODEL="phi4:14b"
SECONDS=60
THREADS=0
PROMPT="Explain how TCP congestion control works in detail, including slow start, congestion avoidance, fast retransmit, and fast recovery."
VERBOSE=false

while [[ $# -gt 0 ]]; do
    case $1 in
        --model) MODEL="$2"; shift 2 ;;
        --seconds) SECONDS="$2"; shift 2 ;;
        --threads) THREADS="$2"; shift 2 ;;
        --prompt) PROMPT="$2"; shift 2 ;;
        --verbose) VERBOSE=true; shift ;;
        --help|-h)
            head -30 "$0" | tail -28
            exit 0
            ;;
        *)
            echo "Unknown option: $1"
            exit 1
            ;;
    esac
done

if ! command -v ollama &> /dev/null; then
    echo "Error: ollama not found"
    exit 1
fi

echo "Starting LLM stress: model=${MODEL}, threads=${THREADS}, duration=${SECONDS}s"

END_TIME=$((SECONDS + $(date +%s)))
ITER=0

while [[ $(date +%s) -lt $END_TIME ]]; do
    ITER=$((ITER + 1))

    if [[ "$VERBOSE" == "true" ]]; then
        echo "--- Iteration $ITER ---"
        if [[ $THREADS -gt 0 ]]; then
            ollama run "$MODEL" "$PROMPT" --verbose --num-thread "$THREADS" 2>&1 | tail -5
        else
            ollama run "$MODEL" "$PROMPT" --verbose 2>&1 | tail -5
        fi
    else
        if [[ $THREADS -gt 0 ]]; then
            ollama run "$MODEL" "$PROMPT" --num-thread "$THREADS" > /dev/null 2>&1
        else
            ollama run "$MODEL" "$PROMPT" > /dev/null 2>&1
        fi
    fi
done

echo "LLM stress completed: $ITER iterations"
