#include "hunterclaw/memory/search_manager.h"
#include <stdexcept>
#include <sstream>
#include <algorithm>

namespace hunterclaw {
namespace memory {

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// 1. Memory Types Implementation
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

std::string to_string(MemorySource source) {
    switch (source) {
        case MemorySource::MEMORY:   return "memory";
        case MemorySource::SESSIONS: return "sessions";
        default: return "unknown";
    }
}

MemorySource parse_memory_source(const std::string& source_str) {
    if (source_str == "memory") return MemorySource::MEMORY;
    if (source_str == "sessions") return MemorySource::SESSIONS;
    throw std::invalid_argument("Invalid memory source: " + source_str);
}

std::string MemoryProviderStatus::to_json() const {
    std::ostringstream oss;
    oss << "{\n";
    oss << "  \"backend\": \"" << (backend == Backend::BUILTIN ? "builtin" : "qmd") << "\",\n";
    oss << "  \"provider\": \"" << provider << "\",\n";
    oss << "  \"model\": \"" << model << "\",\n";
    oss << "  \"files\": " << files << ",\n";
    oss << "  \"chunks\": " << chunks << ",\n";
    oss << "  \"dirty\": " << (dirty ? "true" : "false") << "\n";
    
    if (cache.has_value()) {
        oss << "  ,\"cache\": {\n";
        oss << "    \"enabled\": " << (cache->enabled ? "true" : "false") << ",\n";
        oss << "    \"entries\": " << cache->entries << ",\n";
        oss << "    \"maxEntries\": " << cache->max_entries << "\n";
        oss << "  }\n";
    }
    
    if (fts.has_value()) {
        oss << "  ,\"fts\": {\n";
        oss << "    \"enabled\": " << (fts->enabled ? "true" : "false") << ",\n";
        oss << "    \"available\": " << (fts->available ? "true" : "false");
        if (!fts->error.empty()) {
            oss << ",\n    \"error\": \"" << fts->error << "\"";
        }
        oss << "\n  }\n";
    }
    
    if (vector.has_value()) {
        oss << "  ,\"vector\": {\n";
        oss << "    \"enabled\": " << (vector->enabled ? "true" : "false") << ",\n";
        oss << "    \"available\": " << (vector->available ? "true" : "false") << ",\n";
        oss << "    \"dims\": " << vector->dims;
        if (!vector->load_error.empty()) {
            oss << ",\n    \"loadError\": \"" << vector->load_error << "\"";
        }
        oss << "\n  }\n";
    }
    
    if (fallback.has_value()) {
        oss << "  ,\"fallback\": {\n";
        oss << "    \"from\": \"" << fallback->from << "\",\n";
        oss << "    \"reason\": \"" << fallback->reason << "\"\n";
        oss << "  }\n";
    }
    
    oss << "}";
    return oss.str();
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// 2. Fallback Memory Manager Implementation
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

FallbackMemoryManager::FallbackMemoryManager(
    std::shared_ptr<MemorySearchManager> primary,
    FallbackFactory fallback_factory,
    std::function<void()> on_close
) : primary_(std::move(primary)),
    fallback_factory_(std::move(fallback_factory)),
    on_close_(std::move(on_close)) {}

std::vector<MemorySearchResult> FallbackMemoryManager::search(
    const std::string& query,
    const SearchOptions& opts
) {
    // Try primary first
    if (!primary_failed_) {
        try {
            return primary_->search(query, opts);
        } catch (const std::exception& e) {
            // Primary failed, switch to fallback
            primary_failed_ = true;
            last_error_ = e.what();
            
            // Log warning (in production, use proper logging)
            // log.warn("qmd memory failed; switching to builtin index: " + last_error_);
            
            try {
                primary_->close();
            } catch (...) {
                // Ignore close errors
            }
            
            // Evict cache entry to allow retry
            evict_cache_entry();
        }
    }
    
    // Use fallback
    auto fallback = ensure_fallback();
    if (fallback) {
        return fallback->search(query, opts);
    }
    
    throw std::runtime_error(
        last_error_.empty() ? "memory search unavailable" : last_error_
    );
}

MemorySearchManager::ReadFileResult FallbackMemoryManager::read_file(
    const std::string& rel_path,
    int from,
    int lines
) {
    // Try primary first
    if (!primary_failed_) {
        return primary_->read_file(rel_path, from, lines);
    }
    
    // Use fallback
    auto fallback = ensure_fallback();
    if (fallback) {
        return fallback->read_file(rel_path, from, lines);
    }
    
    throw std::runtime_error(
        last_error_.empty() ? "memory read unavailable" : last_error_
    );
}

MemoryProviderStatus FallbackMemoryManager::status() {
    // Try primary first
    if (!primary_failed_) {
        return primary_->status();
    }
    
    // Use fallback status
    auto fallback = fallback_;
    MemoryProviderStatus fallback_info;
    fallback_info.fallback = MemoryProviderStatus::FallbackInfo{
        "qmd",
        last_error_.empty() ? "unknown" : last_error_
    };
    
    if (fallback) {
        auto status = fallback->status();
        status.fallback = fallback_info.fallback;
        status.custom["fallback_disabled"] = "true";
        status.custom["fallback_reason"] = last_error_;
        return status;
    }
    
    // Return primary status with fallback info
    auto status = primary_->status();
    status.fallback = fallback_info.fallback;
    status.custom["fallback_disabled"] = "true";
    status.custom["fallback_reason"] = last_error_;
    return status;
}

void FallbackMemoryManager::sync(const SyncParams& params) {
    if (!primary_failed_) {
        primary_->sync(params);
    } else if (fallback_) {
        fallback_->sync(params);
    }
}

MemoryEmbeddingProbeResult FallbackMemoryManager::probe_embedding_availability() {
    if (!primary_failed_) {
        return primary_->probe_embedding_availability();
    }
    
    auto fallback = ensure_fallback();
    if (fallback) {
        return fallback->probe_embedding_availability();
    }
    
    return MemoryEmbeddingProbeResult{
        false,
        last_error_.empty() ? "unavailable" : last_error_
    };
}

bool FallbackMemoryManager::probe_vector_availability() {
    if (!primary_failed_) {
        return primary_->probe_vector_availability();
    }
    
    auto fallback = ensure_fallback();
    if (fallback) {
        return fallback->probe_vector_availability();
    }
    
    return false;
}

void FallbackMemoryManager::close() {
    if (on_close_) {
        on_close_();
    }
    
    if (primary_) {
        try {
            primary_->close();
        } catch (...) {
            // Ignore
        }
    }
    
    if (fallback_) {
        try {
            fallback_->close();
        } catch (...) {
            // Ignore
        }
    }
}

std::shared_ptr<MemorySearchManager> FallbackMemoryManager::ensure_fallback() {
    if (!fallback_ && fallback_factory_) {
        fallback_ = fallback_factory_();
    }
    return fallback_;
}

void FallbackMemoryManager::evict_cache_entry() {
    if (cache_evicted_) return;
    cache_evicted_ = true;
    
    // Notify cache to evict this wrapper
    // (This allows next request to retry QMD with fresh manager)
    // Implementation depends on cache key management
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// 3. Memory Search Manager Factory Implementation
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

MemorySearchManagerResult MemorySearchManagerFactory::get_memory_search_manager(
    const GetParams& params
) {
    // Resolve backend config
    bool use_qmd = (params.config.count("backend") > 0 && 
                    params.config.at("backend") == "qmd");
    
    if (use_qmd) {
        // Try to create QMD manager
        bool status_only = (params.purpose == GetParams::Purpose::STATUS);
        
        std::string cache_key;
        if (!status_only) {
            cache_key = build_qmd_cache_key(params.agent_id, params.config);
            auto cached = MemorySearchManagerCache::instance().get(cache_key);
            if (cached) {
                return MemorySearchManagerResult{cached, ""};
            }
        }
        
        // Create QMD manager (stub - needs QmdMemoryManager implementation)
        // auto qmd_manager = QmdMemoryManager::create(params);
        // if (qmd_manager) { ... }
        
        // For now, fallback to builtin
    }
    
    // Create builtin manager (stub - needs MemoryIndexManager implementation)
    // auto builtin_manager = MemoryIndexManager::get(params);
    // return MemorySearchManagerResult{builtin_manager, ""};
    
    return MemorySearchManagerResult{nullptr, "Not implemented yet"};
}

void MemorySearchManagerFactory::close_all_memory_search_managers() {
    // Clear QMD cache
    auto& cache = MemorySearchManagerCache::instance();
    auto all_managers = cache.cache_;  // Copy
    cache.clear();
    
    for (auto& [key, manager] : all_managers) {
        try {
            manager->close();
        } catch (...) {
            // Log warning
        }
    }
    
    // Close builtin managers
    // MemoryIndexManager::close_all();
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// 4. Memory Search Manager Cache Implementation
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

MemorySearchManagerCache& MemorySearchManagerCache::instance() {
    static MemorySearchManagerCache cache;
    return cache;
}

std::shared_ptr<MemorySearchManager> MemorySearchManagerCache::get(const std::string& cache_key) {
    auto it = cache_.find(cache_key);
    return it != cache_.end() ? it->second : nullptr;
}

void MemorySearchManagerCache::set(
    const std::string& cache_key,
    std::shared_ptr<MemorySearchManager> manager
) {
    cache_[cache_key] = std::move(manager);
}

void MemorySearchManagerCache::remove(const std::string& cache_key) {
    cache_.erase(cache_key);
}

void MemorySearchManagerCache::clear() {
    cache_.clear();
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// 5. Utilities Implementation
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

std::string build_qmd_cache_key(
    const std::string& agent_id,
    const std::map<std::string, std::string>& qmd_config
) {
    std::ostringstream oss;
    oss << "qmd:" << agent_id << ":";
    
    // Sort config keys for stable cache key
    std::vector<std::string> keys;
    for (const auto& [k, v] : qmd_config) {
        keys.push_back(k);
    }
    std::sort(keys.begin(), keys.end());
    
    for (size_t i = 0; i < keys.size(); ++i) {
        if (i > 0) oss << ",";
        oss << keys[i] << "=" << qmd_config.at(keys[i]);
    }
    
    return oss.str();
}

}  // namespace memory
}  // namespace hunterclaw
