# HunterClaw Verbose & Reasoning 配置实现

本文档说明 HunterClaw 如何严格对齐 OpenClaw 的 `verboseDefault` 和 `reasoningDefault` 实现。

---

## 📋 目录

1. [实现概览](#实现概览)
2. [核心文件](#核心文件)
3. [使用示例](#使用示例)
4. [配置说明](#配置说明)
5. [与 OpenClaw 的对齐](#与-openclaw-的对齐)

---

## 实现概览

### ✅ 已实现功能

| 功能 | OpenClaw | HunterClaw | 状态 |
|------|----------|------------|------|
| **verboseDefault 配置** | ✅ | ✅ | 完全对齐 |
| **3 级 verbose 模式** | ✅ off/on/full | ✅ Off/On/Full | 完全对齐 |
| **3 层优先级解析** | ✅ | ✅ | 完全对齐 |
| **工具输出过滤** | ✅ | ✅ | 完全对齐 |
| **模型能力检测** | ✅ | ✅ | 完全对齐 |
| **reasoning 自动启用** | ✅ | ✅ | 完全对齐 |
| **推理格式化** | ✅ "Reasoning:\n_..._" | ✅ | 完全对齐 |
| **会话级别覆盖** | ✅ /verbose full | ⚠️ 待集成 | 框架就绪 |

---

## 核心文件

### 1. 配置结构 (`verbose_reasoning_config.h`)

```cpp
#include "hunterclaw/react/verbose_reasoning_config.h"

// Verbose 级别 (对齐 OpenClaw)
enum class VerboseLevel {
    Off,   // 不显示工具信息
    On,    // 只显示工具名称和参数
    Full   // 显示完整输出 ✅
};

// Reasoning 级别 (对齐 OpenClaw)
enum class ReasoningLevel {
    Off,      // 不显示推理
    On,       // 显示推理
    Stream    // 流式推理
};

// Agent 默认配置
struct AgentDefaultsConfig {
    std::optional<VerboseLevel> verbose_default;  // verboseDefault ✅
    bool block_streaming_default = false;
    BlockStreamingBreak block_streaming_break = BlockStreamingBreak::TextEnd;
    bool auto_enable_reasoning_for_capable_models = true;  // 自动启用 ✅
};
```

### 2. 配置解析 (`verbose_reasoning_config.cc`)

#### 3 层优先级解析

```cpp
// OpenClaw 对齐: src/auto-reply/reply/get-reply-directives.ts:356-359
VerboseLevel resolve_verbose_level(
    const MessageDirectives& directives,         // 优先级 1: 内联指令
    const SessionLevelOverrides* session,        // 优先级 2: 会话覆盖
    const AgentDefaultsConfig& agent_defaults    // 优先级 3: 全局配置 ✅
);

// OpenClaw 对齐: src/auto-reply/reply/get-reply-directives.ts:409-419
ReasoningLevel resolve_reasoning_level(
    const MessageDirectives& directives,         // 优先级 1: 内联指令
    const SessionLevelOverrides* session,        // 优先级 2: 会话覆盖
    const AgentDefaultsConfig& agent_defaults,   // 优先级 3: 全局配置
    const ModelCapabilities& model_caps,         // 模型能力检测 ✅
    bool thinking_active                         // thinking 激活状态
);
```

#### 工具输出过滤

```cpp
// OpenClaw 对齐: src/gateway/server-chat.ts:539-551
bool should_include_tool_output(VerboseLevel verbose) {
    return (verbose == VerboseLevel::Full);  // ✅ 只有 Full 才包含输出
}

void filter_tool_event_data(ToolEventData& data, VerboseLevel verbose) {
    if (verbose == VerboseLevel::On) {
        data.result = core::JsonValue();         // 删除 result ❌
        data.partial_result = core::JsonValue(); // 删除 partialResult ❌
    }
    // Full: 保留完整输出 ✅
}
```

### 3. 模型目录 (`model_catalog.h/cc`)

#### 模型能力检测

```cpp
#include "hunterclaw/react/model_catalog.h"

// 检查模型是否支持 reasoning (OpenClaw 对齐)
auto& catalog = ModelCatalog::instance();
bool supports = catalog.supports_reasoning("anthropic", "claude-opus-4");
// ✅ 返回 true (Claude Opus 4 支持推理)
```

#### 内置模型注册

```cpp
// 自动注册常见模型 (对齐 OpenClaw 的模型目录)
ModelCatalog::instance();  // 单例构造时自动注册

// 支持 reasoning 的模型:
// - anthropic/claude-opus-4-20250514       ✅
// - anthropic/claude-sonnet-4-20250514     ✅
// - openai/o1, o1-preview, o1-mini         ✅
// - google/gemini-2.0-flash-thinking-exp   ✅
// - deepseek/deepseek-reasoner             ✅
```

---

## 使用示例

### 示例 1: 解析配置

```cpp
#include "hunterclaw/react/verbose_reasoning_config.h"
#include "hunterclaw/react/model_catalog.h"

// 1. 准备配置
AgentDefaultsConfig agent_defaults;
agent_defaults.verbose_default = VerboseLevel::Full;  // 全局默认 ✅
agent_defaults.auto_enable_reasoning_for_capable_models = true;

// 2. 准备消息指令 (可选)
MessageDirectives directives;
directives.verbose_level = VerboseLevel::On;  // 内联指令 (/verbose on)

// 3. 准备会话覆盖 (可选)
SessionLevelOverrides session_overrides;
session_overrides.verbose_level = VerboseLevel::Full;  // 会话覆盖

// 4. 获取模型能力
auto& catalog = ModelCatalog::instance();
auto model_caps = catalog.get_capabilities("anthropic", "claude-opus-4");

// 5. 解析完整配置
auto resolved = resolve_verbose_reasoning_config(
    directives,          // 优先级 1
    &session_overrides,  // 优先级 2
    agent_defaults,      // 优先级 3
    model_caps,
    false  // thinking_active
);

// 6. 使用解析结果
std::cout << "Verbose level: " << to_string(resolved.verbose_level) << "\n";
std::cout << "Reasoning level: " << to_string(resolved.reasoning_level) << "\n";
// 输出:
// Verbose level: on          (内联指令优先级最高)
// Reasoning level: on        (模型支持 reasoning,自动启用 ✅)
```

### 示例 2: 工具输出过滤

```cpp
// 1. 工具执行完成
ToolEventData tool_data;
tool_data.tool_name = "read_file";
tool_data.args = {{"path", "/path/to/file.py"}};
tool_data.result = {{"content", "def hello():\n    print('Hello')\n"}};
tool_data.phase = "result";

// 2. 根据 verbose 级别过滤
VerboseLevel verbose = VerboseLevel::On;  // 用户设置为 "on"

if (verbose == VerboseLevel::Off) {
    // 完全跳过工具事件 ❌
    return;
}

filter_tool_event_data(tool_data, verbose);

// 3. 输出结果
if (verbose == VerboseLevel::On) {
    // 只显示工具名称和参数 ⚠️
    spdlog::info("🔧 {}: {}", tool_data.tool_name, tool_data.args.dump());
    spdlog::info("✅ 工具执行成功");
    // tool_data.result 已被清空
}
else if (verbose == VerboseLevel::Full) {
    // 显示完整输出 ✅
    spdlog::info("🔧 {}: {}", tool_data.tool_name, tool_data.args.dump());
    spdlog::info("📄 工具输出:\n{}", tool_data.result.dump(2));
}
```

### 示例 3: 推理内容格式化

```cpp
// 1. 获取原始推理内容
std::string raw_thinking = "I need to analyze the file first, then extract key information...";

// 2. 格式化推理 (OpenClaw 格式)
std::string formatted = format_reasoning_text(raw_thinking);
// 结果: "Reasoning:\n_I need to analyze the file first, then extract key information..._"

// 3. 判断推理发送时机
bool before_answer = should_emit_reasoning_before_answer(
    BlockStreamingBreak::MessageEnd,  // message_end 模式
    false  // 不是在消息中添加
);
// before_answer == true  (先发推理,再发答案)

// 4. 发送推理
if (before_answer) {
    send_message(formatted);  // 先发推理 ✅
    send_message(answer);     // 再发答案
} else {
    send_message(answer);     // 先发答案
    send_message(formatted);  // 再发推理
}
```

### 示例 4: 自定义模型注册

```cpp
// 注册自定义模型
ModelCapabilities custom_model;
custom_model.provider = "custom";
custom_model.model_id = "my-reasoning-model";
custom_model.supports_reasoning = true;  // ✅ 支持推理
custom_model.supports_tool_use = true;
custom_model.max_tokens = 8192;
custom_model.context_window = 128000;

ModelCatalog::instance().register_model(custom_model);

// 检查能力
bool supports = ModelCatalog::instance().supports_reasoning("custom", "my-reasoning-model");
// supports == true ✅
```

---

## 配置说明

### YAML 配置示例

```yaml
agents:
  defaults:
    # ✅ Verbose 配置 (OpenClaw 对齐)
    verboseDefault: "full"        # off / on / full
    
    # ✅ Block Streaming 配置
    blockStreamingDefault: true   # 启用实时输出
    blockStreamingBreak: "text_end"  # text_end / message_end
    
    # ✅ 自动启用 reasoning (HunterClaw 扩展)
    autoEnableReasoningForCapableModels: true

# 模型能力配置 (可选,自定义模型)
models:
  - provider: "custom"
    model_id: "my-model"
    supports_reasoning: true
    supports_tool_use: true
    max_tokens: 4096
    context_window: 128000
```

### C++ 配置加载

```cpp
// 1. 加载配置
AgentDefaultsConfig config;
config.verbose_default = parse_verbose_level(
    yaml["agents"]["defaults"]["verboseDefault"].as<std::string>()
);
config.block_streaming_default = 
    yaml["agents"]["defaults"]["blockStreamingDefault"].as<bool>();
config.auto_enable_reasoning_for_capable_models = 
    yaml["agents"]["defaults"]["autoEnableReasoningForCapableModels"].as<bool>();

// 2. 加载自定义模型
ModelCatalog::instance().load_from_config("models.json");
```

---

## 与 OpenClaw 的对齐

### 配置对齐表

| OpenClaw 配置 | HunterClaw 配置 | 对齐状态 |
|--------------|----------------|---------|
| `agents.defaults.verboseDefault` | `AgentDefaultsConfig::verbose_default` | ✅ 完全对齐 |
| `agents.defaults.blockStreamingDefault` | `AgentDefaultsConfig::block_streaming_default` | ✅ 完全对齐 |
| `agents.defaults.blockStreamingBreak` | `AgentDefaultsConfig::block_streaming_break` | ✅ 完全对齐 |
| **无 reasoningDefault 配置** | `auto_enable_reasoning_for_capable_models` | ✅ 对齐逻辑 |

### 代码对齐表

| OpenClaw 文件 | HunterClaw 文件 | 对齐内容 |
|--------------|----------------|---------|
| `src/config/types.agent-defaults.ts:189` | `verbose_reasoning_config.h` | 配置结构 ✅ |
| `src/auto-reply/reply/get-reply-directives.ts:356-359` | `resolve_verbose_level()` | Verbose 解析 ✅ |
| `src/auto-reply/reply/get-reply-directives.ts:409-419` | `resolve_reasoning_level()` | Reasoning 解析 ✅ |
| `src/gateway/server-chat.ts:539-551` | `filter_tool_event_data()` | 工具输出过滤 ✅ |
| `src/agents/model-selection.ts:669-681` | `ModelCatalog::supports_reasoning()` | 模型能力检测 ✅ |
| `src/agents/pi-embedded-utils.ts:278` | `format_reasoning_text()` | 推理格式化 ✅ |

### 行为对齐验证

#### Verbose 行为对齐

| Verbose 级别 | OpenClaw 行为 | HunterClaw 行为 | 对齐 |
|-------------|--------------|----------------|-----|
| `off` | 不显示工具事件 | `verbose == Off` 跳过事件 | ✅ |
| `on` | 只显示名称/参数 | 删除 `result`/`partial_result` | ✅ |
| `full` | 显示完整输出 | 保留所有字段 | ✅ |

#### Reasoning 行为对齐

| 条件 | OpenClaw 行为 | HunterClaw 行为 | 对齐 |
|-----|--------------|----------------|-----|
| 用户设置 `/reasoning on` | 启用推理 | `directives.reasoning_level` 优先 | ✅ |
| 模型支持 reasoning | 自动启用 | `ModelCatalog::supports_reasoning()` | ✅ |
| thinking 已激活 | 不自动启用 | `thinking_active == true` 跳过 | ✅ |
| 格式化 | `"Reasoning:\n_..._"` | `format_reasoning_text()` | ✅ |

---

## 集成步骤

### 1. 添加到构建系统

```cmake
# CMakeLists.txt
add_library(hunterclaw_react
    src/react/verbose_reasoning_config.cc
    src/react/model_catalog.cc
    # ... 其他文件
)

target_include_directories(hunterclaw_react PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(hunterclaw_react
    spdlog::spdlog
    nlohmann_json::nlohmann_json
)
```

### 2. 在 MessageProcessor 中使用

```cpp
#include "hunterclaw/react/verbose_reasoning_config.h"
#include "hunterclaw/react/model_catalog.h"

class MessageProcessor {
public:
    void process_message(const InboundMessage& msg) {
        // 1. 解析配置
        auto resolved = resolve_verbose_reasoning_config(
            parse_directives(msg),
            get_session_overrides(msg.session_key),
            agent_defaults_,
            ModelCatalog::instance().get_capabilities(provider_, model_id_),
            false
        );
        
        // 2. 设置工具回调
        callbacks.on_tool_result = [resolved](const ToolEventData& data) {
            if (resolved.verbose_level == VerboseLevel::Off) return;
            
            auto filtered = data;
            filter_tool_event_data(filtered, resolved.verbose_level);
            
            // 输出工具信息
            log_tool_result(filtered, resolved.verbose_level);
        };
        
        // 3. 设置推理回调
        if (resolved.reasoning_level != ReasoningLevel::Off) {
            callbacks.on_reasoning = [](const std::string& thinking) {
                auto formatted = format_reasoning_text(thinking);
                send_message(formatted);
            };
        }
    }
};
```

### 3. 加载配置

```cpp
// 启动时加载
void initialize() {
    // 加载内置模型 (自动)
    ModelCatalog::instance();
    
    // 加载自定义模型 (可选)
    ModelCatalog::instance().load_from_config("models.json");
    
    // 加载 agent 配置
    agent_defaults_.verbose_default = VerboseLevel::Full;
    agent_defaults_.auto_enable_reasoning_for_capable_models = true;
}
```

---

## 测试

### 单元测试示例

```cpp
#include <gtest/gtest.h>
#include "hunterclaw/react/verbose_reasoning_config.h"

TEST(VerboseReasoningConfig, ThreeLayerPriority) {
    AgentDefaultsConfig defaults;
    defaults.verbose_default = VerboseLevel::Off;
    
    SessionLevelOverrides session;
    session.verbose_level = VerboseLevel::On;
    
    MessageDirectives directives;
    directives.verbose_level = VerboseLevel::Full;
    
    // 内联指令优先级最高
    auto level = resolve_verbose_level(directives, &session, defaults);
    EXPECT_EQ(level, VerboseLevel::Full);
    
    // 会话覆盖次之
    directives.verbose_level = std::nullopt;
    level = resolve_verbose_level(directives, &session, defaults);
    EXPECT_EQ(level, VerboseLevel::On);
    
    // 全局配置最低
    session.verbose_level = std::nullopt;
    level = resolve_verbose_level(directives, &session, defaults);
    EXPECT_EQ(level, VerboseLevel::Off);
}

TEST(ModelCatalog, ReasoningDetection) {
    auto& catalog = ModelCatalog::instance();
    
    // Claude Opus 4 支持 reasoning
    EXPECT_TRUE(catalog.supports_reasoning("anthropic", "claude-opus-4"));
    
    // Claude 3.5 Sonnet 不支持
    EXPECT_FALSE(catalog.supports_reasoning("anthropic", "claude-3-5-sonnet-20241022"));
    
    // o1 系列支持
    EXPECT_TRUE(catalog.supports_reasoning("openai", "o1"));
}
```

---

## 总结

HunterClaw 严格对齐了 OpenClaw 的 `verboseDefault` 和 `reasoningDefault` 实现:

✅ **配置结构**: 完全对齐 OpenClaw 的 `AgentDefaultsConfig`  
✅ **3 层优先级**: 内联指令 > 会话覆盖 > 全局配置  
✅ **3 级 verbose**: Off / On / Full  
✅ **工具输出过滤**: 根据 verbose 级别过滤 result/partialResult  
✅ **模型能力检测**: 自动检测模型是否支持 reasoning  
✅ **reasoning 自动启用**: 支持推理的模型自动启用  
✅ **推理格式化**: "Reasoning:\n_..._" (Markdown 斜体)  

**参考文档**: `/data/workspace/QQMail/HunterClaw/OPENCLAW_VERBOSE_REASONING_CODE_FLOW.md`
