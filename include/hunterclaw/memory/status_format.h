#pragma once

// 严格对齐 OpenClaw src/memory/status-format.ts

#include <string>

namespace hunterclaw {
namespace memory {

// ============================================================================
// Tone (对齐 status-format.ts:Tone)
// ============================================================================
enum class Tone {
    Ok,    // "ok"
    Warn,  // "warn"
    Muted  // "muted"
};

// ============================================================================
// resolveMemoryVectorState (对齐 status-format.ts:resolveMemoryVectorState)
// ============================================================================
struct VectorState {
    Tone tone;
    std::string state;  // "ready" | "unavailable" | "disabled" | "unknown"
};

VectorState resolve_memory_vector_state(bool enabled, bool* available = nullptr);

// ============================================================================
// resolveMemoryFtsState (对齐 status-format.ts:resolveMemoryFtsState)
// ============================================================================
struct FtsState {
    Tone tone;
    std::string state;  // "ready" | "unavailable" | "disabled"
};

FtsState resolve_memory_fts_state(bool enabled, bool available);

// ============================================================================
// resolveMemoryCacheSummary (对齐 status-format.ts:resolveMemoryCacheSummary)
// ============================================================================
struct CacheSummary {
    Tone tone;
    std::string text;
};

CacheSummary resolve_memory_cache_summary(bool enabled, int* entries = nullptr);

// ============================================================================
// resolveMemoryCacheState (对齐 status-format.ts:resolveMemoryCacheState)
// ============================================================================
struct CacheState {
    Tone tone;
    std::string state;  // "enabled" | "disabled"
};

CacheState resolve_memory_cache_state(bool enabled);

} // namespace memory
} // namespace hunterclaw
