# StreamCallbacks 快速参考

## 基础配置

```cpp
#include "hunterclaw/react/react_engine.h"

react::StreamCallbacks callbacks;

// 1️⃣ 实时输出 (最常用)
callbacks.on_assistant_delta = [](const std::string& text) {
    std::cout << text << std::flush;
};

// 2️⃣ 块回复 (OpenClaw 核心功能)
callbacks.on_block_reply = [](const std::string& block) {
    std::cout << "\n[块输出]\n" << block << "\n";
};

// 3️⃣ 工具调用
callbacks.on_tool_start = [](const std::string& name, const JsonValue& args) {
    std::cout << "[工具] " << name << "\n";
};

// 使用
auto engine = ReactEngine::create(tool_mgr, llm_client);
auto result = engine->run("你的问题", {}, callbacks);
```

## 所有回调类型

| 回调 | 触发时机 | 用途 |
|------|---------|------|
| `on_reasoning_stream` | AI 推理过程 | 显示思考过程 |
| `on_assistant_delta` | AI 输出增量 | 流式打字效果 |
| **`on_block_reply`** | **工具调用前** | **分段输出** ⭐ |
| `on_tool_start` | 工具开始执行 | 显示工具名称 |
| `on_tool_end` | 工具执行完成 | 显示工具结果 |
| `on_step` | 步骤完成 | 记录完整步骤 |
| `on_start` | 执行开始 | 初始化 |
| `on_end` | 执行完成 | 清理/统计 |
| `on_error` | 发生错误 | 错误处理 |

## 块回复配置 (Block Reply)

```cpp
ReactConfig config;

// 启用
config.block_reply.enabled = true;

// 阈值
config.block_reply.min_chars = 100;   // 最小字符数
config.block_reply.max_chars = 1000;  // 最大字符数

// 模式
config.block_reply.break_mode = ReactConfig::BlockReply::BreakMode::TextEnd;

// 偏好
config.block_reply.break_preference = 
    ReactConfig::BlockReply::BreakPreference::Paragraph;

auto engine = ReactEngine::create(tool_mgr, llm_client, config);
```

## WebSocket 推送

```cpp
callbacks.on_block_reply = [session_id](const std::string& block) {
    gateway::ConnectionPool::instance().send_event(
        session_id,
        "agent.block_reply",
        {{"content", block}}
    );
};
```

## 进度跟踪

```cpp
size_t char_count = 0;

callbacks.on_assistant_delta = [&char_count](const std::string& text) {
    char_count += text.size();
    std::cout << "\r进度: " << char_count << " 字符" << std::flush;
};
```

## 完整示例

```cpp
#include "hunterclaw/react/react_engine.h"

int main() {
    auto tool_mgr = tools::ToolManager::create();
    auto llm_client = llm::LLMClient::create(llm_config);
    
    // 配置块回复
    ReactConfig config;
    config.block_reply.enabled = true;
    config.block_reply.min_chars = 100;
    config.block_reply.max_chars = 500;
    
    auto engine = ReactEngine::create(tool_mgr, llm_client, config);
    
    // 设置回调
    StreamCallbacks callbacks;
    callbacks.on_assistant_delta = [](const std::string& text) {
        std::cout << text << std::flush;
    };
    callbacks.on_block_reply = [](const std::string& block) {
        std::cout << "\n--- 块 ---\n" << block << "\n";
    };
    callbacks.on_tool_start = [](const std::string& name, const JsonValue& args) {
        std::cout << "\n[工具: " << name << "]\n";
    };
    
    // 执行
    auto result = engine->run("帮我分析项目", {}, callbacks);
    
    return 0;
}
```

## 关键点

1. ✅ **on_block_reply** 是 OpenClaw 核心功能，用于工具调用前刷新输出
2. ✅ 通过 `ReactConfig.block_reply` 配置行为
3. ✅ 所有回调都是可选的，按需配置
4. ✅ 回调在 ReactEngine 内部线程中执行，注意线程安全
5. ✅ `session_id` 字段用于 WebSocket 直连推送

## 更多信息

- 完整文档: `docs/STREAMING_CALLBACKS_GUIDE.md`
- 示例代码: `examples/streaming_callbacks_demo.cc`
- 类型定义: `include/hunterclaw/react/react_engine.h`
