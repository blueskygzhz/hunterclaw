#pragma once
// ============================================================================
// context_pruning.h
// 🎯 严格对齐 OpenClaw: src/agents/pi-hooks/context-pruning/
//   - settings.ts    → ContextPruningConfig / EffectiveContextPruningSettings
//   - pruner.ts      → prune_context_messages (soft-trim + hard-clear)
//   - tools.ts       → ToolPrunablePredicate
//   - extension.ts   → prune_context_messages_hook (in ReactEngine)
// ============================================================================

#include "hunterclaw/llm/llm_client.h"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace hunterclaw {
namespace context {

// ============================================================================
// Settings (对齐 OC: context-pruning/settings.ts)
// ============================================================================

// 🎯 对齐 OC: ContextPruningMode
enum class ContextPruningMode {
    OFF,        // 不启用
    CACHE_TTL,  // cache-ttl 模式：cache 过期时才剪枝（保护 prompt cache）
};

// 🎯 对齐 OC: ContextPruningToolMatch
struct ContextPruningToolMatch {
    std::vector<std::string> allow;  // 仅这些工具参与剪枝（空=全部）
    std::vector<std::string> deny;   // 这些工具永不剪枝
};

// 🎯 对齐 OC: ContextPruningConfig (原始配置，来自 config.agents.defaults.contextPruning)
struct ContextPruningConfig {
    ContextPruningMode mode = ContextPruningMode::OFF;
    // TTL 毫秒，默认 5 分钟
    int64_t ttl_ms = 5 * 60 * 1000;
    // 最后 N 个 assistant turn 永远保留（"protected tail"）
    int keep_last_assistants = 3;
    // 超出 context window 多少比例触发 soft-trim
    double soft_trim_ratio = 0.3;
    // 超出 context window 多少比例触发 hard-clear
    double hard_clear_ratio = 0.5;
    // hard-clear 门槛：可剪枝 tool result 合计字符数下限
    int min_prunable_tool_chars = 50'000;

    ContextPruningToolMatch tools;

    // Soft-trim：保留工具结果首尾
    struct SoftTrim {
        int max_chars = 4'000;
        int head_chars = 1'500;
        int tail_chars = 1'500;
    } soft_trim;

    // Hard-clear：彻底替换为占位符
    struct HardClear {
        bool enabled = true;
        std::string placeholder = "[Old tool result content cleared]";
    } hard_clear;
};

// 🎯 对齐 OC: EffectiveContextPruningSettings (归一化后的设置)
struct EffectiveContextPruningSettings {
    ContextPruningMode mode = ContextPruningMode::OFF;
    int64_t ttl_ms = 5 * 60 * 1000;
    int keep_last_assistants = 3;
    double soft_trim_ratio = 0.3;
    double hard_clear_ratio = 0.5;
    int min_prunable_tool_chars = 50'000;
    ContextPruningToolMatch tools;
    ContextPruningConfig::SoftTrim soft_trim;
    ContextPruningConfig::HardClear hard_clear;
};

// 🎯 对齐 OC: DEFAULT_CONTEXT_PRUNING_SETTINGS
EffectiveContextPruningSettings default_context_pruning_settings();

// 🎯 对齐 OC: computeEffectiveSettings
// 返回 std::nullopt 表示未启用（mode != cache-ttl）
std::optional<EffectiveContextPruningSettings> compute_effective_settings(
    const ContextPruningConfig& config);

// 🎯 对齐 OC: config.ts 解析 agents.defaults.contextPruning JSON 块
// JSON 结构对齐 OC ContextPruningConfig (settings.ts)：
// {
//   "mode": "off" | "cache-ttl",
//   "ttl": "5m" | "300s" | 300000   // 毫秒、或带单位的字符串 (s/m/h)
//   "keepLastAssistants": 3,
//   "softTrimRatio": 0.3,
//   "hardClearRatio": 0.5,
//   "minPrunableToolChars": 50000,
//   "tools": { "allow": [...], "deny": [...] },
//   "softTrim": { "maxChars": 4000, "headChars": 1500, "tailChars": 1500 },
//   "hardClear": { "enabled": true, "placeholder": "..." }
// }
ContextPruningConfig parse_context_pruning_config_from_json(
    const core::JsonValue& json);

// ============================================================================
// Tool Predicate (对齐 OC: context-pruning/tools.ts)
// ============================================================================

// 返回一个 predicate: (toolName) -> bool，表示该工具是否可被剪枝
// 🎯 对齐 OC: makeToolPrunablePredicate
using ToolPrunablePredicate = std::function<bool(const std::string&)>;
ToolPrunablePredicate make_tool_prunable_predicate(
    const ContextPruningToolMatch& match);

// ============================================================================
// Pruner (对齐 OC: context-pruning/pruner.ts)
// ============================================================================

struct PruneContextParams {
    // 输入消息（拷贝，返回时可能被替换）
    std::vector<llm::LLMMessage> messages;
    // 归一化后的设置
    EffectiveContextPruningSettings settings;
    // Context window (tokens)，<=0 则跳过剪枝
    int context_window_tokens = 0;
    // 工具剪枝谓词，默认由 settings.tools 生成
    std::optional<ToolPrunablePredicate> is_tool_prunable;
    // （可选）估算体积时是否剔除 thinking 块 —— HC 当前 LLMMessage 无独立 thinking 字段，
    // 预留此开关以便未来扩展。
    bool drop_thinking_blocks_for_estimate = false;
};

struct PruneContextResult {
    std::vector<llm::LLMMessage> messages;
    bool changed = false;
    // 诊断信息
    int total_chars_before = 0;
    int total_chars_after = 0;
    int soft_trimmed_count = 0;
    int hard_cleared_count = 0;
};

// 🎯 对齐 OC: pruneContextMessages
PruneContextResult prune_context_messages(const PruneContextParams& params);

// ============================================================================
// Cache-TTL Eligibility (对齐 OC: cache-ttl.ts)
// ============================================================================

// 🎯 对齐 OC: isCacheTtlEligibleProvider
// 只有支持 prompt cache 的 provider 才启用 cache-ttl 模式的剪枝
// （Anthropic messages API 或兼容 API）
bool is_cache_ttl_eligible_provider(
    const std::string& provider,
    const std::string& model_id,
    const std::string& model_api);

// ============================================================================
// Post-Compaction Context — 声明已迁移
// ============================================================================
//
// read_post_compaction_context 的严格对齐实现位于:
//     hunterclaw::react::read_post_compaction_context
//     (include/hunterclaw/react/preflight_memory.h)
// ============================================================================

}  // namespace context
}  // namespace hunterclaw
