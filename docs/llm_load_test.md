# LLM 并发负载下的视频解码卡顿测试

## 测试目的

测量视频解码（local MP4）与 LLM 推理（ollama）并发运行时的帧间隔影响，确认是否存在可感知的卡顿。

## 测试环境

- 平台: Orion O6 ARM Debian
- 视频: YachtRide 3840x2160 h265 8000kbps, decoded at 90fps (450 frames, ~5s)
- 解码器: v4l2h265dec (hardware)
- 并发工具: llm_stress.sh (ollama wrapper)
- 阈值: frame_intervals 模式, minor=1.5x=16.67ms, major=3.0x=33.33ms

## 测试模型

| 模型 | 大小 | 参数 |
|------|------|------|
| phi4:14b | 9.1 GB | 14B |
| lfm2:24b-a2b | 14 GB | 24B (MoE) |
| qwen3.6:35b-a3b-q4_K_M | 23 GB | 35B (MoE) |

## 正确测试方法

### 错误（历史数据无效原因）

旧测试使用 `receiver_stats.sh` 的 `startup_delay_sec=0` 同时启动 LLM + 视频解码。
此时 LLM 在**加载模型**（磁盘 I/O），推理循环尚未开始。
测量到的是磁盘 I/O 争抢，而非 LLM 推理对解码的影响。

此外旧版 `llm_stress.sh` 有 `export OLLAMA_KEEP_ALIVE=0`，
导致 warmup 后模型立即卸载，每个迭代都是冷启动 ——
LLM 实际上没有在做推理，所以 P95/P99 数据和 baseline 几乎一样。

### 正确方法

```
Step 1: llm_stress.sh (warmup → 推理迭代开始)
              │
              ▼  模型加载完成，推理已运行
Step 2: 视频解码
              │
              ▼  LLM 推理 + 视频解码并发
Step 3: 视频结束 → kill LLM → ollama stop 卸载
```

确保测量的是**真实推理负载**下的解码性能，而非模型加载阶段的 I/O 开销。

## 测试结果

### 数据汇总

| 场景 | P95 delta | P99 delta | Max delta | Minor stalls |
|------|-----------|-----------|-----------|-------------|
| Baseline (无负载) | 4.32ms | 4.48ms | 5.98ms | 0 |
| + lfm2:24b-a2b | 5.99ms | 8.97ms | 14.02ms | 0 |
| + phi4:14b | 6.21ms | 8.97ms | 10.67ms | 0 |
| + qwen3.6:35b-a3b | 6.10ms | 9.08ms | 12.48ms | 0 |

### 结论

1. **3 个模型全部 0 卡顿** — 最大 delta (14.02ms) 仍低于 minor stall 阈值 (16.67ms)
2. **P95 上升约 40%**, **P99 约翻倍**，但离阈值还有安全余量
3. **三个模型影响近一致** — 瓶颈在 CPU/内存带宽，非模型参数量
4. 旧测试数据无效（模型未实际推理）

## 发现的问题与解决方案

### 1. OLLAMA_KEEP_ALIVE=0 与 Warmup 冲突

**问题**: `export OLLAMA_KEEP_ALIVE=0` 让每次 `ollama run` 后模型立即卸载。
Warmup 加载模型 → 推理 → 卸载，主循环每个迭代重新加载，warmup 无效。

**解决方案**: 删除全局 `OLLAMA_KEEP_ALIVE=0`，依赖 ollama 默认 5 分钟 keep_alive。
退出时通过 trap 主动 `ollama stop` 卸载。

### 2. trap EXIT INT TERM 导致 cleanup 重复执行

**问题**: 在 bash 中，EXIT trap 在所有退出情形（包括 SIGINT/SIGTERM）都会触发。
同时设置 INT/TERM trap 会让 cleanup 函数执行两次。

**解决方案**: 只用 `trap cleanup_llm EXIT`。

### 3. grep -oP 提取模型名不可靠

**问题**: `grep -oP '(?<=--model )\S+'` 依赖 Perl 正则，
在部分系统不可用，且 args 含特殊字符时可能出错。

**解决方案**: 循环遍历 `LLM_LOAD_ARGS` 数组找 `--model` 后的值。

### 4. curl keep_alive=0 用于卸载有副作用

**问题**: curl 调用 `/api/generate` 时不带 prompt，
如果模型已卸载，ollama 会先加载模型再卸载（反向操作）。

**解决方案**: 使用 `ollama stop`，它不会触发模型加载。

### 5. 模型在 "Stopping..." 状态卡死

**问题**: 当 LLM stress 在 warmup 阶段（模型加载中）被 kill 时，
`ollama stop` 有时无法完全终止 runner 进程，模型停留在 "Stopping..." 状态。

**原因**: runner 进程在模型加载启动阶段被杀，ollama server 的状态追踪出现不一致。

**解决方案**:
- 让 LLM stress 完成 warmup 后再处理其他任务（视频解码等）
- 如已卡死，需要重启 ollama 服务:
  ```bash
  sudo systemctl stop ollama
  kill -9 $(pgrep -f "ollama") 2>/dev/null
  sudo systemctl start ollama
  ```
- 脚本中 trap 的 `ollama stop` 在 warmup 完成后通常正常工作

### 6. 短视频 + 长 LLM 负载的时序冲突

**问题**: 视频 5s 而 LLM 负载 30s+。视频结束后 receiver 退出，
触发 kill LLM。若 warmup 未完成，kill 时机不理想。

**解决方案**:
- `startup_delay_sec` 应让 LLM 先 warmup 再起视频（但对短视频不合理）
- 或手动分两阶段（本文的测试方法）
- 视频时长应 >= LLM warmup 时间以让两者充分重叠
