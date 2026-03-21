#pragma once

#include "hunterclaw/memory/types.h"
#include "hunterclaw/memory/qmd_scope.h"
#include "hunterclaw/memory/qmd_query_parser.h"
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

// QMD Query Result is defined in qmd_query_parser.h

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
    std::string search_mode = "query";  // "query" | "search" | "vsearch"
    
    struct UpdateConfig {
        bool on_boot = true;            // 启动时更新
        bool wait_for_boot_sync = false;// 等待启动同步
        int interval_ms = 0;            // 更新间隔 (ms)
        int command_timeout_ms = 60000; // 命令超时 (ms)
        int update_timeout_ms = 120000; // update 命令超时 (ms)
        int embed_timeout_ms = 300000;  // embed 命令超时 (ms)
        int embed_interval_ms = 0;      // embed 间隔 (ms)
        int debounce_ms = 0;            // 防抖间隔 (ms)
    };
    UpdateConfig update;
    
    struct LimitsConfig {
        int max_results = 20;           // 最大结果数
        int timeout_ms = 30000;         // 搜索超时 (ms)
        int max_snippet_chars = 2000;   // 最大 snippet 字符数
        int max_injected_chars = 0;     // 最大注入字符数 (0=无限)
    };
    LimitsConfig limits;
    
    struct SessionsConfig {
        bool enabled = false;           // 是否启用会话索引
        std::string export_dir;         // 导出目录
        int retention_days = 30;        // 保留天数
    };
    SessionsConfig sessions;
    
    // Scope config (对齐 qmd-scope.ts:QmdScopeConfig)
    QmdScopeConfig scope_config;
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
        const SearchOptions& opts = {}
    ) override;
    
    ReadFileResult read_file(const ReadFileParams& params) override;
    
    MemoryProviderStatus status() override;
    
    void sync(const SyncParams& params = {}) override;
    
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
        bool discard_output = false; // 丢弃输出
    };
    
    QmdCommandResult run_qmd(
        const std::vector<std::string>& args,
        const QmdCommandOptions* opts = nullptr
    );
    
    // ============== Collection Management ==============
    
    void bootstrap_collections();
    void ensure_collections();
    std::map<std::string, ListedCollection> list_collections_best_effort();
    std::map<std::string, ListedCollection> parse_listed_collections(const std::string& output);
    bool should_rebind_collection(const ManagedCollection& collection, const ListedCollection& listed);
    bool paths_match(const std::string& left, const std::string& right);
    void add_collection(const std::string& path, const std::string& name, const std::string& pattern);
    void remove_collection(const std::string& name);
    void ensure_collection_path(const ManagedCollection& collection);
    
    // ============== Update & Sync ==============
    
    void run_update(const std::string& reason, bool force = false);
    bool should_skip_update(bool force) const;
    bool should_run_embed(bool force) const;
    
    // ============== Search ==============
    
    std::vector<std::string> list_managed_collection_names() const;
    std::vector<std::string> compute_managed_collection_names() const;
    std::vector<std::string> build_search_args(
        const std::string& command,
        const std::string& query,
        int limit
    ) const;
    std::vector<std::string> build_collection_filter_args(
        const std::vector<std::string>& collection_names
    ) const;
    std::vector<QmdQueryResult> run_query_across_collections(
        const std::string& query,
        int limit,
        const std::vector<std::string>& collection_names,
        const std::string& command
    );
    
    // ============== Query & Search ==============
    
    std::vector<QmdQueryResult> parse_qmd_query_json(
        const std::string& stdout_data,
        const std::string& stderr_data
    );
    MemorySearchResult qmd_result_to_memory_result(const QmdQueryResult& qmd_result);
    std::string normalize_han_bm25_query(const std::string& query);
    std::vector<MemorySearchResult> diversify_results_by_source(
        const std::vector<MemorySearchResult>& results,
        int limit
    );
    std::vector<MemorySearchResult> clamp_results_by_injected_chars(
        const std::vector<MemorySearchResult>& results
    );
    void wait_for_pending_update();
    
    // ============== Scope ==============
    
    bool is_scope_allowed(const std::optional<std::string>& session_key) const;
    void log_scope_denied(const std::optional<std::string>& session_key) const;
    
    // ============== Error Helpers ==============
    
    bool is_collection_missing_error(const std::string& error_msg) const;
    bool is_collection_already_exists_error(const std::string& error_msg) const;
    bool is_missing_collection_search_error(const std::exception& err) const;
    bool is_sqlite_busy_error(const std::exception& err) const;
    bool is_unsupported_qmd_option_error(const std::exception& err) const;
    
    // ============== Session Management ==============
    
    void export_session_files();
    
    // ============== Utilities ==============
    
    std::map<std::string, std::string> build_xdg_env();
    
    // ============== Member Variables ==============
    
    std::string agent_id_;
    std::string workspace_dir_;
    std::string state_dir_;
    std::string qmd_dir_;
    std::string xdg_config_home_;
    std::string xdg_cache_home_;
    std::string index_path_;
    
    QmdConfig qmd_config_;
    std::map<std::string, CollectionRoot> collection_roots_;
    std::vector<MemorySource> sources_;
    std::vector<std::string> managed_collection_names_;
    
    std::optional<SessionExporterConfig> session_exporter_;
    
    bool closed_ = false;
    int64_t last_update_at_ = 0;
    int64_t last_embed_at_ = 0;
    int64_t embed_backoff_until_ = 0;
    int embed_failure_count_ = 0;
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
