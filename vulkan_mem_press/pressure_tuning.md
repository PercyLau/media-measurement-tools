# Vulkan 压力测试参数调优记录

## 测试环境

| 项目 | 值 |
|------|------|
| GPU | Mali-G720-Immortalis |
| Vulkan 版本 | 1.3.239 |
| 驱动 | ARM Proprietary v1.r53p0 |
| 系统内存 | 64GB LPDDR5 |
| 实测带宽上限 | ~5.2-6.5 GB/s |

---

## 关键发现

### 1. 吞吐量分析

所有测试配置的吞吐量都在 **6.2-6.4 GB/s** 之间，说明：
- GPU 显存带宽已跑满
- 吞吐量不是区分压力档位的有效指标

### 2. 真正的压力差异来源

| 参数组合 | chunks/8 秒 | 每 chunk 数据量 | 压力特征 |
|----------|-------------|----------------|----------|
| einv=48, wg=1024 | ~36 | 1.4 GB | 高频小任务 |
| einv=128, wg=4096 | ~2 | 40.0 GB | 低频大任务 |

**关键机制**：
- `--einv` (elems_per_inv)：每次调度的元素数，影响计算密度
- `--wg` (workgroups)：并行工作组数，影响 GPU 占用率
- `--chunk-iters`：每次 dispatch 的迭代数，影响 CPU-GPU 同步频率

### 3. 纯带宽测试

创建 `memstress_bw.comp`（最小计算开销 shader）与原 `memstress.comp` 对比：

| Shader | Read | Write | ReadWrite |
|--------|------|-------|-----------|
| memstress_bw (新) | 5.24 GB/s | 5.93 GB/s | 5.91 GB/s |
| memstress (原) | 5.32 GB/s | 6.45 GB/s | 6.35 GB/s |

**结论**：原 shader 的额外计算开销对带宽影响仅约 5-10%，瓶颈在 Mali-G720 内存子系统本身。

---

## 10 档压力参数（推荐）

基于**总计算量**梯度设计，使用原 `memstress.spv`：

| 档位 | --mb | --iters | --chunk-iters | --einv | --wg | 预期效果 |
|------|------|---------|---------------|--------|------|----------|
| 1 | 64 | 20 | 10 | 32 | 256 | 极轻 - 几乎无影响 |
| 2 | 96 | 30 | 10 | 32 | 512 | 很轻 - 轻微占用 |
| 3 | 128 | 40 | 15 | 48 | 512 | 轻 - 可忽略卡顿 |
| 4 | 160 | 50 | 15 | 48 | 1024 | 中轻 - 偶发 minor stall |
| 5 | 192 | 60 | 20 | 64 | 1024 | 中 - 明显 minor stall |
| 6 | 224 | 70 | 20 | 64 | 2048 | 中重 - 偶发 major stall |
| 7 | 256 | 80 | 25 | 80 | 2048 | 重 - 明显 major stall |
| 8 | 288 | 90 | 25 | 96 | 3072 | 很重 - 频繁卡顿 |
| 9 | 320 | 100 | 30 | 112 | 4096 | 极重 - 严重卡顿 |
| 10 | 512 | 150 | 40 | 128 | 4096 | 极限 - 系统可能失稳 |

---

## 使用方法

### 手动测试单档

```bash
cd vulkan_mem_press

# 示例：测试第 5 档（中）
./vk_memstress --spv ./memstress.spv \
  --mb 192 \
  --mode rdwr \
  --stride 16 \
  --iters 60 \
  --chunk-iters 20 \
  --einv 64 \
  --wg 1024 \
  --seconds 45
```

### 集成到实验配置

修改 `configs/experiment.json` 中的 `receiver_load.args`：

```json
"receiver_load": {
  "enabled": true,
  "startup_delay_sec": 2,
  "workdir": ".",
  "binary": "./vulkan_mem_press/vk_memstress",
  "args": [
    "--mb", "192",
    "--mode", "rdwr",
    "--stride", "16",
    "--iters", "60",
    "--chunk-iters", "20",
    "--einv", "64",
    "--wg", "1024",
    "--seconds", "45",
    "--spv", "./vulkan_mem_press/memstress.spv"
  ]
}
```

### 运行实验

```bash
# 接收端运行（带负载）
./receiver/receiver_stats.sh configs/experiment.json
```

---

## 调参建议

1. **从中间档位开始**：建议先从第 5 档（中）开始测试
2. **观察指标**：
   - `p95_delta_ms`、`p99_delta_ms`：帧间隔分布
   - `minor_stalls`、`major_stalls`：卡顿计数
   - `pts_jump_count`：源端丢帧
3. **向上/向下调节**：
   - 无明显卡顿 → 增加 2-3 档
   - 卡顿过于严重 → 降低 1-2 档
4. **固定参数优先**：建议固定 `--stride=16` 和 `--mode=rdwr`，只调节其他参数

---

## 文件说明

| 文件 | 用途 |
|------|------|
| `memstress.comp` | 原压力测试 shader（含位运算/原子操作） |
| `memstress.spv` | 编译后的 shader |
| `memstress_bw.comp` | 纯带宽测试 shader（最小计算开销） |
| `memstress_bw.spv` | 编译后的带宽测试 shader |
| `vk_memstress.cpp` | Vulkan 测试程序源码 |

---

## 测试记录

### 2026-03-30 参数梯度测试

| 测试组 | 参数范围 | 吞吐量 | 结论 |
|--------|----------|--------|------|
| MB 梯度 (96-448) | 仅改变 --mb | ~6.3 GB/s | 单一参数不足以产生梯度 |
| 组合参数 (10 档) | mb+iters+einv+wg | ~6.3 GB/s | chunks 从 36 降至 2，压力梯度形成 |
| 纯带宽 shader | stride=4, einv=256 | 5.2-6.0 GB/s | 与原 shader 差异<10% |

### 关键测试数据

```
高频小任务 (einv=48, wg=1024):
  chunks: ~36/8 秒
  每 chunk: ~1.9 GB
  总流量：~68 GB/8 秒

低频大任务 (einv=128, wg=4096):
  chunks: ~2/8 秒
  每 chunk: ~40.0 GB
  总流量：~80 GB/8 秒
```

---

## 后续优化方向

1. **验证实际卡顿梯度**：用视频流测试各档，确认 `stall_count` 是否按预期递增
2. ** finer 粒度**：如需更细粒度，可在每档之间插入中间值（如 5.5 档）
3. **模式对比**：测试 `mode=rd` vs `mode=wr` vs `mode=rdwr` 对卡顿的影响差异
