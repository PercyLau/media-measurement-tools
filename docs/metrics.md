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

### minor stall
当：

delta_i > 50 ms

记为 minor stall

### major stall
当：

delta_i > 100 ms

记为 major stall

---

## 5. 输出字段

CSV 字段定义：

- frame_idx: 接收端本地帧序号
- pts_ns: buffer PTS，若不存在则为 -1
- recv_monotonic_ns: 本地单调时钟时间
- delta_ms: 与上一帧的时间差
- is_stall_minor: 是否超过 50 ms
- is_stall_major: 是否超过 100 ms

---

## 6. 注意事项

- 当前 frame_idx 是接收端本地顺序，不等同于发送端原始帧号
- 当前阶段不处理严格端到端逐帧对齐
- 当前阶段不把显示设备刷新时刻纳入指标定义