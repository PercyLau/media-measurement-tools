# Hardware Encode Benchmark Methodology

## 1. 目的

测量 NVENC 硬件编码器的纯编码吞吐量。结果应反映编码芯片本身的极限，不包含上游（格式转换）和下游（Python 回调）开销。

---

## 2. 正确方法

### 2.1 测试命令

```bash
uv run python scripts/bench_codec.py -W <width> -H <height> -f <fps> --mode encode --duration 10
```

脚本已内置以下优化：
- **NV12 直入**：使用 NVENC 时直接从 `videotestsrc` 输出 NV12，跳过 `videoconvert`（色域转换是独立开销，不应计入编码速度）
- **最快参数**：NVENC 使用 `preset=p1 zerolatency=true`（最快预设，无 B-frame）
- **无磁盘 I/O**：用 `videotestsrc` 生成测试帧，排除磁盘读瓶颈

### 2.2 纯编码验证（可选）

如需验证编码器本身的极限（排除 Python appsink 开销），可用 `gst-launch-1.0` + `fakesink`：

```bash
gst-launch-1.0 videotestsrc is-live=false num-buffers=N \
  ! video/x-raw,format=NV12,width=W,height=H,framerate=FPS/1 \
  ! nvh265enc preset=p1 zerolatency=true \
  ! fakesink sync=false
```

管道结束后的 `Execution ended after ...` 就是纯编码耗时。

### 2.3 逐段拆解法（定位瓶颈）

```bash
# 第 1 层：纯原始帧生成
videotestsrc ! video/x-raw,format=I420,... ! fakesink

# 第 2 层：帧生成 + 色域转换
videotestsrc ! video/x-raw,format=I420,... ! videoconvert ! video/x-raw,format=NV12 ! fakesink

# 第 3 层：NV12 直入编码（纯编码）
videotestsrc ! video/x-raw,format=NV12,... ! nvh265enc preset=p1 zerolatency=true ! fakesink
```

各段耗时相减即可得到每段的独立开销。

---

## 3. bench_codec.py 修复记录

### 已修复的问题

| 修复点 | 修复前 | 修复后 | 影响 |
|--------|--------|--------|------|
| 输入格式 | 硬编码 `format=I420` | NVENC 时用 `format=NV12` 直出 | 消除 ~30% videoconvert 开销 |
| videoconvert | 每次 `I420 → NV12` | 直接移除 | — |
| NVENC preset | 未设置（默认 p3） | `preset=p1 zerolatency=true` | 确保最快模式 |

### 修复前后对比 (1080p@120fps)

| 方法 | H.264 | H.265 |
|------|-------|-------|
| bench_codec.py 修复前 (I420 + videoconvert + appsink) | 168 fps | 161 fps |
| bench_codec.py 修复后 (NV12 直入) | **216 fps** | **211 fps** |
| gst-launch fakesink (纯编码参考值) | 211 fps | 210 fps |

> 修复后 bench_codec.py 结果与纯编码参考值基本一致。

---

## 4. GTX 1650 测试结果

**环境：** WSL Ubuntu 26.04, NVIDIA GeForce GTX 1650 (Turing NVENC, 4GB), Driver 596.36

**测试日期：** 2026-06-01

### 4.1 最快模式 (preset=p1 zerolatency=true)

| 分辨率 | H.264 | H.265 | 目标 | 达标 |
|--------|-------|-------|------|------|
| 1080p@120fps | **216 fps** | **211 fps** | 120 | ✅ ~1.8x |
| 4K@90fps | **71.6 fps** | **70.6 fps** | 90 | ❌ ~0.8x |

### 4.2 瓶颈分析

- **1080p@120fps**：GTX 1650 NVENC 轻松达标，编码不是瓶颈
- **4K@90fps**：NVENC 芯片极限约 71fps，Preset 切换几乎无影响（所有 preset 均在 ~71fps），NVENC 硬件单元已饱和。这是 Turing 架构芯片的硬限制
- **软编 (x265enc)**：4K@90 仅 12fps，完全不适合实时场景

### 4.3 4K@90fps 瓶颈逐段拆解

| 管线段 | 耗时 (900帧) | 等效 FPS | 损失 |
|--------|-------------|---------|------|
| videotestsrc 生成 4K I420 原始帧 | 10.63s | 84.7 | 基准线 |
| + videoconvert I420→NV12 | 15.22s | 59.1 | −30% |
| + nvh265enc 编码 (fakesink) | 17.07s | 52.7 | −11% |
| + Python appsink (bench_codec.py) | 21.86s | 41.1 | −22% |

> 注意：这是修复前的数据。修复后的 bench_codec.py 直接从 NV12 开始，避免了第一步和第二步的开销。

---

## 5. 在其他机器上测试

### 前提条件

```bash
# 确认 NVENC 编码器可用
gst-inspect-1.0 nvh264enc
gst-inspect-1.0 nvh265enc

# 确认项目环境就绪
uv sync
```

### 运行

```bash
# 1080p
uv run python scripts/bench_codec.py -W 1920 -H 1080 -f 120 --mode encode --duration 10

# 4K
uv run python scripts/bench_codec.py -W 3840 -H 2160 -f 90 --mode encode --duration 10

# 只测 H.264 或 H.265
uv run python scripts/bench_codec.py -W 3840 -H 2160 -f 90 --mode encode --codec h265 --duration 10
```

### 预期差异

- **Ampere (RTX 30)** / **Ada (RTX 40)** 的 NVENC 芯片本身吞吐更高，4K@90 可能达标
- 如果机器上没有 NVENC（如纯 AMD 或 Intel），脚本会自动回退到软件编码器
- x265enc/x264enc 的速度取决于 CPU 核心数和 AVX/SSE 指令集

---

## 6. 对项目的影响

- 本项目 sender 采用两阶段架构：Prepare（离线编码 MP4）→ Runtime（MP4 解封装 + RTP 发送）
- **编码只在 Prepare 阶段跑一次**，不影响实验 runtime 的帧发送节奏
- 1080p 实验：GTX 1650 完全够用
- 4K 实验：Prepare 阶段编码较慢（~0.8x 实时），但只是一次性成本，可以接受；如需实时 4K 编码，需更高代际 GPU
- I420→NV12 色域转换应由 `prepare_mp4.sh` 在离线阶段处理，不应计入编码 benchmark
