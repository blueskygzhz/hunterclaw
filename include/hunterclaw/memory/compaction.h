#pragma once

#include <string>
#include <vector>
#include <optional>
#include <chrono>
#include "hunterclaw/core/result.h"

namespace hunterclaw {
namespace memory {

// 摘要指令
struct SummarizationInstructions {
    std::string system_prompt = "You are a helpful assistant that creates concise summaries.";
    std::string user_prompt_template = 
        "Please summarize the following conversation in a concise way, "
        "preserving key information, decisions, and context:\n\n{content}";
    std::optional<std::string> previous_summary;
};

// 压缩配置
struct CompactionConfig {
    // Token 限制
    int context_window = 200000;           // 上下文窗口大小
    int reserve_tokens = 16384;            // 保留 token 目标
    int reserve_tokens_floor = 20000;      // 最小强制保留值
    int keep_recent_tokens = 20000;        // 保留最近的 token 数
    int summarization_overhead = 4096;     // 摘要开销预留
    
    // 压缩策略
    float max_history_share = 0.5f;        // 历史最多占上下文比例 (50%)
    float safety_margin = 1.2f;            // Token 估算安全边距 (20%)
    int min_messages_for_split = 10;       // 最小分割消息数
    int parts = 2;                         // 分块数量
    
    // 分块比率
    float base_chunk_ratio = 0.4f;         // 基础分块比率 (40%)
    float min_chunk_ratio = 0.15f;         // 最小分块比率 (15%)
    
    // 触发条件
    bool auto_compress = true;             // 自动压缩
    int soft_threshold_tokens = 4000;      // 软阈值
    
    // 自定义指令
    SummarizationInstructions summarization;
};

// 压缩统计
struct CompactionStats {
    int total_compactions = 0;
    int messages_before = 0;
    int messages_after = 0;
    int tokens_before = 0;
    int tokens_after = 0;
    int tokens_saved = 0;
    float compression_ratio = 0.0f;
    std::chrono::system_clock::time_point last_compaction;
};

// Agent消息（简化版）
struct AgentMessage {
    std::string role;                      // user, assistant, system, tool
    std::string content;
    std::optional<std::string> name;       // tool name
    std::optional<std::string> tool_call_id;
    std::chrono::system_clock::time_point timestamp;
};

// 压缩结果
struct CompactionResult {
    std::vector<AgentMessage> compressed_messages;  // 压缩后的消息
    std::string summary;                            // 摘要内容
    CompactionStats stats;                          // 压缩统计
};

// Forward declaration
namespace llm {
    class LLMClient;
}

/**
 * 记忆压缩器 - 对齐 OpenClaw 的压缩策略
 */
class MemoryCompactor {
public:
    explicit MemoryCompactor(
        const CompactionConfig& config,
        std::shared_ptr<llm::LLMClient> llm_client = nullptr
    );
    
    // 估算消息的 Token 数（简化版：4 chars ≈ 1 token）
    static int estimate_tokens(const std::string& text);
    static int estimate_messages_tokens(const std::vector<AgentMessage>& messages);
    
    // 计算自适应分块比率
    float compute_adaptive_chunk_ratio(
        const std::vector<AgentMessage>& messages
    ) const;
    
    // 检查是否需要压缩
    bool should_compress(
        const std::vector<AgentMessage>& messages,
        int additional_prompt_tokens = 0
    ) const;
    
    // 多阶段摘要压缩（核心算法）
    core::Result<CompactionResult> summarize_in_stages(
        const std::vector<AgentMessage>& messages,
        const std::string& model_id,
        const std::string& api_key
    );
    
    // 分割消息为多个块
    std::vector<std::vector<AgentMessage>> split_messages_by_tokens(
        const std::vector<AgentMessage>& messages,
        int max_chunk_tokens
    ) const;
    
    // 对单个块生成摘要（需要 LLM 调用）
    core::Result<std::string> summarize_chunk(
        const std::vector<AgentMessage>& chunk,
        const std::string& model_id,
        const std::string& api_key,
        const std::optional<std::string>& previous_summary = std::nullopt
    );
    
    // 获取压缩统计
    const CompactionStats& get_stats() const { return stats_; }
    
    // 重置统计
    void reset_stats() { stats_ = CompactionStats{}; }

private:
    CompactionConfig config_;
    CompactionStats stats_;
    std::shared_ptr<llm::LLMClient> llm_client_;  // LLM 客户端
    
    // 格式化消息为文本（用于摘要）
    std::string format_messages_for_summary(
        const std::vector<AgentMessage>& messages
    ) const;
    
    // 创建摘要消息
    AgentMessage create_summary_message(
        const std::string& summary,
        const std::vector<AgentMessage>& original_messages
    ) const;
};

/**
 * 记忆刷新器 - 预压缩记忆持久化
 * 对应 OpenClaw 的 Memory Flush 机制
 */
class MemoryFlusher {
public:
    struct FlushConfig {
        bool enabled = true;
        int soft_threshold_tokens = 4000;
        size_t force_flush_transcript_bytes = 2 * 1024 * 1024;  // 2MB
        std::string prompt = 
            "Pre-compaction memory flush. "
            "Store durable memories now (use memory/YYYY-MM-DD.md; create memory/ if needed). "
            "IMPORTANT: If the file already exists, APPEND new content only. "
            "If nothing to store, reply with [SILENT].";
        std::string system_prompt = "Pre-compaction memory flush turn.";
    };
    
    explicit MemoryFlusher(const FlushConfig& config) : config_(config) {}
    
    // 检查是否需要刷新
    bool should_flush(
        int current_token_count,
        int context_window,
        int reserve_tokens_floor,
        size_t transcript_size_bytes
    ) const;
    
    // 执行记忆刷新（返回是否执行了刷新）
    core::Result<bool> flush_if_needed(
        int current_token_count,
        int context_window,
        int reserve_tokens_floor,
        size_t transcript_size_bytes,
        const std::function<core::Result<void>()>& flush_callback
    );
    
    const FlushConfig& config() const { return config_; }

private:
    FlushConfig config_;
    std::chrono::system_clock::time_point last_flush_;
};

} // namespace memory
} // namespace hunterclaw
