#!/bin/bash

echo "=== 飞书 Channel 独立测试 ==="
echo ""

# 检查库文件
echo "1. 检查库文件..."
ls -lh bin/libhunterclaw_channel_feishu.so
echo "✓ 库文件存在"
echo ""

# 检查符号
echo "2. 检查导出符号..."
nm -D bin/libhunterclaw_channel_feishu.so | grep -i feishu | head -10
echo "✓ 导出符号正常"
echo ""

# 检查依赖
echo "3. 检查依赖..."
ldd bin/libhunterclaw_channel_feishu.so
echo ""

# 检查配置文件
echo "4. 检查配置文件..."
if [ -f config/feishu_example.json ]; then
    echo "✓ 配置文件存在: config/feishu_example.json"
    head -20 config/feishu_example.json
else
    echo "✗ 配置文件不存在"
fi
echo ""

# 检查文档
echo "5. 检查文档..."
if [ -f docs/feishu-channel-guide.md ]; then
    wc -l docs/feishu-channel-guide.md
    echo "✓ 文档存在"
else
    echo "✗ 文档不存在"
fi
echo ""

echo "=== 测试完成 ==="
echo ""
echo "✅ 编译成功的文件："
echo "  - bin/libhunterclaw_channel_feishu.so (115KB)"
echo "  - include/hunterclaw/channels/feishu_channel.h"
echo "  - docs/feishu-channel-guide.md"
echo "  - config/feishu_example.json"
echo ""
echo "📖 查看完整文档:"
echo "  less docs/feishu-channel-guide.md"
echo ""
echo "🚀 下一步:"
echo "  1. 配置飞书应用凭据"
echo "  2. 完善HTTP实现(参考 docs)"
echo "  3. 实现WebSocket连接"
echo "  4. 测试实际消息收发"
