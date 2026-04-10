#pragma once

#include "hunterclaw/memory/embedding_provider.h"
#include "hunterclaw/memory/types.h"
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

// 严格对齐 OpenClaw:
// - extensions/memory-core/src/memory/provider-adapters.ts (389行)
// - extensions/memory-core/src/memory/provider-adapter-registration.ts (12行)
// - extensions/memory-core/src/memory/embeddings.ts (185行)
// Provider Adapter 动态注册系统

namespace hunterclaw {
namespace memory {

// ============================================================================
// MemoryEmbeddingProviderAdapter (对齐 provider-adapters.ts:20 MemoryEmbeddingProviderAdapter)
// ============================================================================
struct ProviderAdapterCreateResult {
    std::shared_ptr<EmbeddingProvider> provider;
    // runtime 信息 (对齐 MemoryEmbeddingProviderRuntime)
    std::string runtime_id;
    std::optional<std::string> cache_key_data;  // JSON 序列化的 cacheKeyData
};

class MemoryEmbeddingProviderAdapter {
public:
    virtual ~MemoryEmbeddingProviderAdapter() = default;

    // 对齐 adapter.id
    virtual std::string id() const = 0;

    // 对齐 adapter.defaultModel
    virtual std::string default_model() const = 0;

    // 对齐 adapter.transport — "local" | "remote"
    virtual std::string transport() const = 0;

    // 对齐 adapter.autoSelectPriority (lower = higher priority, nullopt = no auto-select)
    virtual std::optional<int> auto_select_priority() const { return std::nullopt; }

    // 对齐 adapter.allowExplicitWhenConfiguredAuto
    virtual bool allow_explicit_when_configured_auto() const { return false; }

    // 对齐 adapter.shouldContinueAutoSelection(err)
    virtual bool should_continue_auto_selection(const std::string& error) const { return false; }

    // 对齐 adapter.formatSetupError(err)
    virtual std::string format_setup_error(const std::string& error) const { return error; }

    // 对齐 adapter.create(options)
    virtual core::Result<ProviderAdapterCreateResult> create(
        const EmbeddingProviderOptions& options) = 0;
};

// ============================================================================
// BuiltinMemoryEmbeddingProviderDoctorMetadata (对齐 provider-adapters.ts:28-34)
// ============================================================================
struct ProviderDoctorMetadata {
    std::string provider_id;
    std::string auth_provider_id;
    std::vector<std::string> env_vars;
    std::string transport;  // "local" | "remote"
    std::optional<int> auto_select_priority;
};

// ============================================================================
// Provider Registry (对齐 memory-core-host-engine-embeddings 全局注册表)
// ============================================================================

// 注册 adapter (对齐 registerMemoryEmbeddingProvider)
void register_memory_embedding_provider_adapter(
    std::shared_ptr<MemoryEmbeddingProviderAdapter> adapter
);

// 获取已注册的 adapter by id (对齐 getMemoryEmbeddingProvider)
std::shared_ptr<MemoryEmbeddingProviderAdapter> get_memory_embedding_provider_adapter(
    const std::string& id
);

// 列举所有已注册 adapters (对齐 listMemoryEmbeddingProviders)
std::vector<std::shared_ptr<MemoryEmbeddingProviderAdapter>> list_memory_embedding_provider_adapters();

// 列举可自动选择的 adapters (对齐 listAutoSelectAdapters)
std::vector<std::shared_ptr<MemoryEmbeddingProviderAdapter>> list_auto_select_adapters();

// ============================================================================
// filterUnregisteredMemoryEmbeddingProviderAdapters (对齐 provider-adapter-registration.ts:5-11)
// ============================================================================
std::vector<std::shared_ptr<MemoryEmbeddingProviderAdapter>>
filter_unregistered_provider_adapters(
    const std::vector<std::shared_ptr<MemoryEmbeddingProviderAdapter>>& builtin_adapters,
    const std::vector<std::shared_ptr<MemoryEmbeddingProviderAdapter>>& registered_adapters
);

// ============================================================================
// registerBuiltInMemoryEmbeddingProviders (对齐 provider-adapters.ts:335-347)
// ============================================================================
void register_builtin_memory_embedding_providers();

// ============================================================================
// getBuiltinMemoryEmbeddingProviderDoctorMetadata (对齐 provider-adapters.ts:349-364)
// ============================================================================
std::optional<ProviderDoctorMetadata> get_builtin_provider_doctor_metadata(
    const std::string& provider_id
);

// 对齐 listBuiltinAutoSelectMemoryEmbeddingProviderDoctorMetadata (provider-adapters.ts:366-377)
std::vector<ProviderDoctorMetadata> list_builtin_auto_select_provider_doctor_metadata();

// ============================================================================
// resolveMemoryEmbeddingAuthProviderId (对齐 provider-adapters.ts:118-120)
// ============================================================================
inline std::string resolve_embedding_auth_provider_id(const std::string& provider_id) {
    return provider_id == "gemini" ? "google" : provider_id;
}

}  // namespace memory
}  // namespace hunterclaw
