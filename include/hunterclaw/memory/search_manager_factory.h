#pragma once

#include "hunterclaw/memory/types.h"
#include "hunterclaw/memory/backend_config.h"
#include "hunterclaw/core/types.h"
#include <memory>
#include <string>
#include <map>
#include <mutex>
#include <functional>

namespace hunterclaw {
namespace memory {

// 严格对齐 OpenClaw src/memory/search-manager.ts

// ============================================================================
// MemorySearchManager工厂结果 (对齐 MemorySearchManagerResult)
// ============================================================================
struct MemorySearchManagerResult {
    std::shared_ptr<MemorySearchManager> manager;
    std::optional<std::string> error;
    
    static MemorySearchManagerResult Ok(std::shared_ptr<MemorySearchManager> mgr) {
        return { .manager = std::move(mgr), .error = std::nullopt };
    }
    
    static MemorySearchManagerResult Err(const std::string& msg) {
        return { .manager = nullptr, .error = msg };
    }
};

// ============================================================================
// 获取Manager参数 (对齐 getMemorySearchManager params)
// ============================================================================
struct GetManagerParams {
    std::string agent_id;
    std::string workspace_dir;
    ResolvedMemoryBackendConfig backend_config;
    std::string purpose = "default";  // "default" | "status"
};

// ============================================================================
// 核心API (对齐 search-manager.ts)
// ============================================================================

// 获取MemorySearchManager (对齐 getMemorySearchManager)
// 优先尝试QMD backend,失败则fallback到builtin
MemorySearchManagerResult get_memory_search_manager(const GetManagerParams& params);

// 关闭所有MemorySearchManager (对齐 closeAllMemorySearchManagers)
void close_all_memory_search_managers();

// ============================================================================
// FallbackMemoryManager - QMD到Builtin的Fallback包装器
// (对齐 FallbackMemoryManager in search-manager.ts:104-247)
// ============================================================================
class FallbackMemoryManager : public MemorySearchManager {
public:
    using FallbackFactory = std::function<core::Result<std::shared_ptr<MemorySearchManager>>()>;
    using OnCloseCallback = std::function<void()>;
    
    // 构造函数
    FallbackMemoryManager(
        std::shared_ptr<MemorySearchManager> primary,
        FallbackFactory fallback_factory,
        OnCloseCallback on_close = nullptr
    );
    
    ~FallbackMemoryManager() override;
    
    // ========================================================================
    // MemorySearchManager接口实现
    // ========================================================================
    
    std::vector<MemorySearchResult> search(
        const std::string& query,
        const SearchOptions& opts = {}
    ) override;
    
    ReadFileResult read_file(const ReadFileParams& params) override;
    
    MemoryProviderStatus status() override;
    
    void sync(const SyncParams& params = {}) override;
    
    MemoryEmbeddingProbeResult probe_embedding_availability() override;
    
    bool probe_vector_availability() override;
    
    void close() override;
    
    // ========================================================================
    // Fallback状态查询
    // ========================================================================
    
    bool is_primary_failed() const { 
        std::lock_guard<std::mutex> lock(mutex_);
        return primary_failed_; 
    }
    
    std::optional<std::string> get_last_error() const { 
        std::lock_guard<std::mutex> lock(mutex_);
        return last_error_; 
    }
    
private:
    std::shared_ptr<MemorySearchManager> primary_;
    FallbackFactory fallback_factory_;
    OnCloseCallback on_close_;
    
    std::shared_ptr<MemorySearchManager> fallback_;
    bool primary_failed_ = false;
    std::optional<std::string> last_error_;
    bool cache_evicted_ = false;
    
    mutable std::mutex mutex_;
    
    // 确保fallback存在 (对齐 ensureFallback)
    core::Result<std::shared_ptr<MemorySearchManager>> ensure_fallback();
    
    // 驱逐缓存条目 (对齐 evictCacheEntry)
    void evict_cache_entry();
    
    // 处理primary失败
    void handle_primary_failure(const std::string& error);
};

// ============================================================================
// QMD缓存管理 (对齐 QMD_MANAGER_CACHE)
// ============================================================================

// 构建QMD缓存key (对齐 buildQmdCacheKey)
std::string build_qmd_cache_key(
    const std::string& agent_id,
    const ResolvedQmdConfig& qmd_config
);

// QMD Manager全局缓存
extern std::map<std::string, std::shared_ptr<MemorySearchManager>> qmd_manager_cache;
extern std::mutex qmd_cache_mutex;

// 清除QMD缓存条目
void evict_qmd_cache_entry(const std::string& cache_key);

} // namespace memory
} // namespace hunterclaw
