#include "hunterclaw/memory/search_manager_factory.h"

namespace hunterclaw {
namespace memory {

// ============================================================================
// MemorySearchManagerFactory Implementation
// ============================================================================

std::unique_ptr<MemorySearchManager> MemorySearchManagerFactory::create(
    const MemoryIndexManagerConfig& config) {
    
    // Create MemoryIndexManager which implements MemorySearchManager interface
    return std::make_unique<MemoryIndexManager>(config);
}

std::unique_ptr<MemorySearchManager> MemorySearchManagerFactory::create_with_fallback(
    const MemoryIndexManagerConfig& primary_config,
    const MemoryIndexManagerConfig& fallback_config) {
    
    try {
        // Try primary config
        auto manager = create(primary_config);
        
        // Probe embedding provider
        auto probe_res = static_cast<MemoryIndexManager*>(manager.get())->probe_embedding_provider();
        if (probe_res.is_ok() && probe_res.value().ok) {
            return manager;
        }
        
        // Primary failed, try fallback
        return create(fallback_config);
    } catch (const std::exception& e) {
        // Primary failed with exception, try fallback
        return create(fallback_config);
    }
}

Result<std::unique_ptr<MemorySearchManager>> MemorySearchManagerFactory::create_auto(
    const AutoConfig& auto_config) {
    
    MemoryIndexManagerConfig config;
    config.db_path = auto_config.db_path;
    config.workspace_dir = auto_config.workspace_dir;
    
    // Auto-select provider: try remote providers first
    EmbeddingProviderOptions provider_opts;
    provider_opts.provider = "auto";
    provider_opts.fallback = auto_config.fallback_provider.value_or("ollama");
    provider_opts.model = auto_config.model;
    provider_opts.remote_api_key = auto_config.api_key;
    provider_opts.remote_base_url = auto_config.base_url;
    
    config.embedding_provider_options = provider_opts;
    
    // Set vector extension path if provided
    config.vector_extension_path = auto_config.vector_extension_path;
    
    // Default search config
    config.max_results = auto_config.max_results.value_or(10);
    config.min_score = auto_config.min_score.value_or(0.7);
    config.fts_enabled = true;
    config.vector_enabled = true;
    config.cache_enabled = true;
    
    // Hybrid weights
    config.vector_weight = 0.7;
    config.text_weight = 0.3;
    
    // MMR config
    config.mmr_enabled = auto_config.mmr_enabled.value_or(true);
    config.mmr_lambda = auto_config.mmr_lambda.value_or(0.5);
    config.mmr_top_k = auto_config.mmr_top_k.value_or(20);
    
    // Temporal decay config
    config.temporal_decay_enabled = auto_config.temporal_decay_enabled.value_or(true);
    config.temporal_decay_half_life_days = auto_config.temporal_decay_half_life_days.value_or(30.0);
    config.temporal_decay_min_score_multiplier = auto_config.temporal_decay_min_multiplier.value_or(0.5);
    
    try {
        auto manager = create(config);
        return Result<std::unique_ptr<MemorySearchManager>>::ok_value(std::move(manager));
    } catch (const std::exception& e) {
        return Result<std::unique_ptr<MemorySearchManager>>::error(
            std::string("Failed to create manager: ") + e.what()
        );
    }
}

// ============================================================================
// Global Manager Cache (对齐 OpenClaw INDEX_CACHE)
// ============================================================================

namespace {
    std::mutex cache_mutex;
    std::map<std::string, std::shared_ptr<MemorySearchManager>> manager_cache;
}

std::shared_ptr<MemorySearchManager> MemorySearchManagerFactory::get_or_create(
    const std::string& cache_key,
    const MemoryIndexManagerConfig& config) {
    
    std::lock_guard<std::mutex> lock(cache_mutex);
    
    auto it = manager_cache.find(cache_key);
    if (it != manager_cache.end()) {
        return it->second;
    }
    
    // Create new manager
    auto manager = create(config);
    auto shared_manager = std::shared_ptr<MemorySearchManager>(std::move(manager));
    
    manager_cache[cache_key] = shared_manager;
    
    return shared_manager;
}

void MemorySearchManagerFactory::clear_cache() {
    std::lock_guard<std::mutex> lock(cache_mutex);
    
    // Close all managers before clearing
    for (auto& [key, manager] : manager_cache) {
        if (auto* idx_manager = dynamic_cast<MemoryIndexManager*>(manager.get())) {
            idx_manager->close();
        }
    }
    
    manager_cache.clear();
}

size_t MemorySearchManagerFactory::cache_size() {
    std::lock_guard<std::mutex> lock(cache_mutex);
    return manager_cache.size();
}

} // namespace memory
} // namespace hunterclaw
