#pragma once

#include "hunterclaw/memory/types.h"
#include "hunterclaw/memory/memory_index_manager.h"
#include "hunterclaw/core/types.h"
#include <memory>
#include <string>
#include <functional>

namespace hunterclaw {
namespace memory {

// 严格对齐 OpenClaw src/memory/search-manager.ts

// ============================================================================
// MemorySearchManager工厂结果 (对齐 search-manager.ts:20-23)
// ============================================================================
struct MemorySearchManagerResult {
    std::shared_ptr<MemorySearchManager> manager;
    std::optional<std::string> error;
};

// ============================================================================
// 工厂函数 (对齐 getMemorySearchManager in search-manager.ts:25-86)
// ============================================================================

struct GetManagerParams {
    std::string agent_id;
    std::string workspace_dir;
    ResolvedMemorySearchConfig settings;
    std::string purpose = "default";  // "default" | "status"
};

// 获取MemorySearchManager
// 优先尝试QMD backend,失败则fallback到builtin
MemorySearchManagerResult get_memory_search_manager(const GetManagerParams& params);

// 关闭所有MemorySearchManager (对齐 closeAllMemorySearchManagers)
void close_all_memory_search_managers();

// ============================================================================
// FallbackMemoryManager - QMD到Builtin的Fallback包装器
// (对齐 search-manager.ts:104-183)
// ============================================================================
class FallbackMemoryManager : public MemorySearchManager {
public:
    // 构造函数
    FallbackMemoryManager(
        std::shared_ptr<MemorySearchManager> primary,
        std::function<std::shared_ptr<MemorySearchManager>()> fallback_factory,
        std::function<void()> on_close = nullptr
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
    
    bool is_primary_failed() const { return primary_failed_; }
    
    std::optional<std::string> get_last_error() const { return last_error_; }
    
private:
    std::shared_ptr<MemorySearchManager> primary_;
    std::function<std::shared_ptr<MemorySearchManager>()> fallback_factory_;
    std::function<void()> on_close_;
    
    std::shared_ptr<MemorySearchManager> fallback_;
    bool primary_failed_ = false;
    std::optional<std::string> last_error_;
    bool cache_evicted_ = false;
    
    mutable std::mutex mutex_;
    
    // 确保fallback存在
    core::Result<std::shared_ptr<MemorySearchManager>> ensure_fallback();
    
    // 驱逐缓存条目
    void evict_cache_entry();
    
    // 处理primary失败
    void handle_primary_failure(const std::string& error);
};

// ============================================================================
// QMD后端配置解析 (对齐 backend-config.ts)
// ============================================================================

struct ResolvedQmdConfig {
    std::string qmd_path;
    std::optional<std::string> workspace_dir;
    std::optional<std::string> provider;
    std::optional<std::string> model;
};

struct ResolvedBackendConfig {
    std::string backend;  // "builtin" | "qmd"
    std::optional<ResolvedQmdConfig> qmd;
};

// 解析memory backend配置
ResolvedBackendConfig resolve_memory_backend_config(const GetManagerParams& params);

// ============================================================================
// QMD缓存管理
// ============================================================================

// 构建QMD缓存key
std::string build_qmd_cache_key(
    const std::string& agent_id,
    const ResolvedQmdConfig& qmd_config
);

// QMD Manager缓存
extern std::map<std::string, std::shared_ptr<MemorySearchManager>> qmd_manager_cache;
extern std::mutex qmd_cache_mutex;

} // namespace memory
} // namespace hunterclaw
