# HunterClaw 流式输出回调配置指南

## 📖 概述

HunterClaw 提供了 `StreamCallbacks` 机制来实时监听 AI 执行过程，支持流式推理、助手输出、工具调用等多种事件回调。

## 🎯 核心概念

### StreamCallbacks 结构体

定义在 `include/hunterclaw/react/react_engine.h`:

```cpp
struct StreamCallbacks {
    // 1. 推理流 (reasoning stream)
    std::function<void(const std::string& reasoning)> on_reasoning_stream;
    
    // 2. 助手输出流 (assistant delta)
    std::function<void(const std::string& text)> on_assistant_delta;
    
    // 3. 块回复 (block reply) - 在工具执行前刷新
    std::function<void(const std::string& block)> on_block_reply;
    
    // 4. 工具调用开始
    std::function<void(const std::string& tool_name, const core::JsonValue& args)> on_tool_start;
    
    // 5. 工具调用完成
    std::function<void(const std::string& tool_name, const tools::ToolResult& result)> on_tool_end;
    
    // 6. 步骤完成
    std::function<void(const ReactStep& step)> on_step;
    
    // 7. 生命周期事件
    std::function<void()> on_start;
    std::function<void()> on_end;
    std::function<void(const std::string& error)> on_error;
    
    // 8. Session ID (用于直连推送)
    std::string session_id;
};
```

## 📊 回调类型详解

### 1. on_reasoning_stream

**触发时机**: 当 AI 模型输出推理过程时（如 Claude 的 `<thinking>` 标签内容）

**用途**: 显示 AI 的思考过程

**示例**:
```cpp
callbacks.on_reasoning_stream = [](const std::string& reasoning) {
    std::cout << "[思考] " << reasoning << std::flush;
};
```

### 2. on_assistant_delta

**触发时机**: AI 助手输出文本的每个增量片段

**用途**: 流式显示 AI 回复（类似 ChatGPT 打字效果）

**示例**:
```cpp
callbacks.on_assistant_delta = [](const std::string& text) {
    std::cout << text << std::flush;  // 实时打印
};
```

### 3. on_block_reply 🎯 重点

**触发时机**: 在工具执行前刷新累积的文本块

**用途**: 分段输出完整内容（OpenClaw 核心功能）

**配置控制**:
```cpp
ReactConfig config;
config.block_reply.enabled = true;               // 启用块回复
config.block_reply.min_chars = 100;              // 最小字符数
config.block_reply.max_chars = 1000;             // 最大字符数
config.block_reply.break_mode = ReactConfig::BlockReply::BreakMode::TextEnd;
config.block_reply.break_preference = ReactConfig::BlockReply::BreakPreference::Paragraph;
```

**示例**:
```cpp
callbacks.on_block_reply = [](const std::string& block) {
    std::cout << "\n--- 块回复 ---\n" << block << "\n--- 结束 ---\n";
};
```

### 4. on_tool_start

**触发时机**: AI 决定调用工具时

**用途**: 显示工具调用参数（调试/日志）

**示例**:
```cpp
callbacks.on_tool_start = [](const std::string& tool_name, const core::JsonValue& args) {
    std::cout << "[工具调用] " << tool_name << " 参数: " << args_to_string(args) << "\n";
};
```

### 5. on_tool_end

**触发时机**: 工具执行完成后

**用途**: 显示工具返回结果

**示例**:
```cpp
callbacks.on_tool_end = [](const std::string& tool_name, const tools::ToolResult& result) {
    std::cout << "[工具结果] " << tool_name << ": " << result.output << "\n";
};
```

### 6. on_step

**触发时机**: 每个 ReAct 步骤完成后

**用途**: 记录完整步骤（思考→行动→观察）

**示例**:
```cpp
callbacks.on_step = [](const ReactStep& step) {
    std::cout << "步骤 " << step.step_number << ": "
              << step.tool_calls.size() << " 个工具调用\n";
};
```

### 7. 生命周期回调

```cpp
callbacks.on_start = []() {
    std::cout << "🚀 执行开始\n";
};

callbacks.on_end = []() {
    std::cout << "✅ 执行完成\n";
};

callbacks.on_error = [](const std::string& error) {
    std::cerr << "❌ 错误: " << error << "\n";
};
```

## 🚀 完整使用示例

### 基础示例：实时流式输出

```cpp
#include "hunterclaw/react/react_engine.h"
#include <iostream>

int main() {
    // 1. 创建 ReactEngine
    auto tool_mgr = tools::ToolManager::create();
    auto llm_client = llm::LLMClient::create(llm_config);
    auto engine = ReactEngine::create(tool_mgr, llm_client);
    
    // 2. 配置回调
    StreamCallbacks callbacks;
    
    callbacks.on_assistant_delta = [](const std::string& text) {
        std::cout << text << std::flush;
    };
    
    callbacks.on_block_reply = [](const std::string& block) {
        std::cout << "\n[块输出]\n" << block << "\n";
    };
    
    callbacks.on_tool_start = [](const std::string& name, const core::JsonValue& args) {
        std::cout << "\n[调用工具: " << name << "]\n";
    };
    
    // 3. 执行
    auto result = engine->run("帮我分析这个项目的架构", {}, callbacks);
    
    std::cout << "\n最终输出: " << result.final_output << "\n";
    
    return 0;
}
```

### 高级示例：WebSocket 实时推送

```cpp
#include "hunterclaw/gateway/connection_pool.h"

void handle_agent_request(const std::string& session_id, const std::string& user_input) {
    // 创建回调推送到 WebSocket
    StreamCallbacks callbacks;
    callbacks.session_id = session_id;
    
    // 推理流推送
    callbacks.on_reasoning_stream = [session_id](const std::string& reasoning) {
        gateway::ConnectionPool::instance().send_event(
            session_id,
            "agent.reasoning",
            {{"content", reasoning}}
        );
    };
    
    // 助手输出推送
    callbacks.on_assistant_delta = [session_id](const std::string& text) {
        gateway::ConnectionPool::instance().send_event(
            session_id,
            "agent.assistant_delta",
            {{"text", text}}
        );
    };
    
    // 块回复推送
    callbacks.on_block_reply = [session_id](const std::string& block) {
        gateway::ConnectionPool::instance().send_event(
            session_id,
            "agent.block_reply",
            {{"content", block}}
        );
    };
    
    // 工具调用推送
    callbacks.on_tool_start = [session_id](const std::string& name, const core::JsonValue& args) {
        gateway::ConnectionPool::instance().send_event(
            session_id,
            "agent.tool_start",
            {{"tool_name", name}, {"args", args}}
        );
    };
    
    callbacks.on_tool_end = [session_id](const std::string& name, const tools::ToolResult& result) {
        gateway::ConnectionPool::instance().send_event(
            session_id,
            "agent.tool_end",
            {{"tool_name", name}, {"result", result.output}}
        );
    };
    
    // 执行
    auto engine = ReactEngine::create(tool_mgr, llm_client);
    auto result = engine->run(user_input, {}, callbacks);
}
```

### 日志记录示例

```cpp
#include <spdlog/spdlog.h>
#include <fstream>

void run_with_logging(const std::string& task) {
    auto logger = spdlog::get("agent_execution");
    
    StreamCallbacks callbacks;
    
    // 记录所有事件
    callbacks.on_reasoning_stream = [logger](const std::string& reasoning) {
        logger->debug("[REASONING] {}", reasoning);
    };
    
    callbacks.on_assistant_delta = [logger](const std::string& text) {
        logger->trace("[ASSISTANT] {}", text);
    };
    
    callbacks.on_tool_start = [logger](const std::string& name, const core::JsonValue& args) {
        logger->info("[TOOL_START] {} with args: {}", name, args_to_json(args));
    };
    
    callbacks.on_tool_end = [logger](const std::string& name, const tools::ToolResult& result) {
        logger->info("[TOOL_END] {} -> status: {}", name, result.ok ? "ok" : "error");
    };
    
    callbacks.on_error = [logger](const std::string& error) {
        logger->error("[ERROR] {}", error);
    };
    
    auto engine = ReactEngine::create(tool_mgr, llm_client);
    engine->run(task, {}, callbacks);
}
```

## ⚙️ 配置块回复 (Block Reply)

块回复是 OpenClaw 核心功能，用于在工具调用前刷新累积的输出。

### 配置选项

```cpp
ReactConfig config;

// 1. 启用/禁用块回复
config.block_reply.enabled = true;

// 2. 设置字符数阈值
config.block_reply.min_chars = 100;   // 最小累积字符数
config.block_reply.max_chars = 1000;  // 最大累积字符数

// 3. 分块模式
config.block_reply.break_mode = ReactConfig::BlockReply::BreakMode::TextEnd;     // 文本结束时
// 或
config.block_reply.break_mode = ReactConfig::BlockReply::BreakMode::MessageEnd;  // 消息结束时

// 4. 分块偏好
config.block_reply.break_preference = ReactConfig::BlockReply::BreakPreference::Paragraph;  // 段落边界
// 或
config.block_reply.break_preference = ReactConfig::BlockReply::BreakPreference::Newline;    // 换行边界
// 或
config.block_reply.break_preference = ReactConfig::BlockReply::BreakPreference::Sentence;   // 句子边界
```

### 工作原理

```
AI 输出流 → 累积到缓冲区 → 达到阈值/工具调用前 → 触发 on_block_reply → 刷新缓冲区
```

## 🎨 推理模式配置

控制推理过程的显示方式：

```cpp
ReactConfig config;

// 1. 关闭推理显示
config.reasoning_mode = ReactConfig::ReasoningMode::Off;

// 2. 显示推理（批量）
config.reasoning_mode = ReactConfig::ReasoningMode::On;

// 3. 流式推理（实时）
config.reasoning_mode = ReactConfig::ReasoningMode::Stream;
```

**推荐配置**:
- 开发调试: `ReasoningMode::Stream` + `on_reasoning_stream`
- 生产环境: `ReasoningMode::Off`（提升性能）
- 用户可见: `ReasoningMode::On` + `on_block_reply`

## 📝 最佳实践

### 1. 性能优化

```cpp
// ❌ 错误：每次回调都做重 I/O
callbacks.on_assistant_delta = [](const std::string& text) {
    std::ofstream file("output.txt", std::ios::app);  // 频繁打开文件
    file << text;
};

// ✅ 正确：使用缓冲
std::ostringstream buffer;
callbacks.on_assistant_delta = [&buffer](const std::string& text) {
    buffer << text;  // 内存操作
};
// 最后再刷新到文件
```

### 2. 线程安全

```cpp
// ✅ 使用互斥锁保护共享状态
std::mutex output_mutex;
std::string accumulated_output;

callbacks.on_assistant_delta = [&output_mutex, &accumulated_output](const std::string& text) {
    std::lock_guard<std::mutex> lock(output_mutex);
    accumulated_output += text;
};
```

### 3. 错误处理

```cpp
callbacks.on_error = [](const std::string& error) {
    spdlog::error("Agent execution failed: {}", error);
    
    // 发送通知
    notify_admin(error);
    
    // 回滚状态
    rollback_state();
};
```

### 4. 进度指示

```cpp
size_t total_tokens = 0;

callbacks.on_assistant_delta = [&total_tokens](const std::string& text) {
    total_tokens += text.size();
    std::cout << "\r进度: " << total_tokens << " 字符" << std::flush;
};
```

## 🔗 与 Gateway 集成

HunterClaw Gateway 自动将回调事件转换为 WebSocket 事件：

### Gateway 事件映射

| StreamCallbacks | WebSocket Event | Payload |
|----------------|-----------------|---------|
| `on_reasoning_stream` | `agent.reasoning` | `{content: string}` |
| `on_assistant_delta` | `agent.assistant_delta` | `{text: string}` |
| `on_block_reply` | `agent.block_reply` | `{content: string}` |
| `on_tool_start` | `agent.tool_start` | `{tool_name, args}` |
| `on_tool_end` | `agent.tool_end` | `{tool_name, result}` |
| `on_step` | `agent.step` | `{step_number, ...}` |
| `on_error` | `agent.error` | `{error: string}` |

### 客户端接收示例

```javascript
// JavaScript WebSocket 客户端
ws.on('message', (data) => {
    const event = JSON.parse(data);
    
    switch (event.event) {
        case 'agent.assistant_delta':
            appendToChat(event.payload.text);
            break;
        case 'agent.block_reply':
            displayBlock(event.payload.content);
            break;
        case 'agent.tool_start':
            showToolIndicator(event.payload.tool_name);
            break;
        case 'agent.tool_end':
            hideToolIndicator(event.payload.tool_name);
            break;
    }
});
```

## 📚 参考资料

- **OpenClaw 对齐**: `openclaw-analysis/src/react/message_processor.ts`
- **实现代码**: `HunterClaw/src/react/react_engine.cc`
- **类型定义**: `HunterClaw/include/hunterclaw/react/react_engine.h`
- **Gateway 集成**: `HunterClaw/src/gateway/rpc_handlers.cc`

## 🎯 快速开始

```cpp
// 最小配置
StreamCallbacks callbacks;
callbacks.on_assistant_delta = [](const std::string& text) {
    std::cout << text << std::flush;
};

auto engine = ReactEngine::create(tool_mgr, llm_client);
auto result = engine->run("你的问题", {}, callbacks);
```

就这么简单！🎉
