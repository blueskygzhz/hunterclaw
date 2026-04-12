#pragma once
// ============================================================================
// context_pruning.h
// 严格对齐 OpenClaw: pi-hooks/context-pruning/ (6 files)
//   context-pruning.ts (入口) + settings.ts + pruner.ts + extension.ts + tools.ts
// ============================================================================

#include <string>
#include <vector>
#include <optional>
#include <set>
#include <functional>

namespace hunterclaw {
namespace context {

// ============================================================================
// Settings (对齐 OC: context-pruning/settings.ts)
// ============================================================================

struct ContextPruningToolMatch {
    std::string tool_name;
    bool keep_result = false;    // keep tool result content
    bool keep_latest_only = true; // only keep most recent invocation
};

struct ContextPruningConfig {
    bool enabled = false;
    int max_turns = 0;                // 0 = no limit
    int max_tool_result_chars = 3000; // truncate long tool results
    std::vector<ContextPruningToolMatch> tool_matches;
    std::set<std::string> preserve_tool_names;  // never prune these tools
};

struct EffectiveContextPruningSettings {
    bool enabled = false;
    int max_turns = 0;
    int max_tool_result_chars = 3000;
    std::vector<ContextPruningToolMatch> tool_matches;
    std::set<std::string> preserve_tool_names;
};

/// Default settings (对齐 OC: DEFAULT_CONTEXT_PRUNING_SETTINGS)
EffectiveContextPruningSettings default_context_pruning_settings();

/// Compute effective settings from config
/// 对齐 OC: computeEffectiveSettings
EffectiveContextPruningSettings compute_effective_settings(
    const ContextPruningConfig& config);

// ============================================================================
// Pruner (对齐 OC: context-pruning/pruner.ts)
// ============================================================================

struct PrunableMessage {
    std::string role;       // "user", "assistant", "tool"
    std::string content;
    std::string tool_name;  // for tool messages
    std::string tool_call_id;
    int64_t timestamp_ms = 0;
    bool is_compaction_marker = false;
};

struct PruneResult {
    std::vector<PrunableMessage> messages;
    int pruned_count = 0;
    int truncated_count = 0;
};

/// Prune context messages according to settings
/// 对齐 OC: pruneContextMessages
PruneResult prune_context_messages(
    const std::vector<PrunableMessage>& messages,
    const EffectiveContextPruningSettings& settings);

// ============================================================================
// Post-Compaction Context (对齐 OC: post-compaction-context.ts 237行)
// ============================================================================

/// Read critical sections from workspace AGENTS.md for post-compaction injection
/// 对齐 OC: readPostCompactionContext
std::optional<std::string> read_post_compaction_context(
    const std::string& workspace_dir,
    const std::string& timezone = "UTC",
    int64_t now_ms = 0);

} // namespace context
} // namespace hunterclaw
