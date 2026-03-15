#include <optional>
#include "hunterclaw/memory/pruning.h"
#include <algorithm>
#include <sstream>

namespace hunterclaw {
namespace memory {

// ============================================================================
// ContextPruner Implementation
// ============================================================================

ContextPruner::ContextPruner(const PruningConfig& config)
    : config_(config), stats_() {}

int ContextPruner::find_assistant_cutoff_index(
    const std::vector<AgentMessage>& messages,
    int keep_last
) const {
    if (messages.empty()) {
        return -1;
    }
    
    int assistant_count = 0;
    
    // 从后向前遍历，找到第 keep_last 个助手消息
    for (int i = static_cast<int>(messages.size()) - 1; i >= 0; --i) {
        if (messages[i].role == "assistant") {
            assistant_count++;
            if (assistant_count >= keep_last) {
                return i;
            }
        }
    }
    
    return 0;  // 没找到足够的助手消息，从头开始
}

int ContextPruner::find_first_user_index(
    const std::vector<AgentMessage>& messages
) const {
    for (size_t i = 0; i < messages.size(); ++i) {
        if (messages[i].role == "user") {
            return static_cast<int>(i);
        }
    }
    return -1;
}

bool ContextPruner::is_message_expired(const AgentMessage& msg) const {
    if (config_.mode != PruningMode::CACHE_TTL) {
        return false;
    }
    
    auto now = std::chrono::system_clock::now();
    auto age = std::chrono::duration_cast<std::chrono::milliseconds>(now - msg.timestamp);
    
    return age > config_.ttl;
}

float ContextPruner::calculate_context_ratio(
    int used_chars,
    int window_chars
) const {
    if (window_chars <= 0) {
        return 0.0f;
    }
    return static_cast<float>(used_chars) / window_chars;
}

bool ContextPruner::matches_pattern(
    const std::string& text,
    const std::string& pattern
) const {
    // 简单通配符匹配
    if (pattern == "*") {
        return true;
    }
    
    // 转换为小写比较
    std::string lower_text = text;
    std::string lower_pattern = pattern;
    std::transform(lower_text.begin(), lower_text.end(), lower_text.begin(), ::tolower);
    std::transform(lower_pattern.begin(), lower_pattern.end(), lower_pattern.begin(), ::tolower);
    
    // 查找通配符位置
    size_t star_pos = lower_pattern.find('*');
    if (star_pos == std::string::npos) {
        // 没有通配符，精确匹配
        return lower_text == lower_pattern;
    }
    
    // 处理 * 通配符
    std::string prefix = lower_pattern.substr(0, star_pos);
    std::string suffix = lower_pattern.substr(star_pos + 1);
    
    if (!prefix.empty() && lower_text.find(prefix) != 0) {
        return false;
    }
    
    if (!suffix.empty() && lower_text.rfind(suffix) != lower_text.length() - suffix.length()) {
        return false;
    }
    
    return true;
}

bool ContextPruner::is_tool_prunable(const std::string& tool_name) const {
    // Deny 列表优先
    for (const auto& pattern : config_.tools.deny) {
        if (matches_pattern(tool_name, pattern)) {
            return false;
        }
    }
    
    // 空 allow 列表 => 全部允许
    if (config_.tools.allow.empty()) {
        return true;
    }
    
    // 检查是否在 allow 列表中
    for (const auto& pattern : config_.tools.allow) {
        if (matches_pattern(tool_name, pattern)) {
            return true;
        }
    }
    
    return false;
}

std::string ContextPruner::take_head_from_text(
    const std::string& text,
    int max_chars
) const {
    if (text.length() <= static_cast<size_t>(max_chars)) {
        return text;
    }
    return text.substr(0, max_chars);
}

std::string ContextPruner::take_tail_from_text(
    const std::string& text,
    int max_chars
) const {
    if (text.length() <= static_cast<size_t>(max_chars)) {
        return text;
    }
    return text.substr(text.length() - max_chars);
}

std::optional<AgentMessage> ContextPruner::soft_trim_tool_result(
    const AgentMessage& msg,
    int context_used_chars,
    int context_window_chars
) {
    // 检查是否是工具消息
    if (msg.role != "tool" || !msg.name) {
        return std::nullopt;
    }
    
    // 检查是否可裁剪
    if (!is_tool_prunable(*msg.name)) {
        return std::nullopt;
    }
    
    // 检查大小是否超过阈值
    if (msg.content.length() < static_cast<size_t>(config_.soft_trim.max_chars)) {
        return std::nullopt;
    }
    
    // 检查上下文占用比率
    float ratio = calculate_context_ratio(context_used_chars, context_window_chars);
    if (ratio < config_.soft_trim_ratio) {
        return std::nullopt;
    }
    
    // 执行软裁剪
    std::string head = take_head_from_text(msg.content, config_.soft_trim.head_chars);
    std::string tail = take_tail_from_text(msg.content, config_.soft_trim.tail_chars);
    
    std::ostringstream oss;
    oss << head << "\n...\n" << tail;
    oss << "\n\n[Tool result trimmed: kept first " << config_.soft_trim.head_chars
        << " chars and last " << config_.soft_trim.tail_chars
        << " chars of " << msg.content.length() << " chars.]";
    
    AgentMessage trimmed = msg;
    trimmed.content = oss.str();
    
    stats_.soft_trims++;
    stats_.chars_saved += static_cast<int>(msg.content.length() - trimmed.content.length());
    
    return trimmed;
}

std::optional<AgentMessage> ContextPruner::hard_clear_tool_result(
    const AgentMessage& msg
) {
    if (!config_.hard_clear.enabled) {
        return std::nullopt;
    }
    
    // 检查是否是工具消息
    if (msg.role != "tool" || !msg.name) {
        return std::nullopt;
    }
    
    // 检查是否可裁剪
    if (!is_tool_prunable(*msg.name)) {
        return std::nullopt;
    }
    
    // 执行硬清除
    AgentMessage cleared = msg;
    int saved = static_cast<int>(cleared.content.length());
    cleared.content = config_.hard_clear.placeholder;
    
    stats_.hard_clears++;
    stats_.chars_saved += saved;
    
    return cleared;
}

std::vector<AgentMessage> ContextPruner::prune_messages(
    const std::vector<AgentMessage>& messages,
    int context_window_tokens
) {
    if (config_.mode == PruningMode::DISABLED) {
        return messages;
    }
    
    std::vector<AgentMessage> pruned;
    pruned.reserve(messages.size());
    
    // 估算上下文窗口的字符数（4 chars ≈ 1 token）
    const int context_window_chars = context_window_tokens * 4;
    
    // 查找保护区域
    const int cutoff_index = find_assistant_cutoff_index(messages, config_.keep_last_assistants);
    const int first_user_index = find_first_user_index(messages);
    const int prune_start_index = config_.protect_bootstrap && first_user_index >= 0
        ? first_user_index
        : 0;
    
    // 第一遍：软裁剪
    int context_used_chars = 0;
    for (size_t i = 0; i < messages.size(); ++i) {
        const auto& msg = messages[i];
        context_used_chars += static_cast<int>(msg.content.length());
        
        // 保护区域直接保留
        if (static_cast<int>(i) < prune_start_index || static_cast<int>(i) >= cutoff_index) {
            pruned.push_back(msg);
            continue;
        }
        
        // 检查是否过期（TTL 模式）
        if (is_message_expired(msg)) {
            stats_.messages_pruned++;
            continue;  // 跳过过期消息
        }
        
        // 尝试软裁剪
        auto trimmed = soft_trim_tool_result(msg, context_used_chars, context_window_chars);
        if (trimmed) {
            pruned.push_back(*trimmed);
        } else {
            pruned.push_back(msg);
        }
    }
    
    // 第二遍：如果还超过阈值，进行硬清除
    context_used_chars = 0;
    for (auto& msg : pruned) {
        context_used_chars += static_cast<int>(msg.content.length());
    }
    
    float ratio = calculate_context_ratio(context_used_chars, context_window_chars);
    if (ratio >= config_.hard_clear_ratio) {
        std::vector<AgentMessage> hard_cleared;
        hard_cleared.reserve(pruned.size());
        
        for (size_t i = 0; i < pruned.size(); ++i) {
            const auto& msg = pruned[i];
            
            // 保护区域直接保留
            if (static_cast<int>(i) < prune_start_index || 
                static_cast<int>(i) >= static_cast<int>(pruned.size()) - config_.keep_last_assistants) {
                hard_cleared.push_back(msg);
                continue;
            }
            
            // 尝试硬清除
            auto cleared = hard_clear_tool_result(msg);
            if (cleared) {
                hard_cleared.push_back(*cleared);
                
                // 重新计算比率
                context_used_chars = 0;
                for (const auto& m : hard_cleared) {
                    context_used_chars += static_cast<int>(m.content.length());
                }
                ratio = calculate_context_ratio(context_used_chars, context_window_chars);
                
                // 如果比率降到阈值以下，停止硬清除
                if (ratio < config_.hard_clear_ratio) {
                    // 添加剩余消息
                    for (size_t j = i + 1; j < pruned.size(); ++j) {
                        hard_cleared.push_back(pruned[j]);
                    }
                    break;
                }
            } else {
                hard_cleared.push_back(msg);
            }
        }
        
        pruned = std::move(hard_cleared);
    }
    
    stats_.total_prunes++;
    stats_.last_prune = std::chrono::system_clock::now();
    
    return pruned;
}

// ============================================================================
// HistoryManager Implementation
// ============================================================================

std::vector<AgentMessage> HistoryManager::limit_history_turns(
    const std::vector<AgentMessage>& messages,
    int turn_limit
) const {
    if (turn_limit <= 0 || messages.empty()) {
        return messages;
    }
    
    int user_count = 0;
    int last_user_index = static_cast<int>(messages.size());
    
    // 从后向前遍历，计数用户消息
    for (int i = static_cast<int>(messages.size()) - 1; i >= 0; --i) {
        if (messages[i].role == "user") {
            user_count++;
            if (user_count > turn_limit) {
                // 从 last_user_index 开始截断
                return std::vector<AgentMessage>(
                    messages.begin() + last_user_index,
                    messages.end()
                );
            }
            last_user_index = i;
        }
    }
    
    return messages;
}

int HistoryManager::find_last_assistant_index(
    const std::vector<AgentMessage>& messages
) const {
    for (int i = static_cast<int>(messages.size()) - 1; i >= 0; --i) {
        if (messages[i].role == "assistant") {
            return i;
        }
    }
    return -1;
}

bool HistoryManager::prune_processed_images(std::vector<AgentMessage>& messages) {
    if (!config_.prune_processed_images) {
        return false;
    }
    
    const int last_assistant_index = find_last_assistant_index(messages);
    if (last_assistant_index < 0) {
        return false;
    }
    
    bool did_mutate = false;
    
    // 替换最后一个助手之前的所有图像内容
    for (int i = 0; i < last_assistant_index; ++i) {
        auto& msg = messages[i];
        if (msg.role != "user") {
            continue;
        }
        
        // 检查内容中是否包含图像标记
        if (msg.content.find("[image:") != std::string::npos ||
            msg.content.find("<image>") != std::string::npos) {
            // 替换为占位符
            // 实际实现中应该解析结构化内容，这里简化处理
            msg.content = config_.pruned_image_marker;
            did_mutate = true;
        }
    }
    
    return did_mutate;
}

} // namespace memory
} // namespace hunterclaw
