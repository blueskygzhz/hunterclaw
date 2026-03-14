#include "hunterclaw/memory/registry.h"
#include "hunterclaw/memory/memory.h"  // 使用现有的 MemoryManager
#include <stdexcept>

namespace hunterclaw {
namespace memory {

// ========================================
// MemoryProviderRegistry 实现
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
        return core::Err("Provider is null");
    }
    
    std::lock_guard<std::mutex> lock(mutex_);
    
    // 检查是否已存在
    if (providers_.find(provider_id) != providers_.end()) {
        return core::Err("Provider '" + provider_id + "' already registered");
    }
    
    providers_[provider_id] = provider;
    
    // 如果是第一个 provider,设置为默认
    if (default_provider_id_.empty()) {
        default_provider_id_ = provider_id;
    }
    
    return core::Ok(true);
}

core::Result<bool> MemoryProviderRegistry::unregister_provider(
    const std::string& provider_id
) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = providers_.find(provider_id);
    if (it == providers_.end()) {
        return core::Err("Provider '" + provider_id + "' not found");
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
    
    return core::Ok(true);
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
        return core::Err("Provider '" + provider_id + "' not found");
    }
    
    return core::Ok(it->second);
}

core::Result<std::shared_ptr<MemoryProvider>> MemoryProviderRegistry::get_default_provider() const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (default_provider_id_.empty()) {
        return core::Err("No default provider set");
    }
    
    auto it = providers_.find(default_provider_id_);
    if (it == providers_.end()) {
        return core::Err("Default provider not found");
    }
    
    return core::Ok(it->second);
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
        if (stats_result) {
            const auto& stats = stats_result.value();
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
        return core::Err("No factory registered for provider type '" + provider_type + "'");
    }
    
    // 创建 provider
    auto result = it->second(config);
    if (!result) {
        return core::Err("Failed to create provider: " + result.error());
    }
    
    // 注册
    providers_[provider_id] = result.value();
    
    // 如果是第一个 provider,设置为默认
    if (default_provider_id_.empty()) {
        default_provider_id_ = provider_id;
    }
    
    return core::Ok(provider_id);
}

core::Result<std::shared_ptr<MemoryProvider>> 
MemoryProviderRegistry::create_session_provider(
    const std::string& underlying_provider_id,
    const SessionMemoryContext& context
) {
    auto provider_result = get_provider(underlying_provider_id);
    if (!provider_result) {
        return core::Err(provider_result.error());
    }
    
    // 创建 Session-scoped Provider
    auto session_provider = std::make_shared<SessionMemoryProvider>(
        provider_result.value(),
        context
    );
    
    return core::Ok(std::static_pointer_cast<MemoryProvider>(session_provider));
}

void MemoryProviderRegistry::shutdown_all() {
    std::lock_guard<std::mutex> lock(mutex_);
    providers_.clear();
    factories_.clear();
    default_provider_id_.clear();
}

// ========================================
// SQLiteMemoryProvider 实现
// ========================================

struct SQLiteMemoryProvider::Impl {
    std::shared_ptr<MemoryManager> manager;
    std::string db_path;
    std::string embedding_provider;
    std::map<std::string, std::string> config;
};

SQLiteMemoryProvider::SQLiteMemoryProvider(
    const std::string& db_path,
    const std::string& embedding_provider,
    const std::map<std::string, std::string>& config
) : impl_(std::make_unique<Impl>()) {
    impl_->db_path = db_path;
    impl_->embedding_provider = embedding_provider;
    impl_->config = config;
    
    // 创建 storage
    auto storage = SQLiteVectorStorage::create(db_path);
    
    // 创建 embedding provider (TODO: 从工厂创建)
    // auto embedding = EmbeddingProviderFactory::create_openai(config["api_key"]);
    
    // 创建 MemoryManager
    SearchConfig search_config;
    // impl_->manager = MemoryManager::create(storage, embedding, search_config);
    
    // 临时: 如果没有 embedding provider,只创建 storage
    // 实际使用时需要完整实现
}

SQLiteMemoryProvider::~SQLiteMemoryProvider() = default;

core::Result<std::string> SQLiteMemoryProvider::add_memory(
    const std::string& content,
    const std::map<std::string, std::string>& metadata
) {
    if (!impl_->manager) {
        return core::Err("Memory manager not initialized");
    }
    
    try {
        std::string id = impl_->manager->add_memory(content, metadata);
        return core::Ok(id);
    } catch (const std::exception& e) {
        return core::Err(std::string("Failed to add memory: ") + e.what());
    }
}

core::Result<std::vector<Memory>> SQLiteMemoryProvider::search_memories(
    const std::string& query,
    const SearchOptions& options
) {
    if (!impl_->manager) {
        return core::Err("Memory manager not initialized");
    }
    
    try {
        auto results = impl_->manager->search(query);
        
        // 转换为 Memory 结构
        std::vector<Memory> memories;
        for (const auto& result : results) {
            Memory mem;
            mem.id = result.id;
            mem.content = result.content;
            mem.metadata = result.metadata;
            mem.embedding = result.embedding;
            mem.score = result.score;
            
            // 从 timestamp 转换
            mem.created_at = std::chrono::system_clock::from_time_t(result.timestamp / 1000);
            mem.updated_at = mem.created_at;
            
            memories.push_back(mem);
        }
        
        return core::Ok(memories);
    } catch (const std::exception& e) {
        return core::Err(std::string("Failed to search memories: ") + e.what());
    }
}

core::Result<bool> SQLiteMemoryProvider::remove_memory(const std::string& id) {
    if (!impl_->manager) {
        return core::Err("Memory manager not initialized");
    }
    
    try {
        bool success = impl_->manager->remove_memory(id);
        return core::Ok(success);
    } catch (const std::exception& e) {
        return core::Err(std::string("Failed to remove memory: ") + e.what());
    }
}

core::Result<bool> SQLiteMemoryProvider::update_memory(
    const std::string& id,
    const std::string& content,
    const std::map<std::string, std::string>& metadata
) {
    // MemoryManager 目前不支持 update
    // 实现为 remove + add
    auto remove_result = remove_memory(id);
    if (!remove_result) {
        return remove_result;
    }
    
    auto add_result = add_memory(content, metadata);
    if (!add_result) {
        return core::Err("Failed to update memory: " + add_result.error());
    }
    
    return core::Ok(true);
}

core::Result<bool> SQLiteMemoryProvider::clear_memories() {
    if (!impl_->manager) {
        return core::Err("Memory manager not initialized");
    }
    
    try {
        // TODO: MemoryManager 需要添加 clear 方法
        return core::Err("Clear not implemented yet");
    } catch (const std::exception& e) {
        return core::Err(std::string("Failed to clear memories: ") + e.what());
    }
}

core::Result<MemoryStats> SQLiteMemoryProvider::get_stats() {
    MemoryStats stats;
    
    // TODO: 从 MemoryManager 获取实际统计
    stats.total_memories = 0;
    stats.total_size = 0;
    stats.last_updated = std::chrono::system_clock::now();
    
    return core::Ok(stats);
}

core::Result<Memory> SQLiteMemoryProvider::get_memory(const std::string& id) {
    // TODO: 实现按 ID 查找
    return core::Err("Get memory by ID not implemented yet");
}

// ========================================
// MemoryProviderFactory 实现
// ========================================

core::Result<std::shared_ptr<MemoryProvider>> MemoryProviderFactory::create_sqlite(
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
        return core::Ok(std::static_pointer_cast<MemoryProvider>(provider));
    } catch (const std::exception& e) {
        return core::Err(std::string("Failed to create SQLite provider: ") + e.what());
    }
}

core::Result<std::shared_ptr<MemoryProvider>> MemoryProviderFactory::create_memos_cloud(
    const std::string& api_key,
    const std::string& endpoint
) {
    // TODO: 实现 MemOS Cloud Provider
    return core::Err("MemOS Cloud provider not implemented yet");
}

core::Result<std::shared_ptr<MemoryProvider>> MemoryProviderFactory::create_supabase(
    const std::string& url,
    const std::string& api_key
) {
    // TODO: 实现 Supabase Provider
    return core::Err("Supabase provider not implemented yet");
}

core::Result<std::shared_ptr<MemoryProvider>> MemoryProviderFactory::create_from_config(
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
        return core::Err("Unknown provider type: " + provider_type);
    }
}

} // namespace memory
} // namespace hunterclaw
