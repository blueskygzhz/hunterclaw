#pragma once
// ============================================================================
// memory_flush_plan.h —
// 严格对齐 OpenClaw extensions/memory-core/src/flush-plan.ts (buildMemoryFlushPlan)
//
// OC 架构里 MemoryFlushPlan 由 memory 插件的 flushPlanResolver 产出
// （见 extensions/memory-core/index.ts:35 + flush-plan.ts:95-139）。
// HC 目前尚未实现插件级 memory capability，但 runMemoryFlushIfNeeded 又强依赖这份
// plan，所以这里以**静态 buildMemoryFlushPlan() 函数**的形式提供等价实现，
// 输入是 Agent 配置，输出是 { prompt, system_prompt, relative_path, 各阈值 }。
//
// 对应 OC 字段 ↔ HC 字段：
//   softThresholdTokens        → soft_threshold_tokens
//   forceFlushTranscriptBytes  → force_flush_transcript_bytes
//   reserveTokensFloor         → reserve_tokens_floor
//   prompt                     → prompt
//   systemPrompt               → system_prompt
//   relativePath               → relative_path   (e.g. "memory/2026-04-19.md")
// ============================================================================

#include <cstdint>
#include <optional>
#include <string>

namespace hunterclaw {
namespace memory {

// 对齐 OC: flush-plan.ts DEFAULT_MEMORY_FLUSH_SOFT_TOKENS / FORCE_TRANSCRIPT_BYTES
constexpr int    kDefaultMemoryFlushSoftThresholdTokens  = 4000;
constexpr int    kDefaultMemoryFlushReserveTokensFloor   = 20000;  // ≈ OC DEFAULT_PI_COMPACTION_RESERVE_TOKENS_FLOOR
constexpr int64_t kDefaultMemoryFlushForceTranscriptBytes = 2 * 1024 * 1024;

// 对齐 OC: SILENT_REPLY_TOKEN ("[silent]")
// HC 统一使用这个字符串，与 OC plugin-sdk/memory-core-host-runtime-core 保持一致
constexpr const char* kSilentReplyToken = "[silent]";

struct MemoryFlushPlan {
    int     soft_threshold_tokens       = kDefaultMemoryFlushSoftThresholdTokens;
    int64_t force_flush_transcript_bytes = kDefaultMemoryFlushForceTranscriptBytes;
    int     reserve_tokens_floor        = kDefaultMemoryFlushReserveTokensFloor;
    std::string prompt;         // flush agent 的 user prompt
    std::string system_prompt;  // flush agent 的 system prompt
    std::string relative_path;  // 默认 "memory/YYYY-MM-DD.md"
};

struct MemoryFlushPlanOverrides {
    // 允许 agent 配置覆盖（对齐 OC cfg.agents.defaults.compaction.memoryFlush.*）
    std::optional<bool>    enabled;
    std::optional<int>     soft_threshold_tokens;
    std::optional<int64_t> force_flush_transcript_bytes;
    std::optional<int>     reserve_tokens_floor;
    std::optional<std::string> custom_prompt;
    std::optional<std::string> custom_system_prompt;
};

// 对齐 OC: buildMemoryFlushPlan(params)
//   enabled=false 时返回 std::nullopt（对齐 OC L105-107: return null）
//   now_ms == 0 时用当前时间
std::optional<MemoryFlushPlan> build_memory_flush_plan(
    const MemoryFlushPlanOverrides& overrides = {},
    int64_t now_ms = 0
);

} // namespace memory
} // namespace hunterclaw
