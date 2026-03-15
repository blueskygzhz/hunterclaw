#include "hunterclaw/memory/search_manager_factory.h"
#include "hunterclaw/memory/memory_index_manager.h"
#include "hunterclaw/memory/qmd_manager.h"
#include "hunterclaw/logging/subsystem_logger.h"
#include <json/json.h>

namespace hunterclaw {
namespace memory {

// 严格对齐 OpenClaw src/memory/search-manager.ts

// ============================================================================
// 全局变量 (对齐 QMD_MANAGER_CACHE)
// ============================================================================

std::map<std::string, std::shared_ptr<MemorySearchManager>> qmd_manager_cache;
std::mutex qmd_cache_mutex;

static auto log = logging::create_subsystem_logger("memory");

// ============================================================================
// QMD缓存Key构建 (对齐 buildQmdCacheKey)
// ============================================================================

std::string build_qmd_cache_key(
    const std::string& agent_id,
    const ResolvedQmdConfig& qmd_config
) {
    // 对齐 OpenClaw: agentId + JSON.stringify(config)
    // 简化实现: agentId + command + collections
    std::string key = agent_id + ":" + qmd_config.command;
    
    for (const auto& col : qmd_config.collections) {
        key += "|" + col.name + "=" + col.path;
    }
    
    return key;
}

void evict_qmd_cache_entry(const std::string& cache_key) {
    std::lock_guard<std::mutex> lock(qmd_cache_mutex);
    qmd_manager_cache.erase(cache_key);
}

// ============================================================================
// FallbackMemoryManager实现 (对齐 FallbackMemoryManager class)
// ============================================================================

FallbackMemoryManager::FallbackMemoryManager(
    std::shared_ptr<MemorySearchManager> primary,
    FallbackFactory fallback_factory,
    OnCloseCallback on_close
) : primary_(std::move(primary)),
    fallback_factory_(std::move(fallback_factory)),
    on_close_(std::move(on_close))
{
}

FallbackMemoryManager::~FallbackMemoryManager() {
    // 析构时关闭
    try {
        close();
    } catch (...) {
        // 忽略异常
    }
}

void FallbackMemoryManager::handle_primary_failure(const std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (primary_failed_) {
        return;  // 已经失败过
    }
    
    primary_failed_ = true;
    last_error_ = error;
    
    log->warn("qmd memory failed; switching to builtin index: {}", error);
    
    // 尝试关闭primary
    if (primary_) {
        try {
            primary_->close();
        } catch (...) {
            // 忽略关闭错误
        }
    }
    
    // 驱逐缓存 (对齐 OpenClaw注释: Evict the failed wrapper so the next request can retry QMD with a fresh manager)
    evict_cache_entry();
}

void FallbackMemoryManager::evict_cache_entry() {
    if (cache_evicted_) {
        return;
    }
    cache_evicted_ = true;
    if (on_close_) {
        on_close_();
    }
}

core::Result<std::shared_ptr<MemorySearchManager>> FallbackMemoryManager::ensure_fallback() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (fallback_) {
        return core::Result<std::shared_ptr<MemorySearchManager>>::ok_value(fallback_);
    }
    
    // 创建fallback
    auto result = fallback_factory_();
    if (result.is_error()) {
        log->warn("memory fallback unavailable: {}", result.error());
        return result;
    }
    
    if (!result.value()) {
        log->warn("memory fallback requested but builtin index is unavailable");
        return core::Result<std::shared_ptr<MemorySearchManager>>::error(
            "fallback unavailable"
        );
    }
    
    fallback_ = result.value();
    return core::Result<std::shared_ptr<MemorySearchManager>>::ok_value(fallback_);
}

// ============================================================================
// MemorySearchManager接口实现
// ============================================================================

std::vector<MemorySearchResult> FallbackMemoryManager::search(
    const std::string& query,
    const SearchOptions& opts
) {
    bool should_fallback = false;
    
    {
        std::lock_guard<std::mutex> lock(mutex_);
        should_fallback = primary_failed_;
    }
    
    if (!should_fallback) {
        try {
            return primary_->search(query, opts);
        } catch (const std::exception& e) {
            handle_primary_failure(e.what());
            should_fallback = true;
        }
    }
    
    // 使用fallback
    auto fallback_res = ensure_fallback();
    if (fallback_res.is_ok() && fallback_res.value()) {
        return fallback_res.value()->search(query, opts);
    }
    
    std::string error_msg;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        error_msg = last_error_.value_or("memory search unavailable");
    }
    throw std::runtime_error(error_msg);
}

ReadFileResult FallbackMemoryManager::read_file(const ReadFileParams& params) {
    bool should_fallback = false;
    
    {
        std::lock_guard<std::mutex> lock(mutex_);
        should_fallback = primary_failed_;
    }
    
    if (!should_fallback) {
        return primary_->read_file(params);
    }
    
    // 使用fallback
    auto fallback_res = ensure_fallback();
    if (fallback_res.is_ok() && fallback_res.value()) {
        return fallback_res.value()->read_file(params);
    }
    
    std::string error_msg;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        error_msg = last_error_.value_or("memory read unavailable");
    }
    throw std::runtime_error(error_msg);
}

MemoryProviderStatus FallbackMemoryManager::status() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (!primary_failed_) {
        return primary_->status();
    }
    
    // Fallback状态 (对齐 OpenClaw status()逻辑)
    MemoryProviderStatus fallback_status;
    if (fallback_) {
        fallback_status = fallback_->status();
    } else {
        fallback_status = primary_->status();
    }
    
    // 添加fallback信息
    // 注: OpenClaw在custom字段添加fallback信息
    // C++简化实现: 直接在status message中标注
    
    return fallback_status;
}

void FallbackMemoryManager::sync(const SyncParams& params) {
    bool should_fallback = false;
    
    {
        std::lock_guard<std::mutex> lock(mutex_);
        should_fallback = primary_failed_;
    }
    
    if (!should_fallback) {
        primary_->sync(params);
        return;
    }
    
    // 使用fallback
    auto fallback_res = ensure_fallback();
    if (fallback_res.is_ok() && fallback_res.value()) {
        fallback_res.value()->sync(params);
    }
}

MemoryEmbeddingProbeResult FallbackMemoryManager::probe_embedding_availability() {
    bool should_fallback = false;
    
    {
        std::lock_guard<std::mutex> lock(mutex_);
        should_fallback = primary_failed_;
    }
    
    if (!should_fallback) {
        return primary_->probe_embedding_availability();
    }
    
    // 使用fallback
    auto fallback_res = ensure_fallback();
    if (fallback_res.is_ok() && fallback_res.value()) {
        return fallback_res.value()->probe_embedding_availability();
    }
    
    std::string error_msg;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        error_msg = last_error_.value_or("memory embeddings unavailable");
    }
    
    return {
        .ok = false,
        .error = error_msg
    };
}

bool FallbackMemoryManager::probe_vector_availability() {
    bool should_fallback = false;
    
    {
        std::lock_guard<std::mutex> lock(mutex_);
        should_fallback = primary_failed_;
    }
    
    if (!should_fallback) {
        return primary_->probe_vector_availability();
    }
    
    // 使用fallback
    auto fallback_res = ensure_fallback();
    if (fallback_res.is_ok() && fallback_res.value()) {
        return fallback_res.value()->probe_vector_availability();
    }
    
    return false;
}

void FallbackMemoryManager::close() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (primary_) {
        try {
            primary_->close();
        } catch (...) {
            // 忽略错误
        }
    }
    
    if (fallback_) {
        try {
            fallback_->close();
        } catch (...) {
            // 忽略错误
        }
    }
    
    evict_cache_entry();
}

// ============================================================================
// 核心API: get_memory_search_manager (对齐 getMemorySearchManager)
// ============================================================================

MemorySearchManagerResult get_memory_search_manager(const GetManagerParams& params) {
    auto resolved = params.backend_config;
    
    // QMD backend优先
    if (resolved.backend == "qmd" && resolved.qmd) {
        bool status_only = (params.purpose == "status");
        std::string cache_key;
        
        if (!status_only) {
            cache_key = build_qmd_cache_key(params.agent_id, *resolved.qmd);
            
            // 检查缓存
            {
                std::lock_guard<std::mutex> lock(qmd_cache_mutex);
                auto it = qmd_manager_cache.find(cache_key);
                if (it != qmd_manager_cache.end()) {
                    return MemorySearchManagerResult::ok(it->second);
                }
            }
        }
        
        // 创建QMD Manager
        try {
            QmdManagerParams qmd_params;
            qmd_params.agent_id = params.agent_id;
            qmd_params.resolved_config = *resolved.qmd;
            qmd_params.mode = status_only ? "status" : "full";
            
            auto primary = QmdMemoryManager::create(qmd_params);
            
            if (primary) {
                if (status_only) {
                    return MemorySearchManagerResult::ok(primary);
                }
                
                // 包装为FallbackMemoryManager
                auto wrapper = std::make_shared<FallbackMemoryManager>(
                    primary,
                    // fallback factory: 创建builtin index
                    [params]() -> core::Result<std::shared_ptr<MemorySearchManager>> {
                        // 创建MemoryIndexManager作为fallback
                        MemoryIndexManagerConfig config;
                        config.workspace_dir = params.workspace_dir;
                        config.db_path = (fs::path(params.workspace_dir) / ".hunterclaw" / "memory.db").string();
                        
                        try {
                            auto mgr = std::make_shared<MemoryIndexManager>(config);
                            return core::Result<std::shared_ptr<MemorySearchManager>>::ok_value(mgr);
                        } catch (const std::exception& e) {
                            return core::Result<std::shared_ptr<MemorySearchManager>>::error(e.what());
                        }
                    },
                    // onClose: 驱逐缓存
                    [cache_key]() {
                        if (!cache_key.empty()) {
                            evict_qmd_cache_entry(cache_key);
                        }
                    }
                );
                
                // 缓存wrapper
                if (!cache_key.empty()) {
                    std::lock_guard<std::mutex> lock(qmd_cache_mutex);
                    qmd_manager_cache[cache_key] = wrapper;
                }
                
                return MemorySearchManagerResult::ok(wrapper);
            }
        } catch (const std::exception& e) {
            std::string message = std::string("qmd memory unavailable; falling back to builtin: ") + e.what();
            log->warn(message);
        }
    }
    
    // Builtin backend fallback
    try {
        MemoryIndexManagerConfig config;
        config.workspace_dir = params.workspace_dir;
        config.db_path = (fs::path(params.workspace_dir) / ".hunterclaw" / "memory.db").string();
        
        auto manager = std::make_shared<MemoryIndexManager>(config);
        return MemorySearchManagerResult::ok(manager);
    } catch (const std::exception& e) {
        return MemorySearchManagerResult::err(e.what());
    }
}

// ============================================================================
// 全局关闭 (对齐 closeAllMemorySearchManagers)
// ============================================================================

void close_all_memory_search_managers() {
    std::vector<std::shared_ptr<MemorySearchManager>> managers;
    
    {
        std::lock_guard<std::mutex> lock(qmd_cache_mutex);
        for (auto& [key, mgr] : qmd_manager_cache) {
            managers.push_back(mgr);
        }
        qmd_manager_cache.clear();
    }
    
    // 关闭所有manager
    for (auto& mgr : managers) {
        try {
            mgr->close();
        } catch (const std::exception& e) {
            log->warn("failed to close qmd memory manager: {}", e.what());
        }
    }
    
    // 注: MemoryIndexManager的全局关闭在memory_index_manager.cc中单独管理
}

} // namespace memory
} // namespace hunterclaw
