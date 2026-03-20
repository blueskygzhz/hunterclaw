#include "hunterclaw/memory/provider_registry.h"
#include "hunterclaw/memory/memory_index_manager.h"
#include <spdlog/spdlog.h>
#include <stdexcept>
#include <mutex>

namespace hunterclaw {
namespace memory {

namespace {
    auto log() {
        auto logger = spdlog::get("memory");
        if (!logger) {
            logger = spdlog::stdout_logger_mt("memory");
        }
        return logger;
    }
}

MemoryProviderRegistry& MemoryProviderRegistry::instance() {
    static MemoryProviderRegistry instance;
    return instance;
}

core::Result<bool> MemoryProviderRegistry::register_provider(
    const std::string& agent_id,
    std::shared_ptr<MemorySearchManager> manager
) {
    if (!manager) {
        return core::Result<bool>::Err("Manager is null");
    }
    std::lock_guard<std::mutex> lock(mutex_);
    providers_[agent_id] = manager;
    if (default_provider_id_.empty()) {
        default_provider_id_ = agent_id;
    }
    log()->info("[MemoryProviderRegistry] Registered manager for agent: {}", agent_id);
    return core::Result<bool>::Ok(true);
}

core::Result<bool> MemoryProviderRegistry::unregister_provider(
    const std::string& agent_id
) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = providers_.find(agent_id);
    if (it == providers_.end()) {
        return core::Result<bool>::Err("Agent '" + agent_id + "' not found");
    }
    providers_.erase(it);
    if (default_provider_id_ == agent_id) {
        default_provider_id_.clear();
        if (!providers_.empty()) {
            default_provider_id_ = providers_.begin()->first;
        }
    }
    return core::Result<bool>::Ok(true);
}

bool MemoryProviderRegistry::has_provider(const std::string& agent_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return providers_.find(agent_id) != providers_.end();
}

core::Result<std::shared_ptr<MemorySearchManager>> MemoryProviderRegistry::get_provider(
    const std::string& agent_id
) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = providers_.find(agent_id);
    if (it == providers_.end()) {
        return core::Result<std::shared_ptr<MemorySearchManager>>::Err(
            "Agent '" + agent_id + "' not found"
        );
    }
    return core::Result<std::shared_ptr<MemorySearchManager>>::Ok(it->second);
}

core::Result<std::shared_ptr<MemorySearchManager>> MemoryProviderRegistry::get_default_provider() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (default_provider_id_.empty()) {
        return core::Result<std::shared_ptr<MemorySearchManager>>::Err("No default provider set");
    }
    auto it = providers_.find(default_provider_id_);
    if (it == providers_.end()) {
        return core::Result<std::shared_ptr<MemorySearchManager>>::Err("Default provider not found");
    }
    return core::Result<std::shared_ptr<MemorySearchManager>>::Ok(it->second);
}

void MemoryProviderRegistry::set_default_provider(const std::string& agent_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    default_provider_id_ = agent_id;
}

std::vector<std::string> MemoryProviderRegistry::list_providers() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> ids;
    for (const auto& [id, _] : providers_) {
        ids.push_back(id);
    }
    return ids;
}

std::vector<MemoryProviderRegistry::ProviderInfo>
MemoryProviderRegistry::get_all_provider_info() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<ProviderInfo> infos;
    for (const auto& [id, manager] : providers_) {
        ProviderInfo info;
        info.id = id;
        info.is_default = (id == default_provider_id_);
        auto status = manager->status();
        info.backend = status.backend;
        info.provider = status.provider;
        infos.push_back(info);
    }
    return infos;
}

void MemoryProviderRegistry::register_factory(
    const std::string& type,
    ManagerFactory factory
) {
    std::lock_guard<std::mutex> lock(mutex_);
    factories_[type] = factory;
}

core::Result<std::string> MemoryProviderRegistry::create_and_register_provider(
    const std::string& type,
    const std::string& agent_id,
    const std::map<std::string, std::string>& config
) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = factories_.find(type);
    if (it == factories_.end()) {
        return core::Result<std::string>::Err("No factory for type '" + type + "'");
    }
    auto result = it->second(config);
    if (result.is_err()) {
        return core::Result<std::string>::Err("Failed to create manager: " + result.error);
    }
    providers_[agent_id] = result.get();
    if (default_provider_id_.empty()) {
        default_provider_id_ = agent_id;
    }
    return core::Result<std::string>::Ok(agent_id);
}

void MemoryProviderRegistry::shutdown_all() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [id, manager] : providers_) {
        if (manager) {
            manager->close();
        }
    }
    providers_.clear();
    factories_.clear();
    default_provider_id_.clear();
}

} // namespace memory
} // namespace hunterclaw
