#pragma once

#include "hunterclaw/memory/search_manager.h"
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <optional>
#include <functional>

namespace hunterclaw {
namespace memory {

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// QMD Memory Manager (对齐 qmd-manager.ts)
// 
// QMD (Query My Docs) 是一个外部 CLI 进程,提供高性能的向量搜索和全文检索
// QmdMemoryManager 通过子进程调用 `qmd` 命令行工具进行索引和搜索
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

// QMD Query Result (对齐 qmd-query-parser.ts:QmdQueryResult)
struct QmdQueryResult {
    std::string docid;          // 文档 ID
    double score;               // 相似度分数
    std::string collection;     // Collection 名称
    std::string file;           // 文件相对路径
    std::string snippet;        // 摘要片段
    std::string body;           // 完整文本 (可选)
};

// Collection Root (Collection 根路径)
struct CollectionRoot {
    std::string path;           // Collection 路径
    MemorySource kind;          // Collection 类型 (memory | sessions)
};

// Listed Collection (从 QMD 列出的 Collection)
struct ListedCollection {
    std::string path;           // Collection 路径
    std::string pattern;        // Glob 模式
};

// Managed Collection (管理的 Collection)
struct ManagedCollection {
    std::string name;           // Collection 名称
    std::string path;           // Collection 路径
    std::string pattern;        // Glob 模式
    std::string kind;           // "memory" | "custom" | "sessions"
};

// Session Exporter Config (会话导出配置)
struct SessionExporterConfig {
    std::string dir;                    // 导出目录
    std::optional<int64_t> retention_ms; // 保留时长 (ms)
    std::string collection_name;        // Collection 名称
};

// QMD Config (对齐 backend-config.ts:ResolvedQmdConfig)
struct QmdConfig {
    std::string qmd_binary_path;        // QMD 二进制路径
    std::vector<ManagedCollection> collections;  // Collections 列表
    
    struct UpdateConfig {
        bool on_boot = true;            // 启动时更新
        bool wait_for_boot_sync = false;// 等待启动同步
        int interval_ms = 0;            // 更新间隔 (ms)
        int command_timeout_ms = 60000; // 命令超时 (ms)
    };
    UpdateConfig update;
    
    struct SessionsConfig {
        bool enabled = false;           // 是否启用会话索引
        std::string export_dir;         // 导出目录
        int retention_days = 30;        // 保留天数
    };
    SessionsConfig sessions;
};

// QMD Manager Mode
enum class QmdManagerMode {
    FULL,       // 完整功能 (索引 + 搜索)
    STATUS      // 仅状态查询 (轻量级)
};

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// QMD Memory Manager (对齐 qmd-manager.ts:QmdMemoryManager)
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

class QmdMemoryManager : public MemorySearchManager {
public:
    // Factory Method (对齐 qmd-manager.ts:QmdMemoryManager.create)
    struct CreateParams {
        std::string agent_id;           // Agent ID
        std::string workspace_dir;      // 工作区目录
        QmdConfig qmd_config;           // QMD 配置
        QmdManagerMode mode = QmdManagerMode::FULL;
    };
    
    static std::shared_ptr<QmdMemoryManager> create(const CreateParams& params);
    
    // Destructor
    ~QmdMemoryManager() override;
    
    // ============== MemorySearchManager Interface ==============
    
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
    // Private Constructor
    QmdMemoryManager(const CreateParams& params);
    
    // Initialize (对齐 qmd-manager.ts:initialize)
    void initialize(QmdManagerMode mode);
    
    // ============== QMD Process Management ==============
    
    // Run QMD Command (执行 QMD 命令)
    struct QmdCommandResult {
        std::string stdout_data;    // 标准输出
        std::string stderr_data;    // 标准错误
        int exit_code;              // 退出码
    };
    
    struct QmdCommandOptions {
        int timeout_ms = 10000;     // 超时 (ms)
        std::map<std::string, std::string> env;  // 环境变量
    };
    
    QmdCommandResult run_qmd(
        const std::vector<std::string>& args,
        const QmdCommandOptions& opts = QmdCommandOptions()
    );
    
    // ============== Collection Management ==============
    
    // Bootstrap Collections (初始化 Collections)
    void bootstrap_collections();
    
    // Ensure Collections (确保 Collections 存在)
    void ensure_collections();
    
    // List Collections (列出 Collections)
    std::map<std::string, ListedCollection> list_collections_best_effort();
    
    // Add Collection (添加 Collection)
    void add_collection(
        const std::string& path,
        const std::string& name,
        const std::string& pattern
    );
    
    // Remove Collection (删除 Collection)
    void remove_collection(const std::string& name);
    
    // Ensure Collection Path (确保 Collection 路径存在)
    void ensure_collection_path(const ManagedCollection& collection);
    
    // ============== Update & Sync ==============
    
    // Run Update (执行更新)
    void run_update(const std::string& reason, bool force = false);
    
    // ============== Query & Search ==============
    
    // Parse QMD Query Result (解析 QMD 查询结果)
    std::vector<QmdQueryResult> parse_qmd_query_json(
        const std::string& stdout_data,
        const std::string& stderr_data
    );
    
    // Convert QMD Result to Memory Search Result
    MemorySearchResult qmd_result_to_memory_result(const QmdQueryResult& qmd_result);
    
    // Normalize BM25 Query for Han script (中文查询优化)
    std::string normalize_han_bm25_query(const std::string& query);
    
    // ============== Session Management ==============
    
    // Export Session Files (导出会话文件)
    void export_session_files();
    
    // ============== Utilities ==============
    
    // Build XDG Environment (构建 XDG 环境变量)
    std::map<std::string, std::string> build_xdg_env();
    
    // Check if error is "collection not found"
    bool is_collection_missing_error(const std::string& error_msg);
    
    // Check if error is "collection already exists"
    bool is_collection_already_exists_error(const std::string& error_msg);
    
    // ============== Member Variables ==============
    
    std::string agent_id_;              // Agent ID
    std::string workspace_dir_;         // 工作区目录
    std::string state_dir_;             // 状态目录 (~/.openclaw/agents/{agent_id})
    std::string qmd_dir_;               // QMD 目录 (state_dir/qmd)
    std::string xdg_config_home_;       // XDG_CONFIG_HOME (qmd_dir/xdg-config)
    std::string xdg_cache_home_;        // XDG_CACHE_HOME (qmd_dir/xdg-cache)
    std::string index_path_;            // 索引路径 (xdg_cache/qmd/index.sqlite)
    
    QmdConfig qmd_config_;              // QMD 配置
    std::map<std::string, CollectionRoot> collection_roots_;  // Collection 根路径映射
    std::vector<MemorySource> sources_; // 启用的来源
    std::vector<std::string> managed_collection_names_;  // 管理的 Collection 名称
    
    std::optional<SessionExporterConfig> session_exporter_;  // 会话导出器配置
    
    bool closed_ = false;               // 是否已关闭
    int64_t last_update_at_ = 0;        // 上次更新时间戳
    int64_t last_embed_at_ = 0;         // 上次 Embedding 时间戳
};

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// Utilities
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

// Parse QMD Query JSON (对齐 qmd-query-parser.ts)
std::vector<QmdQueryResult> parse_qmd_query_json(
    const std::string& stdout_data,
    const std::string& stderr_data
);

}  // namespace memory
}  // namespace hunterclaw
