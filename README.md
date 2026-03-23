# RTP ARM Phase 1

## 1. 项目目标

本项目用于构建一个最小可控的实验平台：

- 发送端在 WSL Ubuntu 上读取裸 YUV 视频
- 通过 GStreamer 进行 H.264/H.265 编码
- 经 RTP/UDP 发送到 ARM Debian 接收端
- 接收端只做接收、解码与逐帧统计
- 正式实验中不显示画面，只输出卡顿与帧间隔统计

当前阶段重点研究：

- 接收端处理节奏
- 帧间隔抖动
- 卡顿事件
- dropped/late/QoS 类现象

当前阶段**不包含**：

- WebRTC
- 拥塞控制
- ABR
- RTSP server
- 发送端与接收端严格逐帧对齐
- 负载注入逻辑（由外部脚本负责）

---

## 2. 当前实验结构

### 发送端
WSL Ubuntu

链路：

raw YUV -> encoder -> RTP payloader -> UDP sink

### 接收端
ARM Debian

链路：

UDP source -> RTP jitter buffer -> depay -> decoder -> appsink

### 调试模式
如果需要确认画面是否正常，可切换到 preview 脚本：

UDP source -> RTP jitter buffer -> depay -> decoder -> autovideosink

---

## 3. 目录说明

```text
rtp-arm-phase1/
├─ README.md
├─ configs/
├─ sender/
├─ receiver/
├─ output/
└─ docs/