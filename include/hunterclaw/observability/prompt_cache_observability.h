// ✅ 严格对齐 OpenClaw: src/agents/pi-embedded-runner/prompt-cache-observability.ts (217行)
// LLM 提示词缓存可观测 — 追踪缓存命中/中断，检测模型/工具/系统提示变更引起的 cache break
#pragma once

#include <string>
#include <vector>
#include <optional>
#include <map>
#include <cstdint>

namespace hunterclaw {
namespace observability {

// ============================================================================
// Types — 严格对齐 OpenClaw
// ============================================================================

// ✅ 对齐 OpenClaw: PromptCacheChangeCode
enum class PromptCacheChangeCode {
    CACHE_RETENTION,
    MODEL,
    STREAM_STRATEGY,
    SYSTEM_PROMPT,
    TOOLS,
    TRANSPORT
};

inline std::string prompt_cache_change_code_to_string(PromptCacheChangeCode code) {
    switch (code) {
        case PromptCacheChangeCode::CACHE_RETENTION: return "cacheRetention";
        case PromptCacheChangeCode::MODEL:           return "model";
        case PromptCacheChangeCode::STREAM_STRATEGY: return "streamStrategy";
        case PromptCacheChangeCode::SYSTEM_PROMPT:   return "systemPrompt";
        case PromptCacheChangeCode::TOOLS:           return "tools";
        case PromptCacheChangeCode::TRANSPORT:       return "transport";
        default: return "unknown";
    }
}

// ✅ 对齐 OpenClaw: PromptCacheChange
struct PromptCacheChange {
    PromptCacheChangeCode code;
    std::string detail;
};

// ✅ 对齐 OpenClaw: PromptCacheSnapshot
struct PromptCacheSnapshot {
    std::string provider;
    std::string model_id;
    std::string model_api;        // optional
    std::string cache_retention;  // "none" | "short" | "long"
    std::string stream_strategy;
    std::string transport;        // optional
    std::string system_prompt_digest;
    std::string tool_digest;
    int tool_count = 0;
    std::vector<std::string> tool_names;
};

// ✅ 对齐 OpenClaw: PromptCacheObservationStart
struct PromptCacheObservationStart {
    PromptCacheSnapshot snapshot;
    std::optional<std::vector<PromptCacheChange>> changes;
    std::optional<int64_t> previous_cache_read;
};

// ✅ 对齐 OpenClaw: PromptCacheBreak
struct PromptCacheBreak {
    int64_t previous_cache_read;
    int64_t cache_read;
    std::optional<std::vector<PromptCacheChange>> changes;
};

// ============================================================================
// Public API
// ============================================================================

// ✅ 对齐 OpenClaw: collectPromptCacheToolNames
std::vector<std::string> collect_prompt_cache_tool_names(
    const std::vector<std::string>& tool_names);

// ✅ 对齐 OpenClaw: beginPromptCacheObservation
struct BeginPromptCacheObservationParams {
    std::string session_id;
    std::string session_key;
    std::string provider;
    std::string model_id;
    std::string model_api;
    std::string cache_retention;
    std::string stream_strategy;
    std::string transport;
    std::string system_prompt;
    std::vector<std::string> tool_names;
};

PromptCacheObservationStart begin_prompt_cache_observation(
    const BeginPromptCacheObservationParams& params);

// ✅ 对齐 OpenClaw: completePromptCacheObservation
struct CompletePromptCacheObservationParams {
    std::string session_id;
    std::string session_key;
    std::optional<int64_t> cache_read;
};

std::optional<PromptCacheBreak> complete_prompt_cache_observation(
    const CompletePromptCacheObservationParams& params);

// ✅ 对齐 OpenClaw: resetPromptCacheObservabilityForTest
void reset_prompt_cache_observability_for_test();

}  // namespace observability
}  // namespace hunterclaw
