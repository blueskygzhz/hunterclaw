#include "hunterclaw/memory/provider_adapter.h"
#include <algorithm>
#include <set>

namespace hunterclaw {
namespace memory {

// ============================================================================
// Global registry (对齐 OpenClaw 全局 adapter 注册表)
// ============================================================================

static std::mutex adapter_registry_mutex;
static std::vector<std::shared_ptr<MemoryEmbeddingProviderAdapter>> registered_adapters;

// ============================================================================
// registerMemoryEmbeddingProviderAdapter (对齐 registerMemoryEmbeddingProvider)
// ============================================================================

void register_memory_embedding_provider_adapter(
    std::shared_ptr<MemoryEmbeddingProviderAdapter> adapter
) {
    std::lock_guard<std::mutex> lock(adapter_registry_mutex);

    // 去重：移除已有的同 id adapter
    auto it = std::remove_if(registered_adapters.begin(), registered_adapters.end(),
        [&adapter](const std::shared_ptr<MemoryEmbeddingProviderAdapter>& existing) {
            return existing->id() == adapter->id();
        });
    registered_adapters.erase(it, registered_adapters.end());

    registered_adapters.push_back(std::move(adapter));
}

// ============================================================================
// getMemoryEmbeddingProviderAdapter (对齐 getMemoryEmbeddingProvider)
// ============================================================================

std::shared_ptr<MemoryEmbeddingProviderAdapter> get_memory_embedding_provider_adapter(
    const std::string& id
) {
    std::lock_guard<std::mutex> lock(adapter_registry_mutex);
    for (const auto& adapter : registered_adapters) {
        if (adapter->id() == id) {
            return adapter;
        }
    }
    return nullptr;
}

// ============================================================================
// listMemoryEmbeddingProviderAdapters (对齐 listMemoryEmbeddingProviders)
// ============================================================================

std::vector<std::shared_ptr<MemoryEmbeddingProviderAdapter>>
list_memory_embedding_provider_adapters() {
    std::lock_guard<std::mutex> lock(adapter_registry_mutex);
    return registered_adapters;  // copy
}

// ============================================================================
// listAutoSelectAdapters (对齐 embeddings.ts:69-82)
// ============================================================================

std::vector<std::shared_ptr<MemoryEmbeddingProviderAdapter>>
list_auto_select_adapters() {
    auto all = list_memory_embedding_provider_adapters();

    // 对齐: filter(adapter => typeof adapter.autoSelectPriority === "number")
    std::vector<std::shared_ptr<MemoryEmbeddingProviderAdapter>> result;
    for (const auto& adapter : all) {
        if (adapter->auto_select_priority().has_value()) {
            result.push_back(adapter);
        }
    }

    // 对齐: .toSorted((a, b) => a.autoSelectPriority - b.autoSelectPriority)
    std::sort(result.begin(), result.end(),
        [](const std::shared_ptr<MemoryEmbeddingProviderAdapter>& a,
           const std::shared_ptr<MemoryEmbeddingProviderAdapter>& b) {
            return a->auto_select_priority().value_or(9999)
                 < b->auto_select_priority().value_or(9999);
        });

    return result;
}

// ============================================================================
// filterUnregisteredMemoryEmbeddingProviderAdapters (对齐 provider-adapter-registration.ts:5-11)
// ============================================================================

std::vector<std::shared_ptr<MemoryEmbeddingProviderAdapter>>
filter_unregistered_provider_adapters(
    const std::vector<std::shared_ptr<MemoryEmbeddingProviderAdapter>>& builtin_adapters,
    const std::vector<std::shared_ptr<MemoryEmbeddingProviderAdapter>>& existing_adapters
) {
    // 对齐: const existingIds = new Set(params.registeredAdapters.map(a => a.id))
    std::set<std::string> existing_ids;
    for (const auto& a : existing_adapters) {
        existing_ids.insert(a->id());
    }

    // 对齐: return builtinAdapters.filter(a => !existingIds.has(a.id))
    std::vector<std::shared_ptr<MemoryEmbeddingProviderAdapter>> result;
    for (const auto& a : builtin_adapters) {
        if (existing_ids.find(a->id()) == existing_ids.end()) {
            result.push_back(a);
        }
    }
    return result;
}

// ============================================================================
// Builtin Adapter wrappers (对齐 provider-adapters.ts:122-323)
// 将现有的 EmbeddingProviderId 枚举硬编码包装为 MemoryEmbeddingProviderAdapter
// ============================================================================

class BuiltinProviderAdapter : public MemoryEmbeddingProviderAdapter {
public:
    BuiltinProviderAdapter(
        std::string id_str,
        std::string default_model_str,
        std::string transport_str,
        std::optional<int> priority,
        bool allow_explicit,
        EmbeddingProviderId provider_id
    ) : id_(std::move(id_str)),
        default_model_(std::move(default_model_str)),
        transport_(std::move(transport_str)),
        priority_(priority),
        allow_explicit_(allow_explicit),
        provider_id_(provider_id) {}

    std::string id() const override { return id_; }
    std::string default_model() const override { return default_model_; }
    std::string transport() const override { return transport_; }
    std::optional<int> auto_select_priority() const override { return priority_; }
    bool allow_explicit_when_configured_auto() const override { return allow_explicit_; }

    bool should_continue_auto_selection(const std::string& error) const override {
        // 对齐: isMissingApiKeyError — remote providers continue on API key error
        if (transport_ == "remote") {
            return error.find("No API key found") != std::string::npos ||
                   error.find("api_key") != std::string::npos;
        }
        // local: always continue
        return transport_ == "local";
    }

    core::Result<ProviderAdapterCreateResult> create(
        const EmbeddingProviderOptions& options) override
    {
        // Delegate to the existing EmbeddingProviderFactory (preserves current behavior)
        auto result = EmbeddingProviderFactory::create(options);
        if (result.is_err()) {
            return core::Result<ProviderAdapterCreateResult>::Err(result.error);
        }
        auto& epresult = result.get();
        ProviderAdapterCreateResult out;
        out.provider = epresult.provider;
        out.runtime_id = id_;
        return core::Result<ProviderAdapterCreateResult>::Ok(std::move(out));
    }

private:
    std::string id_;
    std::string default_model_;
    std::string transport_;
    std::optional<int> priority_;
    bool allow_explicit_;
    EmbeddingProviderId provider_id_;
};

// ============================================================================
// registerBuiltInMemoryEmbeddingProviders (对齐 provider-adapters.ts:335-347)
// ============================================================================

void register_builtin_memory_embedding_providers() {
    // 对齐 builtinMemoryEmbeddingProviderAdapters (provider-adapters.ts:317-323)
    // 顺序: local(10), openai(20), gemini(30), voyage(40), mistral(50)
    std::vector<std::shared_ptr<MemoryEmbeddingProviderAdapter>> builtins = {
        std::make_shared<BuiltinProviderAdapter>(
            "local", DEFAULT_LOCAL_MODEL, "local", 10, false, EmbeddingProviderId::Local),
        std::make_shared<BuiltinProviderAdapter>(
            "openai", "text-embedding-3-small", "remote", 20, true, EmbeddingProviderId::OpenAI),
        std::make_shared<BuiltinProviderAdapter>(
            "gemini", "gemini-embedding-001", "remote", 30, true, EmbeddingProviderId::Gemini),
        std::make_shared<BuiltinProviderAdapter>(
            "voyage", "voyage-2", "remote", 40, true, EmbeddingProviderId::Voyage),
        std::make_shared<BuiltinProviderAdapter>(
            "mistral", "mistral-embed", "remote", 50, true, EmbeddingProviderId::Mistral),
    };

    // 对齐: filterUnregisteredMemoryEmbeddingProviderAdapters + register
    auto already_registered = list_memory_embedding_provider_adapters();
    auto to_register = filter_unregistered_provider_adapters(builtins, already_registered);

    for (auto& adapter : to_register) {
        register_memory_embedding_provider_adapter(std::move(adapter));
    }
}

// ============================================================================
// getBuiltinMemoryEmbeddingProviderDoctorMetadata (对齐 provider-adapters.ts:349-364)
// ============================================================================

std::optional<ProviderDoctorMetadata> get_builtin_provider_doctor_metadata(
    const std::string& provider_id
) {
    auto adapter = get_memory_embedding_provider_adapter(provider_id);
    if (!adapter) {
        return std::nullopt;
    }

    std::string auth_id = resolve_embedding_auth_provider_id(adapter->id());

    ProviderDoctorMetadata meta;
    meta.provider_id = adapter->id();
    meta.auth_provider_id = auth_id;
    meta.transport = adapter->transport();
    meta.auto_select_priority = adapter->auto_select_priority();

    // 对齐: envVars = getProviderEnvVars(authProviderId)
    // C++ 简化: 根据 provider 返回常见环境变量名
    if (auth_id == "openai") {
        meta.env_vars = {"OPENAI_API_KEY"};
    } else if (auth_id == "google") {
        meta.env_vars = {"GOOGLE_API_KEY", "GEMINI_API_KEY"};
    } else if (auth_id == "voyage") {
        meta.env_vars = {"VOYAGE_API_KEY"};
    } else if (auth_id == "mistral") {
        meta.env_vars = {"MISTRAL_API_KEY"};
    }

    return meta;
}

// ============================================================================
// listBuiltinAutoSelectMemoryEmbeddingProviderDoctorMetadata (对齐 provider-adapters.ts:366-377)
// ============================================================================

std::vector<ProviderDoctorMetadata> list_builtin_auto_select_provider_doctor_metadata() {
    auto adapters = list_auto_select_adapters();
    std::vector<ProviderDoctorMetadata> result;
    result.reserve(adapters.size());

    for (const auto& adapter : adapters) {
        ProviderDoctorMetadata meta;
        meta.provider_id = adapter->id();
        meta.auth_provider_id = resolve_embedding_auth_provider_id(adapter->id());
        meta.transport = adapter->transport();
        meta.auto_select_priority = adapter->auto_select_priority();

        if (meta.auth_provider_id == "openai") {
            meta.env_vars = {"OPENAI_API_KEY"};
        } else if (meta.auth_provider_id == "google") {
            meta.env_vars = {"GOOGLE_API_KEY", "GEMINI_API_KEY"};
        } else if (meta.auth_provider_id == "voyage") {
            meta.env_vars = {"VOYAGE_API_KEY"};
        } else if (meta.auth_provider_id == "mistral") {
            meta.env_vars = {"MISTRAL_API_KEY"};
        }

        result.push_back(std::move(meta));
    }

    return result;
}

}  // namespace memory
}  // namespace hunterclaw
