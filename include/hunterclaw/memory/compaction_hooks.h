#pragma once

#include <string>
#include <chrono>
#include <functional>
#include <map>

namespace hunterclaw {
namespace memory {

// ============================================================================
// Compaction Metrics & Hooks (严格对齐 OpenClaw)
// ============================================================================

/**
 * Compaction Hook Event (对齐 OpenClaw internal-hooks.ts)
 */
struct CompactionHookEvent {
    std::string session_id;
    std::string session_key;
    std::string agent_id;
    std::string workspace_dir;
    std::string message_provider;
    
    // Metrics
    int message_count = 0;
    int token_count = 0;
    int compacted_count = 0;        // After 才有
    int message_count_original = 0; // Before 才有
    int token_count_original = 0;   // Before 才有
    
    // Compaction result
    std::string summary;
    int summary_length = 0;
    std::string first_kept_entry_id;
    int tokens_before = 0;
    int tokens_after = 0;
};

/**
 * Compaction Hook Runner (对齐 OpenClaw hooks)
 */
class CompactionHooks {
public:
    using BeforeCompactionCallback = std::function<void(const CompactionHookEvent&)>;
    using AfterCompactionCallback = std::function<void(const CompactionHookEvent&)>;
    
    /**
     * 注册 before_compaction hook (对齐 OpenClaw session:compact:before)
     */
    void register_before_compaction(BeforeCompactionCallback callback) {
        before_callbacks_.push_back(callback);
    }
    
    /**
     * 注册 after_compaction hook (对齐 OpenClaw session:compact:after)
     */
    void register_after_compaction(AfterCompactionCallback callback) {
        after_callbacks_.push_back(callback);
    }
    
    /**
     * 触发 before hooks
     */
    void trigger_before_compaction(const CompactionHookEvent& event) const;
    
    /**
     * 触发 after hooks
     */
    void trigger_after_compaction(const CompactionHookEvent& event) const;
    
    /**
     * 检查是否有 hooks
     */
    bool has_before_hooks() const { return !before_callbacks_.empty(); }
    bool has_after_hooks() const { return !after_callbacks_.empty(); }

private:
    std::vector<BeforeCompactionCallback> before_callbacks_;
    std::vector<AfterCompactionCallback> after_callbacks_;
};

/**
 * Compaction Metrics Collector (对齐 OpenClaw compaction diagnostics)
 */
struct CompactionMetrics {
    // Message 统计
    int messages_count = 0;
    int history_text_chars = 0;
    int tool_result_chars = 0;
    int est_tokens = 0;
    
    // Top contributors (对齐 OpenClaw [compaction-diag] contributors)
    struct Contributor {
        std::string role;
        int chars = 0;
        std::string tool_name;
    };
    std::vector<Contributor> top_contributors;  // Top 3
    
    // Timing
    int64_t duration_ms = 0;
};

/**
 * Compaction Diagnostics (对齐 OpenClaw [compaction-diag] logs)
 */
class CompactionDiagnostics {
public:
    /**
     * 生成诊断 ID (对齐 OpenClaw createCompactionDiagId)
     */
    static std::string generate_diag_id();
    
    /**
     * 汇总 compaction 消息指标 (对齐 OpenClaw summarizeCompactionMessages)
     */
    static CompactionMetrics summarize_messages(
        const std::vector<AgentMessage>& messages
    );
    
    /**
     * 分类 compaction 原因 (对齐 OpenClaw classifyCompactionReason)
     */
    static std::string classify_reason(const std::string& reason);
    
    /**
     * 记录 compaction 开始日志
     */
    static void log_compaction_start(
        const std::string& diag_id,
        const std::string& session_key,
        const std::string& trigger,
        const std::string& provider,
        const std::string& model_id,
        int attempt,
        int max_attempts,
        const CompactionMetrics& pre_metrics
    );
    
    /**
     * 记录 compaction 结束日志
     */
    static void log_compaction_end(
        const std::string& diag_id,
        const std::string& session_key,
        const std::string& trigger,
        const std::string& provider,
        const std::string& model_id,
        int attempt,
        int max_attempts,
        const std::string& outcome,
        const std::string& reason,
        int64_t duration_ms,
        bool retrying,
        const CompactionMetrics& pre_metrics,
        const CompactionMetrics& post_metrics
    );
};

} // namespace memory

// Forward declarations
namespace memory {
    struct AgentMessage;
}

} // namespace hunterclaw
