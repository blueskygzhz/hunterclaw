#include "hunterclaw/memory/compaction.h"
#include "hunterclaw/llm/llm_client.h"
#include "hunterclaw/core/utils.h"
#include <algorithm>
#include <numeric>
#include <sstream>
#include <iomanip>
#include <ctime>
#include <spdlog/spdlog.h>

namespace hunterclaw {
namespace memory {

namespace {
    auto log() {
        static auto logger = spdlog::get("memory") ? spdlog::get("memory") : spdlog::default_logger();
        return logger;
    }
}

// ============================================================================
// MemoryCompactor Implementation
// ============================================================================

MemoryCompactor::MemoryCompactor(
    const CompactionConfig& config,
    std::shared_ptr<llm::LLMClient> llm_client
) : config_(config), 
    stats_(),
    llm_client_(llm_client) {}

int MemoryCompactor::estimate_tokens(const std::string& text) {
    // 简化估算：平均 4 个字符 ≈ 1 个 token
    // 更精确的实现应使用 tiktoken 等 tokenizer
    return static_cast<int>(text.length() / 4);
}

int MemoryCompactor::estimate_messages_tokens(
    const std::vector<AgentMessage>& messages
) {
    int total = 0;
    for (const auto& msg : messages) {
        // 消息格式开销：role + 标点等约 10 tokens
        total += 10;
        total += estimate_tokens(msg.content);
        if (msg.name) {
            total += estimate_tokens(*msg.name);
        }
    }
    return total;
}

float MemoryCompactor::compute_adaptive_chunk_ratio(
    const std::vector<AgentMessage>& messages
) const {
    if (messages.empty()) {
        return config_.base_chunk_ratio;
    }
    
    const int total_tokens = estimate_messages_tokens(messages);
    const int avg_tokens = total_tokens / static_cast<int>(messages.size());
    const int safe_avg_tokens = static_cast<int>(avg_tokens * config_.safety_margin);
    const float avg_ratio = static_cast<float>(safe_avg_tokens) / config_.context_window;
    
    // 如果平均消息 > 上下文窗口的 10%，降低分块比率
    if (avg_ratio > 0.1f) {
        const float reduction = std::min(
            avg_ratio * 2.0f, 
            config_.base_chunk_ratio - config_.min_chunk_ratio
        );
        return std::max(config_.min_chunk_ratio, config_.base_chunk_ratio - reduction);
    }
    
    return config_.base_chunk_ratio;
}

bool MemoryCompactor::should_compress(
    const std::vector<AgentMessage>& messages,
    int additional_prompt_tokens
) const {
    if (!config_.auto_compress) {
        return false;
    }
    
    const int current_tokens = estimate_messages_tokens(messages) + additional_prompt_tokens;
    const int threshold = config_.context_window - config_.reserve_tokens;
    
    return current_tokens > threshold;
}

std::vector<std::vector<AgentMessage>> MemoryCompactor::split_messages_by_tokens(
    const std::vector<AgentMessage>& messages,
    int max_chunk_tokens
) const {
    std::vector<std::vector<AgentMessage>> chunks;
    std::vector<AgentMessage> current_chunk;
    int current_tokens = 0;
    
    for (const auto& msg : messages) {
        const int msg_tokens = estimate_tokens(msg.content) + 10;  // +10 for metadata
        
        if (current_tokens + msg_tokens > max_chunk_tokens && !current_chunk.empty()) {
            // 当前块已满，开始新块
            chunks.push_back(std::move(current_chunk));
            current_chunk.clear();
            current_tokens = 0;
        }
        
        current_chunk.push_back(msg);
        current_tokens += msg_tokens;
    }
    
    if (!current_chunk.empty()) {
        chunks.push_back(std::move(current_chunk));
    }
    
    return chunks;
}

std::string MemoryCompactor::format_messages_for_summary(
    const std::vector<AgentMessage>& messages
) const {
    std::ostringstream oss;
    
    for (size_t i = 0; i < messages.size(); ++i) {
        const auto& msg = messages[i];
        
        // 格式化时间戳
        auto time = std::chrono::system_clock::to_time_t(msg.timestamp);
        oss << "[" << std::put_time(std::localtime(&time), "%Y-%m-%d %H:%M:%S") << "] ";
        
        // 格式化角色
        if (msg.role == "user") {
            oss << "User: ";
        } else if (msg.role == "assistant") {
            oss << "Assistant: ";
        } else if (msg.role == "tool") {
            oss << "Tool (" << msg.name.value_or("unknown") << "): ";
        } else {
            oss << msg.role << ": ";
        }
        
        // 内容
        oss << msg.content;
        
        if (i < messages.size() - 1) {
            oss << "\n\n";
        }
    }
    
    return oss.str();
}

AgentMessage MemoryCompactor::create_summary_message(
    const std::string& summary,
    const std::vector<AgentMessage>& original_messages
) const {
    AgentMessage summary_msg;
    summary_msg.role = "system";
    summary_msg.content = "[Previous conversation summary]\n\n" + summary;
    summary_msg.timestamp = std::chrono::system_clock::now();
    
    // 如果有原始消息，使用第一条的时间戳
    if (!original_messages.empty()) {
        summary_msg.timestamp = original_messages.front().timestamp;
    }
    
    return summary_msg;
}

core::Result<std::string> MemoryCompactor::summarize_chunk(
    const std::vector<AgentMessage>& chunk,
    const std::string& model_id,
    const std::string& api_key,
    const std::optional<std::string>& previous_summary
) {
    // 格式化消息
    std::string content = format_messages_for_summary(chunk);
    
    // 如果有之前的摘要，添加到开头
    if (previous_summary) {
        content = "[Previous summary]\n" + *previous_summary + "\n\n" + content;
    }
    
    // 构建提示
    std::string prompt = config_.summarization.user_prompt_template;
    size_t pos = prompt.find("{content}");
    if (pos != std::string::npos) {
        prompt.replace(pos, 9, content);
    }
    
    // ===== LLM 调用 (100% 对齐 OpenClaw) =====
    
    // 检查 LLM Client 是否可用
    if (!llm_client_) {
        // 降级: 返回简化摘要
        return Result<std::string>::ok(
            "[Summarized " + std::to_string(chunk.size()) + 
            " messages covering conversation history]"
        );
    }
    
    // 构建 LLM 请求消息
    std::vector<llm::Message> messages;
    
    // System prompt
    llm::Message system_msg;
    system_msg.role = "system";
    system_msg.content = config_.summarization.system_prompt;
    messages.push_back(system_msg);
    
    // User prompt
    llm::Message user_msg;
    user_msg.role = "user";
    user_msg.content = prompt;
    messages.push_back(user_msg);
    
    // 创建完成请求
    llm::CompletionRequest request;
    request.model = model_id;
    request.messages = messages;
    request.max_tokens = 2000;
    request.temperature = 0.5;  // 较低温度以获得稳定摘要
    request.stream = false;
    
    // 调用 LLM
    auto result = llm_client_->complete(request);
    
    if (result.is_err()) {
        return Result<std::string>::err(
            "LLM summarization failed: " + result.error()
        );
    }
    
    // 提取摘要内容
    std::string summary = result.value().content;
    
    // 后处理: 移除多余空白 (假设 core::utils::trim 存在)
    // 如果不存在，使用简单的实现
    auto trim = [](const std::string& s) -> std::string {
        auto start = s.find_first_not_of(" \t\n\r");
        if (start == std::string::npos) return "";
        auto end = s.find_last_not_of(" \t\n\r");
        return s.substr(start, end - start + 1);
    };
    
    summary = trim(summary);
    
    log()->info("Generated summary for {} messages ({} chars)",
               chunk.size(), summary.length());
    
    // 更新统计 (如果提供了 usage 信息)
    if (result.value().usage.total_tokens > 0) {
        stats_.total_compactions++;
    }
    
    return Result<std::string>::ok(summary);
}

core::Result<CompactionResult> MemoryCompactor::summarize_in_stages(
    const std::vector<AgentMessage>& messages,
    const std::string& model_id,
    const std::string& api_key
) {
    CompactionResult result;
    result.stats.messages_before = static_cast<int>(messages.size());
    result.stats.tokens_before = estimate_messages_tokens(messages);
    
    // 检查是否需要分割
    if (messages.size() < static_cast<size_t>(config_.min_messages_for_split)) {
        // 消息太少，直接摘要整体
        auto summary_result = summarize_chunk(messages, model_id, api_key);
        if (!summary_result.is_ok()) {
            return Result<CompactionResult>::error(summary_result.error);
        }
        
        result.summary = summary_result.value;
        result.compressed_messages.push_back(create_summary_message(result.summary, messages));
    } else {
        // 计算自适应分块比率
        const float chunk_ratio = compute_adaptive_chunk_ratio(messages);
        const int max_chunk_tokens = static_cast<int>(
            config_.context_window * chunk_ratio
        );
        
        // 分割消息
        auto chunks = split_messages_by_tokens(messages, max_chunk_tokens);
        
        // 对每个块生成摘要
        std::vector<std::string> chunk_summaries;
        std::optional<std::string> previous_summary;
        
        for (const auto& chunk : chunks) {
            auto summary_result = summarize_chunk(chunk, model_id, api_key, previous_summary);
            if (!summary_result.is_ok()) {
                return Result<CompactionResult>::error(summary_result.error);
            }
            
            chunk_summaries.push_back(summary_result.value);
            previous_summary = summary_result.value;
        }
        
        // 合并所有块的摘要
        std::ostringstream oss;
        for (size_t i = 0; i < chunk_summaries.size(); ++i) {
            if (i > 0) oss << "\n\n";
            oss << "Part " << (i + 1) << "/" << chunk_summaries.size() << ":\n";
            oss << chunk_summaries[i];
        }
        
        result.summary = oss.str();
        result.compressed_messages.push_back(create_summary_message(result.summary, messages));
    }
    
    // 保留最近的几条消息
    const int keep_count = std::min(
        5,  // 最多保留 5 条
        static_cast<int>(messages.size())
    );
    
    if (keep_count > 0) {
        result.compressed_messages.insert(
            result.compressed_messages.end(),
            messages.end() - keep_count,
            messages.end()
        );
    }
    
    // 更新统计
    result.stats.messages_after = static_cast<int>(result.compressed_messages.size());
    result.stats.tokens_after = estimate_messages_tokens(result.compressed_messages);
    result.stats.tokens_saved = result.stats.tokens_before - result.stats.tokens_after;
    result.stats.compression_ratio = result.stats.tokens_after > 0
        ? static_cast<float>(result.stats.tokens_before) / result.stats.tokens_after
        : 1.0f;
    result.stats.last_compaction = std::chrono::system_clock::now();
    
    // 更新内部统计
    stats_.total_compactions++;
    stats_.tokens_saved += result.stats.tokens_saved;
    stats_.last_compaction = result.stats.last_compaction;
    
    return Result<CompactionResult>::ok(std::move(result));
}

// ============================================================================
// MemoryFlusher Implementation
// ============================================================================

bool MemoryFlusher::should_flush(
    int current_token_count,
    int context_window,
    int reserve_tokens_floor,
    size_t transcript_size_bytes
) const {
    if (!config_.enabled) {
        return false;
    }
    
    // Token 阈值检查
    const int flush_threshold = context_window - reserve_tokens_floor - config_.soft_threshold_tokens;
    const bool token_threshold_met = current_token_count >= flush_threshold;
    
    // 文件大小检查
    const bool size_threshold_met = transcript_size_bytes >= config_.force_flush_transcript_bytes;
    
    return token_threshold_met || size_threshold_met;
}

core::Result<bool> MemoryFlusher::flush_if_needed(
    int current_token_count,
    int context_window,
    int reserve_tokens_floor,
    size_t transcript_size_bytes,
    const std::function<Result<void>()>& flush_callback
) {
    if (!should_flush(current_token_count, context_window, reserve_tokens_floor, transcript_size_bytes)) {
        return Result<bool>::ok(false);
    }
    
    // 执行刷新回调
    auto result = flush_callback();
    if (!result.is_ok()) {
        return Result<bool>::error(result.error);
    }
    
    last_flush_ = std::chrono::system_clock::now();
    return Result<bool>::ok(true);
}

} // namespace memory
} // namespace hunterclaw
