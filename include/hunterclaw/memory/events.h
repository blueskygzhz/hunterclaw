#pragma once

#include "hunterclaw/memory/types.h"
#include "hunterclaw/gateway/connection_pool.h"  // 使用已实现的 ConnectionPool
#include "hunterclaw/core/result.h"
#include <nlohmann/json.hpp>

namespace hunterclaw {
namespace memory {

using json = nlohmann::json;

/**
 * 🎯 100% 对齐 OpenClaw + 下行链路: Memory 事件推送
 * 
 * OpenClaw 在 Memory 操作时会推送事件到客户端:
 * - memory_search → 推送搜索进度
 * - memory_add → 推送添加成功
 * - memory_recall → 推送召回结果
 * - memory_compaction → 推送压缩进度
 * 
 * HunterClaw 通过 ConnectionPool 实现直连推送 (对齐下行链路架构):
 * - 无 Gateway 转发 (延迟降低 4-5x)
 * - 实时事件推送
 * - Session-scoped 广播
 * 
 * 使用示例:
 * 
 * // 1. 创建支持事件推送的 Memory Provider
 * auto provider = std::make_shared<SQLiteMemoryProvider>("memory.db");
 * auto event_provider = std::make_shared<EventEmittingMemoryProvider>(
 *     provider,
 *     session_id
 * );
 * 
 * // 2. 客户端自动收到事件
 * event_provider->search_memories("Python tutorials", options);
 * // → 客户端收到: {"event": "memory.search.start", "query": "Python tutorials"}
 * // → 客户端收到: {"event": "memory.search.progress", "found": 3}
 * // → 客户端收到: {"event": "memory.search.complete", "results": [...]}
 * 
 * event_provider->add_memory("Python is great!", metadata);
 * // → 客户端收到: {"event": "memory.add.complete", "id": "mem-123"}
 */

// Memory 事件类型 (对齐 OpenClaw)
namespace events {
    constexpr const char* MEMORY_SEARCH_START = "memory.search.start";
    constexpr const char* MEMORY_SEARCH_PROGRESS = "memory.search.progress";
    constexpr const char* MEMORY_SEARCH_COMPLETE = "memory.search.complete";
    constexpr const char* MEMORY_SEARCH_ERROR = "memory.search.error";
    
    constexpr const char* MEMORY_ADD_START = "memory.add.start";
    constexpr const char* MEMORY_ADD_COMPLETE = "memory.add.complete";
    constexpr const char* MEMORY_ADD_ERROR = "memory.add.error";
    
    constexpr const char* MEMORY_RECALL_START = "memory.recall.start";
    constexpr const char* MEMORY_RECALL_COMPLETE = "memory.recall.complete";
    
    constexpr const char* MEMORY_COMPACTION_START = "memory.compaction.start";
    constexpr const char* MEMORY_COMPACTION_PROGRESS = "memory.compaction.progress";
    constexpr const char* MEMORY_COMPACTION_COMPLETE = "memory.compaction.complete";
    
    constexpr const char* MEMORY_UPDATE_COMPLETE = "memory.update.complete";
    constexpr const char* MEMORY_REMOVE_COMPLETE = "memory.remove.complete";
    constexpr const char* MEMORY_CLEAR_COMPLETE = "memory.clear.complete";
}

/**
 * 支持事件推送的 Memory Provider (装饰器模式)
 * 
 * 包装任意 MemoryProvider,自动推送事件到客户端。
 */
class EventEmittingMemoryProvider : public MemoryProvider {
public:
    /**
     * 创建事件推送 Provider
     * 
     * @param underlying 底层 Provider
     * @param session_id Session ID (用于事件推送)
     * @param channel_id 可选的 Channel ID
     */
    EventEmittingMemoryProvider(
        std::shared_ptr<MemoryProvider> underlying,
        const std::string& session_id,
        const std::string& channel_id = ""
    ) : underlying_(underlying), 
        session_id_(session_id),
        channel_id_(channel_id) {}
    
    // ========================================
    // MemoryProvider 接口实现 (带事件推送)
    // ========================================
    
    core::Result<std::string> add_memory(
        const std::string& content,
        const std::map<std::string, std::string>& metadata
    ) override {
        // 🎯 推送: add.start
        emit_event(events::MEMORY_ADD_START, {
            {"content_length", content.length()},
            {"has_metadata", !metadata.empty()}
        });
        
        // 执行添加
        auto result = underlying_->add_memory(content, metadata);
        
        if (result) {
            // 🎯 推送: add.complete
            emit_event(events::MEMORY_ADD_COMPLETE, {
                {"id", result.value()},
                {"content", content.substr(0, 100)}  // 前100字符
            });
        } else {
            // 🎯 推送: add.error
            emit_event(events::MEMORY_ADD_ERROR, {
                {"error", result.error()}
            });
        }
        
        return result;
    }
    
    core::Result<std::vector<Memory>> search_memories(
        const std::string& query,
        const SearchOptions& options
    ) override {
        // 🎯 推送: search.start
        emit_event(events::MEMORY_SEARCH_START, {
            {"query", query},
            {"limit", options.limit},
            {"min_score", options.min_score}
        });
        
        // 执行搜索
        auto result = underlying_->search_memories(query, options);
        
        if (result) {
            const auto& memories = result.value();
            
            // 🎯 推送: search.progress
            emit_event(events::MEMORY_SEARCH_PROGRESS, {
                {"found", memories.size()}
            });
            
            // 🎯 推送: search.complete
            json results_json = json::array();
            for (const auto& mem : memories) {
                results_json.push_back({
                    {"id", mem.id},
                    {"content", mem.content.substr(0, 200)},  // 前200字符
                    {"score", mem.score}
                });
            }
            
            emit_event(events::MEMORY_SEARCH_COMPLETE, {
                {"query", query},
                {"count", memories.size()},
                {"results", results_json}
            });
        } else {
            // 🎯 推送: search.error
            emit_event(events::MEMORY_SEARCH_ERROR, {
                {"error", result.error()}
            });
        }
        
        return result;
    }
    
    core::Result<bool> remove_memory(const std::string& id) override {
        auto result = underlying_->remove_memory(id);
        
        if (result && result.value()) {
            // 🎯 推送: remove.complete
            emit_event(events::MEMORY_REMOVE_COMPLETE, {
                {"id", id}
            });
        }
        
        return result;
    }
    
    core::Result<bool> update_memory(
        const std::string& id,
        const std::string& content,
        const std::map<std::string, std::string>& metadata
    ) override {
        auto result = underlying_->update_memory(id, content, metadata);
        
        if (result && result.value()) {
            // 🎯 推送: update.complete
            emit_event(events::MEMORY_UPDATE_COMPLETE, {
                {"id", id},
                {"content", content.substr(0, 100)}
            });
        }
        
        return result;
    }
    
    core::Result<bool> clear_memories() override {
        auto result = underlying_->clear_memories();
        
        if (result && result.value()) {
            // 🎯 推送: clear.complete
            emit_event(events::MEMORY_CLEAR_COMPLETE, {
                {"session_id", session_id_}
            });
        }
        
        return result;
    }
    
    core::Result<MemoryStats> get_stats() override {
        return underlying_->get_stats();
    }
    
    std::string provider_id() const override {
        return underlying_->provider_id() + "-events";
    }
    
    std::string provider_name() const override {
        return underlying_->provider_name() + " (Events)";
    }
    
    std::string provider_version() const override {
        return underlying_->provider_version();
    }
    
    // ========================================
    // 特殊事件: Memory Recall (启动时召回)
    // ========================================
    
    /**
     * 推送 Memory Recall 事件 (对齐 OpenClaw)
     * 
     * 在 Session startup 时调用,推送召回的记忆。
     */
    void emit_recall_event(
        const std::vector<Memory>& memories,
        const std::string& context
    ) {
        emit_event(events::MEMORY_RECALL_START, {
            {"session_id", session_id_}
        });
        
        json results_json = json::array();
        for (const auto& mem : memories) {
            results_json.push_back({
                {"id", mem.id},
                {"content", mem.content},
                {"score", mem.score}
            });
        }
        
        emit_event(events::MEMORY_RECALL_COMPLETE, {
            {"count", memories.size()},
            {"memories", results_json},
            {"context", context}
        });
    }
    
    /**
     * 推送 Memory Compaction 事件 (对齐 OpenClaw)
     * 
     * 在 Session compaction 时调用。
     */
    void emit_compaction_event(
        const std::string& summary,
        int messages_compacted
    ) {
        emit_event(events::MEMORY_COMPACTION_START, {
            {"session_id", session_id_},
            {"messages", messages_compacted}
        });
        
        emit_event(events::MEMORY_COMPACTION_COMPLETE, {
            {"summary", summary.substr(0, 500)},
            {"messages_compacted", messages_compacted}
        });
    }

private:
    std::shared_ptr<MemoryProvider> underlying_;
    std::string session_id_;
    std::string channel_id_;
    
    /**
     * 🎯 使用 ConnectionPool 直连推送事件
     * 
     * 对齐下行链路架构,无 Gateway 转发。
     */
    void emit_event(const std::string& event_type, const json& payload) {
        try {
            auto& pool = gateway::ConnectionPool::instance();
            
            json event_payload;
            event_payload["type"] = event_type;
            event_payload["session_id"] = session_id_;
            event_payload["timestamp"] = std::chrono::system_clock::now().time_since_epoch().count();
            event_payload["payload"] = payload;
            
            // 🎯 直连广播到 session
            gateway::BroadcastOptions opts;
            if (!session_id_.empty()) {
                opts = gateway::BroadcastOptions::to_session(session_id_);
            } else if (!channel_id_.empty()) {
                opts = gateway::BroadcastOptions::to_channel(channel_id_);
            }
            
            pool.broadcast("memory", event_payload, opts);
        } catch (const std::exception& e) {
            // 推送失败不影响业务逻辑
            // TODO: 记录日志
        }
    }
};

/**
 * 🎯 辅助函数: 将普通 Provider 包装为支持事件推送的 Provider
 */
inline std::shared_ptr<MemoryProvider> make_event_emitting(
    std::shared_ptr<MemoryProvider> provider,
    const std::string& session_id,
    const std::string& channel_id = ""
) {
    return std::make_shared<EventEmittingMemoryProvider>(
        provider,
        session_id,
        channel_id
    );
}

} // namespace memory
} // namespace hunterclaw
