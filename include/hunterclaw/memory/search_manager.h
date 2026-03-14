#pragma once

#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <optional>
#include <map>

namespace hunterclaw {
namespace memory {

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// 1. Memory Types (对齐 types.ts)
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

// Memory Source (memory | sessions)
enum class MemorySource {
    MEMORY,     // 工作区文件记忆
    SESSIONS    // 会话历史记忆
};

std::string to_string(MemorySource source);
MemorySource parse_memory_source(const std::string& source_str);

// Memory Search Result
struct MemorySearchResult {
    std::string path;           // 文件相对路径或会话标识
    int start_line;             // 起始行号
    int end_line;               // 结束行号
    double score;               // 相似度分数 (0.0-1.0)
    std::string snippet;        // 文本片段
    MemorySource source;        // 来源 (memory | sessions)
    std::string citation;       // 可选引用格式
};

// Memory Embedding Probe Result
struct MemoryEmbeddingProbeResult {
    bool ok;                        // 是否可用
    std::string error;              // 错误信息 (如果 ok = false)
};

// Memory Sync Progress Update
struct MemorySyncProgressUpdate {
    int completed;                  // 已完成数量
    int total;                      // 总数
    std::string label;              // 进度标签
};

// Memory Provider Status (详细状态)
struct MemoryProviderStatus {
    // Backend
    enum class Backend {
        BUILTIN,    // 内置 SQLite 索引
        QMD         // 外部 QMD 进程
    };
    Backend backend;
    std::string provider;           // 当前使用的 Provider (openai/gemini/local...)
    std::string model;              // Embedding 模型
    std::string requested_provider; // 用户请求的 Provider
    
    // Files & Chunks
    int files = 0;                  // 文件数量
    int chunks = 0;                 // 块数量
    bool dirty = false;             // 是否需要同步
    
    // Paths
    std::string workspace_dir;      // 工作区目录
    std::string db_path;            // 数据库路径
    std::vector<std::string> extra_paths;  // 额外监控路径
    
    // Sources
    std::vector<MemorySource> sources;  // 启用的来源
    struct SourceCount {
        MemorySource source;
        int files;
        int chunks;
    };
    std::vector<SourceCount> source_counts;
    
    // Cache
    struct CacheStatus {
        bool enabled;
        int entries = 0;
        int max_entries = 0;
    };
    std::optional<CacheStatus> cache;
    
    // FTS (Full-Text Search)
    struct FtsStatus {
        bool enabled;
        bool available;
        std::string error;
    };
    std::optional<FtsStatus> fts;
    
    // Fallback
    struct FallbackInfo {
        std::string from;           // fallback 来源 (qmd/builtin)
        std::string reason;         // fallback 原因
    };
    std::optional<FallbackInfo> fallback;
    
    // Vector
    struct VectorStatus {
        bool enabled;
        bool available = false;
        std::string extension_path;
        std::string load_error;
        int dims = 0;
    };
    std::optional<VectorStatus> vector;
    
    // Batch
    struct BatchStatus {
        bool enabled;
        int failures;
        int limit;
        bool wait;
        int concurrency;
        int poll_interval_ms;
        int timeout_ms;
        std::string last_error;
        std::string last_provider;
    };
    std::optional<BatchStatus> batch;
    
    // Custom fields (扩展字段)
    std::map<std::string, std::string> custom;
    
    // Utility
    std::string to_json() const;
};

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// 2. Memory Search Manager Interface (对齐 types.ts:MemorySearchManager)
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

// Memory Search Manager (核心接口)
class MemorySearchManager {
public:
    virtual ~MemorySearchManager() = default;
    
    // Search Options
    struct SearchOptions {
        int max_results = 6;        // 最大结果数 (default: 6)
        double min_score = 0.35;    // 最低分数阈值 (default: 0.35)
        std::string session_key;    // 会话键 (用于会话记忆过滤)
    };
    
    // Search Result Pair
    struct ReadFileResult {
        std::string text;           // 文件文本内容
        std::string path;           // 文件绝对路径
    };
    
    // ============== Core Methods ==============
    
    // 1. search() - 搜索记忆
    virtual std::vector<MemorySearchResult> search(
        const std::string& query,
        const SearchOptions& opts = SearchOptions()
    ) = 0;
    
    // 2. read_file() - 读取文件内容
    virtual ReadFileResult read_file(
        const std::string& rel_path,
        int from = 0,              // 起始行号 (0-based)
        int lines = -1             // 读取行数 (-1 表示全部)
    ) = 0;
    
    // 3. status() - 获取状态
    virtual MemoryProviderStatus status() = 0;
    
    // 4. sync() - 同步索引 (可选)
    struct SyncParams {
        std::string reason;         // 同步原因
        bool force = false;         // 强制同步
        std::function<void(const MemorySyncProgressUpdate&)> progress;  // 进度回调
    };
    virtual void sync(const SyncParams& params = SyncParams()) {}
    
    // 5. probe_embedding_availability() - 探测 Embedding 可用性
    virtual MemoryEmbeddingProbeResult probe_embedding_availability() = 0;
    
    // 6. probe_vector_availability() - 探测 Vector 扩展可用性
    virtual bool probe_vector_availability() = 0;
    
    // 7. close() - 关闭 Manager (可选)
    virtual void close() {}
};

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// 3. Memory Search Manager Factory (对齐 search-manager.ts)
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

// Memory Search Manager Result (工厂返回类型)
struct MemorySearchManagerResult {
    std::shared_ptr<MemorySearchManager> manager;  // Manager 实例 (可能为 null)
    std::string error;                             // 错误信息 (如果创建失败)
};

// Memory Search Manager Factory (对齐 search-manager.ts)
class MemorySearchManagerFactory {
public:
    // Get Memory Search Manager (主入口)
    struct GetParams {
        std::string agent_id;           // Agent ID
        std::string workspace_dir;      // 工作区目录
        std::map<std::string, std::string> config;  // 配置参数
        
        enum class Purpose {
            DEFAULT,    // 正常使用 (完整功能)
            STATUS      // 仅获取状态 (轻量级)
        };
        Purpose purpose = Purpose::DEFAULT;
    };
    
    static MemorySearchManagerResult get_memory_search_manager(const GetParams& params);
    
    // Close All Managers (全局清理)
    static void close_all_memory_search_managers();
};

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// 4. Fallback Memory Manager (对齐 search-manager.ts:FallbackMemoryManager)
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

// Fallback Memory Manager (Primary → Fallback 自动切换)
class FallbackMemoryManager : public MemorySearchManager {
public:
    using FallbackFactory = std::function<std::shared_ptr<MemorySearchManager>()>;
    
    FallbackMemoryManager(
        std::shared_ptr<MemorySearchManager> primary,
        FallbackFactory fallback_factory,
        std::function<void()> on_close = nullptr
    );
    
    // Implement MemorySearchManager
    std::vector<MemorySearchResult> search(
        const std::string& query,
        const SearchOptions& opts = SearchOptions()
    ) override;
    
    ReadFileResult read_file(
        const std::string& rel_path,
        int from = 0,
        int lines = -1
    ) override;
    
    MemoryProviderStatus status() override;
    
    void sync(const SyncParams& params = SyncParams()) override;
    
    MemoryEmbeddingProbeResult probe_embedding_availability() override;
    
    bool probe_vector_availability() override;
    
    void close() override;

private:
    std::shared_ptr<MemorySearchManager> primary_;
    std::shared_ptr<MemorySearchManager> fallback_;
    FallbackFactory fallback_factory_;
    std::function<void()> on_close_;
    
    bool primary_failed_ = false;
    bool cache_evicted_ = false;
    std::string last_error_;
    
    // Ensure fallback manager is created
    std::shared_ptr<MemorySearchManager> ensure_fallback();
    
    // Evict cache entry (notify factory)
    void evict_cache_entry();
};

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// 5. Global Cache (对齐 search-manager.ts)
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

// Memory Search Manager Cache (全局单例)
class MemorySearchManagerCache {
public:
    static MemorySearchManagerCache& instance();
    
    // Get cached manager
    std::shared_ptr<MemorySearchManager> get(const std::string& cache_key);
    
    // Set cached manager
    void set(const std::string& cache_key, std::shared_ptr<MemorySearchManager> manager);
    
    // Remove from cache
    void remove(const std::string& cache_key);
    
    // Clear all
    void clear();

private:
    MemorySearchManagerCache() = default;
    std::map<std::string, std::shared_ptr<MemorySearchManager>> cache_;
};

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// Utilities
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

// Build cache key (for QMD managers)
std::string build_qmd_cache_key(
    const std::string& agent_id,
    const std::map<std::string, std::string>& qmd_config
);

}  // namespace memory
}  // namespace hunterclaw
