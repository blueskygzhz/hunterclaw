#include "hunterclaw/memory/search_manager_factory.h"
#include "hunterclaw/memory/memory_index_manager.h"
#include "hunterclaw/memory/qmd_manager.h"
#include "hunterclaw/logging/subsystem_logger.h"
#include <nlohmann/json.hpp>

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
    // 对齐 OpenClaw: `${agentId}:${JSON.stringify(config)}`
    // ResolvedQmdConfig 按稳定字段顺序组装，直接序列化整个 config
    nlohmann::json j;
    j["command"]               = qmd_config.command;
    j["searchMode"]            = qmd_config.search_mode;
    j["includeDefaultMemory"]  = qmd_config.include_default_memory;
    nlohmann::json cols = nlohmann::json::array();
    for (const auto& col : qmd_config.collections) {
        nlohmann::json c;
        c["name"]    = col.name;
        c["path"]    = col.path;
        c["pattern"] = col.pattern;
        c["kind"]    = col.kind;
        cols.push_back(c);
    }
    j["collections"] = cols;
    return agent_id + ":" + j.dump();
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
        return core::Result<std::shared_ptr<MemorySearchManager>>::Ok(fallback_);
    }
    
    // 创建fallback
    auto result = fallback_factory_();
    if (result.is_err()) {
        log->warn("memory fallback unavailable: " + result.error);
        return result;
    }
    
    if (!result.value) {
        log->warn("memory fallback requested but builtin index is unavailable");
        return core::Result<std::shared_ptr<MemorySearchManager>>::Err(
            "fallback unavailable"
        );
    }
    
    fallback_ = result.get();
    return core::Result<std::shared_ptr<MemorySearchManager>>::Ok(fallback_);
}

// ============================================================================
// MemorySearchManager接口实现
// ============================================================================

std::vector<MemorySearchResult> FallbackMemoryManager::search(
    const std::string& query,
    const SearchOptions& opts
) {
    // 对齐 openclaw FallbackMemoryManager.search()
    bool is_failed;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        is_failed = primary_failed_;
    }

    if (!is_failed) {
        try {
            return primary_->search(query, opts);
        } catch (const std::exception& e) {
            // 对齐 openclaw: inline 处理 primary 失败
            {
                std::lock_guard<std::mutex> lock(mutex_);
                primary_failed_ = true;
                last_error_ = e.what();
            }
            log->warn(std::string("qmd memory failed; switching to builtin index: ") + e.what());
            // 对齐 openclaw: await this.deps.primary.close?.().catch(() => {})
            try { primary_->close(); } catch (...) {}
            // Evict the failed wrapper so the next request can retry QMD with a fresh manager.
            evict_cache_entry();
        }
    }

    // 使用 fallback
    auto fallback_res = ensure_fallback();
    if (fallback_res.is_ok() && fallback_res.get()) {
        return fallback_res.get()->search(query, opts);
    }
    std::string error_msg;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        error_msg = last_error_.value_or("memory search unavailable");
    }
    throw std::runtime_error(error_msg);
}

ReadFileResult FallbackMemoryManager::read_file(const ReadFileParams& params) {
    // 对齐 openclaw: !primaryFailed 时直接透传，不做 try/catch
    bool is_failed;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        is_failed = primary_failed_;
    }
    if (!is_failed) {
        return primary_->read_file(params);
    }
    auto fallback_res = ensure_fallback();
    if (fallback_res.is_ok() && fallback_res.get()) {
        return fallback_res.get()->read_file(params);
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
    
    // 对齐 openclaw FallbackMemoryManager.status():
    // fallback 状态下在 custom 字段注入 fallback 信息
    MemoryProviderStatus s = fallback_ ? fallback_->status() : primary_->status();
    
    // fallback: { from: "qmd", reason: lastError }
    MemoryProviderStatus::FallbackInfo fi;
    fi.from   = "qmd";
    fi.reason = last_error_;
    s.fallback = fi;
    
    // custom.fallback: { disabled: true, reason }
    if (!s.custom) {
        s.custom = MemoryProviderStatus::CustomStatus{};
    }
    // 对齐 openclaw: custom: { ...custom, fallback: { disabled: true, reason } }
    // 用 provider_unavailable_reason 字段承载 reason（C++ 无动态 Record）
    s.custom->provider_unavailable_reason = last_error_;
    
    return s;
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
    if (fallback_res.is_ok() && fallback_res.get()) {
        fallback_res.get()->sync(params);
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
    if (fallback_res.is_ok() && fallback_res.get()) {
        return fallback_res.get()->probe_embedding_availability();
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
    if (fallback_res.is_ok() && fallback_res.get()) {
        return fallback_res.get()->probe_vector_availability();
    }
    
    return false;
}

void FallbackMemoryManager::close() {
    // 对齐 openclaw: close primary + fallback + evict cache
    std::shared_ptr<MemorySearchManager> p, f;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        p = primary_;
        f = fallback_;
    }
    if (p) {
        try { p->close(); } catch (...) {}
    }
    if (f) {
        try { f->close(); } catch (...) {}
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
                    return MemorySearchManagerResult::Ok(it->second);
                }
            }
        }
        
        // 创建QMD Manager
        try {
            QmdMemoryManager::CreateParams qmd_params;
            qmd_params.agent_id = params.agent_id;
            qmd_params.workspace_dir = params.workspace_dir;
            qmd_params.qmd_config.qmd_binary_path = resolved.qmd->command;
            qmd_params.qmd_config.collections.clear();
            for (const auto& col : resolved.qmd->collections) {
                ManagedCollection mc;
                mc.name = col.name;
                mc.path = col.path;
                mc.pattern = col.pattern;
                mc.kind = col.kind;
                qmd_params.qmd_config.collections.push_back(mc);
            }
            qmd_params.mode = status_only ? QmdManagerMode::STATUS : QmdManagerMode::FULL;
            
            auto primary = QmdMemoryManager::create(qmd_params);
            
            if (primary) {
                if (status_only) {
                    return MemorySearchManagerResult::Ok(primary);
                }
                
                // 包装为FallbackMemoryManager
                auto wrapper = std::make_shared<FallbackMemoryManager>(
                    primary,
                    // fallback factory: 创建builtin index
                    [params]() -> core::Result<std::shared_ptr<MemorySearchManager>> {
                        // 创建MemoryIndexManager作为fallback
                        MemoryIndexManager::Params mgr_params;
                        mgr_params.agent_id = params.agent_id;
                        mgr_params.workspace_dir = params.workspace_dir;
                        // 使用默认的 builtin 搜索配置
                        mgr_params.settings = ResolvedMemorySearchConfig{};
                        
                        auto result = MemoryIndexManager::get(mgr_params);
                        if (result.is_err()) {
                            return core::Result<std::shared_ptr<MemorySearchManager>>::Err(result.error);
                        }
                        // 向上转换: MemoryIndexManager -> MemorySearchManager
                        std::shared_ptr<MemorySearchManager> mgr = result.get();
                        return core::Result<std::shared_ptr<MemorySearchManager>>::Ok(mgr);
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
                
                return MemorySearchManagerResult::Ok(wrapper);
            }
        } catch (const std::exception& e) {
            std::string message = std::string("qmd memory unavailable; falling back to builtin: ") + e.what();
            log->warn(message);
        }
    }
    
    // Builtin backend fallback
    try {
        MemoryIndexManager::Params mgr_params;
        mgr_params.agent_id = params.agent_id;
        mgr_params.workspace_dir = params.workspace_dir;
        // 使用默认的 builtin 搜索配置
        mgr_params.settings = ResolvedMemorySearchConfig{};
        
        auto result = MemoryIndexManager::get(mgr_params);
        if (result.is_err()) {
            return MemorySearchManagerResult::Err(result.error);
        }
        // 向上转换: MemoryIndexManager -> MemorySearchManager
        std::shared_ptr<MemorySearchManager> mgr = result.get();
        return MemorySearchManagerResult::Ok(mgr);
    } catch (const std::exception& e) {
        return MemorySearchManagerResult::Err(e.what());
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
    
    // 关闭所有 QMD manager
    for (auto& mgr : managers) {
        try {
            mgr->close();
        } catch (const std::exception& e) {
            log->warn(std::string("failed to close qmd memory manager: ") + e.what());
        }
    }
    
    // 对齐 openclaw closeAllMemorySearchManagers:
    // 同时关闭所有 MemoryIndexManager (builtin)
    close_all_memory_index_managers();
}

} // namespace memory
} // namespace hunterclaw
