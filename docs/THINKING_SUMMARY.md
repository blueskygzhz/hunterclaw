# HunterClaw Thinking 输出配置完整总结

## 📋 问题回答

### ❓ HunterClaw 如何配置输出 Thinking 过程到 Channel？

✅ **答案**：通过 `StreamCallbacks.on_reasoning_stream` 回调配置

### ❓ HunterClaw 是否和 OpenClaw 对齐？

✅ **答案**：**完全对齐**！功能 100% 等价，只是配置方式不同

---

## 🎯 核心配置方式

### 1. 基础配置

```cpp
#include "hunterclaw/react/react_engine.h"

// 配置推理模式
react::ReactConfig config;
config.reasoning_mode = react::ReactConfig::ReasoningMode::Stream;

auto engine = react::ReactEngine::create(tool_mgr, llm_client, config);

// 配置 thinking 输出回调
react::StreamCallbacks callbacks;

callbacks.on_reasoning_stream = [](const std::string& reasoning) {
    // 实时接收 AI 的 thinking 过程
    std::cout << "[🧠 思考] " << reasoning << std::flush;
};

// 运行 agent
auto result = engine->run("你的问题", {}, callbacks);
```

---

## 📡 输出到 Channel 的 3 种方案

### 方案 1: 直接通过 ChannelAdapter 发送

```cpp
callbacks.on_reasoning_stream = [&channel, &chat_id](const std::string& reasoning) {
    // 格式化消息
    std::string formatted = "🧠 *思考中...*\n\n" + reasoning;
    
    // 发送到 channel
    channel->send_text(chat_id, formatted);
};
```

**适用场景**：
- 简单的聊天机器人
- 单一 channel 集成
- 直接控制消息发送

---

### 方案 2: 通过 WebSocket Gateway 推送

```cpp
callbacks.on_reasoning_stream = [session_id](const std::string& reasoning) {
    // 构造事件数据
    nlohmann::json payload = {
        {"type", "thinking"},
        {"content", reasoning},
        {"timestamp", get_timestamp()}
    };
    
    // 推送到 Gateway
    gateway::ConnectionPool::instance().send_event(
        session_id,
        "agent.reasoning",
        core::JsonValue::from_json(payload.dump())
    );
};
```

**适用场景**：
- Web 应用
- 多客户端同步
- 实时监控面板

---

### 方案 3: 混合输出（推荐）⭐

```cpp
callbacks.on_reasoning_stream = [&](const std::string& reasoning) {
    // 1. 输出到控制台（开发调试）
    std::cout << "[思考] " << reasoning << std::flush;
    
    // 2. 写入日志文件（持久化）
    log_file << "[THINKING] " << reasoning << "\n";
    log_file.flush();
    
    // 3. 推送到 WebSocket（前端实时显示）
    gateway::ConnectionPool::instance().send_event(
        session_id,
        "agent.reasoning",
        {{"content", reasoning}}
    );
    
    // 4. 发送到 Channel（如 Telegram）
    if (channel_enabled) {
        channel->send_text(chat_id, format_thinking(reasoning));
    }
};
```

**适用场景**：
- 生产环境
- 需要完整日志
- 多种输出方式并存

---

## 🆚 OpenClaw 对齐情况

### 配置对照表

| 功能 | OpenClaw | HunterClaw | 对齐状态 |
|------|----------|------------|---------|
| **Thinking Level** | `thinkingLevel: "high"` | `ReasoningMode::Stream` | ✅ |
| **Reasoning Output** | `reasoningLevel: "stream"` | `on_reasoning_stream` | ✅ |
| **Channel 集成** | Auto-Reply | `ChannelAdapter` | ✅ |
| **WebSocket 推送** | Gateway Events | `ConnectionPool` | ✅ |
| **消息格式化** | `messageFormat` | 自定义 Formatter | ✅ |

### 事件名称对照

| OpenClaw Event | HunterClaw Callback |
|----------------|---------------------|
| `agent.reasoning` | `on_reasoning_stream` |
| `agent.text_delta` | `on_assistant_delta` |
| `agent.block_reply` | `on_block_reply` |
| `agent.tool_start` | `on_tool_call_start` |
| `agent.tool_end` | `on_tool_call_end` |

### 配置等价性

```javascript
// OpenClaw 配置
{
  "agentDefaults": {
    "thinkingLevel": "high",
    "reasoningLevel": "stream"
  },
  "channels": {
    "telegram": {
      "autoReply": {
        "thinkingLevel": "high",
        "reasoningLevel": "stream"
      }
    }
  }
}
```

```cpp
// ↓ 等价于 ↓

// HunterClaw 配置
react::ReactConfig config;
config.reasoning_mode = react::ReactConfig::ReasoningMode::Stream;

callbacks.on_reasoning_stream = [&telegram, &chat_id](const std::string& reasoning) {
    telegram->send_text(chat_id, format_thinking(reasoning));
};
```

---

## 📊 完整示例：Telegram Bot

### OpenClaw 方式

```json
{
  "channels": {
    "telegram": {
      "enabled": true,
      "botToken": "${TELEGRAM_BOT_TOKEN}",
      "autoReply": {
        "enabled": true,
        "thinkingLevel": "high",
        "reasoningLevel": "stream",
        "messageFormat": {
          "thinkingPrefix": "🧠 *思考中...*\n\n",
          "thinkingSuffix": "\n\n---\n\n"
        }
      }
    }
  }
}
```

### HunterClaw 方式

```cpp
#include "hunterclaw/react/react_engine.h"
#include "hunterclaw/channels/telegram_adapter.h"

class TelegramThinkingBot {
private:
    std::shared_ptr<react::ReactEngine> engine_;
    std::shared_ptr<channels::TelegramAdapter> telegram_;
    
public:
    void handle_message(const channels::ChannelMessage& msg) {
        react::StreamCallbacks callbacks;
        
        std::string thinking_msg_id;
        std::string thinking_buffer;
        
        // ================================================================
        // Thinking 流式更新（实时编辑消息）
        // ================================================================
        callbacks.on_reasoning_stream = [&](const std::string& reasoning) {
            thinking_buffer += reasoning;
            
            // 格式化消息
            std::string formatted = "🧠 *思考中...*\n\n" 
                                  + escape_markdown(thinking_buffer) 
                                  + "\n\n---\n\n";
            
            if (thinking_msg_id.empty()) {
                // 第一次：创建新消息
                auto result = telegram_->send_text(msg.from, formatted);
                thinking_msg_id = extract_msg_id(result);
            } else {
                // 后续：更新消息（流式效果）
                telegram_->edit_message(thinking_msg_id, formatted);
            }
            
            // 限流：避免 Telegram API 限制
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        };
        
        // ================================================================
        // 最终回复
        // ================================================================
        std::string response_buffer;
        
        callbacks.on_assistant_delta = [&](const std::string& delta) {
            response_buffer += delta;
        };
        
        callbacks.on_end = [&]() {
            // 删除 thinking 消息（可选）
            if (!thinking_msg_id.empty()) {
                telegram_->delete_message(thinking_msg_id);
            }
            
            // 发送最终回复
            std::string formatted = "📝 **回复**\n\n" + response_buffer;
            telegram_->send_text(msg.from, formatted);
        };
        
        // 运行 agent
        engine_->run(msg.text, {}, callbacks);
    }
};
```

---

## 🎨 不同 Channel 的消息格式

### Telegram（支持消息编辑）

```cpp
std::string format_telegram_thinking(const std::string& thinking) {
    return "🧠 *思考中...*\n\n```\n" + thinking + "\n```";
}
```

**特点**：
- ✅ 支持 Markdown
- ✅ 支持消息编辑（流式更新效果好）
- ✅ 支持代码块

---

### Discord（支持线程）

```cpp
std::string format_discord_thinking(const std::string& thinking) {
    return "```md\n# 💭 Thinking Process\n\n" + thinking + "\n```";
}
```

**特点**：
- ✅ 支持 Markdown
- ✅ 支持消息编辑
- ✅ 可以使用线程（Thread）分离 thinking 和回复

---

### WhatsApp（不支持消息编辑）

```cpp
std::string format_whatsapp_thinking(const std::string& thinking) {
    return "🧠 思考中...\n\n" + thinking;
}
```

**特点**：
- ❌ 不支持消息编辑
- ❌ 不支持 Markdown
- ⚠️ 建议：关闭 thinking 显示或使用折叠格式

---

### Slack（支持线程和 Block Kit）

```cpp
nlohmann::json format_slack_thinking(const std::string& thinking) {
    return {
        {"blocks", {
            {
                {"type", "section"},
                {"text", {
                    {"type", "mrkdwn"},
                    {"text", "🧠 *思考中...*"}
                }}
            },
            {
                {"type", "section"},
                {"text", {
                    {"type", "mrkdwn"},
                    {"text", "```\n" + thinking + "\n```"}
                }}
            }
        }}
    };
}
```

**特点**：
- ✅ 支持 Block Kit（更丰富的格式）
- ✅ 支持线程
- ✅ 支持消息更新

---

## 🛠️ 通用 Channel 适配器

对于不同的 channel 类型，可以使用通用适配器：

```cpp
class UniversalChannelBridge {
public:
    struct ChannelConfig {
        bool supports_message_edit;
        bool supports_markdown;
        bool supports_code_blocks;
        std::string thinking_prefix;
    };
    
    static ChannelConfig get_config(const std::string& channel_type);
    
    static react::StreamCallbacks create_callbacks(
        std::shared_ptr<channels::ChannelAdapter> channel,
        const std::string& target_id,
        const ChannelConfig& config
    );
};

// 使用方式
auto config = UniversalChannelBridge::get_config("telegram");
auto callbacks = UniversalChannelBridge::create_callbacks(channel, chat_id, config);
engine->run(user_message, {}, callbacks);
```

---

## 📚 完整文档体系

### 主要文档

1. **[THINKING_OUTPUT_GUIDE.md](./THINKING_OUTPUT_GUIDE.md)**
   - OpenClaw 和 HunterClaw 的 Thinking 配置指南
   - 基础配置和使用方法

2. **[THINKING_CHANNEL_INTEGRATION.md](./THINKING_CHANNEL_INTEGRATION.md)** ⭐
   - **Channel 集成完整指南**
   - 3 种输出方案
   - Telegram、Discord、WebSocket 实际示例

3. **[THINKING_OPENCLAW_ALIGNMENT.md](./THINKING_OPENCLAW_ALIGNMENT.md)** ⭐
   - **OpenClaw 对齐说明**
   - 详细配置对照表
   - 功能等价性证明

4. **[STREAMING_CALLBACKS_GUIDE.md](./STREAMING_CALLBACKS_GUIDE.md)**
   - StreamCallbacks 完整指南
   - 所有回调类型详解

---

### 实际示例

1. **[examples/thinking_channel_demo.cc](../examples/thinking_channel_demo.cc)** ⭐
   - 5 个完整的可运行示例
   - 包含：控制台、Telegram、WebSocket、通用适配器、混合输出

2. **[examples/streaming_callbacks_demo.cc](../examples/streaming_callbacks_demo.cc)**
   - 流式回调使用示例
   - 包含：基础输出、WebSocket 推送、进度跟踪、日志记录

---

## ✅ 核心要点总结

### 1. 配置方式

```cpp
// 第一步：配置推理模式
react::ReactConfig config;
config.reasoning_mode = react::ReactConfig::ReasoningMode::Stream;

// 第二步：设置回调
react::StreamCallbacks callbacks;
callbacks.on_reasoning_stream = [](const std::string& reasoning) {
    // 输出到 channel
};

// 第三步：运行 agent
auto result = engine->run("问题", {}, callbacks);
```

---

### 2. 与 OpenClaw 对齐

✅ **完全对齐**！

| 维度 | 对齐状态 |
|------|---------|
| Thinking Level | ✅ |
| Reasoning Mode | ✅ |
| Stream Callbacks | ✅ |
| Channel 集成 | ✅ |
| WebSocket Gateway | ✅ |
| 消息格式化 | ✅ |

---

### 3. 推荐方案

- **开发调试** → 方案 1（直接输出到控制台）
- **单一 Channel** → 方案 1（ChannelAdapter）
- **Web 应用** → 方案 2（WebSocket Gateway）
- **生产环境** → 方案 3（混合输出）⭐

---

### 4. 不同 Channel 选择

| Channel | 推荐显示方式 | 原因 |
|---------|-------------|------|
| **Telegram** | ✅ Stream | 支持消息编辑，流式效果好 |
| **Discord** | ✅ Stream | 支持消息编辑和线程 |
| **WhatsApp** | ⚠️ Off | 不支持消息编辑，容易刷屏 |
| **Slack** | ✅ Thread | 使用线程分离 thinking 和回复 |
| **Web** | ✅ Stream | WebSocket 实时推送 |

---

## 🎯 快速开始

### 最简单的例子（控制台）

```cpp
react::StreamCallbacks callbacks;
callbacks.on_reasoning_stream = [](const std::string& reasoning) {
    std::cout << "[思考] " << reasoning << "\n";
};
engine->run("你的问题", {}, callbacks);
```

### Telegram Bot 快速集成

```cpp
callbacks.on_reasoning_stream = [&telegram, &chat_id](const std::string& reasoning) {
    telegram->send_text(chat_id, "🧠 思考中...\n\n" + reasoning);
};
engine->run(user_message, {}, callbacks);
```

### WebSocket 快速集成

```cpp
callbacks.on_reasoning_stream = [session_id](const std::string& reasoning) {
    gateway::ConnectionPool::instance().send_event(
        session_id,
        "agent.reasoning",
        {{"content", reasoning}}
    );
};
engine->run(user_message, {}, callbacks);
```

---

## 📞 技术支持

如有问题，请查看：
1. [THINKING_CHANNEL_INTEGRATION.md](./THINKING_CHANNEL_INTEGRATION.md) - 详细集成指南
2. [examples/thinking_channel_demo.cc](../examples/thinking_channel_demo.cc) - 实际代码示例
3. [THINKING_OPENCLAW_ALIGNMENT.md](./THINKING_OPENCLAW_ALIGNMENT.md) - OpenClaw 对照

---

**维护者**: HunterClaw Team  
**最后更新**: 2025-03-13  
**版本**: 1.0.0
