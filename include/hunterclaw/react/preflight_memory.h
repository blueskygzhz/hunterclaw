#pragma once
// ============================================================================
// preflight_memory.h — 严格对齐 OpenClaw:
//   agent-runner-memory.ts  (849行) — pre-run compaction + memory flush
//   memory-flush.ts          (132行) — 门控函数 + context hash
//   post-compaction-context.ts (237行) — 压缩后上下文恢复
// ============================================================================

#include <string>
#include <vector>
#include <optional>
#include <functional>
#include <cstdint>
#include <utility>

namespace hunterclaw {
namespace react {

// ============================================================================
// 对齐 agent-runner-memory.ts:90-101: estimatePromptTokensForMemoryFlush
// ============================================================================
std::optional<int> estimate_prompt_tokens_for_memory_flush(const std::string& prompt);

// ============================================================================
// 对齐 agent-runner-memory.ts:103-114: resolveEffectivePromptTokens
// ============================================================================
int resolve_effective_prompt_tokens(
    int base_prompt_tokens = 0,
    int last_output_tokens = 0,
    int prompt_token_estimate = 0
);

// ============================================================================
// 对齐 agent-runner-memory.ts:116-119: SessionTranscriptUsageSnapshot
// ============================================================================
struct SessionTranscriptUsageSnapshot {
    std::optional<int> prompt_tokens;
    std::optional<int> output_tokens;
};

// ============================================================================
// 对齐 agent-runner-memory.ts:200-203 + 227-267: SessionLogSnapshot
// ============================================================================
struct SessionLogSnapshot {
    std::optional<int64_t> byte_size;
    std::optional<SessionTranscriptUsageSnapshot> usage;
};

SessionLogSnapshot read_session_log_snapshot(
    const std::string& log_path,
    bool include_byte_size,
    bool include_usage
);

// ============================================================================
// 对齐 agent-runner-memory.ts:269-298: readLastNonzeroUsageFromSessionLog
// 多 chunk 向前扫描
// ============================================================================
std::optional<SessionTranscriptUsageSnapshot> read_last_nonzero_usage_from_session_log(
    const std::string& log_path
);

// ============================================================================
// 对齐 agent-runner-memory.ts:301-327: estimatePromptTokensFromSessionTranscript
// 实际读取 transcript 消息并估算 token 数
// ============================================================================
std::optional<int> estimate_prompt_tokens_from_session_transcript(
    const std::string& session_id,
    const std::string& store_path = "",
    const std::string& session_file = ""
);

// shouldRunPreflightCompaction / shouldRunMemoryFlush / hasAlreadyFlushedForCurrentCompaction
// → 已在 memory_flush_gate.h 中声明（使用 struct params 版本）

// ============================================================================
// 对齐 memory-flush.ts L125-131: computeContextHash
// SHA-256 截断为 16 hex chars，用于 flush 去重
// ============================================================================
std::string compute_context_hash(
    const std::vector<std::pair<std::string, std::string>>& messages // {role, content}
);

// ============================================================================
// 对齐 agent-runner-memory.ts:346-503: runPreflightCompactionIfNeeded
//
// 🎯 架构说明（对齐 OC 并修正 HC 实现）：
//   OC 里 preflight 门控与执行统一于 compactEmbeddedPiSession（trigger: "budget"），
//   agent-runner-memory.ts 仅做"是否触发"的门控，然后直接调用 compactEmbeddedPiSession
//   （无回调 indirection）。
//
//   HC 对齐做法：本头文件只暴露"门控判定"。实际 compaction 执行由 ReactEngine
//   持有的 CompactionManager 完成（ReactEngine::trigger_compaction），这是 HC
//   内 token-overflow / manual / preflight 三条入口共用的唯一实现。
//
//   即此处的函数只回答"要不要 compact"，不负责调用 compact。
// ============================================================================

struct PreflightRunParams {
    std::string session_key;
    std::string session_id;
    std::string session_file;
    std::string store_path;
    std::string workspace_dir;
    std::string provider;
    std::string model;
    std::string default_model;
    std::optional<int> agent_cfg_context_tokens;
    bool is_heartbeat = false;
    bool is_cli = false;

    // Token state
    std::optional<int> persisted_total_tokens;
    bool total_tokens_fresh = false;
    std::optional<int> compaction_count;
    std::optional<std::string> prompt_for_estimate;

    // Context window config
    int context_window_tokens = 200000;
    int reserve_tokens_floor = 20000;
    int soft_threshold_tokens = 4000;
};

// 对齐 OC agent-runner-memory.ts L419-445: gating decision
// 仅表达"是否应当触发 preflight compaction"，不包含执行结果
struct PreflightCompactionDecision {
    bool should_compact = false;
    std::optional<int> projected_token_count;  // transcript 估算 + prompt_estimate
    int threshold = 0;                          // contextWindow - reserve - soft
    std::string skip_reason;                    // 门控跳过原因（便于日志）
};

PreflightCompactionDecision evaluate_preflight_compaction(
    const PreflightRunParams& params
);

// ============================================================================
// 对齐 agent-runner-memory.ts:505-848: runMemoryFlushIfNeeded
// 完整: near-threshold output 读取 + transcript token 回写 + session rotation
// ============================================================================

struct MemoryFlushParams {
    std::string session_key;
    std::string session_id;
    std::string session_file;
    std::string store_path;
    std::string workspace_dir;
    std::string provider;
    std::string model;
    std::string default_model;
    std::optional<int> agent_cfg_context_tokens;
    bool is_heartbeat = false;
    bool is_cli = false;
    bool memory_flush_writable = true;

    // Token state
    std::optional<int> persisted_total_tokens;
    bool total_tokens_fresh = false;
    std::optional<int> compaction_count;
    std::optional<int> memory_flush_compaction_count;
    std::optional<std::string> prompt_for_estimate;

    // Memory flush plan
    int reserve_tokens_floor = 20000;
    int soft_threshold_tokens = 4000;
    int context_window_tokens = 200000;
    int64_t force_flush_transcript_bytes = 0;
    std::string flush_write_path;
    std::string flush_system_prompt;
    std::string flush_prompt;
    std::string session_log_path;  // ✅ 新增: transcript 文件路径 (用于 session log 读取)

    // Callbacks
    std::function<void(const std::string& prompt, const std::string& system_prompt)> run_flush_agent;
    std::function<void(int new_compaction_count, const std::string& new_session_id)> on_flush_complete;
    // ✅ 新增: 回写 transcript-derived token 到 session store (对齐 OC L622-649)
    std::function<void(int derived_total_tokens)> persist_session_tokens;
};

struct MemoryFlushResult {
    bool flushed = false;
    bool compaction_completed = false;
    std::optional<std::string> new_session_id;
};

MemoryFlushResult run_memory_flush_if_needed(const MemoryFlushParams& params);

// ============================================================================
// 对齐 post-compaction-context.ts:64-156: readPostCompactionContext
// 支持: custom sections config, default/custom prose 区分, 完整 timeLine
// ============================================================================
std::optional<std::string> read_post_compaction_context(
    const std::string& workspace_dir,
    const std::vector<std::string>& configured_sections = {},  // 空=使用默认
    int64_t now_ms = 0  // 0=使用当前时间
);

} // namespace react
} // namespace hunterclaw
