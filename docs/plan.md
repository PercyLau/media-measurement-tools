# Phase 1 Plan

## 1. 目标

构建一个最小实验平台，用于研究：

- 接收端帧处理节奏
- 卡顿事件
- 帧间隔抖动

当前阶段不研究：

- 拥塞控制
- 自适应码率
- WebRTC
- 复杂控制面
- 负载注入脚本实现

---

## 2. 阶段边界

### 包含
- WSL 发送端
- ARM 接收端
- RTP/UDP
- H.264 默认路线
- JSON 配置
- appsink 统计
- preview 调试模式

### 不包含
- H.265 实测
- 硬件编解码实测
- 负载脚本
- UI
- 复杂日志聚合
- 多流同步

---

## 3. 总体设计

### 发送端
固定流程：

裸 YUV -> rawvideoparse -> encoder -> parser -> RTP payloader -> udpsink

### 接收端（正式）
固定流程：

udpsrc -> rtpjitterbuffer -> depay -> decoder -> appsink

### 接收端（调试）
固定流程：

udpsrc -> rtpjitterbuffer -> depay -> decoder -> autovideosink

---

## 4. 配置管理

统一使用 JSON。

目的：

- 参数集中
- 脚本与代码解耦
- 后续便于批量实验
- 后续可扩展 H.265 / 硬件 codec

---

## 5. 指标口径

### 5.1 基础帧时间
定义：

t_i = 接收端 appsink 取到第 i 帧时的本地单调时钟时间

### 5.2 帧间隔
定义：

delta_i = t_i - t_{i-1}

单位：

ms

### 5.3 卡顿事件
当前阈值：

- minor stall: delta_i > 50 ms
- major stall: delta_i > 100 ms

### 5.4 辅助日志
记录：

- WARNING
- ERROR
- EOS
- QoS 消息
- summary

---

## 6. 文件交付

### 必须交付
- README.md
- configs/experiment.json
- sender/sender.sh
- receiver/receiver_preview.sh
- receiver/receiver_stats.sh
- receiver/receiver_stats.py
- docs/plan.md
- docs/metrics.md

### 输出产物
- output/receiver_metrics.csv
- output/receiver_events.log

---

## 7. 实施步骤

### Step 1: 环境确认
完成条件：

- sender / receiver 都能执行 gst-launch-1.0
- sender 具备 rawvideoparse / x264enc / rtph264pay
- receiver 具备 rtpjitterbuffer / rtph264depay / avdec_h264 / appsink

### Step 2: RTP 链路跑通
完成条件：

- preview 模式能看到正常画面
- 参数错误时能快速定位
- 1080p120 素材已成功联调

### Step 3: 配置化
完成条件：

- sender 不再手写命令
- receiver 不再手写命令
- 参数全部转入 JSON

### Step 4: 正式统计接收端
完成条件：

- receiver_stats.py 可稳定运行
- appsink 可逐帧写 CSV
- 输出 delta_ms 与 stall flags

### Step 5: 文档补齐
完成条件：

- README 能指导别人复现
- plan.md 描述清晰
- metrics.md 定义指标语义

---

## 8. 当前版本验收标准

当以下条件都满足时，Phase 1 视为完成：

1. 配置文件可驱动 sender/receiver
2. sender 能稳定从 YUV 文件发 RTP
3. receiver_stats.py 能稳定运行而不需要显示画面
4. CSV 能包含逐帧记录
5. log 能包含运行事件与 summary
6. preview 模式可用于 debug 但不进入正式实验

---

## 9. 未来扩展预留

### H.265
- JSON 已预留 codec = h265
- 代码中已预留 rtph265depay / avdec_h265 路线
- 当前仅占位

### 硬件编解码
- JSON 已预留 hardware placeholders
- sender / receiver 代码保留入口
- 不承诺当前平台立即可用

### 更严格时序观测
后续若需要更接近真实显示时刻，可考虑：

- tee + preview + appsink 双路
- 更底层 sink 统计
- 专用显示链测量