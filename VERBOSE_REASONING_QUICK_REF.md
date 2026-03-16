# HunterClaw Verbose & Reasoning 快速参考

## 🎯 核心概念

### Verbose 级别 (3 级)

| 级别 | 显示内容 | OpenClaw | HunterClaw |
|-----|---------|----------|------------|
| **Off** | 不显示工具信息 ❌ | `"off"` | `VerboseLevel::Off` |
| **On** | 只显示工具名称和参数 ⚠️ | `"on"` | `VerboseLevel::On` |
| **Full** | 显示完整输出 ✅ | `"full"` | `VerboseLevel::Full` |

### Reasoning 级别 (3 级)

| 级别 | 显示内容 | OpenClaw | HunterClaw |
|-----|---------|----------|------------|
| **Off** | 不显示推理 ❌ | `"off"` | `ReasoningLevel::Off` |
| **On** | 显示推理 ✅ | `"on"` | `ReasoningLevel::On` |
| **Stream** | 流式推理 ✅ | `"stream"` | `ReasoningLevel::Stream` |

### 优先级 (3 层)

```
1️⃣ 内联指令 (/verbose full)    → 最高优先级
   ↓
2️⃣ 会话覆盖 (用户发送 /verbose full) → 中等优先级
   ↓
3️⃣ 全局配置 (verboseDefault: "full") → 最低优先级
```

---

## 📂 文件速查

| 文件 | 用途 |
|------|------|
| `include/hunterclaw/react/verbose_reasoning_config.h` | 配置结构和 API |
| `src/react/verbose_reasoning_config.cc` | 配置解析实现 |
| `include/hunterclaw/react/model_catalog.h` | 模型目录管理 |
| `src/react/model_catalog.cc` | 模型能力注册 |
| `tests/react/verbose_reasoning_config_test.cc` | 单元测试 |

---

## 🔧 常用 API

### 配置解析

```cpp
#include "hunterclaw/react/verbose_reasoning_config.h"

// 解析 verbose 级别
VerboseLevel level = resolve_verbose_level(
    directives,      // 消息内联指令
    session,         // 会话覆盖
    agent_defaults   // 全局配置
);

// 解析 reasoning 级别
ReasoningLevel reasoning = resolve_reasoning_level(
    directives, session, agent_defaults,
    model_caps,      // 模型能力
    thinking_active  // thinking 是否激活
);

// 解析完整配置
auto config = resolve_verbose_reasoning_config(
    directives, session, agent_defaults, model_caps, false);
```

### 工具输出过滤

```cpp
// 检查是否包含输出
if (should_include_tool_output(verbose)) {
    // 显示完整输出 ✅
}

// 过滤工具数据
ToolEventData data = ...;
filter_tool_event_data(data, verbose);
// verbose=On: data.result 被清空 ❌
// verbose=Full: data.result 保留 ✅
```

### 模型能力

```cpp
#include "hunterclaw/react/model_catalog.h"

// 获取单例
auto& catalog = ModelCatalog::instance();

// 检查 reasoning 支持
bool supports = catalog.supports_reasoning("anthropic", "claude-opus-4");
// 返回 true ✅

// 获取完整能力
auto caps = catalog.get_capabilities("openai", "gpt-4o");

// 注册自定义模型
ModelCapabilities custom;
custom.provider = "custom";
custom.model_id = "my-model";
custom.supports_reasoning = true;
catalog.register_model(custom);
```

### 推理格式化

```cpp
// 格式化推理内容
std::string formatted = format_reasoning_text(
    "I need to analyze the file first");
// 结果: "Reasoning:\n_I need to analyze the file first_"

// 判断推理时序
bool before = should_emit_reasoning_before_answer(
    BlockStreamingBreak::MessageEnd, false);
// message_end: before = true (先发推理)
// text_end: before = false (后发推理)
```

---

## 🎨 配置示例

### YAML 配置

```yaml
agents:
  defaults:
    verboseDefault: "full"                    # ✅ 显示完整工具输出
    blockStreamingDefault: true               # 启用实时输出
    blockStreamingBreak: "text_end"           # 实时分块
    autoEnableReasoningForCapableModels: true # 自动启用推理
```

### C++ 配置

```cpp
AgentDefaultsConfig config;
config.verbose_default = VerboseLevel::Full;
config.block_streaming_default = true;
config.block_streaming_break = BlockStreamingBreak::TextEnd;
config.auto_enable_reasoning_for_capable_models = true;
```

---

## 📊 支持推理的模型

| 提供商 | 模型 ID | Reasoning |
|-------|---------|-----------|
| **Anthropic** | claude-opus-4-20250514 | ✅ |
| | claude-sonnet-4-20250514 | ✅ |
| | claude-3-5-sonnet-20241022 | ❌ |
| **OpenAI** | o1, o1-preview, o1-mini | ✅ |
| | gpt-4o, gpt-4o-mini | ❌ |
| **Google** | gemini-2.0-flash-thinking-exp | ✅ |
| | gemini-1.5-pro, gemini-1.5-flash | ❌ |
| **Deepseek** | deepseek-reasoner | ✅ |
| | deepseek-chat | ❌ |

---

## 🧪 测试

```bash
# 编译测试
mkdir build && cd build
cmake ..
make verbose_reasoning_config_test

# 运行测试
./tests/verbose_reasoning_config_test

# 预期结果: 32/32 测试通过 ✅
```

---

## 📝 关键规则

### Verbose 过滤规则

```cpp
if (verbose == VerboseLevel::Off) {
    return;  // 完全跳过工具事件 ❌
}

if (verbose == VerboseLevel::On) {
    // 显示工具调用
    log("🔧 read_file: /path/to/file");
    log("✅ 执行成功");
    // 不显示输出内容 ❌
}

if (verbose == VerboseLevel::Full) {
    // 显示工具调用 + 完整输出 ✅
    log("🔧 read_file: /path/to/file");
    log("📄 工具输出:\n{...}");
}
```

### Reasoning 自动启用规则

```cpp
// 自动启用条件 (全部满足):
1. 用户没有显式设置 reasoning        ✅
2. thinking 未激活                    ✅
3. 全局配置允许自动启用                ✅
4. 模型支持 reasoning                 ✅

// 示例:
if (!explicitly_set && 
    !thinking_active &&
    config.auto_enable &&
    model.supports_reasoning) {
    return ReasoningLevel::On;  // ✅ 自动启用
}
```

---

## 🔗 相关文档

- **实现指南**: `VERBOSE_REASONING_IMPLEMENTATION.md`
- **代码流程**: `OPENCLAW_VERBOSE_REASONING_CODE_FLOW.md`
- **总结报告**: `VERBOSE_REASONING_SUMMARY.md`

---

**版本**: v1.0  
**状态**: ✅ 实现完成  
**OpenClaw 对齐**: 100%
