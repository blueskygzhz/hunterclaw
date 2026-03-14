# HunterClaw Thinking 输出配置 - 文档索引

## 📚 文档导航

本目录包含 HunterClaw Thinking（推理过程）输出到 Channel 的完整文档体系。

---

## 🚀 快速开始

### 1️⃣ 如果你想快速了解如何配置
👉 阅读：[**THINKING_SUMMARY.md**](./THINKING_SUMMARY.md) ⭐

**内容**：
- ✅ 核心配置方式（3 分钟上手）
- ✅ 3 种输出方案对比
- ✅ 不同 Channel 的推荐配置
- ✅ 快速集成代码示例

---

### 2️⃣ 如果你想将 Thinking 输出到 Telegram/Discord
👉 阅读：[**THINKING_CHANNEL_INTEGRATION.md**](./THINKING_CHANNEL_INTEGRATION.md) ⭐

**内容**：
- ✅ Channel 集成完整指南
- ✅ Telegram Bot 实现示例
- ✅ Discord Bot 实现示例
- ✅ WebSocket Gateway 集成
- ✅ 通用 Channel 适配器

**配套代码**：
- 👉 [examples/thinking_channel_demo.cc](../examples/thinking_channel_demo.cc)

---

### 3️⃣ 如果你想了解与 OpenClaw 的对齐情况
👉 阅读：[**THINKING_OPENCLAW_ALIGNMENT.md**](./THINKING_OPENCLAW_ALIGNMENT.md) ⭐

**内容**：
- ✅ OpenClaw vs HunterClaw 详细对比
- ✅ 配置映射关系表
- ✅ API 对齐说明
- ✅ 功能等价性证明

---

### 4️⃣ 如果你想了解 OpenClaw 和 HunterClaw 的基础配置
👉 阅读：[**THINKING_OUTPUT_GUIDE.md**](./THINKING_OUTPUT_GUIDE.md)

**内容**：
- ✅ OpenClaw 配置文件详解
- ✅ HunterClaw StreamCallbacks 配置
- ✅ Thinking Level 和 Reasoning Level 说明
- ✅ Channel 特定配置
- ✅ 调试和验证方法

---

### 5️⃣ 如果你想深入了解 StreamCallbacks 机制
👉 阅读：[**STREAMING_CALLBACKS_GUIDE.md**](./STREAMING_CALLBACKS_GUIDE.md)

**内容**：
- ✅ StreamCallbacks 完整说明
- ✅ 9 种回调类型详解
- ✅ 块回复（Block Reply）配置
- ✅ 性能优化建议

**配套代码**：
- 👉 [examples/streaming_callbacks_demo.cc](../examples/streaming_callbacks_demo.cc)

---

## 📖 文档矩阵

### 按目标分类

| 你想做什么？ | 推荐阅读 | 配套代码 |
|-------------|---------|---------|
| **快速上手** | [THINKING_SUMMARY.md](./THINKING_SUMMARY.md) | [thinking_channel_demo.cc](../examples/thinking_channel_demo.cc) |
| **集成 Telegram Bot** | [THINKING_CHANNEL_INTEGRATION.md](./THINKING_CHANNEL_INTEGRATION.md) | [thinking_channel_demo.cc](../examples/thinking_channel_demo.cc) |
| **集成 Discord Bot** | [THINKING_CHANNEL_INTEGRATION.md](./THINKING_CHANNEL_INTEGRATION.md) | [thinking_channel_demo.cc](../examples/thinking_channel_demo.cc) |
| **WebSocket 推送** | [THINKING_CHANNEL_INTEGRATION.md](./THINKING_CHANNEL_INTEGRATION.md) | [thinking_channel_demo.cc](../examples/thinking_channel_demo.cc) |
| **了解 OpenClaw 对齐** | [THINKING_OPENCLAW_ALIGNMENT.md](./THINKING_OPENCLAW_ALIGNMENT.md) | - |
| **配置 Thinking Level** | [THINKING_OUTPUT_GUIDE.md](./THINKING_OUTPUT_GUIDE.md) | - |
| **使用 StreamCallbacks** | [STREAMING_CALLBACKS_GUIDE.md](./STREAMING_CALLBACKS_GUIDE.md) | [streaming_callbacks_demo.cc](../examples/streaming_callbacks_demo.cc) |

---

### 按技术层次分类

#### 🟢 入门级
1. [THINKING_SUMMARY.md](./THINKING_SUMMARY.md) - 快速总结
2. [THINKING_OUTPUT_GUIDE.md](./THINKING_OUTPUT_GUIDE.md) - 基础配置

#### 🟡 进阶级
3. [THINKING_CHANNEL_INTEGRATION.md](./THINKING_CHANNEL_INTEGRATION.md) - Channel 集成
4. [STREAMING_CALLBACKS_GUIDE.md](./STREAMING_CALLBACKS_GUIDE.md) - 回调机制

#### 🔵 专家级
5. [THINKING_OPENCLAW_ALIGNMENT.md](./THINKING_OPENCLAW_ALIGNMENT.md) - 对齐分析

---

## 🎯 核心概念速查

### Thinking（推理）是什么？

AI 在生成最终答案前的思考过程，通常包含在 `<thinking>` 标签中。

**示例**：
```
用户问题：什么是快速排序？

🧠 Thinking:
我需要解释快速排序算法。应该包括：
1. 基本原理（分治法）
2. 时间复杂度分析
3. 代码实现示例
...

📝 回复:
快速排序是一种高效的排序算法...
```

---

### Thinking Level

控制 AI 推理的深度和详细程度。

| Level | 说明 | 适用场景 |
|-------|------|----------|
| `off` | 关闭 | 生产环境、快速响应 |
| `low` | 低级别 | 日常对话 |
| `medium` | 中等 | 复杂任务 |
| `high` | 高级别 | 编程、分析任务 |

---

### Reasoning Mode

控制 Thinking 输出的方式。

| Mode | OpenClaw | HunterClaw | 效果 |
|------|----------|------------|------|
| **Off** | `reasoningLevel: "off"` | 不设置回调 | 不显示 |
| **On** | `reasoningLevel: "on"` | 在 `on_end` 显示 | 块显示 |
| **Stream** | `reasoningLevel: "stream"` | `on_reasoning_stream` | 流式显示 |

---

### StreamCallbacks

HunterClaw 的回调机制，用于接收 AI 的实时输出。

**核心回调**：
- `on_reasoning_stream` - 🧠 Thinking 过程
- `on_assistant_delta` - 📝 文本增量
- `on_block_reply` - 📦 块回复
- `on_tool_call_start` - 🔧 工具开始
- `on_tool_call_end` - ✅ 工具结束

---

## 💼 实际使用案例

### 案例 1: Telegram Bot 显示 Thinking

```cpp
callbacks.on_reasoning_stream = [&telegram, &chat_id](const std::string& reasoning) {
    telegram->send_text(chat_id, "🧠 *思考中...*\n\n" + reasoning);
};
```

👉 详见：[THINKING_CHANNEL_INTEGRATION.md](./THINKING_CHANNEL_INTEGRATION.md#示例-1-telegram-bot-集成)

---

### 案例 2: Web 应用实时推送

```cpp
callbacks.on_reasoning_stream = [session_id](const std::string& reasoning) {
    gateway::ConnectionPool::instance().send_event(
        session_id, "agent.reasoning", {{"content", reasoning}}
    );
};
```

👉 详见：[THINKING_CHANNEL_INTEGRATION.md](./THINKING_CHANNEL_INTEGRATION.md#方案-2-通过-websocket-gateway)

---

### 案例 3: 混合输出（控制台 + 日志 + WebSocket）

```cpp
callbacks.on_reasoning_stream = [&](const std::string& reasoning) {
    std::cout << "[思考] " << reasoning << "\n";        // 控制台
    log_file << reasoning << "\n";                      // 日志
    ws_client.send_event("agent.reasoning", reasoning); // WebSocket
};
```

👉 详见：[THINKING_CHANNEL_INTEGRATION.md](./THINKING_CHANNEL_INTEGRATION.md#方案-3-混合方案推荐)

---

## 🔗 相关文档链接

### HunterClaw 核心文档
- [React Engine 配置](./REACT_ENGINE_CONFIG.md)
- [Channel Adapter 开发指南](./CHANNEL_ADAPTER_GUIDE.md)
- [WebSocket Gateway API](./GATEWAY_API.md)

### OpenClaw 文档
- [OpenClaw 配置文件指南](./OPENCLAW_CONFIG_GUIDE.md)
- [OpenClaw Block Reply 配置](./OPENCLAW_BLOCK_REPLY_CONFIG.md)

---

## ❓ 常见问题

### Q1: HunterClaw 是否支持 Thinking 输出？
**A**: ✅ 完全支持，通过 `StreamCallbacks.on_reasoning_stream` 回调配置。

### Q2: HunterClaw 和 OpenClaw 对齐吗？
**A**: ✅ 完全对齐，功能 100% 等价。详见：[THINKING_OPENCLAW_ALIGNMENT.md](./THINKING_OPENCLAW_ALIGNMENT.md)

### Q3: 如何将 Thinking 输出到 Telegram？
**A**: 使用 `on_reasoning_stream` 回调，调用 Telegram API 发送消息。详见：[THINKING_CHANNEL_INTEGRATION.md](./THINKING_CHANNEL_INTEGRATION.md#示例-1-telegram-bot-集成)

### Q4: 如何配置 Thinking Level？
**A**: 通过 `ReactConfig.reasoning_mode` 配置。详见：[THINKING_OUTPUT_GUIDE.md](./THINKING_OUTPUT_GUIDE.md#hunterclaw-配置)

### Q5: 支持哪些 Channel？
**A**: Telegram、Discord、Slack、WhatsApp、WebSocket、自定义 Channel。详见：[THINKING_CHANNEL_INTEGRATION.md](./THINKING_CHANNEL_INTEGRATION.md#channel-集成方案)

---

## 📞 技术支持

如有问题，请：
1. 查看相应文档
2. 运行配套示例代码
3. 查看 [FAQ](./FAQ.md)
4. 提交 Issue

---

## 📝 贡献指南

欢迎贡献：
- 📖 改进文档
- 💡 添加示例
- 🐛 报告问题
- ✨ 新功能建议

---

**维护者**: HunterClaw Team  
**最后更新**: 2025-03-13  
**版本**: 1.0.0

---

## 📊 文档完整性检查

- [x] 快速总结 - [THINKING_SUMMARY.md](./THINKING_SUMMARY.md)
- [x] Channel 集成指南 - [THINKING_CHANNEL_INTEGRATION.md](./THINKING_CHANNEL_INTEGRATION.md)
- [x] OpenClaw 对齐说明 - [THINKING_OPENCLAW_ALIGNMENT.md](./THINKING_OPENCLAW_ALIGNMENT.md)
- [x] 基础配置指南 - [THINKING_OUTPUT_GUIDE.md](./THINKING_OUTPUT_GUIDE.md)
- [x] 流式回调指南 - [STREAMING_CALLBACKS_GUIDE.md](./STREAMING_CALLBACKS_GUIDE.md)
- [x] Channel 集成示例 - [examples/thinking_channel_demo.cc](../examples/thinking_channel_demo.cc)
- [x] 流式回调示例 - [examples/streaming_callbacks_demo.cc](../examples/streaming_callbacks_demo.cc)

**文档覆盖率**: 100% ✅
