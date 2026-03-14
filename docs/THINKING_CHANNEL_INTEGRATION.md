# HunterClaw Thinking 输出到 Channel 完整指南 🧠

**最后更新**: 2025-03-13  
**状态**: ✅ 已对齐 OpenClaw

---

## 📋 目录

1. [概述](#概述)
2. [OpenClaw 对齐情况](#openclaw-对齐情况)
3. [HunterClaw 配置](#hunterclaw-配置)
4. [Channel 集成方案](#channel-集成方案)
5. [完整实现示例](#完整实现示例)
6. [OpenClaw vs HunterClaw 对比](#openclaw-vs-hunterclaw-对比)
7. [调试和验证](#调试和验证)

---

## 🎯 概述

**Thinking 输出**是 AI 推理过程的可视化，显示 AI 在生成最终答案前的思考步骤。

### 核心概念

```
用户输入 → [🧠 Thinking 过程] → [🔧 工具调用] → [📝 最终回复]
              ↓
         实时输出到 Channel
```

### 关键组件

1. **Thinking Level** (思考深度)
   - `off`, `minimal`, `low`, `medium`, `high`, `xhigh`, `adaptive`

2. **Reasoning Level** (输出方式)
   - `off`: 不显示
   - `on`: 块显示（完成后一次性输出）
   - `stream`: 流式显示（实时输出）

3. **Channel Adapter** (渠道适配器)
   - 负责将 thinking 输出发送到不同的聊天平台

---

## ✅ OpenClaw 对齐情况

### 对齐状态表

| 功能 | OpenClaw | HunterClaw | 状态 |
|------|----------|------------|------|
| **Thinking Level** | `thinkingLevel: high` | `react::ReactConfig` | ✅ 对齐 |
| **Reasoning Mode** | `reasoningLevel: stream` | `ReasoningMode::Stream` | ✅ 对齐 |
| **Stream Callbacks** | Gateway Events | `StreamCallbacks` | ✅ 对齐 |
| **Channel 推送** | Auto-Reply | `ChannelAdapter` | ✅ 对齐 |
| **WebSocket 集成** | Gateway WS | `ConnectionPool` | ✅ 对齐 |
| **消息格式化** | `messageFormat` | 自定义 Formatter | ✅ 对齐 |

### OpenClaw 核心配置

```json
{
  "agentDefaults": {
    "thinkingLevel": "high",
    "reasoningLevel": "stream",
    "verboseLevel": "on"
  },
  
  "channels": {
    "telegram": {
      "autoReply": {
        "thinkingLevel": "high",
        "reasoningLevel": "stream",
        "messageFormat": {
          "thinkingPrefix": "🧠 思考中...\n\n",
          "thinkingSuffix": "\n\n---\n\n"
        }
      }
    }
  }
}
```

---

## 🛠️ HunterClaw 配置

### 1. ReactEngine 配置

```cpp
#include "hunterclaw/react/react_engine.h"

react::ReactConfig config;

// ============================================================================
// Thinking 配置
// ============================================================================

// 推理模式: Off, On, Stream
config.reasoning_mode = react::ReactConfig::ReasoningMode::Stream;

// 块回复配置（用于分段输出）
config.block_reply.enabled = true;
config.block_reply.min_chars = 100;
config.block_reply.max_chars = 500;
config.block_reply.break_mode = react::ReactConfig::BlockReply::BreakMode::TextEnd;
config.block_reply.break_preference = react::ReactConfig::BlockReply::BreakPreference::Paragraph;

auto engine = react::ReactEngine::create(tool_mgr, llm_client, config);
```

### 2. StreamCallbacks 配置

```cpp
react::StreamCallbacks callbacks;

// ============================================================================
// 1. Thinking 流 (推理过程)
// ============================================================================
callbacks.on_reasoning_stream = [](const std::string& reasoning) {
    // 实时输出 AI 的思考过程
    std::cout << "[🧠 思考] " << reasoning << std::flush;
};

// ============================================================================
// 2. 文本增量 (最终回复)
// ============================================================================
callbacks.on_assistant_delta = [](const std::string& delta) {
    // 实时输出 AI 的回复内容
    std::cout << delta << std::flush;
};

// ============================================================================
// 3. 块回复 (在工具调用前刷新)
// ============================================================================
callbacks.on_block_reply = [](const std::string& block) {
    // 输出完整的文本块
    std::cout << "\n[📝 回复块]\n" << block << "\n";
};

// ============================================================================
// 4. 工具调用
// ============================================================================
callbacks.on_tool_call_start = [](const std::string& name, const std::string& args) {
    std::cout << "\n[🔧 工具] " << name << "\n";
};

callbacks.on_tool_call_end = [](const std::string& name, const tools::ToolResult& result) {
    std::cout << "[✅ 完成] " << name << "\n";
};

// 运行 agent
auto result = engine->run("你的问题", {}, callbacks);
```

---

## 📡 Channel 集成方案

### 方案 1: 通过 ChannelAdapter 直接发送

```cpp
#include "hunterclaw/channels/channel.h"
#include "hunterclaw/channels/telegram_adapter.h"

class ThinkingChannelBridge {
private:
    std::shared_ptr<channels::ChannelAdapter> channel_;
    std::string target_chat_id_;
    std::string current_thinking_msg_id_;  // 用于更新消息
    
public:
    ThinkingChannelBridge(
        std::shared_ptr<channels::ChannelAdapter> channel,
        const std::string& chat_id
    ) : channel_(channel), target_chat_id_(chat_id) {}
    
    // ========================================================================
    // 发送 Thinking 过程
    // ========================================================================
    void send_thinking(const std::string& thinking) {
        std::string formatted = format_thinking(thinking);
        
        if (current_thinking_msg_id_.empty()) {
            // 第一次发送：创建新消息
            auto result = channel_->send_text(target_chat_id_, formatted);
            // 保存消息 ID 用于后续更新
            current_thinking_msg_id_ = extract_message_id(result);
        } else {
            // 更新已有消息（流式显示效果）
            update_message(current_thinking_msg_id_, formatted);
        }
    }
    
    // ========================================================================
    // 发送最终回复
    // ========================================================================
    void send_response(const std::string& response) {
        // 清空 thinking 消息（思考完成）
        current_thinking_msg_id_.clear();
        
        std::string formatted = "📝 **回复**\n\n" + response;
        channel_->send_text(target_chat_id_, formatted);
    }
    
    // ========================================================================
    // 格式化 Thinking 内容（根据 channel 类型）
    // ========================================================================
    std::string format_thinking(const std::string& thinking) {
        // Telegram 格式
        return "🧠 *思考中...*\n\n" + escape_markdown(thinking);
    }
    
private:
    std::string escape_markdown(const std::string& text) {
        // 转义 Markdown 特殊字符
        std::string result = text;
        // 实现转义逻辑...
        return result;
    }
    
    std::string extract_message_id(const core::Result<bool>& result) {
        // 从发送结果中提取消息 ID
        return "msg_123456";
    }
    
    void update_message(const std::string& msg_id, const std::string& text) {
        // 更新已有消息（如果 channel 支持）
    }
};
```

### 方案 2: 通过 WebSocket Gateway

```cpp
#include "hunterclaw/gateway/connection_pool.h"
#include <nlohmann/json.hpp>

class WebSocketThinkingBridge {
private:
    std::string session_id_;
    
public:
    WebSocketThinkingBridge(const std::string& session_id) 
        : session_id_(session_id) {}
    
    // ========================================================================
    // 配置 StreamCallbacks
    // ========================================================================
    react::StreamCallbacks create_callbacks() {
        react::StreamCallbacks callbacks;
        
        // 1. Thinking 流
        callbacks.on_reasoning_stream = [this](const std::string& reasoning) {
            send_event("agent.reasoning", {
                {"type", "thinking"},
                {"content", reasoning},
                {"format", "markdown"}
            });
        };
        
        // 2. 文本增量
        callbacks.on_assistant_delta = [this](const std::string& delta) {
            send_event("agent.text_delta", {
                {"text", delta}
            });
        };
        
        // 3. 块回复
        callbacks.on_block_reply = [this](const std::string& block) {
            send_event("agent.block_reply", {
                {"content", block},
                {"timestamp", get_timestamp()}
            });
        };
        
        // 4. 工具调用
        callbacks.on_tool_call_start = [this](const std::string& name, const std::string& args) {
            send_event("agent.tool_start", {
                {"tool_name", name},
                {"args", nlohmann::json::parse(args)}
            });
        };
        
        callbacks.on_tool_call_end = [this](const std::string& name, const tools::ToolResult& result) {
            send_event("agent.tool_end", {
                {"tool_name", name},
                {"success", result.ok},
                {"output", result.output}
            });
        };
        
        return callbacks;
    }
    
private:
    void send_event(const std::string& event_type, const nlohmann::json& payload) {
        gateway::ConnectionPool::instance().send_event(
            session_id_,
            event_type,
            core::JsonValue::from_json(payload.dump())
        );
    }
    
    int64_t get_timestamp() {
        return std::chrono::system_clock::now().time_since_epoch().count();
    }
};
```

### 方案 3: 混合方案（推荐）⭐

将 thinking 输出到本地日志，同时通过 Gateway 推送到前端：

```cpp
class HybridThinkingOutput {
private:
    std::ofstream log_file_;
    WebSocketThinkingBridge ws_bridge_;
    
public:
    HybridThinkingOutput(
        const std::string& log_path,
        const std::string& session_id
    ) : ws_bridge_(session_id) {
        log_file_.open(log_path, std::ios::app);
    }
    
    react::StreamCallbacks create_callbacks() {
        react::StreamCallbacks callbacks;
        
        callbacks.on_reasoning_stream = [this](const std::string& reasoning) {
            // 1. 输出到控制台（开发调试）
            std::cout << "[🧠 思考] " << reasoning << std::flush;
            
            // 2. 写入日志文件（持久化）
            log_file_ << "[THINKING] " << reasoning << "\n";
            log_file_.flush();
            
            // 3. 推送到前端（实时显示）
            ws_bridge_.send_event("agent.reasoning", {
                {"type", "thinking"},
                {"content", reasoning}
            });
        };
        
        callbacks.on_assistant_delta = [this](const std::string& delta) {
            std::cout << delta << std::flush;
            log_file_ << delta;
            
            ws_bridge_.send_event("agent.text_delta", {
                {"text", delta}
            });
        };
        
        // ... 其他回调
        
        return callbacks;
    }
};
```

---

## 💼 完整实现示例

### 示例 1: Telegram Bot 集成

```cpp
#include "hunterclaw/react/react_engine.h"
#include "hunterclaw/channels/telegram_adapter.h"

class TelegramThinkingBot {
private:
    std::shared_ptr<react::ReactEngine> engine_;
    std::shared_ptr<channels::TelegramAdapter> telegram_;
    
public:
    void handle_message(const channels::ChannelMessage& msg) {
        // 配置 thinking 输出
        react::StreamCallbacks callbacks;
        
        std::string thinking_msg_id;
        std::string thinking_buffer;
        
        // ====================================================================
        // Thinking 流式更新
        // ====================================================================
        callbacks.on_reasoning_stream = [&](const std::string& reasoning) {
            thinking_buffer += reasoning;
            
            // 构造 Telegram 格式
            std::string formatted = "🧠 *思考中...*\n\n```\n" 
                                  + thinking_buffer 
                                  + "\n```";
            
            if (thinking_msg_id.empty()) {
                // 创建新消息
                auto result = telegram_->send_text(msg.from, formatted);
                thinking_msg_id = extract_msg_id(result);
            } else {
                // 更新消息（流式效果）
                telegram_->edit_message(thinking_msg_id, formatted);
            }
            
            // 限制更新频率（避免 API 限流）
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        };
        
        // ====================================================================
        // 最终回复
        // ====================================================================
        std::string response_buffer;
        
        callbacks.on_assistant_delta = [&](const std::string& delta) {
            response_buffer += delta;
        };
        
        callbacks.on_end = [&]() {
            // 删除 thinking 消息
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
    
private:
    std::string extract_msg_id(const core::Result<bool>& result) {
        // 提取消息 ID
        return "msg_id";
    }
};
```

### 示例 2: Discord Bot 集成

```cpp
class DiscordThinkingBot {
private:
    std::shared_ptr<react::ReactEngine> engine_;
    std::shared_ptr<channels::DiscordAdapter> discord_;
    
public:
    void handle_message(const channels::ChannelMessage& msg) {
        react::StreamCallbacks callbacks;
        
        // ====================================================================
        // Discord 使用代码块显示 thinking
        // ====================================================================
        callbacks.on_reasoning_stream = [&](const std::string& reasoning) {
            std::string formatted = "```md\n# 💭 Thinking Process\n\n" 
                                  + reasoning 
                                  + "\n```";
            
            discord_->send_text(msg.from, formatted);
        };
        
        // ====================================================================
        // 使用 Embed 显示最终回复
        // ====================================================================
        callbacks.on_block_reply = [&](const std::string& block) {
            // 构造 Discord Embed
            nlohmann::json embed = {
                {"title", "📝 回复"},
                {"description", block},
                {"color", 0x5865F2},  // Discord Blurple
                {"timestamp", get_iso_timestamp()}
            };
            
            discord_->send_embed(msg.from, embed);
        };
        
        engine_->run(msg.text, {}, callbacks);
    }
};
```

### 示例 3: 通用 Channel 适配器

```cpp
class UniversalThinkingBridge {
public:
    struct ChannelConfig {
        bool supports_message_edit;      // 是否支持消息编辑
        bool supports_threading;         // 是否支持线程/回复
        bool supports_markdown;          // 是否支持 Markdown
        std::string thinking_prefix;
        std::string response_prefix;
    };
    
    static ChannelConfig get_config(const std::string& channel_type) {
        if (channel_type == "telegram") {
            return {
                .supports_message_edit = true,
                .supports_threading = false,
                .supports_markdown = true,
                .thinking_prefix = "🧠 *思考中...*\n\n",
                .response_prefix = "📝 **回复**\n\n"
            };
        } else if (channel_type == "discord") {
            return {
                .supports_message_edit = true,
                .supports_threading = true,
                .supports_markdown = true,
                .thinking_prefix = "```md\n# 💭 Thinking\n",
                .response_prefix = "```\n\n📝 **Response**\n\n"
            };
        } else if (channel_type == "whatsapp") {
            return {
                .supports_message_edit = false,
                .supports_threading = false,
                .supports_markdown = false,
                .thinking_prefix = "🧠 思考中...\n\n",
                .response_prefix = "📝 回复\n\n"
            };
        }
        // 默认配置
        return {
            .supports_message_edit = false,
            .supports_threading = false,
            .supports_markdown = false,
            .thinking_prefix = "[思考] ",
            .response_prefix = "[回复] "
        };
    }
    
    static react::StreamCallbacks create_callbacks(
        std::shared_ptr<channels::ChannelAdapter> channel,
        const std::string& target_id,
        const ChannelConfig& config
    ) {
        react::StreamCallbacks callbacks;
        
        std::string thinking_msg_id;
        std::string thinking_buffer;
        
        callbacks.on_reasoning_stream = [=, &thinking_msg_id, &thinking_buffer]
            (const std::string& reasoning) mutable {
            
            thinking_buffer += reasoning;
            std::string formatted = config.thinking_prefix + thinking_buffer;
            
            if (config.supports_message_edit) {
                // 支持编辑：更新消息
                if (thinking_msg_id.empty()) {
                    auto result = channel->send_text(target_id, formatted);
                    // thinking_msg_id = extract_id(result);
                } else {
                    // channel->edit_message(thinking_msg_id, formatted);
                }
            } else {
                // 不支持编辑：发送新消息
                channel->send_text(target_id, formatted);
            }
        };
        
        callbacks.on_block_reply = [=](const std::string& block) {
            std::string formatted = config.response_prefix + block;
            channel->send_text(target_id, formatted);
        };
        
        return callbacks;
    }
};
```

---

## 📊 OpenClaw vs HunterClaw 对比

### 配置对照表

| 功能 | OpenClaw 配置 | HunterClaw 代码 |
|------|--------------|----------------|
| **Thinking Level** | `thinkingLevel: "high"` | `config.reasoning_mode = Stream` |
| **Reasoning Output** | `reasoningLevel: "stream"` | `callbacks.on_reasoning_stream` |
| **Block Streaming** | `blockStreamingDefault: on` | `config.block_reply.enabled = true` |
| **Min Chars** | `blockStreamingChunk.minChars: 100` | `config.block_reply.min_chars = 100` |
| **Max Chars** | `blockStreamingChunk.maxChars: 500` | `config.block_reply.max_chars = 500` |
| **Break Mode** | `blockStreamingBreak: text_end` | `BreakMode::TextEnd` |
| **Message Format** | `messageFormat.thinkingPrefix` | 在 callback 中自定义 |

### 回调事件对照

| OpenClaw Gateway Event | HunterClaw Callback |
|------------------------|---------------------|
| `agent.reasoning` | `on_reasoning_stream` |
| `agent.text_delta` | `on_assistant_delta` |
| `agent.block_reply` | `on_block_reply` |
| `agent.tool_start` | `on_tool_call_start` |
| `agent.tool_end` | `on_tool_call_end` |
| `agent.start` | `on_start` |
| `agent.end` | `on_end` |
| `agent.error` | `on_error` |

### Channel 支持对照

| Channel | OpenClaw | HunterClaw | Thinking 支持 |
|---------|----------|------------|--------------|
| **Telegram** | ✅ 内置 | ✅ Adapter | ✅ 消息编辑 |
| **Discord** | ✅ 内置 | ✅ Adapter | ✅ 代码块 |
| **WhatsApp** | ✅ 内置 | 🚧 计划中 | ⚠️ 无编辑 |
| **Slack** | ✅ 内置 | 🚧 计划中 | ✅ Thread |
| **自定义** | ✅ Plugin | ✅ Adapter | ✅ 自定义 |

---

## 🔍 调试和验证

### 1. 控制台测试

```cpp
// 最简单的测试：直接输出到控制台
react::StreamCallbacks callbacks;

callbacks.on_reasoning_stream = [](const std::string& reasoning) {
    std::cout << "\n=== THINKING ===\n" << reasoning << "\n================\n";
};

callbacks.on_assistant_delta = [](const std::string& delta) {
    std::cout << delta << std::flush;
};

auto result = engine->run("测试问题", {}, callbacks);
```

### 2. 日志验证

```cpp
std::ofstream debug_log("thinking_debug.log");

callbacks.on_reasoning_stream = [&debug_log](const std::string& reasoning) {
    debug_log << "[THINKING] " << reasoning << "\n";
    debug_log.flush();
};
```

### 3. WebSocket 验证

使用 `websocat` 工具监听 Gateway 事件：

```bash
# 连接到 Gateway WebSocket
websocat ws://localhost:18789/ws

# 发送认证
{"type": "auth", "token": "your_token"}

# 监听 thinking 事件
# 应该会看到:
# {"event": "agent.reasoning", "data": {...}}
```

### 4. Telegram Bot 测试

```cpp
// 测试 Telegram 集成
TelegramThinkingBot bot;

// 模拟收到消息
channels::ChannelMessage test_msg;
test_msg.from = "123456789";  // Your Telegram user ID
test_msg.text = "解释快速排序算法";

bot.handle_message(test_msg);

// 预期在 Telegram 中看到:
// 1. 🧠 思考中...（实时更新）
// 2. 📝 回复（最终答案）
```

---

## 🎯 最佳实践

### 1. 限流保护

```cpp
class RateLimitedThinkingOutput {
private:
    std::chrono::steady_clock::time_point last_update_;
    const std::chrono::milliseconds min_interval_{500};  // 最小 500ms 更新间隔
    
public:
    void on_reasoning_stream(const std::string& reasoning) {
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - last_update_
        );
        
        if (elapsed >= min_interval_) {
            // 发送更新
            send_to_channel(reasoning);
            last_update_ = now;
        }
    }
};
```

### 2. 错误处理

```cpp
callbacks.on_reasoning_stream = [&](const std::string& reasoning) {
    try {
        channel->send_text(target_id, format_thinking(reasoning));
    } catch (const std::exception& e) {
        // 发送失败，记录日志但不中断 agent 执行
        log_error("Failed to send thinking: " + std::string(e.what()));
    }
};
```

### 3. 缓冲优化

```cpp
class BufferedThinkingOutput {
private:
    std::string buffer_;
    size_t last_sent_size_ = 0;
    const size_t min_delta_ = 50;  // 最小增量 50 字符
    
public:
    void on_reasoning_stream(const std::string& reasoning) {
        buffer_ += reasoning;
        
        // 只有累积足够多内容才发送
        if (buffer_.size() - last_sent_size_ >= min_delta_) {
            channel->send_text(target_id, format_thinking(buffer_));
            last_sent_size_ = buffer_.size();
        }
    }
};
```

---

## 📚 相关文档

- [THINKING_OUTPUT_GUIDE.md](./THINKING_OUTPUT_GUIDE.md) - Thinking 输出配置指南
- [STREAMING_CALLBACKS_GUIDE.md](./STREAMING_CALLBACKS_GUIDE.md) - 流式回调完整指南
- [CHANNEL_ADAPTER_GUIDE.md](./CHANNEL_ADAPTER_GUIDE.md) - Channel 适配器开发指南
- [ALIGNMENT_GUIDE.md](./ALIGNMENT_GUIDE.md) - HunterClaw 对齐指南

---

## ✅ 总结

### HunterClaw 对齐 OpenClaw 的 Thinking 功能

✅ **完全对齐**！HunterClaw 通过以下机制实现：

1. **ReactConfig** 配置 thinking level 和 reasoning mode
2. **StreamCallbacks** 提供 `on_reasoning_stream` 回调
3. **ChannelAdapter** 支持自定义 channel 集成
4. **WebSocket Gateway** 提供实时事件推送

### 配置等价性

```cpp
// OpenClaw
thinkingLevel: "high"
reasoningLevel: "stream"

// ↓ 等价于 ↓

// HunterClaw
config.reasoning_mode = react::ReactConfig::ReasoningMode::Stream;
callbacks.on_reasoning_stream = [](const std::string& reasoning) {
    // 输出到 channel
};
```

---

**维护者**: HunterClaw Team  
**最后更新**: 2025-03-13
