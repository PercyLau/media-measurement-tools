# Vulkan Test Project

## 简介
本项目包含两个基于Vulkan的C++测试程序：
- `vk_memstress`：用于内存压力测试，可自定义多种参数。
- `vk_compute_min`：用于Vulkan计算测试。


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
- `--einv N`           每次调用的字数（默认64）
- `--wg N`             工作组数（默认4096）
- `--seconds N`        持续时间（默认10秒）

### vk_compute_min
```sh
./vk_compute_min
```

## 备注
- 需确保系统已安装Vulkan运行环境。
- `.spv`/`.comp`文件为着色器相关文件。
