#include "hunterclaw/memory/provider_registry.h"
#include "hunterclaw/memory/sqlite_plugin.h"  // SQLiteMemoryProvider 定义
#include "hunterclaw/memory/memory.h"  // 使用现有的 MemoryManager
#include "hunterclaw/memory/sqlite_storage.h"
#include <spdlog/spdlog.h>
#include <stdexcept>
#include <mutex>
#include <chrono>
#include <optional>

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

// ========================================
// MemoryProviderRegistry 实现 (对齐 OpenClaw)
// ========================================

MemoryProviderRegistry& MemoryProviderRegistry::instance() {
    static MemoryProviderRegistry instance;
    return instance;
}

core::Result<bool> MemoryProviderRegistry::register_provider(
    const std::string& provider_id,
    std::shared_ptr<MemoryProvider> provider
) {
    if (!provider) {
        log()->error("[MemoryProviderRegistry] Cannot register null provider");
        return core::Result<bool>::Err("Provider is null");
    }
    
    std::lock_guard<std::mutex> lock(mutex_);
    
    // 检查是否已存在
    if (providers_.find(provider_id) != providers_.end()) {
        log()->warn("[MemoryProviderRegistry] Provider '{}' already registered, replacing", provider_id);
        // 对齐 OpenClaw: 允许覆盖已有的 provider
    }
    
    providers_[provider_id] = provider;
    
    // 如果是第一个 provider,设置为默认
    if (default_provider_id_.empty()) {
        default_provider_id_ = provider_id;
    }
    
    log()->info("[MemoryProviderRegistry] Registered provider: {} ({})", 
               provider_id, provider->provider_name());
    
    return core::Result<bool>::Ok(true);
}

core::Result<bool> MemoryProviderRegistry::unregister_provider(
    const std::string& provider_id
) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = providers_.find(provider_id);
    if (it == providers_.end()) {
        return core::Result<bool>::Err("Provider '" + provider_id + "' not found");
    }
    
    providers_.erase(it);
    
    // 如果删除的是默认 provider,清空默认
    if (default_provider_id_ == provider_id) {
        default_provider_id_.clear();
        // 选择新的默认 provider
        if (!providers_.empty()) {
            default_provider_id_ = providers_.begin()->first;
        }
    }
    
    return core::Result<bool>::Ok(true);
}

bool MemoryProviderRegistry::has_provider(const std::string& provider_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return providers_.find(provider_id) != providers_.end();
}

core::Result<std::shared_ptr<MemoryProvider>> MemoryProviderRegistry::get_provider(
    const std::string& provider_id
) const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = providers_.find(provider_id);
    if (it == providers_.end()) {
        return core::Result<std::shared_ptr<MemoryProvider>>::Err("Provider '" + provider_id + "' not found");
    }
    
    return core::Result<std::shared_ptr<MemoryProvider>>::Ok(it->second);
}

core::Result<std::shared_ptr<MemoryProvider>> MemoryProviderRegistry::get_default_provider() const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (default_provider_id_.empty()) {
        return core::Result<std::shared_ptr<MemoryProvider>>::Err("No default provider set");
    }
    
    auto it = providers_.find(default_provider_id_);
    if (it == providers_.end()) {
        return core::Result<std::shared_ptr<MemoryProvider>>::Err("Default provider not found");
    }
    
    return core::Result<std::shared_ptr<MemoryProvider>>::Ok(it->second);
}

void MemoryProviderRegistry::set_default_provider(const std::string& provider_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    default_provider_id_ = provider_id;
}

std::vector<std::string> MemoryProviderRegistry::list_providers() const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    std::vector<std::string> provider_ids;
    for (const auto& [id, _] : providers_) {
        provider_ids.push_back(id);
    }
    
    return provider_ids;
}

std::vector<MemoryProviderRegistry::ProviderInfo> 
MemoryProviderRegistry::get_all_provider_info() const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    std::vector<ProviderInfo> infos;
    
    for (const auto& [id, provider] : providers_) {
        ProviderInfo info;
        info.id = id;
        info.name = provider->provider_name();
        info.version = provider->provider_version();
        info.is_default = (id == default_provider_id_);
        
        // 尝试获取统计信息
        auto stats_result = provider->get_stats();
        if (stats_result.is_ok()) {
            const auto& stats = stats_result.value;
            info.memory_count = stats.total_memories;
            info.total_size = stats.total_size;
        }
        
        infos.push_back(info);
    }
    
    return infos;
}

void MemoryProviderRegistry::register_factory(
    const std::string& provider_type,
    ProviderFactory factory
) {
    std::lock_guard<std::mutex> lock(mutex_);
    factories_[provider_type] = factory;
}

core::Result<std::string> MemoryProviderRegistry::create_and_register_provider(
    const std::string& provider_type,
    const std::string& provider_id,
    const std::map<std::string, std::string>& config
) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    // 查找工厂
    auto it = factories_.find(provider_type);
    if (it == factories_.end()) {
        return core::Result<std::string>::Err("No factory registered for provider type '" + provider_type + "'");
    }
    
    // 创建 provider
    auto result = it->second(config);
    if (result.is_err()) {
        return core::Result<std::string>::Err("Failed to create provider: " + result.error);
    }
    
    // 注册
    providers_[provider_id] = result.value;
    
    // 如果是第一个 provider,设置为默认
    if (default_provider_id_.empty()) {
        default_provider_id_ = provider_id;
    }
    
    return core::Result<std::string>::Ok(provider_id);
}

core::Result<std::shared_ptr<MemoryProvider>> 
MemoryProviderRegistry::create_session_provider(
    const std::string& underlying_provider_id,
    const SessionMemoryContext& context
) {
    auto provider_result = get_provider(underlying_provider_id);
    if (provider_result.is_err()) {
        return core::Result<std::shared_ptr<MemoryProvider>>::Err(provider_result.error);
    }
    
    // TODO: 创建 Session-scoped Provider
    // SessionMemoryProvider 需要单独实现
    // auto session_provider = std::make_shared<SessionMemoryProvider>(
    //     provider_result.value,
    //     context
    // );
    
    // 暂时直接返回底层provider
    return provider_result;
}

void MemoryProviderRegistry::shutdown_all() {
    std::lock_guard<std::mutex> lock(mutex_);
    providers_.clear();
    factories_.clear();
    default_provider_id_.clear();
}

// ========================================
// MemoryProviderFactory 实现
// Note: SQLiteMemoryProvider 实现在 sqlite_plugin.cc 中
// ========================================

core::Result<std::shared_ptr<MemoryProvider>> create_sqlite(
    const std::string& db_path,
    const std::string& embedding_provider,
    const std::map<std::string, std::string>& config
) {
    try {
        auto provider = std::make_shared<SQLiteMemoryProvider>(
            db_path,
            embedding_provider,
            config
        );
        return core::Result<std::shared_ptr<MemoryProvider>>::Ok(std::static_pointer_cast<MemoryProvider>(provider));
    } catch (const std::exception& e) {
        return core::Result<std::shared_ptr<MemoryProvider>>::Err(std::string("Failed to create SQLite provider: ") + e.what());
    }
}

core::Result<std::shared_ptr<MemoryProvider>> create_memos_cloud(
    const std::string& api_key,
    const std::string& endpoint
) {
    // TODO: 实现 MemOS Cloud Provider
    return core::Result<std::shared_ptr<MemoryProvider>>::Err("MemOS Cloud provider not implemented yet");
}

core::Result<std::shared_ptr<MemoryProvider>> create_supabase(
    const std::string& url,
    const std::string& api_key
) {
    // TODO: 实现 Supabase Provider
    return core::Result<std::shared_ptr<MemoryProvider>>::Err("Supabase provider not implemented yet");
}

core::Result<std::shared_ptr<MemoryProvider>> create_from_config(
    const std::string& provider_type,
    const std::map<std::string, std::string>& config
) {
    if (provider_type == "sqlite") {
        std::string db_path = config.count("db_path") ? config.at("db_path") : "./memory.db";
        std::string embedding = config.count("embedding") ? config.at("embedding") : "openai";
        return create_sqlite(db_path, embedding, config);
    } else if (provider_type == "memos_cloud") {
        std::string api_key = config.count("api_key") ? config.at("api_key") : "";
        std::string endpoint = config.count("endpoint") ? config.at("endpoint") : "https://api.memos.cloud";
        return create_memos_cloud(api_key, endpoint);
    } else if (provider_type == "supabase") {
        std::string url = config.count("url") ? config.at("url") : "";
        std::string api_key = config.count("api_key") ? config.at("api_key") : "";
        return create_supabase(url, api_key);
    } else {
        return core::Result<std::shared_ptr<MemoryProvider>>::Err("Unknown provider type: " + provider_type);
    }
}

} // namespace memory
} // namespace hunterclaw
