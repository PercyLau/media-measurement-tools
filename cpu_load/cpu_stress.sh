#!/usr/bin/env bash
#
# CPU 负载测试工具包装器
# 类似 vulkan_mem_press 的使用风格，支持多种 stress-ng 负载模式
#
# 用法:
#   ./cpu_stress.sh --mode matrix --threads 4 --seconds 60
#   ./cpu_stress.sh --mode stream --threads 2 --seconds 30
#   ./cpu_stress.sh --mode all --threads 4 --seconds 60
#
# 模式说明:
#   matrix   - 矩阵运算（高 CPU 计算密度，类似 LLM 推理）
#   stream   - 内存带宽测试（STREAM benchmark 风格）
#   cache    - CPU 缓存压力（L1/L2/L3 cache thrashing）
#   cpu      - 纯 CPU 计算（素数、斐波那契等）
#   all      - 组合多种负载模式
#
# 参数:
#   --mode MODE       负载模式 (默认: matrix)
#   --threads N       CPU 线程数 (默认: 4)
#   --seconds N       持续时间秒 (默认: 60)
#   --cpu-list LIST   指定 CPU 核心，如 "0-3" 或 "0,1,2,3"
#   --verbose         输出详细日志
#   --help            显示帮助
#

set -euo pipefail

# 默认参数
MODE="matrix"
THREADS=4
SECONDS=60
CPU_LIST=""
VERBOSE=false

# 解析参数
while [[ $# -gt 0 ]]; do
    case $1 in
        --mode)
            MODE="$2"
            shift 2
            ;;
        --threads)
            THREADS="$2"
            shift 2
            ;;
        --seconds)
            SECONDS="$2"
            shift 2
            ;;
        --cpu-list)
            CPU_LIST="$2"
            shift 2
            ;;
        --verbose)
            VERBOSE=true
            shift
            ;;
        --help|-h)
            head -30 "$0" | tail -28
            exit 0
            ;;
        *)
            echo "Unknown option: $1"
            echo "Use --help for usage information"
            exit 1
            ;;
    esac
done

# 验证 stress-ng 存在
if ! command -v stress-ng &> /dev/null; then
    echo "Error: stress-ng not found. Install with: sudo apt install stress-ng"
    exit 1
fi

# 构建 stress-ng 参数
STRESS_ARGS=(
    "--timeout" "${SECONDS}s"
    "--metrics-brief"
)

# 添加 CPU 绑定
if [[ -n "$CPU_LIST" ]]; then
    STRESS_ARGS+=("--taskset" "$CPU_LIST")
fi

# 根据模式添加负载
case $MODE in
    matrix)
        STRESS_ARGS+=("--matrix" "$THREADS")
        STRESS_ARGS+=("--matrix-size" "512")
        echo "Starting matrix stress: $THREADS threads, 512x512 matrix, ${SECONDS}s"
        ;;
    stream)
        STRESS_ARGS+=("--stream" "$THREADS")
        echo "Starting stream stress: $THREADS threads, ${SECONDS}s"
        ;;
    cache)
        STRESS_ARGS+=("--cache" "$THREADS")
        STRESS_ARGS+=("--cache-ops" "all")
        echo "Starting cache stress: $THREADS threads, ${SECONDS}s"
        ;;
    cpu)
        STRESS_ARGS+=("--cpu" "$THREADS")
        STRESS_ARGS+=("--cpu-method" "all")
        echo "Starting CPU stress: $THREADS threads, ${SECONDS}s"
        ;;
    all)
        STRESS_ARGS+=("--matrix" "$THREADS")
        STRESS_ARGS+=("--stream" "$THREADS")
        STRESS_ARGS+=("--cache" "$THREADS")
        echo "Starting combined stress: matrix+stream+cache, $THREADS threads each, ${SECONDS}s"
        ;;
    *)
        echo "Error: Unknown mode '$MODE'"
        echo "Valid modes: matrix, stream, cache, cpu, all"
        exit 1
        ;;
esac

# 执行 stress-ng
if [[ "$VERBOSE" == "true" ]]; then
    echo "Command: stress-ng ${STRESS_ARGS[*]}"
    stress-ng "${STRESS_ARGS[@]}"
else
    stress-ng "${STRESS_ARGS[@]}" 2>&1 | grep -E "(bogo|real|user|sys|metric)"
fi

echo "CPU stress test completed"
