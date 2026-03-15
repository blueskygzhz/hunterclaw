#!/bin/bash
# 自动修复 C++17 optional 头文件缺失问题

cd /data/workspace/QQMail/HunterClaw

echo "🔍 查找需要 optional 的文件..."
FILES=$(find src/memory include/hunterclaw/memory -type f \( -name "*.h" -o -name "*.cc" \) -exec grep -l "std::optional\|std::nullopt" {} \;)

echo "📝 处理 $(echo "$FILES" | wc -l) 个文件..."

for file in $FILES; do
  # 检查是否已经包含 optional
  if ! grep -q "#include <optional>" "$file"; then
    echo "  ✅ 添加 <optional> 到: $file"
    
    # 找到第一个 #include 行的位置
    if grep -q "^#include" "$file"; then
      # 在第一个 #include 之后插入
      sed -i '0,/^#include/s||#include <optional>\n&|' "$file"
    elif grep -q "^#pragma once" "$file"; then
      # 如果有 #pragma once，在其后插入
      sed -i '/^#pragma once/a #include <optional>' "$file"
    else
      # 否则在文件开头插入
      sed -i '1i#include <optional>' "$file"
    fi
  else
    echo "  ⏭️  跳过（已有 optional）: $file"
  fi
done

echo ""
echo "✨ 修复完成！开始编译..."
echo ""
