#pragma once
// ============================================================================
// verbose_reasoning_config.h
// ✅ 对齐 OpenClaw: src/config/types.agent-defaults.ts
//                  src/auto-reply/reply/get-reply-directives.ts
// ============================================================================

#include "hunterclaw/core/types.h"
#include "hunterclaw/react/model_catalog.h"   // ✅ ModelCatalogEntry
#include <string>
#include <optional>
#include <vector>

namespace hunterclaw {
namespace react {

// ── VerboseLevel ──────────────────────────────────────────────────────────────
// ✅ 对齐 OpenClaw: type VerboseLevel = "off" | "on" | "full"
enum class VerboseLevel {
    Off,   // "off"
    On,    // "on"
    Full   // "full"
};

// ── ReasoningLevel ────────────────────────────────────────────────────────────
// ✅ 对齐 OpenClaw: type ReasoningLevel = "off" | "on" | "stream"
enum class ReasoningLevel {
    Off,      // "off"
    On,       // "on"
    Stream    // "stream"
};

// ── BlockStreamingBreak ───────────────────────────────────────────────────────
// ✅ 对齐 OpenClaw: type BlockStreamingBreak = "text_end" | "message_end"
enum class BlockStreamingBreak {
    TextEnd,     // "text_end"
    MessageEnd   // "message_end"
};

// ── AgentDefaultsConfig ───────────────────────────────────────────────────────
// ✅ 对齐 OpenClaw: AgentDefaultsConfig (types.agent-defaults.ts)
struct AgentDefaultsConfig {
    // ✅ 对应 OpenClaw: verboseDefault?: "off" | "on" | "full"
    std::optional<VerboseLevel> verbose_default;

    // ✅ 对应 OpenClaw: blockStreamingDefault?: "on" | "off"
    bool block_streaming_default = false;

    // ✅ 对应 OpenClaw: blockStreamingBreak?: "text_end" | "message_end"
    BlockStreamingBreak block_streaming_break = BlockStreamingBreak::TextEnd;

    // ⚠️ HunterClaw 扩展: auto-enable reasoning when model supports it
    // OpenClaw 用 resolveReasoningDefault(provider, model, catalog) 实现
    bool auto_enable_reasoning_for_capable_models = true;
};

// ── SessionLevelOverrides ─────────────────────────────────────────────────────
// ✅ 对齐 OpenClaw: SessionEntry (sessions/level-overrides.ts)
struct SessionLevelOverrides {
    std::optional<VerboseLevel> verbose_level;
    std::optional<ReasoningLevel> reasoning_level;
};

// ── MessageDirectives ─────────────────────────────────────────────────────────
// ✅ 对齐 OpenClaw: ReplyDirectives (auto-reply/reply/get-reply-directives.ts)
struct MessageDirectives {
    std::optional<VerboseLevel> verbose_level;
    std::optional<ReasoningLevel> reasoning_level;
};

// ── ResolvedVerboseReasoningConfig ────────────────────────────────────────────
struct ResolvedVerboseReasoningConfig {
    VerboseLevel verbose_level = VerboseLevel::Off;
    ReasoningLevel reasoning_level = ReasoningLevel::Off;
    BlockStreamingBreak streaming_break = BlockStreamingBreak::TextEnd;
    bool block_streaming_enabled = false;
};

// ── ToolEventData ─────────────────────────────────────────────────────────────
// ✅ 对齐 OpenClaw: tool event data filtering (server-chat.ts:539-551)
struct ToolEventData {
    std::string tool_call_id;
    std::string tool_name;
    core::JsonValue args;
    core::JsonValue result;
    core::JsonValue partial_result;
    bool is_error = false;
    std::string phase;  // "start", "update", "result"
};

// ── 配置解析函数 ───────────────────────────────────────────────────────────────

// ✅ 对齐 OpenClaw: resolvedVerboseLevel = directives.verboseLevel ?? sessionEntry?.verboseLevel ?? agentCfg?.verboseDefault
VerboseLevel resolve_verbose_level(
    const MessageDirectives& directives,
    const SessionLevelOverrides* session_overrides,
    const AgentDefaultsConfig& agent_defaults
);

// ✅ 对齐 OpenClaw: resolveReasoningDefault + auto-enable logic (get-reply-directives.ts:409-419)
// 使用 ModelCatalogEntry（对齐 OpenClaw ModelCatalogEntry）
ReasoningLevel resolve_reasoning_level(
    const MessageDirectives& directives,
    const SessionLevelOverrides* session_overrides,
    const AgentDefaultsConfig& agent_defaults,
    const ModelCatalogEntry& model_entry,   // ✅ 改为 ModelCatalogEntry
    bool thinking_active
);

// ✅ 对齐 OpenClaw: resolveReasoningDefault — entry?.reasoning === true ? "on" : "off"
bool model_supports_reasoning(const ModelCatalogEntry& entry);   // ✅ 改为 ModelCatalogEntry

ResolvedVerboseReasoningConfig resolve_verbose_reasoning_config(
    const MessageDirectives& directives,
    const SessionLevelOverrides* session_overrides,
    const AgentDefaultsConfig& agent_defaults,
    const ModelCatalogEntry& model_entry,   // ✅ 改为 ModelCatalogEntry
    bool thinking_active = false
);

// ── 工具输出过滤 ───────────────────────────────────────────────────────────────

bool should_include_tool_output(VerboseLevel verbose);
void filter_tool_event_data(ToolEventData& data, VerboseLevel verbose);

// ── 推理内容格式化 ─────────────────────────────────────────────────────────────

std::string format_reasoning_text(const std::string& raw_thinking);
bool should_emit_reasoning_before_answer(BlockStreamingBreak break_mode, bool added_during_message);

// ── 字符串转换 ────────────────────────────────────────────────────────────────

std::string to_string(VerboseLevel level);
std::string to_string(ReasoningLevel level);
std::string to_string(BlockStreamingBreak mode);

std::optional<VerboseLevel> parse_verbose_level(const std::string& str);
std::optional<ReasoningLevel> parse_reasoning_level(const std::string& str);
std::optional<BlockStreamingBreak> parse_block_streaming_break(const std::string& str);

// ============================================================================
// xhigh / binary thinking support (对齐 OpenClaw thinking.ts:41-81)
// ============================================================================

// 对齐 thinking.ts:41-59: isBinaryThinkingProvider
// 检测 provider 是否只支持 on/off 两级 thinking (无 low/medium/high)
bool is_binary_thinking_provider(
    const std::string& provider,
    const std::string& model = ""
);

// 对齐 thinking.ts:61-81: supportsXHighThinking
// 检测 provider+model 是否支持 xhigh thinking level
bool supports_xhigh_thinking(
    const std::string& provider,
    const std::string& model = ""
);

// 对齐 thinking.ts:83-89: listThinkingLevels
// 返回可用的 thinking levels (考虑 xhigh 支持)
std::vector<std::string> list_thinking_levels(
    const std::string& provider = "",
    const std::string& model = ""
);

// 对齐 thinking.ts:91-96: listThinkingLevelLabels
// 返回 UI 展示的标签 (binary provider → ["off", "on"])
std::vector<std::string> list_thinking_level_labels(
    const std::string& provider = "",
    const std::string& model = ""
);

} // namespace react
} // namespace hunterclaw
