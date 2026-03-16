#pragma once

#include "hunterclaw/core/types.h"
#include <string>
#include <optional>
#include <vector>

namespace hunterclaw {
namespace react {

// ============================================================================
// OpenClaw 严格对齐: Verbose 和 Reasoning 配置
// 参考: openclaw-analysis/src/config/types.agent-defaults.ts
// ============================================================================

/**
 * Verbose 级别 (OpenClaw 对齐)
 * 
 * 对应 OpenClaw: type VerboseLevel = "off" | "on" | "full"
 */
enum class VerboseLevel {
    Off,   // "off": 不显示工具调用和输出
    On,    // "on": 只显示工具名称和参数
    Full   // "full": 显示工具名称、参数和完整输出 ✅
};

/**
 * Reasoning 级别 (OpenClaw 对齐)
 * 
 * 对应 OpenClaw: type ReasoningLevel = "off" | "on" | "stream"
 */
enum class ReasoningLevel {
    Off,      // "off": 不显示推理过程
    On,       // "on": 显示推理(作为单独消息)
    Stream    // "stream": 流式显示推理 (仅限特定渠道如 Telegram)
};

/**
 * Block Streaming Break 模式 (OpenClaw 对齐)
 * 
 * 对应 OpenClaw: type BlockStreamingBreak = "text_end" | "message_end"
 * 参考: openclaw-analysis/docs/concepts/streaming.md
 */
enum class BlockStreamingBreak {
    TextEnd,     // "text_end": 实时输出,每完成一段就发送
    MessageEnd   // "message_end": 延迟输出,等整个消息完成
};

/**
 * 模型能力信息 (OpenClaw 对齐)
 * 
 * 对应 OpenClaw: ModelCatalogEntry
 * 参考: openclaw-analysis/src/agents/model-selection.ts
 */
struct ModelCapabilities {
    std::string provider;              // 提供商 (anthropic, openai, etc.)
    std::string model_id;              // 模型 ID
    bool supports_reasoning = false;   // 是否支持推理 (OpenClaw: reasoning?: boolean) ✅
    bool supports_tool_use = true;     // 是否支持工具调用
    bool supports_streaming = true;    // 是否支持流式输出
    int max_tokens = 4096;             // 最大输出 tokens
    int context_window = 200000;       // 上下文窗口大小
};

/**
 * Agent Defaults 配置 (OpenClaw 对齐)
 * 
 * 对应 OpenClaw: AgentDefaultsConfig
 * 参考: openclaw-analysis/src/config/types.agent-defaults.ts:189
 */
struct AgentDefaultsConfig {
    // ✅ Verbose 配置
    // 对应 OpenClaw: verboseDefault?: "off" | "on" | "full"
    std::optional<VerboseLevel> verbose_default;
    
    // ✅ Block Streaming 配置
    // 对应 OpenClaw: blockStreamingDefault?: "on" | "off"
    bool block_streaming_default = false;
    
    // 对应 OpenClaw: blockStreamingBreak?: "text_end" | "message_end"
    BlockStreamingBreak block_streaming_break = BlockStreamingBreak::TextEnd;
    
    // 注意: OpenClaw 没有 reasoningDefault 配置!
    // reasoning 默认行为由模型能力自动决定
    // 参考: openclaw-analysis/src/agents/model-selection.ts:669-681
    
    // ✅ 自动启用支持推理的模型 (HunterClaw 扩展)
    bool auto_enable_reasoning_for_capable_models = true;
};

/**
 * 会话级别覆盖配置 (OpenClaw 对齐)
 * 
 * 对应 OpenClaw: SessionEntry
 * 参考: openclaw-analysis/src/sessions/level-overrides.ts
 */
struct SessionLevelOverrides {
    std::optional<VerboseLevel> verbose_level;      // 会话级 verbose 覆盖
    std::optional<ReasoningLevel> reasoning_level;  // 会话级 reasoning 覆盖
};

/**
 * 消息内联指令 (OpenClaw 对齐)
 * 
 * 对应 OpenClaw: ReplyDirectives
 * 参考: openclaw-analysis/src/auto-reply/reply/get-reply-directives.ts
 */
struct MessageDirectives {
    std::optional<VerboseLevel> verbose_level;      // 消息内联 /verbose 指令
    std::optional<ReasoningLevel> reasoning_level;  // 消息内联 /reasoning 指令
};

/**
 * 解析后的有效配置 (3 层优先级合并结果)
 * 
 * 对应 OpenClaw 的配置解析逻辑
 * 参考: openclaw-analysis/src/auto-reply/reply/get-reply-directives.ts:356-419
 */
struct ResolvedVerboseReasoningConfig {
    VerboseLevel verbose_level;           // 解析后的 verbose 级别
    ReasoningLevel reasoning_level;       // 解析后的 reasoning 级别
    BlockStreamingBreak streaming_break;  // 解析后的 streaming break 模式
    bool block_streaming_enabled;         // 是否启用 block streaming
};

// ============================================================================
// 配置解析函数 (OpenClaw 严格对齐)
// ============================================================================

/**
 * 解析 verbose 级别 (3 层优先级)
 * 
 * 对应 OpenClaw: get-reply-directives.ts:356-359
 * 
 * 优先级:
 * 1. 消息内联指令 (/verbose full)
 * 2. 会话级别覆盖 (用户发送 /verbose full)
 * 3. 全局默认配置 (verboseDefault)
 */
VerboseLevel resolve_verbose_level(
    const MessageDirectives& directives,
    const SessionLevelOverrides* session_overrides,
    const AgentDefaultsConfig& agent_defaults
);

/**
 * 解析 reasoning 级别 (3 层优先级 + 模型能力检测)
 * 
 * 对应 OpenClaw: get-reply-directives.ts:409-419
 * 
 * 优先级:
 * 1. 消息内联指令 (/reasoning on)
 * 2. 会话级别覆盖
 * 3. 模型能力自动检测 ✅ (如果模型支持 reasoning,自动启用)
 * 4. 默认关闭
 */
ReasoningLevel resolve_reasoning_level(
    const MessageDirectives& directives,
    const SessionLevelOverrides* session_overrides,
    const AgentDefaultsConfig& agent_defaults,
    const ModelCapabilities& model_caps,
    bool thinking_active  // OpenClaw: 如果 thinking 已激活,不自动启用 reasoning
);

/**
 * 检查模型是否支持 reasoning
 * 
 * 对应 OpenClaw: resolveReasoningDefault()
 * 参考: openclaw-analysis/src/agents/model-selection.ts:669-681
 */
bool model_supports_reasoning(const ModelCapabilities& caps);

/**
 * 解析完整配置 (包括所有参数)
 */
ResolvedVerboseReasoningConfig resolve_verbose_reasoning_config(
    const MessageDirectives& directives,
    const SessionLevelOverrides* session_overrides,
    const AgentDefaultsConfig& agent_defaults,
    const ModelCapabilities& model_caps,
    bool thinking_active = false
);

// ============================================================================
// 工具输出过滤 (OpenClaw 严格对齐)
// ============================================================================

/**
 * 工具事件数据
 * 
 * 对应 OpenClaw: Tool Event data
 * 参考: openclaw-analysis/src/gateway/server-chat.ts:539-551
 */
struct ToolEventData {
    std::string tool_call_id;
    std::string tool_name;
    core::JsonValue args;
    core::JsonValue result;          // 工具输出 (可能被过滤)
    core::JsonValue partial_result;  // 增量输出 (可能被过滤)
    bool is_error = false;
    std::string phase;  // "start", "update", "result"
};

/**
 * 根据 verbose 级别过滤工具输出
 * 
 * 对应 OpenClaw: server-chat.ts:539-551
 * 
 * 规则:
 * - verbose === "off": 返回 false (完全不显示工具事件)
 * - verbose === "on": 删除 result/partialResult (只显示名称)
 * - verbose === "full": 保留完整输出 ✅
 */
bool should_include_tool_output(VerboseLevel verbose);

/**
 * 过滤工具事件数据
 * 
 * 修改 data 对象,根据 verbose 级别删除敏感字段
 */
void filter_tool_event_data(ToolEventData& data, VerboseLevel verbose);

// ============================================================================
// 推理内容格式化 (OpenClaw 对齐)
// ============================================================================

/**
 * 格式化推理内容
 * 
 * 对应 OpenClaw: pi-embedded-utils.ts:278
 * 
 * 格式: "Reasoning:\n_<content>_" (Markdown 斜体)
 */
std::string format_reasoning_text(const std::string& raw_thinking);

/**
 * 判断推理发送时机
 * 
 * 对应 OpenClaw: pi-embedded-subscribe.handlers.messages.ts:345-425
 * 
 * @return true: 在答案之前发送 (message_end 模式)
 *         false: 在答案之后发送 (text_end 模式)
 */
bool should_emit_reasoning_before_answer(
    BlockStreamingBreak break_mode,
    bool added_during_message
);

// ============================================================================
// 字符串转换辅助函数
// ============================================================================

std::string to_string(VerboseLevel level);
std::string to_string(ReasoningLevel level);
std::string to_string(BlockStreamingBreak mode);

std::optional<VerboseLevel> parse_verbose_level(const std::string& str);
std::optional<ReasoningLevel> parse_reasoning_level(const std::string& str);
std::optional<BlockStreamingBreak> parse_block_streaming_break(const std::string& str);

} // namespace react
} // namespace hunterclaw
