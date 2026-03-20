#pragma once

#include <optional>
#include "hunterclaw/memory/types.h"
#include "hunterclaw/memory/manager_embedding_ops.h"  // MemoryManagerEmbeddingOps (继承链)
#include "hunterclaw/memory/chunking.h"
#include "hunterclaw/core/types.h"
#include <memory>
#include <string>
#include <set>
#include <map>
#include <mutex>
#include <future>

namespace hunterclaw {
namespace memory {

// ============================================================================
// MemoryIndexManager
// 严格对齐 openclaw manager.ts:
//   export class MemoryIndexManager extends MemoryManagerEmbeddingOps
//                                    implements MemorySearchManager
//
// 继承链: MemoryManagerSyncOps
//           └── MemoryManagerEmbeddingOps
//                 └── MemoryIndexManager (implements MemorySearchManager)
// ============================================================================
class MemoryIndexManager : public MemoryManagerEmbeddingOps, public MemorySearchManager {
public:
    // ========================================================================
    // 工厂方法 (对齐 manager.ts:135 - static async get())
    // ========================================================================

    struct Params {
        std::string agent_id;
        std::string workspace_dir;
        ResolvedMemorySearchConfig settings;
    };

    // 获取或创建 MemoryIndexManager 实例 (带缓存，对齐 INDEX_CACHE)
    // 返回 nullptr 表示 memory 未配置 (对齐 openclaw: returns null if not configured)
    static core::Result<std::shared_ptr<MemoryIndexManager>> get(const Params& params);

    // 关闭所有缓存的 Manager 实例 (对齐 closeAllMemoryIndexManagers)
    static void close_all();

    ~MemoryIndexManager() override;

    // ========================================================================
    // MemorySearchManager 接口实现 (types.ts:61-81)
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
    // Session Management (对齐 manager.ts:warmSession)
    // ========================================================================

    // 预热 session memory (对齐 manager.ts:warmSession)
    void warm_session(const std::optional<std::string>& session_key = std::nullopt);

    // ========================================================================
    // 公共访问器
    // ========================================================================

    std::string get_workspace_dir() const { return workspace_dir_; }
    std::string get_agent_id() const { return agent_id_; }
    bool is_closed() const { return closed_; }
    bool is_dirty() const { return dirty_; }

private:
    // 私有构造函数 (对齐 manager.ts: private constructor)
    explicit MemoryIndexManager(const Params& params);

    // ========================================================================
    // 额外状态 (MemoryIndexManager 特有，不在 MemoryManagerSyncOps 中)
    // ========================================================================

    // 缓存 key (对齐 manager.ts:cacheKey)
    std::string cache_key_;

    // Session warm tracking (对齐 manager.ts:sessionWarm)
    std::set<std::string> session_warm_;

    // 同步状态 (对齐 manager.ts:syncing/queuedSessionFiles/queuedSessionSync)
    std::shared_ptr<std::future<void>> syncing_;
    std::set<std::string> queued_session_files_;
    std::shared_ptr<std::future<void>> queued_session_sync_;

    // Readonly recovery 状态 (对齐 manager.ts:readonlyRecovery*)
    int readonly_recovery_attempts_ = 0;
    int readonly_recovery_successes_ = 0;
    int readonly_recovery_failures_ = 0;
    std::optional<std::string> readonly_recovery_last_error_;

    // ========================================================================
    // 内部方法
    // ========================================================================

    // 初始化 (对齐 manager.ts constructor)
    void initialize();

    // 运行同步 (对齐 manager.ts:runSync)
    core::Result<void> run_sync(const SyncParams& params);

    // 带 readonly 恢复的同步 (对齐 manager.ts:runSyncWithReadonlyRecovery)
    void run_sync_with_readonly_recovery(const SyncParams& params);

    // 入队 session 同步 (对齐 manager.ts:enqueueTargetedSessionSync)
    void enqueue_targeted_session_sync(const std::vector<std::string>& session_files);

    // 检测 readonly 错误 (对齐 manager.ts:isReadonlyDbError)
    bool is_readonly_db_error(const std::exception& e) const;

    // MemoryManagerSyncOps 抄象方法实现
    std::string compute_provider_key() override;
    void do_sync(const SyncParams& params) override;
    int get_index_concurrency() override;
    void prune_embedding_cache_if_needed() override;
    // ========================================================================
    // 静态缓存 (对齐 manager.ts:INDEX_CACHE / INDEX_CACHE_PENDING)
    // ========================================================================

    static std::map<std::string, std::shared_ptr<MemoryIndexManager>> index_cache_;
    static std::map<std::string, std::shared_ptr<std::future<std::shared_ptr<MemoryIndexManager>>>> index_cache_pending_;
    static std::mutex cache_mutex_;
};

// ============================================================================
// 全局函数 (对齐 manager.ts:closeAllMemoryIndexManagers)
// ============================================================================

void close_all_memory_index_managers();

} // namespace memory
} // namespace hunterclaw
