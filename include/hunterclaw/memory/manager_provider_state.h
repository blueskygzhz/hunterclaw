#pragma once

#include <string>
#include <optional>
#include "hunterclaw/memory/types.h"

// 严格对齐 OpenClaw src/memory/manager-provider-state.ts (100行)
// Embedding provider 状态解析和 fallback 逻辑

namespace hunterclaw {
namespace memory {

// Forward declarations
class EmbeddingProvider;

// ============================================================================
// MemoryResolvedProviderState (对齐 manager-provider-state.ts:12-18)
// ============================================================================
struct MemoryResolvedProviderState {
    EmbeddingProvider* provider = nullptr;
    std::optional<std::string> fallback_from;
    std::optional<std::string> fallback_reason;
    std::optional<std::string> provider_unavailable_reason;
    // providerRuntime is omitted — C++ uses direct provider methods
};

// ============================================================================
// resolveMemoryPrimaryProviderRequest (对齐 manager-provider-state.ts:20-38)
// ============================================================================
struct PrimaryProviderRequest {
    std::string provider;
    std::string model;
    std::optional<std::string> fallback;
};

PrimaryProviderRequest resolve_memory_primary_provider_request(
    const ResolvedMemorySearchConfig& settings
);

// ============================================================================
// resolveMemoryProviderState (对齐 manager-provider-state.ts:40-53)
// 使用 types.h 中已定义的 EmbeddingProviderResult
// ============================================================================

MemoryResolvedProviderState resolve_memory_provider_state(
    const EmbeddingProviderResult& result
);

// ============================================================================
// applyMemoryFallbackProviderState (对齐 manager-provider-state.ts:55-68)
// ============================================================================

struct ApplyFallbackParams {
    MemoryResolvedProviderState current;
    std::string fallback_from;
    std::string reason;
    EmbeddingProvider* new_provider = nullptr;
};

MemoryResolvedProviderState apply_memory_fallback_provider_state(
    const ApplyFallbackParams& params
);

// ============================================================================
// resolveMemoryFallbackProviderRequest (对齐 manager-provider-state.ts:70-99)
// ============================================================================

struct FallbackProviderRequest {
    std::string provider;
    std::string model;
    bool valid = false;  // false => no fallback available (对齐 openclaw: returns null)
};

FallbackProviderRequest resolve_memory_fallback_provider_request(
    const ResolvedMemorySearchConfig& settings,
    const std::optional<std::string>& current_provider_id
);

} // namespace memory
} // namespace hunterclaw
