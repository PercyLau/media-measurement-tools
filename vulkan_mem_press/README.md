# Vulkan Test Project

## 简介
本项目包含两个基于Vulkan的C++测试程序：
- `vk_memstress`：用于 GPU 内存压力测试与带宽 benchmark，可自定义多种参数。
- `vk_compute_min`：用于Vulkan计算测试。

当前 `vk_memstress` 支持两类用途：
- `stress` 路径：保留原有“制造接收端负载”的使用方式，强调可控卡顿与参数分档。
- `benchmark` 路径：更接近纯 GPU 带宽测量，支持 batched submit、GPU timestamp 和 `DEVICE_LOCAL` + staging 初始化。


## 依赖
- C++17 编译器（如 g++）
- Vulkan SDK 运行环境
	- **Debian/Ubuntu** 安装命令：
		```sh
		sudo apt update
		sudo apt install libvulkan-dev vulkan-tools
		```
	- **其他平台** 请参考 [Vulkan 官网](https://vulkan.lunarg.com/) 获取 SDK 和驱动安装方法。

## 编译

### 使用 Makefile
```sh
make
```

### 使用 CMake
```sh
mkdir build
cd build
cmake ..
make
```

## 运行

### vk_memstress
```sh
./vk_memstress --help
```

常用参数：
- `--mb N`            数据缓冲区大小（MB，默认512）
- `--mode rd|wr|rdwr` 读/写/读写模式（默认rdwr）
- `--stride N`         步长，4的倍数（默认64）
- `--iters N`          迭代次数（默认200）
- `--chunk-iters N`    每次分派最大迭代（默认40）
- `--dispatches-per-submit N` 单次提交里包含的 dispatch 数（默认1）
- `--warmup-submits N` 计时前的预热提交次数（默认0）
- `--einv N`           每次调用的字数（默认64）
- `--wg N`             工作组数（默认4096）
- `--seconds N`        持续时间（默认10秒）
- `--benchmark`        启用更接近带宽 benchmark 的执行路径（GPU timestamp + batched submit）
- `--gpu-timing`       打印 GPU timestamp 口径的吞吐
- `--strict-device-local` 要求使用 `DEVICE_LOCAL` 缓冲区
- `--buffer-layout output|copy` 指定 binding1 是 per-invocation 输出还是 copy 目标缓冲区
- `--spv PATH`         指定使用的 shader SPIR-V 文件

### Shader / 布局选择

`vk_memstress` 现在支持两种主要 shader：

- `memstress_alu.spv`
	ALU/计算压力 shader（MUL/ADD + 位运算），适合做接收端负载注入。
- `memstress_bw.spv`
	streaming copy 带宽 shader，建议配合 `--buffer-layout copy` 使用，binding0 为源数据，binding1 直接作为目标缓冲区。

`--buffer-layout` 的语义（仅对 `memstress_bw` 生效）：

- `output`：binding1 大小约为 `workgroups * 256 * sizeof(u32)`，用于每个 invocation 写一份私有结果。
- `copy`：binding1 大小与源数据缓冲区相同，适合 `src -> dst` 的 copy 型 benchmark。

### 推荐用法

保留原有压力测试行为：

```sh
./vk_memstress \
	--mb 192 \
	--mode rdwr \
	--stride 16 \
	--iters 60 \
	--chunk-iters 20 \
	--einv 64 \
	--wg 1024 \
	--seconds 45 \
	--spv ./memstress_alu.spv
```

示例：更接近纯 GPU 带宽测量的运行方式

```sh
./vk_memstress \
	--benchmark \
	--buffer-layout copy \
	--mode rdwr \
	--chunk-iters 8 \
	--dispatches-per-submit 2 \
	--warmup-submits 0 \
	--einv 64 \
	--wg 1024 \
	--spv ./memstress_bw.spv
```

说明：
- `est throughput` 仍然是 wall-clock 口径。
- `measured gpu time` / `gpu throughput` 是基于 GPU timestamp 的更接近 benchmark 的口径。
- `warmup wall time` 单独统计预热阶段；`measured wall time` 单独统计正式测量阶段。
- `dispatches_completed` 表示完成的 dispatch 数；`measured_submits_completed` 表示完成的 measured submit 数。
- 若需要更干净的 streaming copy 路径，优先配合 `memstress_bw.spv` 和 `--buffer-layout copy` 使用。
- 当目标缓冲区使用 `DEVICE_LOCAL` 时，工具现在会自动用 staging buffer 初始化源数据并清零目标缓冲区。
- `--benchmark` 会自动启用更适合 benchmark 的默认行为：`gpu_timing=true`、`strict_device_local=true`，并在未显式设置时使用更大的 `dispatches-per-submit` 和少量 warmup。

### 与实验配置的兼容性

项目里的接收端负载链路保持不变：

- `receiver/receiver_stats.sh` 会把 `receiver_load.args` 原样透传给 `vk_memstress`。
- `receiver/receiver_stats.py` 会把 `receiver_load.args` 原样写入运行信息和 hash 载荷。

这意味着新增的 benchmark 参数不需要 receiver 侧代码配合，只需要在 `configs/experiment.json` 中调整 `receiver_load.args`。

推荐的实验配置负载参数示例：

```json
"receiver_load": {
	"enabled": true,
	"startup_delay_sec": 0,
	"workdir": ".",
	"binary": "./vulkan_mem_press/vk_memstress",
	"args": [
		"--mb", "256",
		"--benchmark",
		"--buffer-layout", "copy",
		"--mode", "rdwr",
		"--stride", "16",
		"--iters", "40",
		"--chunk-iters", "8",
		"--dispatches-per-submit", "2",
		"--warmup-submits", "0",
		"--einv", "64",
		"--wg", "1024",
		"--seconds", "10",
		"--spv", "./vulkan_mem_press/memstress_bw.spv"
	]
}
```

如果目标仍然是“制造卡顿”而不是“测纯带宽”，建议继续使用 `memstress_alu.spv`，不要默认切到 benchmark 路径。

### vk_compute_min
```sh
./vk_compute_min
```

## 备注
- 需确保系统已安装Vulkan运行环境。
- `.spv`/`.comp`文件为着色器相关文件。
