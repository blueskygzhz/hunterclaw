#!/bin/bash

# HunterClaw Channel Monitor Demo - 编译和运行脚本

set -e

echo "╔══════════════════════════════════════════════════════════════╗"
echo "║     🦅 HunterClaw Channel Monitor Demo - Build & Run 🦅     ║"
echo "╚══════════════════════════════════════════════════════════════╝"
echo ""

# 编译
echo "📦 正在编译..."
g++ -std=c++17 -pthread -O2 \
    channel_monitor_demo.cc \
    -o channel_monitor_demo \
    -Wall -Wextra

if [ $? -eq 0 ]; then
    echo "✅ 编译成功!"
    echo ""
    
    # 运行
    echo "🚀 运行 Demo..."
    echo ""
    ./channel_monitor_demo
else
    echo "❌ 编译失败!"
    exit 1
fi
