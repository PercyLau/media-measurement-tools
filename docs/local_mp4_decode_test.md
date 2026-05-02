# Local MP4 Decode Comparison Test

## 1. 目的

本测试用于把 receiver 问题拆成两部分：

- 本地解封装 + 解码链路是否本身就会出现 `PTS_JUMP` / `estimated_late_frames`
- 只有走 RTP/UDP live 接收链路时才会出现 `PTS_JUMP` / `estimated_late_frames`

如果本地 MP4 直解没有问题，而 RTP 接收模式有问题，则优先怀疑：

- `rtpjitterbuffer`
- depay / parser / decoder 在 live 模式下的联动
- live QoS / skip 行为

如果本地 MP4 直解也出现同类问题，则要进一步怀疑：

- bitstream 兼容性
- decoder 本身的实时输出行为

---

## 2. 前提

当前仓库中的 `receiver/receiver_stats.py` 已支持一个新的 receiver 模式：

- `local_mp4_full_stats`

该模式复用正式 receiver 的统计逻辑，但输入改为：

```text
filesrc -> qtdemux -> parse -> decoder -> appsink
```

而不是：

```text
udpsrc -> rtpjitterbuffer -> depay -> parse -> decoder -> appsink
```

---

## 3. 本地 MP4 测试命令

### 3.1 使用当前 experiment.json 并临时切到本地 MP4 模式

下面这条命令会：

- 临时复制一份 config
- 把 `receiver.mode` 改成 `local_mp4_full_stats`
- 指定待测 MP4 文件
- 启动本地解封装 + 解码 + appsink 统计

```bash
tmp=$(mktemp) && \
jq '.receiver.mode="local_mp4_full_stats" \
  | .sender.preencoded_mp4_path="prepared/yachtride_3840x2160_3840x2160_120fps_60fps_h265_8000kbps_8bit.mp4"' \
  configs/experiment.json > "$tmp" && \
/home/radxa/Projects/media-measurement-tools/.venv/bin/python receiver/receiver_stats.py --config "$tmp"
```

### 3.2 说明

- `receiver.mode="local_mp4_full_stats"` 会启用本地 MP4 输入模式
- `sender.preencoded_mp4_path` 指向本次要测试的 MP4 文件
- 统计口径和正式 receiver `full_stats` 相同，输出仍然是：
  - `receiver_metrics.csv`
  - `receiver_events.log`
  - `resolved_config.json`
  - `run_info.json`

---

## 4. 正式 RTP 接收测试命令

用于与本地 MP4 模式做对照：

```bash
/home/radxa/Projects/media-measurement-tools/.venv/bin/python receiver/receiver_stats.py --config configs/experiment.json
```

该命令走的是正式 live 接收链路：

```text
udpsrc -> rtpjitterbuffer -> depay -> parse -> decoder -> appsink
```

---

## 5. 如何看结果

### 5.1 关注 receiver_events.log

优先看这些事件：

- `SAMPLE_CAPS`
- `PTS_JUMP`
- `MAJOR_STALL`
- `QOS`
- `Decoder selected`

例如本地 MP4 正常时，常见现象是：

- 只有一次 `SAMPLE_CAPS`
- 没有 `PTS_JUMP`
- 没有 `QOS`
- `estimated_late_frames_total = 0`

### 5.2 关注 run_info.json summary

重点字段：

- `total_samples`
- `minor_stalls`
- `major_stalls`
- `pts_jump_count`
- `estimated_late_frames_total`
- `max_estimated_late_frames_per_gap`

如果本地 MP4 模式下这些字段接近 0，而 RTP 模式下明显升高，则说明问题更像 live 接收链路引起。

---

## 6. CSV 分析命令

下面这条命令可以快速汇总一次 run 的 `receiver_metrics.csv`：

```bash
python - <<'PY'
import csv
from pathlib import Path
from statistics import mean

path = Path('output/<your_run_dir>/receiver_metrics.csv')
rows = list(csv.DictReader(path.open()))

print('rows', len(rows))

deltas = [float(r['delta_ms']) for r in rows if r['delta_ms']]
ptsj = [r for r in rows if r['is_pts_jump'] == '1']
maj = [r for r in rows if r['is_stall_major'] == '1']
minor = [r for r in rows if r['is_stall_minor'] == '1']
late = [int(r['estimated_late_frames']) for r in rows if r['estimated_late_frames']]

print('delta_mean', round(mean(deltas), 3) if deltas else 0)
print('delta_max', max(deltas) if deltas else 0)
print('minor', len(minor), 'major', len(maj), 'pts_jump', len(ptsj), 'late_total', sum(late))

print('top_late', sorted(
    (
        (
            int(r['frame_idx']),
            int(r['estimated_late_frames']),
            float(r['pts_gap_frames']) if r['pts_gap_frames'] else 0.0,
            float(r['delta_ms']) if r['delta_ms'] else 0.0,
        )
        for r in rows
    ),
    key=lambda x: x[1],
    reverse=True,
)[:10])

print('top_delta', sorted(
    (
        (
            int(r['frame_idx']),
            float(r['delta_ms']) if r['delta_ms'] else 0.0,
            int(r['estimated_late_frames']),
            float(r['pts_gap_frames']) if r['pts_gap_frames'] else 0.0,
        )
        for r in rows
    ),
    key=lambda x: x[1],
    reverse=True,
)[:10])
PY
```

把 `output/<your_run_dir>/receiver_metrics.csv` 替换成实际 run 目录。

---

## 7. 帧附近切片分析命令

当你已经知道几个异常帧号时，可以用下面这条命令看局部窗口：

```bash
python - <<'PY'
import csv
from pathlib import Path

path = Path('output/<your_run_dir>/receiver_metrics.csv')
rows = list(csv.DictReader(path.open()))

for center in [0, 48, 56, 83, 89, 182, 220]:
    lo = max(0, center - 2)
    hi = min(len(rows), center + 5)
    print(f'--- around {center} ---')
    for r in rows[lo:hi]:
        print(
            r['frame_idx'],
            r['delta_ms'],
            r['pts_gap_frames'],
            r['estimated_late_frames'],
            r['is_stall_minor'],
            r['is_stall_major'],
            r['is_pts_jump'],
        )
PY
```

这条命令适合判断：

- `PTS_JUMP` 是孤立突发还是连续 burst
- `MAJOR_STALL` 是否一定伴随 `PTS_JUMP`
- `delta_ms` 和 `pts_gap_frames` 是否同步变大

---

## 8. 如何解释结果

### 8.1 本地 MP4 正常，RTP 模式异常

优先怀疑：

- live RTP 接收链路时序
- `rtpjitterbuffer` / depay / parser / decoder 的 live 联动
- decoder 在 live 模式下的 QoS / skip 行为

### 8.2 本地 MP4 也异常

优先怀疑：

- bitstream 兼容性
- decoder 本身的输出行为
- 同参数但不同机器编码出的码流差异

### 8.3 本地 MP4 和 RTP 都正常

说明当前参数组合没有稳定复现问题，应回到触发问题的具体素材、码率、帧率或 decoder 路径重新缩小范围。

---

## 9. 建议的对照矩阵

最小建议矩阵：

1. ARM 本地生成 MP4，本地解码
2. x86 生成 MP4，本地解码
3. x86 生成 MP4，经 RTP 发送到 ARM 解码

判断逻辑：

- `1 正常 + 2 正常 + 3 异常`：优先怀疑 live RTP 接收链路
- `1 正常 + 2 异常 + 3 异常`：优先怀疑 bitstream / 编码差异
- `1 异常 + 2 异常 + 3 异常`：优先怀疑 decoder 或统计口径问题
