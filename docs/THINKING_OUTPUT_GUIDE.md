# Thinking 过程输出配置指南 🧠

完整指南：如何在 **OpenClaw** 和 **HunterClaw** 中配置和显示 AI 的 thinking（推理/思考）过程。

---

## 📋 目录

1. [OpenClaw 配置](#openclaw-配置)
2. [HunterClaw 配置](#hunterclaw-配置)
3. [Channel 对话中显示 Thinking](#channel-对话中显示-thinking)
4. [实用示例](#实用示例)
5. [最佳实践](#最佳实践)

---

## 🔧 OpenClaw 配置

### **1. 配置文件位置**

```bash
~/.openclaw/openclaw.json
```

### **2. 关键配置项**

#### **A. Thinking Level（思考级别）**

OpenClaw 支持多个 thinking 级别：

| 级别 | 说明 | 适用场景 |
|------|------|----------|
| `off` | 关闭推理输出 | 生产环境、快速响应 |
| `minimal` | 最小推理 | 轻量级任务 |
| `low` | 低级别推理 | 日常对话 |
| `medium` | 中等推理 | 复杂任务 |
| `high` | 高级别推理 | 编程、分析任务 |
| `xhigh` | 超高级别推理（仅特定模型） | GPT-5.x 模型 |
| `adaptive` | 自适应推理 | 根据任务自动调整 |

#### **B. 配置文件示例**

```json
{
  "agentDefaults": {
    "model": "claude-3-5-sonnet-20241022",
    
    // Thinking 级别配置（会话级别）
    "thinkingLevel": "high",
    
    // 推理可见性配置
    "reasoningLevel": "on",
    // 选项:
    // - "off": 不显示推理过程
    // - "on": 显示推理结果（块形式）
    // - "stream": 流式显示推理过程
    
    // 详细输出配置
    "verboseLevel": "on",
    // 选项: "off", "on", "full"
    
    // 系统通知配置
    "noticeLevel": "on"
    // 选项: "off", "on", "full"
  },
  
  // 会话级别配置
  "sessions": {
    "default": {
      "thinkingLevel": "medium"
    },
    "coding": {
      "thinkingLevel": "high"
    }
  }
}
```

---

### **3. 通过命令配置（运行时）**

#### **A. TUI 模式（Terminal UI）**

```bash
# 启动 TUI
openclaw tui

# 在 TUI 中切换 thinking 显示（按键）
# 按 't' 键切换 showThinking

# 使用命令切换
/think high        # 设置为高级别
/think medium      # 设置为中等级别
/think off         # 关闭推理
/think adaptive    # 自适应模式
```

#### **B. 通过 Gateway API**

```bash
# 更新会话的 thinking 级别
curl -X PATCH http://localhost:18789/api/v1/sessions/main \
  -H "Authorization: Bearer $OPENCLAW_GATEWAY_TOKEN" \
  -H "Content-Type: application/json" \
  -d '{
    "thinkingLevel": "high"
  }'
```

#### **C. 通过命令行参数**

```bash
# 使用特定 thinking 级别运行 agent
openclaw agent \
  --to "+1234567890" \
  --message "解释快速排序算法" \
  --think high \
  --deliver
```

---

### **4. Reasoning Level 配置**

控制推理过程在 channel 中的显示方式：

```json
{
  "agentDefaults": {
    "reasoningLevel": "stream"
  }
}
```

| 值 | 效果 | 说明 |
|----|------|------|
| `off` | ❌ 不显示 | 推理过程完全隐藏 |
| `on` | ✅ 块显示 | 推理完成后一次性显示 |
| `stream` | 🌊 流式显示 | 实时流式输出推理过程 |

---

### **5. Channel 特定配置**

不同频道可以有不同的 thinking 配置：

```json
{
  "channels": {
    "whatsapp": {
      "autoReply": {
        "thinkingLevel": "medium",
        "reasoningLevel": "on"
      }
    },
    "telegram": {
      "autoReply": {
        "thinkingLevel": "high",
        "reasoningLevel": "stream"
      }
    },
    "discord": {
      "autoReply": {
        "thinkingLevel": "low",
        "reasoningLevel": "off"
      }
    }
  }
}
```

---

## 🛠️ HunterClaw 配置

### **1. 配置文件示例**

```cpp
#include <hunterclaw/react/react_engine.h>

using namespace hunterclaw;

// 创建 ReactConfig
react::ReactConfig config;

// 配置推理模式
config.reasoning_mode = react::ReactConfig::ReasoningMode::Stream;
// 选项:
// - Off: 关闭推理输出
// - On: 显示推理结果
// - Stream: 流式显示推理过程

// 创建 engine
auto engine = react::ReactEngine::create(tool_mgr, llm_client, config);
```

---

### **2. StreamCallbacks 配置**

#### **A. 基础配置**

```cpp
react::StreamCallbacks callbacks;

// 配置推理流回调（thinking 过程）
callbacks.on_reasoning_stream = [](const std::string& reasoning) {
    std::cout << "[思考中] " << reasoning << std::flush;
};

// 运行 agent
auto result = engine->run("你的问题", {}, callbacks);
```

#### **B. 完整示例（带格式化）**

```cpp
#include <iostream>
#include <iomanip>
#include <chrono>

react::StreamCallbacks callbacks;

// 1. 推理流（thinking 标签内容）
callbacks.on_reasoning_stream = [](const std::string& reasoning) {
    // 带时间戳的输出
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    
    std::cout << "\n🧠 [思考] " 
              << std::put_time(std::localtime(&time), "%H:%M:%S")
              << "\n" << reasoning << std::flush;
};

// 2. 文本增量（普通响应内容）
callbacks.on_text_delta = [](const std::string& delta) {
    std::cout << delta << std::flush;
};

// 3. 块回复（完整文本块）
callbacks.on_block_reply = [](const std::string& block) {
    std::cout << "\n📝 [回复块]\n" << block << "\n";
};

// 4. 工具调用
callbacks.on_tool_call_start = [](const std::string& tool_name, 
                                    const std::string& args) {
    std::cout << "\n🔧 [工具调用] " << tool_name << "\n";
    std::cout << "参数: " << args << "\n";
};

// 运行
auto result = engine->run("解释快速排序", {}, callbacks);
```

---

### **3. WebSocket/Gateway 集成**

如果需要将 thinking 过程发送到 channel（如 Telegram、Discord）：

```cpp
#include <nlohmann/json.hpp>
#include <websocketpp/client.hpp>

react::StreamCallbacks callbacks;

// WebSocket 客户端（示例）
auto ws_client = /* 初始化 WebSocket */;

callbacks.on_reasoning_stream = [&ws_client](const std::string& reasoning) {
    // 构造消息
    nlohmann::json msg = {
        {"type", "thinking"},
        {"content", reasoning},
        {"timestamp", std::time(nullptr)}
    };
    
    // 发送到 channel
    ws_client.send(msg.dump());
    
    // 同时本地输出
    std::cout << "[思考] " << reasoning << std::flush;
};

callbacks.on_text_delta = [&ws_client](const std::string& delta) {
    nlohmann::json msg = {
        {"type", "text"},
        {"content", delta}
    };
    ws_client.send(msg.dump());
};
```

---

## 💬 Channel 对话中显示 Thinking

### **方法 1: 使用 OpenClaw 的 Auto-Reply**

```json
{
  "channels": {
    "telegram": {
      "enabled": true,
      "autoReply": {
        "enabled": true,
        
        // 关键配置 ⭐
        "thinkingLevel": "high",
        "reasoningLevel": "stream",
        
        // 显示推理过程
        "verboseLevel": "on",
        
        // 打字指示器配置
        "typing": {
          "mode": "thinking",
          "minDurationMs": 500
        }
      }
    }
  }
}
```

**效果**：
```
[Bot 正在输入...]

🧠 思考中...
我需要分析快速排序的时间复杂度...
首先考虑最坏情况...

📝 回复：
快速排序是一种高效的排序算法...
```

---

### **方法 2: 自定义消息格式**

#### **A. 配置消息模板**

```json
{
  "agentDefaults": {
    "messageFormat": {
      "thinkingPrefix": "🧠 思考中:\n",
      "thinkingSuffix": "\n\n---\n\n",
      "includeThinking": true
    }
  }
}
```

#### **B. 在代码中格式化**

```typescript
// OpenClaw 插件示例
export async function formatThinkingForChannel(
  thinking: string,
  channel: string
): Promise<string> {
  switch (channel) {
    case 'telegram':
      return `<i>🧠 思考中...</i>\n\n${thinking}`;
    
    case 'discord':
      return `\`\`\`\n💭 Thinking\n${thinking}\n\`\`\``;
    
    case 'whatsapp':
      return `_🧠 思考中..._\n\n${thinking}`;
    
    default:
      return thinking;
  }
}
```

---

### **方法 3: 使用多条消息**

有些 channel 可以发送多条消息来显示 thinking 过程：

```typescript
// 伪代码示例
async function sendThinkingToChannel(
  channelId: string,
  thinking: string,
  response: string
) {
  // 1. 发送 thinking 消息
  await channel.send({
    to: channelId,
    text: `🧠 *思考过程*\n\n${thinking}`,
    parseMode: 'Markdown'
  });
  
  // 2. 短暂延迟（模拟思考时间）
  await sleep(500);
  
  // 3. 发送最终回复
  await channel.send({
    to: channelId,
    text: `📝 *回复*\n\n${response}`,
    parseMode: 'Markdown'
  });
}
```

---

### **方法 4: 使用 Spoiler/Collapsible（Telegram）**

Telegram 支持折叠文本：

```typescript
function formatThinkingWithSpoiler(
  thinking: string,
  response: string
): string {
  return `||🧠 思考过程:\n${thinking}||\n\n📝 ${response}`;
}
```

**效果**：用户需要点击才能看到 thinking 内容。

---

## 📊 实用示例

### **示例 1: Telegram Bot 显示 Thinking**

```json
// ~/.openclaw/openclaw.json
{
  "channels": {
    "telegram": {
      "enabled": true,
      "botToken": "${TELEGRAM_BOT_TOKEN}",
      
      "autoReply": {
        "enabled": true,
        "thinkingLevel": "high",
        "reasoningLevel": "stream",
        
        // 消息格式化
        "messageFormat": {
          "prefix": "",
          "suffix": "\n\n_Powered by OpenClaw_",
          "thinkingPrefix": "🧠 *思考中...*\n\n",
          "thinkingSuffix": "\n\n---\n\n"
        }
      }
    }
  }
}
```

---

### **示例 2: Discord Bot 显示 Thinking（代码块）**

```json
{
  "channels": {
    "discord": {
      "enabled": true,
      "botToken": "${DISCORD_BOT_TOKEN}",
      
      "autoReply": {
        "enabled": true,
        "thinkingLevel": "medium",
        "reasoningLevel": "on",
        
        "messageFormat": {
          "thinkingPrefix": "```md\n# 💭 Thinking Process\n",
          "thinkingSuffix": "\n```\n\n"
        }
      }
    }
  }
}
```

---

### **示例 3: HunterClaw 集成 WebSocket**

```cpp
#include <hunterclaw/react/react_engine.h>
#include <websocketpp/client.hpp>
#include <nlohmann/json.hpp>

class ChannelBridge {
private:
    websocketpp::client<websocketpp::config::asio_client> ws_client_;
    websocketpp::connection_hdl connection_;
    
public:
    void send_thinking(const std::string& thinking) {
        nlohmann::json msg = {
            {"type", "thinking"},
            {"content", "🧠 思考中...\n\n" + thinking},
            {"format", "markdown"}
        };
        
        ws_client_.send(connection_, msg.dump(), 
                       websocketpp::frame::opcode::text);
    }
    
    void send_response(const std::string& response) {
        nlohmann::json msg = {
            {"type", "response"},
            {"content", "📝 回复\n\n" + response},
            {"format", "markdown"}
        };
        
        ws_client_.send(connection_, msg.dump(), 
                       websocketpp::frame::opcode::text);
    }
};

// 使用示例
int main() {
    ChannelBridge bridge;
    bridge.connect("ws://your-channel-gateway:8080");
    
    react::StreamCallbacks callbacks;
    
    callbacks.on_reasoning_stream = [&bridge](const std::string& reasoning) {
        bridge.send_thinking(reasoning);
    };
    
    callbacks.on_block_reply = [&bridge](const std::string& block) {
        bridge.send_response(block);
    };
    
    // 运行 agent
    auto result = engine->run("你的问题", {}, callbacks);
    
    return 0;
}
```

---

## 🎯 最佳实践

### **1. 根据场景选择 Thinking Level**

```json
{
  "sessions": {
    // 快速对话
    "casual": {
      "thinkingLevel": "minimal",
      "reasoningLevel": "off"
    },
    
    // 编程任务
    "coding": {
      "thinkingLevel": "high",
      "reasoningLevel": "stream"
    },
    
    // 数据分析
    "analysis": {
      "thinkingLevel": "xhigh",
      "reasoningLevel": "on"
    }
  }
}
```

---

### **2. Channel 适配**

不同 channel 对 thinking 的显示方式不同：

| Channel | 推荐方式 | 说明 |
|---------|----------|------|
| **Telegram** | `reasoningLevel: "stream"` + Markdown | 支持实时更新消息 |
| **Discord** | `reasoningLevel: "on"` + Code Block | 使用代码块显示 thinking |
| **WhatsApp** | `reasoningLevel: "off"` | 不显示（避免刷屏） |
| **Slack** | `reasoningLevel: "on"` + Thread | 使用线程显示 |

---

### **3. 性能优化**

```json
{
  "agentDefaults": {
    // 生产环境：关闭 thinking（提升速度）
    "thinkingLevel": "off",
    "reasoningLevel": "off",
    
    // 开发/调试：开启 thinking
    // "thinkingLevel": "high",
    // "reasoningLevel": "stream"
  }
}
```

---

### **4. 用户控制**

允许用户控制 thinking 显示：

```typescript
// 处理用户命令
function handleUserCommand(command: string, sessionId: string) {
  switch (command) {
    case '/think on':
      updateSession(sessionId, { thinkingLevel: 'high', reasoningLevel: 'stream' });
      return '✅ 已开启思考过程显示';
    
    case '/think off':
      updateSession(sessionId, { thinkingLevel: 'off', reasoningLevel: 'off' });
      return '✅ 已关闭思考过程显示';
    
    case '/think minimal':
      updateSession(sessionId, { thinkingLevel: 'minimal', reasoningLevel: 'on' });
      return '✅ 已切换到最小思考模式';
  }
}
```

---

## 🔍 调试和验证

### **1. 查看当前配置**

```bash
# OpenClaw
openclaw config get agentDefaults.thinkingLevel
openclaw config get agentDefaults.reasoningLevel

# 查看会话配置
curl http://localhost:18789/api/v1/sessions/main \
  -H "Authorization: Bearer $OPENCLAW_GATEWAY_TOKEN" | jq '.thinkingLevel'
```

### **2. 测试 Thinking 输出**

```bash
# TUI 模式测试
openclaw tui
# 按 't' 切换 showThinking
# 输入: 解释快速排序算法

# Agent 模式测试
openclaw agent \
  --to "+1234567890" \
  --message "解释快速排序" \
  --think high \
  --verbose
```

### **3. 查看日志**

```bash
# 查看 thinking 相关日志
tail -f ~/.openclaw/logs/openclaw.log | grep -i "thinking\|reasoning"
```

---

## 📚 相关文档

### 核心文档
- [THINKING_CHANNEL_INTEGRATION.md](./THINKING_CHANNEL_INTEGRATION.md) - ⭐ **Thinking 输出到 Channel 完整指南**
- [THINKING_OPENCLAW_ALIGNMENT.md](./THINKING_OPENCLAW_ALIGNMENT.md) - ⭐ **与 OpenClaw 对齐说明**
- [STREAMING_CALLBACKS_GUIDE.md](./STREAMING_CALLBACKS_GUIDE.md) - HunterClaw 流式回调完整指南

### OpenClaw 相关
- [OPENCLAW_BLOCK_REPLY_CONFIG.md](./OPENCLAW_BLOCK_REPLY_CONFIG.md) - OpenClaw Block Reply 配置
- [OPENCLAW_CONFIG_GUIDE.md](./OPENCLAW_CONFIG_GUIDE.md) - OpenClaw 配置文件指南

### 实际示例
- [examples/thinking_channel_demo.cc](../examples/thinking_channel_demo.cc) - ⭐ **Channel 集成实际示例**
- [examples/streaming_callbacks_demo.cc](../examples/streaming_callbacks_demo.cc) - 流式回调示例

---

**最后更新**: 2025-03-13  
**维护者**: HunterClaw Team
