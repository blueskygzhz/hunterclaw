#include "hunterclaw/react/verbose_reasoning_config.h"
#include <spdlog/spdlog.h>
#include <algorithm>

namespace hunterclaw {
namespace react {

namespace {
// Logger
auto log() {
    static auto logger = spdlog::get("react");
    if (!logger) {
        logger = spdlog::stdout_logger_mt("react");
    }
    return logger;
}
} // anonymous namespace

// ============================================================================
// 配置解析实现 (OpenClaw 严格对齐)
// ============================================================================

/**
 * 解析 verbose 级别 (3 层优先级)
 * 
 * 对应 OpenClaw: src/auto-reply/reply/get-reply-directives.ts:356-359
 * 
 * const resolvedVerboseLevel =
 *   directives.verboseLevel ??                     // 优先级 1: 内联指令
 *   (sessionEntry?.verboseLevel) ??                // 优先级 2: 会话覆盖
 *   (agentCfg?.verboseDefault);                    // 优先级 3: 全局配置 ✅
 */
VerboseLevel resolve_verbose_level(
    const MessageDirectives& directives,
    const SessionLevelOverrides* session_overrides,
    const AgentDefaultsConfig& agent_defaults
) {
    // 优先级 1: 消息内联指令 (/verbose full)
    if (directives.verbose_level.has_value()) {
        log()->debug("[VerboseConfig] Using inline directive: {}", 
            to_string(*directives.verbose_level));
        return *directives.verbose_level;
    }
    
    // 优先级 2: 会话级别覆盖
    if (session_overrides && session_overrides->verbose_level.has_value()) {
        log()->debug("[VerboseConfig] Using session override: {}", 
            to_string(*session_overrides->verbose_level));
        return *session_overrides->verbose_level;
    }
    
    // 优先级 3: 全局默认配置 (verboseDefault) ✅
    if (agent_defaults.verbose_default.has_value()) {
        log()->debug("[VerboseConfig] Using global default: {}", 
            to_string(*agent_defaults.verbose_default));
        return *agent_defaults.verbose_default;
    }
    
    // 默认: Off
    log()->debug("[VerboseConfig] Using fallback default: Off");
    return VerboseLevel::Off;
}

/**
 * 解析 reasoning 级别 (3 层优先级 + 模型能力检测)
 * 
 * 对应 OpenClaw: src/auto-reply/reply/get-reply-directives.ts:409-419
 * 
 * let resolvedReasoningLevel: ReasoningLevel =
 *   directives.reasoningLevel ??                   // 优先级 1: 内联指令
 *   (sessionEntry?.reasoningLevel) ??              // 优先级 2: 会话覆盖
 *   "off";                                         // 优先级 3: 默认关闭
 * 
 * const reasoningExplicitlySet = ...
 * const thinkingActive = ...
 * 
 * // 如果用户没有设置 reasoning,且 thinking 未激活,根据模型能力自动启用 ✅
 * if (!reasoningExplicitlySet && resolvedReasoningLevel === "off" && !thinkingActive) {
 *   resolvedReasoningLevel = await modelState.resolveDefaultReasoningLevel();
 * }
 */
ReasoningLevel resolve_reasoning_level(
    const MessageDirectives& directives,
    const SessionLevelOverrides* session_overrides,
    const AgentDefaultsConfig& agent_defaults,
    const ModelCatalogEntry& model_entry,
    bool thinking_active
) {
    bool explicitly_set = false;
    ReasoningLevel resolved = ReasoningLevel::Off;
    
    // 优先级 1: 消息内联指令 (/reasoning on)
    if (directives.reasoning_level.has_value()) {
        resolved = *directives.reasoning_level;
        explicitly_set = true;
        log()->debug("[ReasoningConfig] Using inline directive: {}", 
            to_string(resolved));
    }
    // 优先级 2: 会话级别覆盖
    else if (session_overrides && session_overrides->reasoning_level.has_value()) {
        resolved = *session_overrides->reasoning_level;
        explicitly_set = true;
        log()->debug("[ReasoningConfig] Using session override: {}", 
            to_string(resolved));
    }
    
    // 优先级 3: 模型能力自动检测 ✅
    // ✅ 对齐 OpenClaw: get-reply-directives.ts:417
    // if (!reasoningExplicitlySet && resolvedReasoningLevel === "off" && !thinkingActive) {
    //   resolvedReasoningLevel = await modelState.resolveDefaultReasoningLevel();
    // }
    if (!explicitly_set && 
        resolved == ReasoningLevel::Off && 
        !thinking_active &&
        agent_defaults.auto_enable_reasoning_for_capable_models &&
        model_supports_reasoning(model_entry)) {
        
        resolved = ReasoningLevel::On;
        log()->info("[ReasoningConfig] Auto-enabled reasoning for capable model: {}/{}",
            model_entry.provider, model_entry.id);  // ✅ model_entry.id (不是 model_id)
    }
    
    if (resolved == ReasoningLevel::Off) {
        log()->debug("[ReasoningConfig] Reasoning disabled (explicitly_set={}, thinking_active={})",
            explicitly_set, thinking_active);
    }
    
    return resolved;
}

/**
 * 检查模型是否支持 reasoning
 * 
 * ✅ 严格对齐 OpenClaw: resolveReasoningDefault (model-selection.ts:669-681)
 * 
 * export function resolveReasoningDefault(params: {
 *   provider: string;
 *   model: string;
 *   catalog?: ModelCatalogEntry[];
 * }): "on" | "off" {
 *   const candidate = catalog?.find(entry =>
 *     entry.provider === params.provider && entry.id === params.model
 *   );
 *   return candidate?.reasoning === true ? "on" : "off";
 * }
 */
bool model_supports_reasoning(const ModelCatalogEntry& entry) {
    // ✅ 对齐 OpenClaw: entry?.reasoning === true
    return entry.reasoning.value_or(false);
}

/**
 * 解析完整配置 (包括所有参数)
 */
ResolvedVerboseReasoningConfig resolve_verbose_reasoning_config(
    const MessageDirectives& directives,
    const SessionLevelOverrides* session_overrides,
    const AgentDefaultsConfig& agent_defaults,
    const ModelCatalogEntry& model_entry,
    bool thinking_active
) {
    ResolvedVerboseReasoningConfig config;
    
    // 解析 verbose 级别
    config.verbose_level = resolve_verbose_level(
        directives, session_overrides, agent_defaults);
    
    // 解析 reasoning 级别
    config.reasoning_level = resolve_reasoning_level(
        directives, session_overrides, agent_defaults, 
        model_entry, thinking_active);
    
    // 解析 block streaming 配置
    config.block_streaming_enabled = agent_defaults.block_streaming_default;
    config.streaming_break = agent_defaults.block_streaming_break;
    
    log()->info("[Config] Resolved configuration: verbose={}, reasoning={}, "
        "block_streaming={}, break={}",
        to_string(config.verbose_level),
        to_string(config.reasoning_level),
        config.block_streaming_enabled,
        to_string(config.streaming_break));
    
    return config;
}

// ============================================================================
// 工具输出过滤实现 (OpenClaw 严格对齐)
// ============================================================================

/**
 * 根据 verbose 级别判断是否包含工具输出
 * 
 * 对应 OpenClaw: src/gateway/server-chat.ts:539-551
 * src/tui/tui-event-handlers.ts:286-316
 * 
 * const allowToolOutput = (verbose === "full");  // ✅ 关键判断
 */
bool should_include_tool_output(VerboseLevel verbose) {
    return (verbose == VerboseLevel::Full);  // ✅ 只有 Full 才包含输出
}

/**
 * 过滤工具事件数据
 * 
 * 对应 OpenClaw: src/gateway/server-chat.ts:539-551
 * 
 * const toolPayload =
 *   isToolEvent && toolVerbose !== "full"
 *     ? (() => {
 *         const data = evt.data ? { ...evt.data } : {};
 *         delete data.result;         // 删除工具输出 ❌
 *         delete data.partialResult;  // 删除增量输出 ❌
 *         return { ...eventForClients, data };
 *       })()
 *     : agentPayload;  // 保留完整输出 ✅
 */
void filter_tool_event_data(ToolEventData& data, VerboseLevel verbose) {
    if (verbose == VerboseLevel::Off) {
        // Off: 完全不显示工具事件 (调用者应跳过此事件)
        log()->trace("[ToolFilter] verbose=Off, event should be skipped");
        return;
    }
    
    if (verbose == VerboseLevel::On) {
        // On: 只显示工具名称和参数,删除输出 ❌
        log()->debug("[ToolFilter] verbose=On, removing tool output");
        data.result = core::JsonValue();         // 清空 result
        data.partial_result = core::JsonValue(); // 清空 partialResult
    }
    
    // Full: 保留完整输出 ✅ (无需操作)
    if (verbose == VerboseLevel::Full) {
        log()->trace("[ToolFilter] verbose=Full, keeping full output");
    }
}

// ============================================================================
// 推理内容格式化实现 (OpenClaw 对齐)
// ============================================================================

/**
 * 格式化推理内容
 * 
 * 对应 OpenClaw: src/agents/pi-embedded-utils.ts:278
 * 
 * // Show reasoning in italics (cursive) for markdown-friendly surfaces.
 * // 推理内容前缀: "Reasoning:\n_<content>_"
 */
std::string format_reasoning_text(const std::string& raw_thinking) {
    if (raw_thinking.empty()) {
        return "";
    }
    
    // OpenClaw 格式: "Reasoning:\n_<content>_" (Markdown 斜体)
    return "Reasoning:\n_" + raw_thinking + "_";
}

/**
 * 判断推理发送时机
 * 
 * 对应 OpenClaw: src/agents/pi-embedded-subscribe.handlers.messages.ts:325-330
 * 
 * const shouldEmitReasoningBeforeAnswer =
 *   shouldEmitReasoning && 
 *   ctx.state.blockReplyBreak === "message_end" &&  // message_end 模式下先发推理
 *   !addedDuringMessage;
 */
bool should_emit_reasoning_before_answer(
    BlockStreamingBreak break_mode,
    bool added_during_message
) {
    // message_end 模式: 先发推理,再发答案
    // text_end 模式: 先发答案,再发推理
    return (break_mode == BlockStreamingBreak::MessageEnd) && !added_during_message;
}

// ============================================================================
// 字符串转换实现
// ============================================================================

std::string to_string(VerboseLevel level) {
    switch (level) {
        case VerboseLevel::Off:  return "off";
        case VerboseLevel::On:   return "on";
        case VerboseLevel::Full: return "full";
        default: return "unknown";
    }
}

std::string to_string(ReasoningLevel level) {
    switch (level) {
        case ReasoningLevel::Off:    return "off";
        case ReasoningLevel::On:     return "on";
        case ReasoningLevel::Stream: return "stream";
        default: return "unknown";
    }
}

std::string to_string(BlockStreamingBreak mode) {
    switch (mode) {
        case BlockStreamingBreak::TextEnd:    return "text_end";
        case BlockStreamingBreak::MessageEnd: return "message_end";
        default: return "unknown";
    }
}

std::optional<VerboseLevel> parse_verbose_level(const std::string& str) {
    std::string lower = str;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    
    if (lower == "off")  return VerboseLevel::Off;
    if (lower == "on")   return VerboseLevel::On;
    if (lower == "full") return VerboseLevel::Full;
    
    return std::nullopt;
}

std::optional<ReasoningLevel> parse_reasoning_level(const std::string& str) {
    std::string lower = str;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    
    if (lower == "off")    return ReasoningLevel::Off;
    if (lower == "on")     return ReasoningLevel::On;
    if (lower == "stream") return ReasoningLevel::Stream;
    
    return std::nullopt;
}

std::optional<BlockStreamingBreak> parse_block_streaming_break(const std::string& str) {
    std::string lower = str;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    
    if (lower == "text_end")    return BlockStreamingBreak::TextEnd;
    if (lower == "message_end") return BlockStreamingBreak::MessageEnd;
    
    return std::nullopt;
}

} // namespace react
} // namespace hunterclaw
