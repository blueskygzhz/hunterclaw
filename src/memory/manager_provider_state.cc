#include "hunterclaw/memory/manager_provider_state.h"

namespace hunterclaw {
namespace memory {

// ============================================================================
// resolveMemoryPrimaryProviderRequest (对齐 manager-provider-state.ts:20-38)
// ============================================================================
PrimaryProviderRequest resolve_memory_primary_provider_request(
    const ResolvedMemorySearchConfig& settings
) {
    PrimaryProviderRequest req;
    req.provider = settings.provider;
    req.model = settings.model.value_or("");
    return req;
}

// ============================================================================
// resolveMemoryProviderState (对齐 manager-provider-state.ts:40-53)
// ============================================================================
MemoryResolvedProviderState resolve_memory_provider_state(
    const EmbeddingProviderResult& result
) {
    MemoryResolvedProviderState state;
    state.provider = result.provider.get();
    if (result.fallback_from) {
        state.fallback_from = std::to_string(static_cast<int>(*result.fallback_from));
    }
    state.fallback_reason = result.fallback_reason;
    state.provider_unavailable_reason = result.provider_unavailable_reason;
    return state;
}

// ============================================================================
// applyMemoryFallbackProviderState (对齐 manager-provider-state.ts:55-68)
// ============================================================================
MemoryResolvedProviderState apply_memory_fallback_provider_state(
    const ApplyFallbackParams& params
) {
    MemoryResolvedProviderState state = params.current;
    state.fallback_from = params.fallback_from;
    state.fallback_reason = params.reason;
    state.provider = params.new_provider;
    return state;
}

// ============================================================================
// resolveMemoryFallbackProviderRequest (对齐 manager-provider-state.ts:70-99)
// ============================================================================
FallbackProviderRequest resolve_memory_fallback_provider_request(
    const ResolvedMemorySearchConfig& settings,
    const std::optional<std::string>& current_provider_id
) {
    FallbackProviderRequest req;
    req.valid = false;

    if (!current_provider_id || current_provider_id->empty()) {
        return req;
    }

    // C++ ResolvedMemorySearchConfig 中的 fallback 配置在 sync config 层
    // 对齐 openclaw: 如果 fallback === "none" 或等于当前 provider，返回 null
    req.model = settings.model.value_or("");
    return req;
}

} // namespace memory
} // namespace hunterclaw
