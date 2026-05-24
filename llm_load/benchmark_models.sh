#!/usr/bin/env bash
#
# 逐个测试 ollama 模型性能，每次测完自动卸载
#
# 用法:
#   ./benchmark_models.sh
#

set -euo pipefail

MODELS=("lfm2:24b-a2b" "qwen3.6:35b-a3b-q4_K_M" "phi4:14b")
PROMPT="Summarize relativity and quantum mechanics in detail, including special relativity, general relativity, uncertainty principle, wave-particle duality, and quantum entanglement."

echo "Model,Load_s,PromptTokens,PromptDur_s,PromptRate_toks,GenTokens,GenDur_s,GenRate_toks,Total_s"

for model in "${MODELS[@]}"; do
    # Unload any previously loaded model first
    curl -s http://localhost:11434/api/generate \
        -d '{"model": "'"$model"'", "keep_alive": 0}' \
        >/dev/null 2>&1 || true
    sleep 2

    # Verify no runners active
    if ps aux | grep -q "[o]llama runner"; then
        echo "WARNING: ollama runner still active before testing $model" >&2
    fi

    # Run benchmark via API
    result=$(curl -s http://localhost:11434/api/generate \
        -d '{"model": "'"$model"'", "prompt": "'"$PROMPT"'", "stream": false}')

    # Parse with python
    echo "$result" | python3 -c "
import json, sys
d = json.load(sys.stdin)
load = d.get('load_duration', 0) / 1e9
pe_count = d.get('prompt_eval_count', 0)
pe_dur = d.get('prompt_eval_duration', 0) / 1e9
ev_count = d.get('eval_count', 0)
ev_dur = d.get('eval_duration', 0) / 1e9
total = d.get('total_duration', 0) / 1e9
pe_rate = pe_count / pe_dur if pe_dur > 0 else 0
ev_rate = ev_count / ev_dur if ev_dur > 0 else 0
print(f'$model,{load:.2f},{pe_count},{pe_dur:.2f},{pe_rate:.2f},{ev_count},{ev_dur:.2f},{ev_rate:.2f},{total:.2f}')
"

    # Unload model immediately after test
    ollama stop "$model" 2>/dev/null || true
    sleep 3

    # Verify runner is gone
    if ps aux | grep -q "[o]llama runner"; then
        echo "WARNING: ollama runner still active after testing $model" >&2
    fi
done

echo "Done."
