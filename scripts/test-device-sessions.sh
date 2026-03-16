#!/bin/bash

# ============================================================================
# HunterClaw Device Session 测试脚本
# 验证基于 Device ID 的会话匹配和禁用自动重置
# ============================================================================

set -e

GATEWAY_URL="${GATEWAY_URL:-http://localhost:8080}"
DEVICE_A="test-device-A-$(date +%s)"
DEVICE_B="test-device-B-$(date +%s)"

echo "=========================================="
echo "HunterClaw Device Session 测试"
echo "=========================================="
echo ""
echo "Gateway URL: $GATEWAY_URL"
echo "Device A ID: $DEVICE_A"
echo "Device B ID: $DEVICE_B"
echo ""

# 测试 1: 不同设备独立会话
echo "========== 测试 1: 不同设备独立会话 =========="

echo "1.1 Device A - 第一条消息"
curl -s -X POST "$GATEWAY_URL/v1/chat/completions" \
  -H "Content-Type: application/json" \
  -H "X-Device-ID: $DEVICE_A" \
  -d '{
    "model": "claude-sonnet-4.5",
    "messages": [{"role": "user", "content": "我的名字是 Alice"}]
  }' | jq -r '.choices[0].message.content'

echo ""
echo "1.2 Device B - 第一条消息 (不同设备)"
curl -s -X POST "$GATEWAY_URL/v1/chat/completions" \
  -H "Content-Type: application/json" \
  -H "X-Device-ID: $DEVICE_B" \
  -d '{
    "model": "claude-sonnet-4.5",
    "messages": [{"role": "user", "content": "我的名字是 Bob"}]
  }' | jq -r '.choices[0].message.content'

echo ""
echo "1.3 Device A - 第二条消息 (验证记忆)"
curl -s -X POST "$GATEWAY_URL/v1/chat/completions" \
  -H "Content-Type: application/json" \
  -H "X-Device-ID: $DEVICE_A" \
  -d '{
    "model": "claude-sonnet-4.5",
    "messages": [{"role": "user", "content": "我叫什么名字？"}]
  }' | jq -r '.choices[0].message.content'

echo ""
echo "✅ 测试 1 完成: 不同设备应该有独立会话"
echo ""

# 测试 2: 同一设备会话复用
echo "========== 测试 2: 同一设备会话复用 =========="

echo "2.1 Device A - 第三条消息"
curl -s -X POST "$GATEWAY_URL/v1/chat/completions" \
  -H "Content-Type: application/json" \
  -H "X-Device-ID: $DEVICE_A" \
  -d '{
    "model": "claude-sonnet-4.5",
    "messages": [{"role": "user", "content": "我最喜欢的颜色是蓝色"}]
  }' | jq -r '.choices[0].message.content'

echo ""
echo "2.2 Device A - 验证上下文连续性"
curl -s -X POST "$GATEWAY_URL/v1/chat/completions" \
  -H "Content-Type: application/json" \
  -H "X-Device-ID: $DEVICE_A" \
  -d '{
    "model": "claude-sonnet-4.5",
    "messages": [{"role": "user", "content": "总结一下我告诉你的关于我的信息"}]
  }' | jq -r '.choices[0].message.content'

echo ""
echo "✅ 测试 2 完成: 同一设备应该复用会话并保持上下文"
echo ""

# 测试 3: 会话持久化 (模拟跨天访问)
echo "========== 测试 3: 会话持久化 (禁用自动重置) =========="

echo "3.1 等待 5 秒后再次访问 Device A..."
sleep 5

echo "3.2 Device A - 验证会话仍然存在"
curl -s -X POST "$GATEWAY_URL/v1/chat/completions" \
  -H "Content-Type: application/json" \
  -H "X-Device-ID: $DEVICE_A" \
  -d '{
    "model": "claude-sonnet-4.5",
    "messages": [{"role": "user", "content": "再次确认，我的名字和喜欢的颜色是什么？"}]
  }' | jq -r '.choices[0].message.content'

echo ""
echo "✅ 测试 3 完成: 会话应该在空闲后仍然保持"
echo ""

# 测试 4: 列出会话
echo "========== 测试 4: 列出会话 =========="

echo "4.1 列出所有 session"
curl -s "$GATEWAY_URL/api/v1/sessions" | jq '.'

echo ""
echo "✅ 测试 4 完成"
echo ""

# 测试 5: Fallback Device ID (不带 X-Device-ID header)
echo "========== 测试 5: Fallback Device ID =========="

echo "5.1 不带 X-Device-ID header 的请求"
curl -s -X POST "$GATEWAY_URL/v1/chat/completions" \
  -H "Content-Type: application/json" \
  -H "User-Agent: TestBot/1.0" \
  -d '{
    "model": "claude-sonnet-4.5",
    "messages": [{"role": "user", "content": "测试 fallback device ID"}]
  }' | jq -r '.choices[0].message.content'

echo ""
echo "✅ 测试 5 完成: 应该使用计算的 fallback device ID"
echo ""

echo "=========================================="
echo "所有测试完成！"
echo "=========================================="
echo ""
echo "验证要点:"
echo "  ✓ Device A 应该记住名字是 Alice"
echo "  ✓ Device B 应该记住名字是 Bob"
echo "  ✓ Device A 应该记住喜欢的颜色是蓝色"
echo "  ✓ 空闲 5 秒后会话仍然保持"
echo "  ✓ 不带 Device ID 时自动计算 fallback ID"
