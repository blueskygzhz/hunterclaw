#!/bin/bash
# HunterClaw Memory 模块编译脚本

set -e

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
BUILD_DIR="$SCRIPT_DIR/build_memory"
SRC_DIR="$SCRIPT_DIR"

echo "==> 编译 HunterClaw Memory 模块"
echo "Source: $SRC_DIR"
echo "Build: $BUILD_DIR"

# 清理
rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR"

# 编译选项
CXX="g++"
CXXFLAGS="-std=c++1y -O2 -Wall -fPIC"
INCLUDES="-I$SRC_DIR/include -I$SRC_DIR/src -I$SRC_DIR/external/json/single_include"

# 定义预处理宏 (绕过复杂模板)
DEFINES="-DHUNTERCLAW_SIMPLE_BUILD"

# 编译源文件
echo ""
echo "==> 编译源文件..."

SOURCES=(
    "src/core/utils.cc"
    "src/core/types.cc"
    "src/memory/compaction.cc"
    "src/memory/compaction_manager.cc"
    "src/memory/compaction_hooks.cc"
    "src/memory/memory_manager.cc"
    "src/memory/session_memory_provider.cc"
)

OBJECTS=()
for src in "${SOURCES[@]}"; do
    obj="$BUILD_DIR/$(basename ${src%.cc}.o)"
    echo "  编译: $src"
    if $CXX $CXXFLAGS $INCLUDES $DEFINES -c "$SRC_DIR/$src" -o "$obj" 2>&1 | tee "$BUILD_DIR/$(basename $src).log" | head -20; then
        OBJECTS+=("$obj")
    else
        echo "  ❌ 编译失败: $src"
        echo "  查看日志: $BUILD_DIR/$(basename $src).log"
    fi
done

# 创建静态库
if [ ${#OBJECTS[@]} -gt 0 ]; then
    echo ""
    echo "==> 创建静态库..."
    ar rcs "$BUILD_DIR/libhunterclaw_memory.a" "${OBJECTS[@]}"
    echo "  ✅ 成功: $BUILD_DIR/libhunterclaw_memory.a"
    echo ""
    echo "==> 编译统计:"
    echo "  成功: ${#OBJECTS[@]}/${#SOURCES[@]} 文件"
    echo "  库大小: $(du -h $BUILD_DIR/libhunterclaw_memory.a | cut -f1)"
else
    echo ""
    echo "❌ 没有成功编译的文件"
    exit 1
fi

echo ""
echo "✅ 编译完成！"
