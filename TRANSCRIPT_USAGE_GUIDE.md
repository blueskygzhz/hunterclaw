# HunterClaw Transcript 清理和修复 - 使用指南

本指南展示如何使用 HunterClaw 的 Transcript 清理和修复功能,完全对齐 OpenClaw 的实现。

---

## 📚 核心模块

### 1. TranscriptSanitizer - Transcript 清理器

清理和脱敏 transcript 内容,适配不同 LLM Provider 的要求。

**主要功能:**
- ✅ Tool Call ID 清理 (`strict`, `strict9`)
- ✅ 图片内容脱敏 (`full`, `images-only`)
- ✅ sessions_spawn 附件脱敏
- ✅ 大内容截断
- ✅ Provider 特定策略

### 2. TranscriptRepairer - Transcript 修复器

修复 transcript 中的结构问题,确保符合 Provider 规范。

**主要功能:**
- ✅ Tool Call/Result 配对修复
- ✅ Tool Call 输入验证
- ✅ Google Turn Ordering
- ✅ Anthropic/Gemini 消息验证
- ✅ 合成 tool results

---

## 🚀 快速开始

### 示例 1: 基础清理

```cpp
#include "hunterclaw/sessions/transcript_sanitizer.h"
#include "hunterclaw/llm/llm_client.h"

using namespace hunterclaw::sessions::sanitizer;

// 准备消息历史
std::vector<llm::LLMMessage> messages = load_history();

// 创建清理选项
SanitizeOptions options;
options.sanitize_mode = SanitizeMode::Full;  // 完全移除图片
options.tool_call_id_mode = ToolCallIdMode::Strict;  // 清理 tool call IDs
options.redact_sessions_spawn_attachments = true;  // 脱敏附件

// 执行清理
auto cleaned_messages = TranscriptSanitizer::sanitize(messages, options);

// 使用清理后的消息调用 LLM
auto response = llm_client->call(cleaned_messages);
```

### 示例 2: Provider 特定策略

```cpp
#include "hunterclaw/sessions/transcript_sanitizer.h"

// 根据 Provider 自动解析策略
auto options = TranscriptSanitizer::resolve_policy_for_provider(
    "google",      // provider
    "gemini-pro"   // model_id (可选)
);

// options 将包含:
// - sanitize_mode = Full
// - tool_call_id_mode = Strict
// - apply_google_turn_ordering = true

auto cleaned_messages = TranscriptSanitizer::sanitize(messages, options);
```

### 示例 3: Tool Call/Result 配对修复

```cpp
#include "hunterclaw/sessions/transcript_repairer.h"

using namespace hunterclaw::sessions::repair;

// 修复 Tool Call/Result 配对
auto report = TranscriptRepairer::repair_tool_use_pairing(
    messages,
    true  // allow_synthetic_results
);

// 检查修复报告
std::cout << "Added synthetic results: " << report.added_synthetic_results << std::endl;
std::cout << "Dropped duplicates: " << report.dropped_duplicate_results << std::endl;
std::cout << "Dropped orphans: " << report.dropped_orphan_results << std::endl;

// 使用修复后的消息
auto fixed_messages = report.messages;
```

### 示例 4: Tool Call 输入验证

```cpp
#include "hunterclaw/sessions/transcript_repairer.h"

// 定义工具白名单
std::vector<std::string> tool_whitelist = {
    "read_file",
    "write_to_file",
    "execute_command",
    "search_content",
    "sessions_spawn"
};

// 验证和清理 tool calls
auto report = TranscriptRepairer::repair_tool_call_inputs(
    messages,
    tool_whitelist
);

std::cout << "Dropped invalid tool calls: " << report.dropped_invalid_tool_calls << std::endl;

// sessions_spawn 的 attachments 已自动脱敏
auto validated_messages = report.messages;
```

---

## 🔧 完整集成示例

### 在 ReAct Engine 中集成

```cpp
// react_engine.cc

#include "hunterclaw/sessions/transcript_sanitizer.h"
#include "hunterclaw/sessions/transcript_repairer.h"

std::vector<llm::LLMMessage> ReactEngine::build_prompt_messages(
    const std::string& user_input,
    const std::vector<llm::LLMMessage>& history,
    const std::string& provider
) const {
    std::vector<llm::LLMMessage> messages;
    
    // 1. System prompt
    if (!config_.system_prompt.empty()) {
        llm::LLMMessage system_msg;
        system_msg.role = llm::LLMMessageRole::System;
        system_msg.content = config_.system_prompt;
        messages.push_back(system_msg);
    }
    
    // 2. 清理和修复历史消息
    auto cleaned_history = history;
    
    // 2.1 修复 Tool Call/Result 配对
    auto repair_report = repair::TranscriptRepairer::repair_tool_use_pairing(
        cleaned_history,
        true  // 允许合成 tool results
    );
    cleaned_history = repair_report.messages;
    
    // 2.2 验证 Tool Call 输入
    auto tool_whitelist = get_enabled_tool_names();
    auto validate_report = repair::TranscriptRepairer::repair_tool_call_inputs(
        cleaned_history,
        tool_whitelist
    );
    cleaned_history = validate_report.messages;
    
    // 2.3 应用 Provider 特定清理
    auto sanitize_options = sanitizer::TranscriptSanitizer::resolve_policy_for_provider(provider);
    cleaned_history = sanitizer::TranscriptSanitizer::sanitize(cleaned_history, sanitize_options);
    
    // 2.4 Provider 特定验证
    if (sanitize_options.apply_google_turn_ordering) {
        cleaned_history = repair::TranscriptRepairer::apply_google_turn_ordering(cleaned_history);
    }
    if (sanitize_options.validate_anthropic_turns) {
        cleaned_history = repair::TranscriptRepairer::validate_anthropic_turns(cleaned_history);
    }
    
    // 3. 追加清理后的历史
    messages.insert(messages.end(), cleaned_history.begin(), cleaned_history.end());
    
    // 4. 当前用户输入
    llm::LLMMessage user_msg;
    user_msg.role = llm::LLMMessageRole::User;
    user_msg.content = user_input;
    messages.push_back(user_msg);
    
    return messages;
}
```

---

## 📋 清理策略参考

### Provider 特定策略

| Provider | sanitize_mode | tool_call_id_mode | 特殊规则 |
|----------|---------------|-------------------|---------|
| **OpenAI** | ImagesOnly | None | 保留原始 tool call IDs |
| **Google/Gemini** | Full | Strict | Turn ordering |
| **Anthropic** | Full | Strict | Turn validation |
| **Mistral** | Full | Strict9 | 9 位字母数字 ID |

### 清理模式说明

#### SanitizeMode

```cpp
enum class SanitizeMode {
    Full,         // 完全移除图片内容
                  // 例: "data:image/png;base64,..." → "[Image content removed]"
    
    ImagesOnly    // 仅脱敏 base64 数据
                  // 例: "data:image/png;base64,ABC..." → "data:image/png;base64,__REMOVED__"
};
```

#### ToolCallIdMode

```cpp
enum class ToolCallIdMode {
    None,      // 不清理（OpenAI）
    
    Strict,    // 严格字母数字（Google, Anthropic）
               // 生成格式: [A-Za-z0-9]{16}
               // 例: "call_abc123" → "kF8xN2pQ7vR4mJ9L"
    
    Strict9    // 严格 9 位字母数字（Mistral）
               // 生成格式: [A-Za-z0-9]{9}
               // 例: "call_xyz" → "A1b2C3d4E"
};
```

---

## 🛠️ 修复机制详解

### 1. Tool Call/Result 配对修复

**问题场景:**
```
[assistant] call_1: read_file
[assistant] call_2: write_file
[tool] result for call_1      ← 位置正确
[user] next question
[tool] result for call_2      ← 位置错误（应该在 call_2 后面）
```

**修复后:**
```
[assistant] call_1: read_file
[tool] result for call_1
[assistant] call_2: write_file
[tool] result for call_2      ← 已移动到正确位置
[user] next question
```

**合成 tool result 示例:**
```cpp
// 如果 call_1 没有对应的 result，自动插入：
[assistant] call_1: read_file
[tool] Error: tool result missing (synthetic)  ← 合成的错误结果
[user] next question
```

### 2. 重复和孤立 tool result 清理

**问题场景:**
```
[assistant] call_1: read_file
[tool] result for call_1
[tool] result for call_1      ← 重复
[tool] result for call_999    ← 孤立（没有对应的 call）
```

**修复后:**
```
[assistant] call_1: read_file
[tool] result for call_1      ← 重复和孤立的已删除
```

### 3. Google Turn Ordering

**问题场景:**
```
[user] hello
[assistant] Hi!
[assistant] How are you?      ← 连续 assistant
[user] Good
```

**修复后:**
```
[user] hello
[assistant] How are you?      ← 只保留最后一条 assistant
[user] Good
```

### 4. Anthropic Turn Validation

**问题场景:**
```
[assistant] Hello!            ← 第一条不是 user
[user] Hi
[assistant] Response
```

**修复后:**
```
[user] [Context start]        ← 插入占位 user 消息
[assistant] Hello!
[user] Hi
[assistant] Response
```

---

## 🎯 最佳实践

### 1. 在构建上下文前清理

```cpp
// ❌ 错误: 使用原始历史
auto response = llm_client->call(history);

// ✅ 正确: 先清理再调用
auto cleaned = TranscriptSanitizer::sanitize(history, options);
auto response = llm_client->call(cleaned);
```

### 2. 使用 Provider 特定策略

```cpp
// ❌ 错误: 硬编码策略
SanitizeOptions options;
options.tool_call_id_mode = ToolCallIdMode::Strict;

// ✅ 正确: 根据 Provider 自动解析
auto options = TranscriptSanitizer::resolve_policy_for_provider(provider);
```

### 3. 修复顺序

```cpp
// 推荐的修复顺序:
auto messages = history;

// 1. 修复 Tool Call/Result 配对（最重要）
messages = TranscriptRepairer::repair_tool_use_pairing(messages).messages;

// 2. 验证 Tool Call 输入
messages = TranscriptRepairer::repair_tool_call_inputs(messages, whitelist).messages;

// 3. Provider 特定清理
messages = TranscriptSanitizer::sanitize(messages, options);

// 4. Provider 特定验证
if (is_google) {
    messages = TranscriptRepairer::apply_google_turn_ordering(messages);
}
```

### 4. 检查修复报告

```cpp
auto report = TranscriptRepairer::repair_tool_use_pairing(messages, true);

// 记录修复统计
log()->info("Transcript repair: +{} synthetic, -{} duplicates, -{} orphans",
            report.added_synthetic_results,
            report.dropped_duplicate_results,
            report.dropped_orphan_results);

// 如果有大量修复，可能需要调查原因
if (report.added_synthetic_results > 5) {
    log()->warn("Many synthetic tool results added, check tool execution");
}
```

---

## 📊 性能考虑

### 清理和修复的开销

| 操作 | 时间复杂度 | 空间复杂度 | 性能影响 |
|------|-----------|-----------|---------|
| `sanitize()` | O(n) | O(n) | 低 |
| `repair_tool_use_pairing()` | O(n²) | O(n) | 中等 |
| `repair_tool_call_inputs()` | O(n·m) | O(n) | 低 |
| `apply_google_turn_ordering()` | O(n) | O(n) | 低 |

**优化建议:**
- 只在必要时启用修复（根据 Provider）
- 缓存清理后的历史（避免重复清理）
- 在上下文压缩后清理（减少消息数量）

---

## 🔍 调试和诊断

### 启用详细日志

```cpp
// 设置 transcript logger 级别
spdlog::get("transcript")->set_level(spdlog::level::debug);

// 清理和修复时会输出详细日志：
// [debug] Sanitize tool call ID: call_abc123 -> kF8xN2pQ7vR4mJ9L
// [debug] Found duplicate tool result: call_xyz
// [debug] Inserting synthetic tool result for: call_missing
```

### 导出修复报告

```cpp
auto report = TranscriptRepairer::repair_tool_use_pairing(messages, true);

// 打印修复操作
for (const auto& action : report.actions) {
    std::cout << "Action: " << (int)action.type 
              << ", Index: " << action.index 
              << ", Reason: " << action.reason << std::endl;
}
```

---

## ✅ 对齐 OpenClaw 清单

- ✅ `sanitizeToolCallIds()` → `sanitize_tool_call_ids()`
- ✅ `sanitizeImageContent()` → `sanitize_images()`
- ✅ `redactSessionsSpawnAttachmentsArgs()` → `redact_sessions_spawn_args()`
- ✅ `resolveTranscriptPolicy()` → `resolve_policy_for_provider()`
- ✅ `repairToolUseResultPairing()` → `repair_tool_use_pairing()`
- ✅ `repairToolCallInputs()` → `repair_tool_call_inputs()`
- ✅ `applyGoogleTurnOrdering()` → `apply_google_turn_ordering()`
- ✅ `validateAnthropicTurns()` → `validate_anthropic_turns()`
- ✅ `validateGeminiTurns()` → `validate_gemini_turns()`

**100% API 对齐!** 🎉

---

**完整文档**: 查看 `TRANSCRIPT_ALIGNMENT_REPORT.md` 了解更多技术细节。
