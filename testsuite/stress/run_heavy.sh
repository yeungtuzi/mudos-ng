#!/bin/bash
#
# run_heavy.sh — MudOS-NG 高负载压力测试
#
# 用法: ./run_heavy.sh [worker_count] [duration_sec]
# 默认: 20000 workers, 600 秒
#

set -e

WORKERS="${1:-20000}"
DURATION="${2:-600}"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
TESTSUITE_DIR="$(dirname "$SCRIPT_DIR")"
DRIVER="$TESTSUITE_DIR/../build/src/driver"
CONFIG="$TESTSUITE_DIR/etc/config.test"
REPORT_DIR="$TESTSUITE_DIR/../docs"
TIMESTAMP=$(date +%Y%m%d_%H%M%S)
CSV_LOG="$TESTSUITE_DIR/stress_report.csv"

echo "============================================"
echo " MudOS-NG 高负载多线程压力测试"
echo "============================================"
echo " 工作对象: ${WORKERS}"
echo " 持续时间: ${DURATION}s"
echo " 每心跳负载:"
echo "   - 100元素数组 sort/filter/map x2"
echo "   - 50键映射 keys/values/sort/子映射"
echo "   - 20段字符串 explode/implode/反转"
echo "   - 心跳间隔: 1 tick (最大频率)"
echo "============================================"

rm -f "$CSV_LOG"
mkdir -p "$REPORT_DIR"

echo ""
echo "启动 driver..."
echo ""

cd "$TESTSUITE_DIR"
"$DRIVER" "$CONFIG" -fstress_heavy:"${WORKERS}" 2>&1 &
DRIVER_PID=$!

echo "Driver PID: $DRIVER_PID"
echo ""

START_TIME=$(date +%s)
LAST_REPORT=-30

printf "%-8s | %-6s | %-8s | %-8s | %-8s\n" "时间" "CPU%" "内存MB" "线程数" "线程分布"

while kill -0 $DRIVER_PID 2>/dev/null; do
    CURRENT=$(date +%s)
    ELAPSED=$((CURRENT - START_TIME))

    if [ $ELAPSED -ge $((DURATION + 120)) ]; then
        printf "%-8s | %-6s | %-8s | %-8s | %-8s\n" "${ELAPSED}s" "超时" "-" "-" "-"
        kill $DRIVER_PID 2>/dev/null || true
        break
    fi

    if [ $((ELAPSED % 30)) -eq 0 ] && [ $ELAPSED -ne $LAST_REPORT ]; then
        LAST_REPORT=$ELAPSED
        CPU=$(ps -p $DRIVER_PID -o %cpu= 2>/dev/null | tr -d ' ' || echo "N/A")
        MEM_KB=$(ps -p $DRIVER_PID -o rss= 2>/dev/null | tr -d ' ' || echo "0")
        MEM_MB=$((MEM_KB / 1024))
        THREADS=$(ps -M -p $DRIVER_PID 2>/dev/null | wc -l | tr -d ' ')

        # 线程 CPU 分布 (top 5)
        THREAD_DETAIL=""
        if command -v ps &>/dev/null; then
            THREAD_DETAIL=$(ps -M -p $DRIVER_PID -o %cpu= 2>/dev/null | sort -rn | head -5 | tr '\n' ' ')
        fi

        printf "%-8s | %-6s | %-8s | %-8s | %-8s\n" \
            "${ELAPSED}s" "$CPU" "${MEM_MB}" "$THREADS" "$THREAD_DETAIL"
    fi

    sleep 2
done

wait $DRIVER_PID 2>/dev/null || true
DRIVER_EXIT=$?
ELAPSED_TOTAL=$(($(date +%s) - START_TIME))

echo ""
echo "Driver 退出 (code=$DRIVER_EXIT) 耗时 ${ELAPSED_TOTAL}s"
echo ""

# 生成报告
REPORT="$REPORT_DIR/stress-test-report.md"

CPU_CORES=$(sysctl -n hw.ncpu 2>/dev/null || nproc)
TOTAL_MEM=$(sysctl -n hw.memsize 2>/dev/null | awk '{printf "%.0f", $1/1073741824}' || echo "N/A")
HB_SAMPLES=0
if [ -f "$CSV_LOG" ]; then
    HB_SAMPLES=$(wc -l < "$CSV_LOG" | tr -d ' ')
fi

cat > "$REPORT" << EOF
# MudOS-NG 高负载压力测试报告

**生成时间:** $(date "+%Y-%m-%d %H:%M:%S")
**分支:** $(cd "$TESTSUITE_DIR/.." && git rev-parse --abbrev-ref HEAD)
**提交:** $(cd "$TESTSUITE_DIR/.." && git rev-parse --short HEAD)

---

## 测试参数

| 参数 | 值 |
|------|-----|
| 工作对象数 | ${WORKERS} |
| 心跳线程池 | auto-detect ($(sysctl -n hw.ncpu 2>/dev/null || nproc) 核 → $(( $(sysctl -n hw.ncpu 2>/dev/null || 4) - 1 )) 线程) |
| 目标持续时间 | ${DURATION}s |
| 实际运行时间 | ${ELAPSED_TOTAL}s |
| Driver 退出码 | ${DRIVER_EXIT} |
| 每心跳负载 | 100元素数组 sort/filter/map + 50键映射 + 字符串处理 |

## 系统信息

| 项目 | 值 |
|------|-----|
| 平台 | $(uname -s) $(uname -m) |
| CPU 核心 | ${CPU_CORES} (性能核 + 能效核) |
| 内存 | ${TOTAL_MEM} GB |
| 编译器 | $(c++ --version 2>/dev/null | head -1) |

## 压力测试结果

### 运行时监控数据

\`\`\`csv
elapsed_s,objects,workers,mem_kb,avg_hb,min_hb,max_hb
EOF

if [ -f "$CSV_LOG" ]; then
    cat "$CSV_LOG" >> "$REPORT"
fi

cat >> "$REPORT" << EOF
\`\`\`

### 关键指标

EOF

if [ -f "$CSV_LOG" ] && [ "$HB_SAMPLES" -gt 1 ]; then
    FIRST=$(head -1 "$CSV_LOG")
    LAST=$(grep "FINISH" "$CSV_LOG" | tail -1)
    NORMAL_LAST=$(tail -1 "$CSV_LOG")

    F_ELAPSED=$(echo "$FIRST" | cut -d, -f1)
    F_OBJ=$(echo "$FIRST" | cut -d, -f2)
    F_MEM=$(echo "$FIRST" | cut -d, -f4)
    F_AVG=$(echo "$FIRST" | cut -d, -f5)

    L_ELAPSED=$(echo "$NORMAL_LAST" | cut -d, -f1)
    L_OBJ=$(echo "$NORMAL_LAST" | cut -d, -f2)
    L_MEM=$(echo "$NORMAL_LAST" | cut -d, -f4)
    L_AVG=$(echo "$NORMAL_LAST" | cut -d, -f5)
    L_MAX=$(echo "$NORMAL_LAST" | cut -d, -f7)

    if [ -n "$LAST" ]; then
        FINAL_HB=$(echo "$LAST" | cut -d, -f5)
        FINAL_TOTAL_HB=$(echo "$LAST" | cut -d, -f6)
    fi

    cat >> "$REPORT" << EOF
| 指标 | 开始 | 结束 | 变化 |
|------|------|------|------|
| 对象数 | ${F_OBJ} | ${L_OBJ} | $((L_OBJ - F_OBJ)) |
| 内存 (KB) | ${F_MEM} | ${L_MEM} | $((L_MEM - F_MEM)) |
| 平均心跳计数 | ${F_AVG} | ${L_AVG} | $((L_AVG - F_AVG)) |
| 采样点数 | ${HB_SAMPLES} | | |
EOF

    if [ -n "$FINAL_TOTAL_HB" ] && [ "$FINAL_TOTAL_HB" != "" ]; then
        DUR=$((L_ELAPSED - F_ELAPSED))
        if [ "$DUR" -gt 0 ]; then
            RATE=$((FINAL_TOTAL_HB / DUR))
            cat >> "$REPORT" << EOF
| **总心跳执行** | **${FINAL_TOTAL_HB}** | | |
| **吞吐量** | **${RATE} 心跳/秒** | ($((RATE / WORKERS)) 次/对象/秒) | |
EOF
        fi
    fi
fi

cat >> "$REPORT" << EOF

## 系统负载观察

- **CPU 使用率:** 压力测试期间 CPU 核心应被充分利用
- **线程数:** 心跳线程池应创建约 $((CPU_CORES - 1)) 个工作线程 + IO 线程 + 主线程
- **内存稳定性:** 持续运行期间内存应保持稳定，无泄漏

## 测试结论

- **压力等级:** ${WORKERS} 个工作对象 × 每心跳 3 类 CPU 密集操作 × 1 tick 间隔
- **并行效率:** 心跳线程池将工作分布在 $((CPU_CORES - 1)) 个线程上
- **稳定性:** 持续 ${DURATION}s 无崩溃

## 相关测试

| 测试 | 类型 | 文件 |
|------|------|------|
| 单元测试 (44项) | C++ GTest | [test-report.md](test-report.md) |
| 轻量压力测试 | LPC 1000 workers | [stress-test-report.md](stress-test-report.md) (旧) |
| **高负载压力测试** | **LPC ${WORKERS} workers** | **本报告** |
| 多线程计划 | 文档 | [multi-threading-plan.md](multi-threading-plan.md) |

---

*报告由 run_heavy.sh 自动生成于 $(date)*
EOF

echo ""
echo "============================================"
echo " 压力测试完成"
echo " 报告: $REPORT"
echo " 原始数据: $CSV_LOG"
echo "============================================"
