#include "hunterclaw/memory/registry.h"
#include <spdlog/spdlog.h>

namespace hunterclaw {
namespace memory {

namespace {
    auto log() {
        static auto logger = spdlog::get("memory") ? spdlog::get("memory") : spdlog::default_logger();
        return logger;
    }
}

// ============================================================================
// MemoryProviderRegistry Implementation
// ============================================================================

MemoryProviderRegistry& MemoryProviderRegistry::instance() {
    static MemoryProviderRegistry instance;
    return instance;
}

void MemoryProviderRegistry::register_provider(
    const std::string& id,
    std::shared_ptr<MemoryProvider> provider
) {
    if (!provider) {
        log()->error("[MemoryProviderRegistry] Cannot register null provider");
        return;
    }
    
    std::unique_lock lock(mutex_);
    
    if (providers_.count(id) > 0) {
        log()->warn("[MemoryProviderRegistry] Provider '{}' already registered, replacing", id);
    }
    
    providers_[id] = provider;
    log()->info("[MemoryProviderRegistry] Registered provider: {} ({})", 
               id, provider->name());
}

void MemoryProviderRegistry::unregister_provider(const std::string& id) {
    std::unique_lock lock(mutex_);
    
    if (providers_.erase(id) > 0) {
        log()->info("[MemoryProviderRegistry] Unregistered provider: {}", id);
    } else {
        log()->warn("[MemoryProviderRegistry] Provider '{}' not found", id);
    }
}

std::shared_ptr<MemoryProvider> MemoryProviderRegistry::get(const std::string& id) const {
    std::shared_lock lock(mutex_);
    
    auto it = providers_.find(id);
    if (it != providers_.end()) {
        return it->second;
    }
    
    log()->warn("[MemoryProviderRegistry] Provider '{}' not found", id);
    return nullptr;
}

std::vector<std::string> MemoryProviderRegistry::list() const {
    std::shared_lock lock(mutex_);
    
    std::vector<std::string> ids;
    ids.reserve(providers_.size());
    
    for (const auto& [id, provider] : providers_) {
        ids.push_back(id);
    }
    
    return ids;
}

bool MemoryProviderRegistry::has(const std::string& id) const {
    std::shared_lock lock(mutex_);
    return providers_.count(id) > 0;
}

void MemoryProviderRegistry::clear() {
    std::unique_lock lock(mutex_);
    providers_.clear();
    log()->info("[MemoryProviderRegistry] Cleared all providers");
}

} // namespace memory
} // namespace hunterclaw
