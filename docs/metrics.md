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

当前 `configs/experiment.json` 默认值：

- minor stall: `delta_i > 50 ms`
- major stall: `delta_i > 200 ms`

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

## 6. PTS 跳变与估算丢帧

当可用 `PTS` 连续可比时，额外定义：

- `pts_delta_i = pts_i - pts_{i-1}`
- `pts_gap_frames_i = pts_delta_i / expected_frame_interval`

若 `pts_gap_frames_i > 1.5`，认为发生一次 `PTS jump`。

同时估算：

- `estimated_dropped_frames = round(pts_gap_frames_i) - 1`

用途：

- 辅助区分“只是显示链路卡住”与“接收链路实际已经跳帧/掉帧”
- 辅助解释 `appsink` 侧还能继续收帧但显示器已经明显冻结的情况

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
- estimated_dropped_frames: 基于 PTS 间隔估算的丢帧数
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
