#pragma once

#include "hunterclaw/core/types.h"
#include <nlohmann/json.hpp>
#include <string>
#include <map>

namespace hunterclaw {
namespace gateway {

using json = nlohmann::json;

/**
 * 🔐 TODO 项
 * 
 * 100% OpenClaw 对齐的 TODO 数据结构,支持 Session 级别隔离。
 * 
 * 核心字段:
 * - id: 唯一标识
 * - session_id: 所属 Session (用于隔离)
 * - content: 内容
 * - status: 状态 (pending, in_progress, completed, cancelled)
 * - created_at: 创建时间
 * - updated_at: 更新时间
 * 
 * OpenClaw 对齐:
 * ```typescript
 * interface TodoItem {
 *   id: string;
 *   sessionId: string;
 *   content: string;
 *   status: 'pending' | 'in_progress' | 'completed' | 'cancelled';
 *   createdAt: number;
 *   updatedAt: number;
 *   metadata?: Record<string, string>;
 * }
 * ```
 */
struct TodoItem {
    std::string id;
    std::string session_id;      // 🔐 关键:Session 隔离
    std::string content;
    std::string status = "pending";
    core::Timestamp created_at;
    core::Timestamp updated_at;
    
    // 可选字段
    std::map<std::string, std::string> metadata;
    
    /**
     * 序列化为 JSON
     */
    json to_json() const {
        json j;
        j["id"] = id;
        j["session_id"] = session_id;
        j["content"] = content;
        j["status"] = status;
        j["created_at"] = created_at.time_since_epoch().count();
        j["updated_at"] = updated_at.time_since_epoch().count();
        
        if (!metadata.empty()) {
            j["metadata"] = json::object();
            for (const auto& [key, value] : metadata) {
                j["metadata"][key] = value;
            }
        }
        
        return j;
    }
    
    /**
     * 从 JSON 反序列化
     */
    static TodoItem from_json(const json& j) {
        TodoItem item;
        item.id = j.value("id", "");
        item.session_id = j.value("session_id", "");
        item.content = j.value("content", "");
        item.status = j.value("status", "pending");
        
        if (j.contains("created_at")) {
            item.created_at = core::Timestamp(
                std::chrono::nanoseconds(j["created_at"].get<int64_t>())
            );
        }
        
        if (j.contains("updated_at")) {
            item.updated_at = core::Timestamp(
                std::chrono::nanoseconds(j["updated_at"].get<int64_t>())
            );
        }
        
        if (j.contains("metadata") && j["metadata"].is_object()) {
            for (const auto& [key, value] : j["metadata"].items()) {
                if (value.is_string()) {
                    item.metadata[key] = value.get<std::string>();
                } else {
                    item.metadata[key] = value.dump();
                }
            }
        }
        
        return item;
    }
};

/**
 * TODO 状态常量
 */
namespace todo_status {
    constexpr const char* PENDING = "pending";
    constexpr const char* IN_PROGRESS = "in_progress";
    constexpr const char* COMPLETED = "completed";
    constexpr const char* CANCELLED = "cancelled";
    
    /**
     * 验证状态是否有效
     */
    inline bool is_valid_status(const std::string& status) {
        return status == PENDING ||
               status == IN_PROGRESS ||
               status == COMPLETED ||
               status == CANCELLED;
    }
}

} // namespace gateway
} // namespace hunterclaw
