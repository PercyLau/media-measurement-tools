# Metrics Definition

## 1. 目标

本文件定义一阶段实验中所有核心指标的语义。

---

## 2. 帧接收时间

对于第 i 帧，定义：

t_i = appsink 取到该帧 sample 的本地单调时钟时间

说明：

- 这是应用侧时间
- 不是最终屏幕显示时刻
- 但适合当前阶段的卡顿研究

---

## 3. 帧间隔

定义：

delta_i = t_i - t_{i-1}

单位：

ms

用途：

- 反映帧流消费节奏
- 观察是否存在停顿或突发长间隔

---

## 4. 卡顿事件

阈值由配置文件 `stall_thresholds_ms` 决定。

支持两种模式：

- `fixed_ms`: 直接使用固定毫秒阈值
- `frame_intervals`: 按当前实验输出帧率的理论帧间隔乘倍率计算

当前 `configs/experiment.json` 默认值：

- `mode = frame_intervals`
- `minor_frame_intervals = 1.5`
- `major_frame_intervals = 3.0`

例如：

- `10fps` 时，理论帧间隔约 `100 ms`
- `minor stall` 约为 `150 ms`
- `major stall` 约为 `300 ms`

- `30fps` 时，理论帧间隔约 `33.3 ms`
- `minor stall` 约为 `50 ms`
- `major stall` 约为 `100 ms`

注意：

- `major stall` 是更严格的子集
- 某一帧如果超过 `major` 阈值，也一定会同时计入 `minor`

---

## 5. 分布统计

除了 stall 次数之外，接收端还输出帧间隔分布统计：

- `max_delta_ms`: 最大帧间隔
- `p95_delta_ms`: 95 分位帧间隔
- `p99_delta_ms`: 99 分位帧间隔

这些指标通常比单纯的 stall 次数更容易反映“明显卡顿但计数变化不大”的情况。

---

## 6. PTS 跳变与估算晚到帧

当可用 `PTS` 连续可比时，额外定义：

- `pts_delta_i = pts_i - pts_{i-1}`
- `pts_gap_frames_i = pts_delta_i / expected_frame_interval`

若 `pts_gap_frames_i > 1.5`，认为发生一次 `PTS jump`。

同时估算：

- `estimated_late_frames = round(pts_gap_frames_i) - 1`

用途：

- 估算有多少帧没有按播放侧预期节奏及时解码/送达 `appsink`
- 辅助解释 `appsink` 侧观测到的输出节奏异常

注意：

- 这是基于解码输出 `PTS` 间隔的播放侧启发式估算
- 它不等价于网络真实丢包，也不等价于发送端真实丢帧

---

## 7. 输出字段

CSV 字段定义：

- frame_idx: 接收端本地帧序号
- pts_ns: buffer PTS，若不存在则为 -1
- recv_monotonic_ns: 本地单调时钟时间
- delta_ms: 与上一帧的时间差
- pts_delta_ms: 与上一帧的 PTS 时间差
- pts_gap_frames: 当前 PTS 间隔相当于多少帧
- is_pts_jump: 是否检测到 PTS 跳变
- estimated_late_frames: 基于 PTS 间隔估算的未及时解码/播放帧数
- is_stall_minor: 是否超过 50 ms
- is_stall_major: 是否超过当前 major 阈值

---

## 8. 注意事项

- 当前 frame_idx 是接收端本地顺序，不等同于发送端原始帧号
- 当前阶段不处理严格端到端逐帧对齐
- 当前阶段不把显示设备刷新时刻纳入指标定义
- 当前统计时刻仍然是 `appsink` 取到解码后帧的时刻，不是显示器真正刷新时刻
- 当前接收链路在 decoder 与 `appsink` 之间加入了 `queue`，并允许通过配置调整 `appsink_max_buffers` 与 `post_decode_queue_max_buffers`
- CSV 默认按批量方式刷盘，周期可通过 `receiver.csv_flush_interval` 调整
- 只有 `receiver.mode = full_stats` 时，上述 CSV 与分布统计指标才有意义
- 当 `receiver.mode = depay_only` 或 `decode_probe` 时，应以 `receiver_events.log` 中的 `ERROR`、`WARNING`、`QOS` 为主要观测信号

---

## 9. 测量盲区：显示路径 vs 解码路径

### 9.1 当前 appsink 测到的是什么

当前 pipeline 在解码路径末端挂 `appsink`：

```text
decoder -> queue -> appsink (记录 delta_ms)
```

`delta_ms` = 上一帧到达 appsink 与当前帧到达 appsink 之间的本地单调时钟差。

它反映的是 **decoder 出帧节奏**——decoder 完成解码、数据经过 queue 到达 appsink 的时间间隔。

它 **不是** 帧在屏幕上出现的间隔。

### 9.2 为什么会出现"肉眼卡顿但数据没变化"

在硬件解码路径下，`v4l2h264dec` / `v4l2h265dec` 调用的是 SoC 上**独立的视频解码硬件 IP**（VPU/IPP），与 GPU 是两个分开的硬件单元：

```text
SoC 内部架构
├── CPU cluster
├── GPU (Mali-G720-Immortalis)   ← vk_memstress 压的是这个
├── VPU/IPP (硬件解码器)         ← v4l2h264dec 用的是这个
├── Display controller
├── DDR 控制器
└── NoC/AMBA 总线
```

VPU 和 GPU：
- 是独立的计算单元，GPU 满载不影响 VPU 的解码逻辑
- 共享 DDR 带宽，但 VPU 的帧读写带宽需求相对稳定
- 各自有独立的时钟域和调度

当 GPU 被负载（如 Vulkan compute stress 或 LLM 推理）压满时：

| 路径 | 受影响？ | appsink 能测到？ |
|------|---------|-----------------|
| decoder (VPU) | 基本不受影响 | 能 |
| compositor GPU 合成 | 严重受影响 | **不能** |
| 色彩转换/显示提交 | 受影响 | **不能** |
| vsync 等待 | 可能受影响 | **不能** |

所以 decoder 仍在匀速出帧，appsink 看到的 `delta_ms` 正常。但 compositor 在 GPU 上忙不过来，帧送不到屏幕，用户就看到了卡顿。

### 9.3 完整显示路径的数据流

对比 appsink 的终点，真实播放器渲染需要经过：

```text
decoder 输出
  → 颜色空间转换 (YUV → RGB)
  → 缩放/裁剪
  → 后处理 (HDR tone mapping、色彩校正)
  → compositor 合成 (叠加 UI、字幕)
  → DRM/KMS 提交到显示器
  → 等待 vsync
  → 扫描到屏幕
```

在 Linux/X11/Wayland 环境下，这条路径通常要走：

| 路径 | 数据经过 | 延迟量级 |
|------|---------|---------|
| 软解 + 桌面 | decoder → system RAM → CPU YUV→RGB → GPU 合成 → 显示器 | ~10-30ms |
| 硬解 + DRM overlay | decoder DMA-buf → DRM plane → 显示器 | ~5-15ms |
| 硬解 + Wayland | decoder DMA-buf → compositor GL 合成 → DRM → 显示器 | ~10-25ms |

appsink 停在第一步之后，不经过后续任何环节。

### 9.4 可选的显示延迟测量方案

| 方案 | 实现难度 | 精度 | 说明 |
|------|---------|------|------|
| tee 分流 + kmssink + appsink | 低 | 中等 | 画面显示到屏幕的同时，appsink 在后台记数据。appsink 侧仍是 decoder 节奏，但能确认画面是否正常 |
| DRM page flip / vblank 事件 | 中 | 高 | 注册 DRM page flip callback，kernel 在帧实际扫描到屏幕时回调。精度亚毫秒级，需要自定义 GStreamer sink 或直接调 DRM API |
| GStreamer latency tracer | 低 | 中等 | 内置 `GST_TRACERS=latency`，记录 buffer 在每个 element 的停留时间。不是显示时刻，且 overhead 较大 |
| EGL/GL Fence Sync | 中 | 中等 | 如果用 GL sink，可在 GPU 渲染完成后 fence sync。但在 Wayland/X11 下 compositor 抽象了实际显示时间 |
| 摄像头拍屏 + OSD 时间戳 | 需硬件 | 最高 | 画面叠加 PTS 时间戳，高速摄像头拍摄比对。这是"帧真正出现在屏幕上"的金标准 |

### 9.5 负载类型的区别

| 负载 | 压的硬件 | 对 appsink 的影响 | 对显示路径的影响 |
|------|---------|-----------------|-----------------|
| vk_memstress (当前) | GPU compute | 小 | 大 |
| llama.cpp (CPU-only, Kleidiai) | CPU SIMD | 中（抢 CPU 调度） | 小 |
| llama.cpp (Vulkan 后端) | GPU tensor compute | 小 | 大 |
| 软解解码 | CPU | 大 | 中 |
| 硬解解码 | VPU/IPP | 小 | 小 |

- Kleidiai 是纯 CPU 优化库，利用 ARMv9-A 的 `i8mm`/`dotprod`/`SVE/SVE2` 指令集，与 GPU 无关
- 要模拟真实大模型 GPU 负载，需要 llama.cpp 开启 Vulkan 后端（当前编译未开启）
- `v4l2h264dec` 走 VPU 硬件，不是 GPU，所以 GPU 压力不会直接影响解码出帧

### 9.6 结论

当前 appsink 测量方案能准确反映 **解码路径的卡顿**，但无法检测 **显示路径的卡顿**。

如果你的研究目标是"GPU 高负载下视频播放的端到端用户体验"，则需要同时测量解码路径和显示路径的延迟。建议优先尝试 **tee 分流 + kmssink + appsink** 方案，快速验证假设。
