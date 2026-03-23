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
```

---

## 4. 运行方式

本项目当前按 Linux / WSL 使用方式编写。

- `sender/sender.sh` 与 `receiver/receiver_stats.sh` 都应在 Ubuntu 24.04 WSL 的 `bash` 中运行
- 不建议在 Windows PowerShell 中直接执行这些脚本
- `receiver_stats.py` 是核心接收统计程序
- `receiver_stats.sh` 是外层 launcher，用于按配置启动 `receiver_stats.py`，并在启用时附带拉起接收端负载程序

---

## 5. receiver_stats.py 与 receiver_stats.sh 的关系

### receiver_stats.py

负责真正的接收与统计：

- 创建 GStreamer pipeline
- 从 `appsink` 逐帧拉取 sample
- 计算 `delta_ms`
- 标记 `minor stall / major stall`
- 写出 `receiver_metrics.csv`、`receiver_events.log`、`resolved_config.json`、`run_info.json`

### receiver_stats.sh

负责实验流程编排：

- 读取同一份 JSON 配置
- 启动 `receiver_stats.py`
- 当 `receiver_load.enabled=true` 时，按配置启动 `vk_memstress`
- 在 receiver 结束后清理负载进程

---

## 6. 已知问题与经验

### 6.1 空的 receiver_metrics.csv

本项目曾出现过一个回归：

- 在引入 `memstress` 启动逻辑后
- 即使把 `receiver_load.enabled` 设为 `false`
- `receiver_metrics.csv` 仍可能为空

根因不是压力参数过大，也不是 `sh + py` 分层方案本身有问题，而是 launcher 逻辑把“无负载场景”也带进了后台托管与清理路径，导致 `receiver_stats.py` 不能稳定完成输出。

当前修复策略是：

- 当 `receiver_load.enabled=false` 时，`receiver_stats.sh` 直接以前台方式运行 `receiver_stats.py`
- 只有当 `receiver_load.enabled=true` 时，才进入带负载的 supervisor 路径
- `receiver_stats.py` 也补充了更稳的 CSV 落盘与终止信号处理

如果后续再次遇到空 CSV，优先检查：

- 当前是否确实在 WSL 的 `bash` 中运行
- `receiver_load.enabled` 是否符合预期
- 同次运行目录下的 `receiver_events.log`
- GStreamer pipeline 是否真的收到了帧

---

## 7. receiver_load 调参建议

如果开启 `receiver_load` 前后，`delta_ms`、stall 次数或 QoS 现象几乎没有变化，常见原因是当前压力不够强。

建议按以下思路调节：

- 增大 `--mb`：提高参与访问的数据规模
- 减小 `--stride`：让访存更密集
- 增大 `--iters` 与 `--chunk-iters`：让单次 dispatch 更重
- 增大 `--einv` 与 `--wg`：提高着色器执行密度
- 适当延长 `--seconds`：让压力覆盖更多播放区间

建议从“中等偏强”档位开始观察：

```text
--mb 512
--mode rdwr
--stride 16
--iters 200
--chunk-iters 40
--einv 128
--wg 4096
--seconds 45
```

如果仍然变化不明显，再逐步继续提高；如果接收端过早失稳、驱动报错或系统明显卡死，再回退参数。
