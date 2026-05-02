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

prepare stage: raw YUV -> encoder -> mp4mux -> MP4

runtime stage: MP4 -> qtdemux -> parser -> RTP payloader -> UDP sink

当前 sender 架构：

- 不再在发送时实时读取 raw YUV 并编码
- 先离线执行 `sender/prepare_mp4.sh`，生成目标码率/帧率的 MP4 资产
- 运行 `sender/sender.sh` 时只做 `qtdemux + parse + RTP payload + UDP send`
- sender 本地 probe 也只测 MP4 runtime 路径，不再测实时编码路径

典型使用方式：

```bash
./sender/prepare_mp4.sh configs/experiment.json
./sender/sender.sh configs/experiment.json
```

如何把样例 YUV 编码成 MP4：

1. 先在 `configs/experiment.json` 里选择输入样例文件，也就是修改 `video_input.path`
2. 按文件名提示填写 `video_input.width`、`video_input.height`、`video_input.source_framerate`
3. 显式填写 `video_input.bit_depth`
4. 根据真实 raw 布局填写 `video_input.format`
5. 选择 `encoder.codec` 与 `sender.preencoded_mp4_path`
6. 运行 `./sender/prepare_mp4.sh configs/experiment.json` 生成 MP4

`sender.preencoded_mp4_path` 可以手动指定，也可以直接写成 `"auto"`。

当它是 `"auto"` 时，脚本会按输入文件名和当前编码参数自动生成输出名，例如：

```text
prepared/yachtride_1920x1080_120fps_420_8bit_yuv_1920x1080_120fps_30fps_h264_8000kbps_8bit.mp4
```

这样切换不同输入文件时，不会再把结果都写到同一个固定 MP4 路径。

两个样例文件：

- `YachtRide_1920x1080_120fps_420_8bit_YUV.yuv`
- `YachtRide_3840x2160_120fps_420_10bit_YUV.yuv`

1080p 8-bit 样例推荐起步配置：

```json
"video_input": {
  "path": "/home/radxa/Videos/Samples/YachtRide_1920x1080_120fps_420_8bit_YUV.yuv",
  "width": 1920,
  "height": 1080,
  "source_framerate": 120,
  "framerate": 30,
  "format": "i420",
  "bit_depth": 8
},
"encoder": {
  "codec": "h264"
},
"sender": {
  "preencoded_mp4_path": "prepared/yacht_1920x1080_120fps_30fps_h264_8000kbps_8bit.mp4"
}
```

4K 10-bit 样例推荐起步配置：

```json
"video_input": {
  "path": "/home/radxa/Videos/Samples/YachtRide_3840x2160_120fps_420_10bit_YUV.yuv",
  "width": 3840,
  "height": 2160,
  "source_framerate": 120,
  "framerate": 30,
  "format": "i420_10le",
  "bit_depth": 10
},
"encoder": {
  "codec": "h265"
},
"sender": {
  "preencoded_mp4_path": "prepared/yacht_3840x2160_120fps_30fps_h265_8000kbps_10bit.mp4"
}
```

生成命令：

```bash
./sender/prepare_mp4.sh configs/experiment.json
```

如果生成成功，脚本会打印：

- `Raw input`
- `Output MP4`
- `Codec`
- `Encoder name`
- `Raw format`
- `Bit depth`
- `Target raw fmt`

然后在 `sender.preencoded_mp4_path` 指向的位置生成 MP4 文件。

生成后可先本地核验 runtime 路径：

```bash
python sender/sender_stats.py --config configs/experiment.json
```

再正式发送：

```bash
./sender/sender.sh configs/experiment.json
```

raw YUV 输入规则：

- `video_input.width` / `height` / `source_framerate` 必须与文件名中的提示一致，否则 `prepare_mp4.sh` 会直接报错
- `video_input.bit_depth` 现在必须显式配置为 `8` 或 `10`
- 文件名中的 `420_8bit` / `420_10bit` 只表达位深，不表达平面布局；`video_input.format` 仍需手动指定真实布局
- 推荐把 `...420_8bit_YUV.yuv` 先按 `i420` 尝试，把 `...420_10bit_YUV.yuv` 先按 `i420_10le` 尝试

当前推荐组合：

- 8-bit 4:2:0 + H.264: `format=i420` 或 `nv12`，`bit_depth=8`
- 8-bit 4:2:0 + H.265: `format=i420` 或 `nv12`，`bit_depth=8`
- 10-bit 4:2:0 + H.265: `format=i420_10le` 或 `p010_10le`，`bit_depth=10`

当前不推荐组合：

- 10-bit + H.264 + `p010_10le`
- 文件名提示为 4K/10-bit，但配置仍写成 1080p/8-bit/NV12

如果需要单独核验 sender runtime 是否仍是瓶颈，可运行：

```bash
python sender/sender_stats.py --config configs/experiment.json
```

该脚本会：

- 在 sender 本机执行 `filesrc -> qtdemux -> parse -> appsink`
- 输出 `sender_metrics.csv`、`sender_events.log`、`resolved_config.json`、`run_info.json`
- 用 `run_info.json` 中的 `samples_per_s` 判断 sender runtime 路径是否达到目标帧率

发送模式：

- 默认按 buffer 时间戳平滑发送
- 默认使用 MP4 容器里的时间戳做实时发送

### 接收端
ARM Debian

链路：

UDP source -> RTP jitter buffer -> depay -> decoder -> appsink

当前默认解码策略：

---

## Vendor (CIX) plugins — safe usage

The project defaults to *not* loading vendor-supplied plugins at runtime. This is deliberate: some vendor plugin bundles
require additional shared libraries and linker configuration (for example `libnoe.so`), and loading an incomplete vendor
bundle can cause `gst-plugin-scanner` warnings or runtime crashes. The config flag `receiver.use_vendor_plugins` in
`configs/experiment.json` controls whether `receiver` attempts to load the vendor plugin path; the default is `false`.

If you want to run with the vendor plugins temporarily (recommended for testing), source the helper script which
activates the project's virtualenv and exports the needed environment variables for the current shell session:

```bash
# run in project root
source scripts/activate_with_vendor.sh

# then run receiver (this uses vendor plugin path for this shell only)
python receiver/receiver_stats.py --config configs/experiment.json
```

To make the vendor shared libraries available system-wide (permanent and requires root):

```bash
echo "/usr/share/cix/lib" | sudo tee /etc/ld.so.conf.d/cix.conf
sudo ldconfig
```

Notes:
- Prefer the temporary `source scripts/activate_with_vendor.sh` flow for experiments. Keep `receiver.use_vendor_plugins=false` in the
  config as the safe default and enable vendor plugins explicitly when you know the platform is prepared.
- If you see `gst-plugin-scanner` errors about missing `lib* .so` files, make sure `/usr/share/cix/lib` is on `LD_LIBRARY_PATH` or
  registered via `ldconfig` as shown above.

- 在 Orion O6 上默认优先使用 `v4l2h264dec` / `v4l2h265dec`
- `receiver_stats.py` 与 `receiver_stats_preview.sh` 使用同一套配置逻辑
- 如需切回软解验证，可将 `receiver.hardware_decoder_placeholder.enabled` 设为 `false`
- receiver 脚本会自动尝试注入 CIX BSP 的 GStreamer 插件路径：`/usr/share/cix/lib/gstreamer-1.0`
- 如果当前环境不存在对应硬解元素（例如 WSL），会自动回退到 `avdec_h264` / `avdec_h265`

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

### Python / uv 环境

项目现在提供了 `uv` 的依赖声明，目标是：

- 新机器先装系统依赖
- 然后在项目根目录直接执行 `uv sync`
- `receiver_stats.py` 所需的 `gi` / `Gst` Python 绑定可在 `.venv` 内导入

推荐在 Ubuntu / Debian 上执行：

```bash
./scripts/bootstrap_ubuntu_uv.sh
```

如果你想手动安装，最少需要先装这些系统包：

```bash
sudo apt update
sudo apt install -y \
  build-essential curl gcc \
  gir1.2-gstreamer-1.0 gobject-introspection libgirepository-2.0-dev \
  libcairo2-dev pkg-config python3-dev python3-venv \
  gstreamer1.0-tools gstreamer1.0-plugins-base gstreamer1.0-plugins-good \
  gstreamer1.0-plugins-bad gstreamer1.0-libav libgirepository-1.0-dev
```

然后：

```bash
uv sync
uv run python -c "import gi; gi.require_version('Gst', '1.0'); from gi.repository import Gst; Gst.init(None); print(Gst.version_string())"
```

说明：

- `PyGObject` 虽然写进了 `pyproject.toml`，但它不是纯 PyPI 依赖，仍然要求系统先安装 `gobject-introspection` / `libgirepository` / GStreamer 相关开发包
- 这也是为什么新机器不能只执行 `uv sync` 而完全跳过系统依赖安装
- 项目默认面向 Linux / WSL；Windows PowerShell 不是推荐运行环境

### GStreamer 安装

本项目默认运行环境是 Ubuntu / Debian。

发送端和接收端至少都应安装这些基础组件：

```bash
sudo apt update
sudo apt install -y \
  gstreamer1.0-tools \
  gstreamer1.0-plugins-base \
  gstreamer1.0-plugins-good \
  gstreamer1.0-plugins-bad \
  gstreamer1.0-libav
```

如果这台机器还要运行 `receiver_stats.py`，也建议一并安装 Python 绑定相关依赖：

```bash
sudo apt install -y \
  python3-dev python3-venv \
  gobject-introspection gir1.2-gstreamer-1.0 \
  libgirepository-2.0-dev libcairo2-dev pkg-config libgirepository-1.0-dev
```

推荐按角色理解：

- `sender` 侧重点是：`rawvideoparse`、`x264enc/x265enc`、`rtph264pay/rtph265pay`、`udpsink`
- `receiver` 侧重点是：`rtpjitterbuffer`、`rtph264depay/rtph265depay`、`avdec_*` 或 `v4l2*dec`、`appsink`
- `preview` 调试还会用到：`autovideosink`、`videoconvert`

安装完成后，建议先做最小验证：

```bash
gst-launch-1.0 --version
gst-inspect-1.0 rtph264depay
gst-inspect-1.0 appsink
gst-inspect-1.0 avdec_h264
```

如果是在 Orion O6 上验证硬解，再额外检查：

```bash
gst-inspect-1.0 v4l2h264dec
gst-inspect-1.0 v4l2h265dec
ls -l /dev/video*
```

如果你使用的是 Orion O6 官方 Debian / Ubuntu BSP，系统里通常还会带有 CIX 的私有 GStreamer 插件目录。当前仓库的 receiver 脚本会自动尝试注入：

```bash
/usr/share/cix/lib/gstreamer-1.0
/usr/share/cix/libexec/gstreamer-1.0/gst-plugin-scanner
```

如果需要手动确认，可执行：

```bash
echo "$GST_PLUGIN_PATH_1_0"
echo "$GST_PLUGIN_SCANNER"
gst-inspect-1.0 v4l2h264dec
```

---

## 5. receiver_stats.py 与 receiver_stats.sh 的关系

### receiver_stats.py

负责真正的接收与统计：

- 创建 GStreamer pipeline
- 支持 `depay_only`、`decode_probe`、`full_stats` 三种模式
- `full_stats` 模式下从 `appsink` 逐帧拉取 sample
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
- 接收链路在 decoder 与 `appsink` 之间增加了 `queue`，并把 `appsink_max_buffers` / `post_decode_queue_max_buffers` 配置化，用于减少末端缓冲过小造成的假性掉帧
- CSV 写盘已从“每帧 flush”改成“批量 flush”，默认可通过 `receiver.csv_flush_interval` 调整
- stall 阈值现在支持按输出帧率自动换算，避免 `10fps` 这类低帧率场景继续沿用过低的固定毫秒阈值

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

---

## 8. 如何用 120fps YUV 做 60fps 实验

裸 YUV 文件本身不携带真正的播放帧率，帧率来自 pipeline 如何解释和处理这些帧。

如果你的素材本身是 `120fps`，但实验希望按 `60fps` 发送，不建议只把 `video_input.framerate` 从 `120` 改成 `60`：

- 那样会把整段素材“按 60fps 解释”
- 所有帧仍然都会被编码
- 视频时长会变成原来的 2 倍
- 不等价于“从 120fps 正常降到 60fps”

当前 sender 已支持分离：

- `video_input.source_framerate`: 原始素材帧率
- `video_input.framerate`: 实验输出帧率

例如，使用 `120fps` 素材做 `60fps` 实验时，可写成：

```json
"video_input": {
  "path": "videos/YachtRide_1920x1080_120fps_420_8bit_YUV.yuv",
  "width": 1920,
  "height": 1080,
  "source_framerate": 120,
  "framerate": 60,
  "format": "i420"
}
```

此时 sender 会在编码前插入：

```text
videorate drop-only=true ! video/x-raw,framerate=60/1
```

也就是：

- 按 `120fps` 解析原始帧序列
- 在编码前做抽帧
- 最终按 `60fps` 送到接收端

接收端统计仍然按 `video_input.framerate` 计算期望帧间隔，因此这里的 `framerate` 应填写实验输出帧率。

另外，sender 当前默认会按输出帧率对应的时间戳节奏发送，而不是把文件尽快推给网络。这一点对 `30fps / 60fps / 120fps` 的基线实验尤其重要，因为它能减少 burst 发送导致的接收端假性掉帧。

---

## 9. 接收端调试模式

接收端现在支持三种模式，通过 `receiver.mode` 切换：

### 1. `depay_only`

链路：

```text
udpsrc -> rtpjitterbuffer -> depay -> queue -> fakesink
```

用途：

- 粗排除网络接收、RTP 重排、depay 是否本身就有问题

重点看：

- `gst-launch` / `receiver_stats.py` 是否稳定运行
- `receiver_events.log` 里有没有 `ERROR`、`WARNING`、`QOS`

这个模式不会生成有意义的 `receiver_metrics.csv` 数据，重点看日志。

### 2. `decode_probe`

链路：

```text
udpsrc -> rtpjitterbuffer -> depay -> decoder -> queue -> fakesink
```

用途：

- 判断一旦加入 decoder，链路是否明显变差

重点看：

- 相比 `depay_only` 是否新增大量 `WARNING` / `QOS`
- 是否更容易中断、停顿或表现异常

这个模式同样以 `receiver_events.log` 为主，不以 CSV 为主。

### 3. `full_stats`

链路：

```text
udpsrc -> rtpjitterbuffer -> depay -> decoder -> queue -> appsink
```

用途：

- 跑完整统计
- 观察 `delta_ms`、`PTS jump`、估算晚到帧等指标

重点看：

- `run_info.json` 中的 `p95_delta_ms`、`p99_delta_ms`
- `PTS jump count`
- `estimated_late_frames_total`
- `receiver_events.log` 中的 `MAJOR_STALL`、`PTS_JUMP`

### 推荐排查顺序

1. 先跑 `depay_only`
2. 再跑 `decode_probe`
3. 最后跑 `full_stats`

判断方式：

- `depay_only` 就差：优先查接收前半段
- `depay_only` 稳、`decode_probe` 差：优先查 decoder
- `decode_probe` 稳、`full_stats` 差：优先查 `appsink` / Python / 写盘

### 操作示例

在 `configs/experiment.json` 中修改：

```json
"receiver": {
  "mode": "depay_only"
}
```

然后运行：

```bash
./receiver/receiver_stats.sh configs/experiment.json
```

测试完后把 `mode` 改成：

- `decode_probe`
- `full_stats`

依次重复即可。

### 防火墙收尾

如果为了在 WSL 上调试接收链路，曾在 Windows / Hyper-V 防火墙中临时放行 `UDP 5004`，测试完成后应及时关闭对应规则，避免长期暴露调试端口。

建议做法：

1. 记录你创建的规则名，例如：
   - `WSL-RTP-UDP-5004`
   - `Allow UDP 5004 to WSL`
2. 测试结束后，以管理员 PowerShell 删除或禁用这些规则

常见命令示例：

```powershell
Remove-NetFirewallHyperVRule -Name "WSL-RTP-UDP-5004"
Remove-NetFirewallRule -DisplayName "Allow UDP 5004 to WSL"
```

如果你不是删除规则，而是临时把 WSL 默认入站策略改成了 `Allow`，也应在测试完成后恢复为原先的更严格策略。

---

## 10. stall 阈值与帧率的关系

当前版本中，`minor stall` / `major stall` 不再默认使用一组固定毫秒值，而是支持按实验输出帧率自动换算。

配置入口在：

```json
"stall_thresholds_ms": {
  "mode": "frame_intervals",
  "minor_frame_intervals": 1.5,
  "major_frame_intervals": 3.0
}
```

### 为什么要这样做

固定阈值在低帧率下会失真。

例如：

- `10fps` 的理论帧间隔约为 `100 ms`
- 如果仍然使用固定 `minor = 50 ms`
- 那么一帧“正常到达”也会被误判为 `minor stall`

这会让低帧率实验的 stall 统计失去意义。

### 当前推荐模式

推荐默认使用：

- `mode = frame_intervals`
- `minor_frame_intervals = 1.5`
- `major_frame_intervals = 3.0`

含义是：

- `minor stall`：当前帧间隔超过理论帧间隔的 `1.5` 倍
- `major stall`：当前帧间隔超过理论帧间隔的 `3.0` 倍

### 示例

`10fps` 时：

- 理论帧间隔约 `100 ms`
- `minor stall` 约为 `150 ms`
- `major stall` 约为 `300 ms`

`30fps` 时：

- 理论帧间隔约 `33.3 ms`
- `minor stall` 约为 `50 ms`
- `major stall` 约为 `100 ms`

`60fps` 时：

- 理论帧间隔约 `16.7 ms`
- `minor stall` 约为 `25 ms`
- `major stall` 约为 `50 ms`

### 如果需要保留旧口径

如果你想做历史对照，仍然可以切回固定毫秒阈值：

```json
"stall_thresholds_ms": {
  "mode": "fixed_ms",
  "minor": 50,
  "major": 200
}
```

但要注意：

- 这种模式更适合固定帧率实验
- 不适合直接横向比较 `10fps / 30fps / 60fps / 120fps`

### 运行时怎么看

`full_stats` 模式下，summary 和 `receiver_events.log` 会打印：

- `Expected frame ms`
- `Threshold mode`

用来帮助确认当前实验到底是按固定毫秒阈值统计，还是按帧率自适应统计。
