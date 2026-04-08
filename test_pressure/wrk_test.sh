#!/bin/bash

# wrk 压力测试脚本
# 用法: ./wrk_test.sh [URL] [并发数] [持续时间] [线程数]
# 示例: ./wrk_test.sh http://127.0.0.1:9006/ 1000 5s 4

URL=${1:-http://127.0.0.1:9006/}
CONNECTIONS=${2:-1000}
DURATION=${3:-5s}
THREADS=${4:-4}

echo "=========================================="
echo "wrk 压力测试"
echo "=========================================="
echo "URL: $URL"
echo "并发连接数: $CONNECTIONS"
echo "测试持续时间: $DURATION"
echo "线程数: $THREADS"
echo "=========================================="
echo ""

wrk -c $CONNECTIONS -d $DURATION -t $THREADS $URL
