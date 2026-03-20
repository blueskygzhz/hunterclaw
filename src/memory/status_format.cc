#include "hunterclaw/memory/status_format.h"
#include <string>

// 严格对齐 OpenClaw src/memory/status-format.ts

namespace hunterclaw {
namespace memory {

// ============================================================================
// resolveMemoryVectorState (对齐 status-format.ts:resolveMemoryVectorState)
// ============================================================================
VectorState resolve_memory_vector_state(bool enabled, bool* available) {
    if (!enabled) {
        return {Tone::Muted, "disabled"};
    }
    if (available != nullptr) {
        if (*available) {
            return {Tone::Ok, "ready"};
        } else {
            return {Tone::Warn, "unavailable"};
        }
    }
    // available === undefined
    return {Tone::Muted, "unknown"};
}

// ============================================================================
// resolveMemoryFtsState (对齐 status-format.ts:resolveMemoryFtsState)
// ============================================================================
FtsState resolve_memory_fts_state(bool enabled, bool available) {
    if (!enabled) {
        return {Tone::Muted, "disabled"};
    }
    return available
        ? FtsState{Tone::Ok, "ready"}
        : FtsState{Tone::Warn, "unavailable"};
}

// ============================================================================
// resolveMemoryCacheSummary (对齐 status-format.ts:resolveMemoryCacheSummary)
// ============================================================================
CacheSummary resolve_memory_cache_summary(bool enabled, int* entries) {
    if (!enabled) {
        return {Tone::Muted, "cache off"};
    }
    std::string suffix;
    if (entries != nullptr) {
        suffix = " (" + std::to_string(*entries) + ")";
    }
    return {Tone::Ok, "cache on" + suffix};
}

// ============================================================================
// resolveMemoryCacheState (对齐 status-format.ts:resolveMemoryCacheState)
// ============================================================================
CacheState resolve_memory_cache_state(bool enabled) {
    return enabled
        ? CacheState{Tone::Ok, "enabled"}
        : CacheState{Tone::Muted, "disabled"};
}

} // namespace memory
} // namespace hunterclaw
