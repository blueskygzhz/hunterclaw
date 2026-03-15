#include "hunterclaw/memory/memory_index_manager.h"
#include "hunterclaw/memory/search_algorithms.h"
#include <chrono>
#include <algorithm>
#include <sstream>

namespace hunterclaw {
namespace memory {

// ============================================================================
// 静态成员初始化
// ============================================================================
std::map<std::string, std::shared_ptr<MemoryIndexManager>> MemoryIndexManager::index_cache_;
std::map<std::string, std::shared_ptr<std::future<std::shared_ptr<MemoryIndexManager>>>> MemoryIndexManager::index_cache_pending_;
std::mutex MemoryIndexManager::cache_mutex_;

// ============================================================================
// 工厂方法实现
// ============================================================================

core::Result<std::shared_ptr<MemoryIndexManager>> MemoryIndexManager::get(const Params& params) {
    // TODO: 实现缓存逻辑
    // 目前简化实现：直接创建新实例
    try {
        auto instance = std::shared_ptr<MemoryIndexManager>(new MemoryIndexManager(params));
        return core::Result<std::shared_ptr<MemoryIndexManager>>::Ok(instance);
    } catch (const std::exception& e) {
        return core::Result<std::shared_ptr<MemoryIndexManager>>::Err(e.what());
    }
}

void MemoryIndexManager::close_all() {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    for (auto& [key, manager] : index_cache_) {
        if (manager) {
            manager->close();
        }
    }
    index_cache_.clear();
    index_cache_pending_.clear();
}

// ============================================================================
// MemoryIndexManager Implementation  
// ============================================================================

MemoryIndexManager::MemoryIndexManager(const Params& params)
    : agent_id_(params.agent_id),
      workspace_dir_(params.workspace_dir),
      settings_(params.settings),
      db_(nullptr),
      provider_(nullptr),
      closed_(false),
      dirty_(false),
      sessions_dirty_(false) {
    
    // 初始化batch配置
    batch_.enabled = false;
    batch_.wait = false;
    batch_.concurrency = 5;
    batch_.poll_interval_ms = 100;
    batch_.timeout_ms = 30000;
    
    // 初始化cache配置
    cache_.enabled = false;
    cache_.max_entries = std::nullopt;
    
    // 初始化vector配置
    vector_.enabled = false;
    vector_.available = std::nullopt;
    vector_.extension_path = std::nullopt;
    vector_.load_error = std::nullopt;
    vector_.dims = std::nullopt;
    
    // 初始化fts配置
    fts_.enabled = true;
    fts_.available = false;
    fts_.load_error = std::nullopt;
    
    // TODO: 完整实现初始化逻辑
    // - 创建/打开数据库
    // - 初始化embedding provider
    // - 加载vector extension
}

MemoryIndexManager::~MemoryIndexManager() {
    close();
}

void MemoryIndexManager::close() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (closed_) {
        return;
    }
    
    closed_ = true;
    // TODO: 关闭数据库和清理资源
}

// ============================================================================
// MemorySearchManager接口实现
// ============================================================================

std::vector<MemorySearchResult> MemoryIndexManager::search(
    const std::string& query,
    const SearchOptions& opts) {
    
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (closed_) {
        return {};
    }
    
    // TODO: 实现完整的搜索逻辑
    // 1. 生成query embedding
    // 2. 向量搜索 + 关键词搜索
    // 3. 混合排序
    return {};
}

ReadFileResult MemoryIndexManager::read_file(const ReadFileParams& params) {
    // TODO: 实现文件读取
    ReadFileResult result;
    result.text = "";
    result.path = params.rel_path;  // 字段名是rel_path
    return result;
}

MemoryProviderStatus MemoryIndexManager::status() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    MemoryProviderStatus status;
    status.backend = "builtin";
    status.provider = requested_provider_.empty() ? "none" : requested_provider_;
    status.requested_provider = requested_provider_.empty() ? std::nullopt : std::make_optional(requested_provider_);
    status.workspace_dir = workspace_dir_;
    status.dirty = dirty_;
    
    // Vector状态
    MemoryProviderStatus::VectorStatus vec_status;
    vec_status.enabled = vector_.enabled;
    vec_status.available = vector_.available.value_or(false);
    vec_status.extension_path = vector_.extension_path;
    vec_status.load_error = vector_.load_error;
    vec_status.dims = vector_.dims;
    status.vector = vec_status;
    
    // FTS状态
    MemoryProviderStatus::FtsStatus fts_status;
    fts_status.enabled = fts_.enabled;
    fts_status.available = fts_.available;
    fts_status.error = fts_.load_error;  // 字段名是error
    status.fts = fts_status;
    
    // Cache状态
    if (cache_.enabled) {
        MemoryProviderStatus::CacheStatus cache_status;
        cache_status.enabled = true;
        cache_status.max_entries = cache_.max_entries;
        cache_status.entries = std::nullopt; // TODO: 从db获取
        status.cache = cache_status;
    }
    
    return status;
}

void MemoryIndexManager::sync(const SyncParams& params) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (closed_) {
        return;
    }
    
    // TODO: 实现文件同步逻辑
    // 1. 扫描workspace目录
    // 2. 检测新增/修改/删除的文件
    // 3. 更新索引
    
    dirty_ = false;
}

MemoryEmbeddingProbeResult MemoryIndexManager::probe_embedding_availability() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    MemoryEmbeddingProbeResult result;
    
    if (!provider_) {
        result.ok = false;
        result.error = "No embedding provider configured";
        return result;
    }
    
    // TODO: 实际测试provider
    result.ok = true;
    result.error = std::nullopt;
    
    return result;
}

bool MemoryIndexManager::probe_vector_availability() {
    std::lock_guard<std::mutex> lock(mutex_);
    return vector_.available.value_or(false);
}

// ============================================================================
// 全局函数
// ============================================================================

void close_all_memory_index_managers() {
    MemoryIndexManager::close_all();
}

} // namespace memory
} // namespace hunterclaw
