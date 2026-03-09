#pragma once

#include <string>
#include <vector>
#include <map>
#include <set>
#include <chrono>
#include <functional>
#include <optional>
#include "hunterclaw/memory/compaction.h"

namespace hunterclaw {
namespace memory {

// 裁剪模式
enum class PruningMode {
    DISABLED,       // 禁用
    CACHE_TTL,      // 基于缓存 TTL
    TOKEN_RATIO,    // 基于 Token 比率
    HYBRID          // 混合模式
};

// 裁剪配置
struct PruningConfig {
    PruningMode mode = PruningMode::CACHE_TTL;
    
    // TTL 配置
    std::chrono::milliseconds ttl = std::chrono::minutes(5);  // 5 分钟
    
    // 保留策略
    int keep_last_assistants = 3;          // 保留最后 N 个助手回复
    bool protect_bootstrap = true;         // 保护第一个用户消息之前的内容
    
    // Token 比率阈值
    float soft_trim_ratio = 0.3f;          // 30% 时开始软裁剪
    float hard_clear_ratio = 0.5f;         // 50% 时开始硬清除
    int min_prunable_tool_chars = 50000;   // 最小可裁剪工具字符数
    
    // 软裁剪设置
    struct SoftTrim {
        int max_chars = 4000;              // 超过此大小才裁剪
        int head_chars = 1500;             // 保留头部字符数
        int tail_chars = 1500;             // 保留尾部字符数
    } soft_trim;
    
    // 硬清除设置
    struct HardClear {
        bool enabled = true;
        std::string placeholder = "[Old tool result content cleared]";
    } hard_clear;
    
    // 工具过滤
    struct ToolMatch {
        std::vector<std::string> allow;    // 允许列表（支持 * 通配符）
        std::vector<std::string> deny;     // 拒绝列表（优先级更高）
    } tools;
};

// 裁剪统计
struct PruningStats {
    int total_prunes = 0;
    int soft_trims = 0;
    int hard_clears = 0;
    int messages_pruned = 0;
    int chars_saved = 0;
    std::chrono::system_clock::time_point last_prune;
};

/**
 * 上下文裁剪器 - 对齐 OpenClaw 的裁剪策略
 */
class ContextPruner {
public:
    explicit ContextPruner(const PruningConfig& config);
    
    // 裁剪消息列表
    std::vector<AgentMessage> prune_messages(
        const std::vector<AgentMessage>& messages,
        int context_window_tokens
    );
    
    // 检查工具是否可裁剪
    bool is_tool_prunable(const std::string& tool_name) const;
    
    // 软裁剪工具结果（保留头尾，插入 ...）
    std::optional<AgentMessage> soft_trim_tool_result(
        const AgentMessage& msg,
        int context_used_chars,
        int context_window_chars
    );
    
    // 硬清除工具结果（完全替换）
    std::optional<AgentMessage> hard_clear_tool_result(
        const AgentMessage& msg
    );
    
    // 获取裁剪统计
    const PruningStats& get_stats() const { return stats_; }
    
    // 重置统计
    void reset_stats() { stats_ = PruningStats{}; }

private:
    PruningConfig config_;
    PruningStats stats_;
    
    // 查找助手消息的截断索引
    int find_assistant_cutoff_index(
        const std::vector<AgentMessage>& messages,
        int keep_last
    ) const;
    
    // 查找第一个用户消息索引
    int find_first_user_index(
        const std::vector<AgentMessage>& messages
    ) const;
    
    // 检查消息是否过期（基于 TTL）
    bool is_message_expired(const AgentMessage& msg) const;
    
    // 计算上下文占用比率
    float calculate_context_ratio(
        int used_chars,
        int window_chars
    ) const;
    
    // 模式匹配（支持 * 通配符）
    bool matches_pattern(
        const std::string& text,
        const std::string& pattern
    ) const;
    
    // 提取头部文本
    std::string take_head_from_text(
        const std::string& text,
        int max_chars
    ) const;
    
    // 提取尾部文本
    std::string take_tail_from_text(
        const std::string& text,
        int max_chars
    ) const;
};

/**
 * 历史管理器 - LRU 淘汰和回合限制
 */
class HistoryManager {
public:
    static constexpr int MAX_HISTORY_KEYS = 1000;
    static constexpr int DEFAULT_GROUP_HISTORY_LIMIT = 50;
    
    struct HistoryConfig {
        int max_keys = MAX_HISTORY_KEYS;
        int default_turn_limit = DEFAULT_GROUP_HISTORY_LIMIT;
        bool prune_processed_images = true;
        std::string pruned_image_marker = "[image data removed - already processed by model]";
    };
    
    explicit HistoryManager(const HistoryConfig& config) : config_(config) {}
    
    // 限制历史回合数
    std::vector<AgentMessage> limit_history_turns(
        const std::vector<AgentMessage>& messages,
        int turn_limit
    ) const;
    
    // 裁剪已处理的图像
    bool prune_processed_images(std::vector<AgentMessage>& messages);
    
    // LRU 淘汰最旧的键
    template<typename T>
    void evict_old_history_keys(
        std::map<std::string, std::vector<T>>& history_map,
        int max_keys = MAX_HISTORY_KEYS
    ) const;
    
    // 追加历史条目（刷新插入顺序）
    template<typename T>
    std::vector<T> append_history_entry(
        std::map<std::string, std::vector<T>>& history_map,
        const std::string& history_key,
        const T& entry,
        int limit
    );

private:
    HistoryConfig config_;
    
    // 查找最后一个助手消息索引
    int find_last_assistant_index(
        const std::vector<AgentMessage>& messages
    ) const;
};

// ============================================================================
// Template Implementation
// ============================================================================

template<typename T>
void HistoryManager::evict_old_history_keys(
    std::map<std::string, std::vector<T>>& history_map,
    int max_keys
) const {
    if (history_map.size() <= static_cast<size_t>(max_keys)) {
        return;
    }
    
    const int keys_to_delete = static_cast<int>(history_map.size()) - max_keys;
    auto it = history_map.begin();
    
    for (int i = 0; i < keys_to_delete && it != history_map.end(); ++i) {
        it = history_map.erase(it);
    }
}

template<typename T>
std::vector<T> HistoryManager::append_history_entry(
    std::map<std::string, std::vector<T>>& history_map,
    const std::string& history_key,
    const T& entry,
    int limit
) {
    // 获取或创建历史列表
    auto it = history_map.find(history_key);
    std::vector<T> history;
    
    if (it != history_map.end()) {
        history = std::move(it->second);
        history_map.erase(it);  // 删除旧键以刷新插入顺序
    }
    
    // 追加新条目
    history.push_back(entry);
    
    // 限制历史长度
    while (history.size() > static_cast<size_t>(limit)) {
        history.erase(history.begin());
    }
    
    // 重新插入（刷新到末尾，最近使用）
    history_map[history_key] = history;
    
    // 淘汰最旧的键
    evict_old_history_keys(history_map, config_.max_keys);
    
    return history;
}

} // namespace memory
} // namespace hunterclaw
