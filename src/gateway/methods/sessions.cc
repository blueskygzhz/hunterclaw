#include "hunterclaw/gateway/gateway.h"
#include "hunterclaw/gateway/method_guard.h"
#include "hunterclaw/core/utils.h"
#include "hunterclaw/agents/agent_pool.h"
#include "hunterclaw/sessions/session_manager.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <filesystem>
#include <fstream>

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace hunterclaw {
namespace gateway {
namespace methods {

// ============================================================================
// Session Methods - 严格对齐 OpenClaw
// 参考: openclaw-analysis/src/gateway/server-methods/sessions.ts
// ============================================================================

namespace {

// 辅助函数: 解析 session key 类型
std::string classify_session_kind(const std::string& key) {
    if (key == "global") return "global";
    if (key == "unknown") return "unknown";
    if (key.find("agent:") == 0) {
        // agent:main:telegram:group:123 -> group
        // agent:main:telegram:direct:456 -> direct
        size_t third_colon = key.find(':', key.find(':', key.find(':') + 1) + 1);
        if (third_colon != std::string::npos) {
            size_t fourth_colon = key.find(':', third_colon + 1);
            if (fourth_colon != std::string::npos) {
                std::string scope = key.substr(third_colon + 1, fourth_colon - third_colon - 1);
                if (scope == "group" || scope == "channel") return "group";
                if (scope == "direct" || scope == "dm") return "direct";
            }
        }
    }
    return "direct";  // 默认 direct
}

// 辅助函数: 从 session key 解析 agent ID
std::string extract_agent_id(const std::string& key) {
    if (key.find("agent:") != 0) return "main";
    
    size_t first_colon = key.find(':');
    size_t second_colon = key.find(':', first_colon + 1);
    if (second_colon == std::string::npos) return "main";
    
    return key.substr(first_colon + 1, second_colon - first_colon - 1);
}

} // anonymous namespace

// ============================================================================
// sessions.list - 列出会话 (严格对齐 OpenClaw)
// 参考: openclaw-analysis/src/gateway/server-methods/sessions.ts:96-110
// ============================================================================
core::Result<json> method_session_list(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    // 🎯 OpenClaw 对齐: 参数提取
    bool include_global = params.value("includeGlobal", false);
    bool include_unknown = params.value("includeUnknown", false);
    bool include_derived_titles = params.value("includeDerivedTitles", false);
    bool include_last_message = params.value("includeLastMessage", false);
    std::string spawned_by = params.value("spawnedBy", "");
    std::string label_filter = params.value("label", "");
    std::string agent_id_filter = params.value("agentId", "");
    std::string search = params.value("search", "");
    
    // activeMinutes: 活跃时间过滤 (分钟)
    int active_minutes = -1;
    if (params.contains("activeMinutes")) {
        active_minutes = params["activeMinutes"].get<int>();
    }
    
    // limit: 结果数量限制
    int limit = 0;
    if (params.contains("limit")) {
        limit = params["limit"].get<int>();
    }
    
    auto now = std::chrono::system_clock::now();
    auto active_threshold = now - std::chrono::minutes(active_minutes > 0 ? active_minutes : 0);
    
    // 🎯 OpenClaw 对齐: 从 session store 读取所有会话
    auto& session_mgr = server->runtime_state()->session_manager;
    if (!session_mgr) {
        return core::Result<json>::Err("Session manager not initialized");
    }
    
    auto all_sessions = session_mgr->list_all_sessions();
    
    // 🎯 OpenClaw 对齐: 过滤会话
    json sessions_array = json::array();
    
    for (const auto& session : all_sessions) {
        const std::string& key = session.session_id;
        
        // 🎯 OpenClaw 对齐: 过滤 cron run sessions
        if (key.find("cron:run:") == 0) {
            continue;
        }
        
        // 🎯 OpenClaw 对齐: 过滤 global/unknown
        if (!include_global && key == "global") continue;
        if (!include_unknown && key == "unknown") continue;
        
        // 🎯 OpenClaw 对齐: agent ID 过滤
        if (!agent_id_filter.empty()) {
            std::string session_agent = extract_agent_id(key);
            if (session_agent != agent_id_filter) continue;
        }
        
        // 🎯 OpenClaw 对齐: spawnedBy 过滤
        if (!spawned_by.empty()) {
            auto metadata_it = session.metadata.find("spawnedBy");
            if (metadata_it == session.metadata.end() || 
                metadata_it->second != spawned_by) {
                continue;
            }
        }
        
        // 🎯 OpenClaw 对齐: label 过滤
        if (!label_filter.empty()) {
            auto label_it = session.metadata.find("label");
            if (label_it == session.metadata.end() || 
                label_it->second != label_filter) {
                continue;
            }
        }
        
        // 🎯 OpenClaw 对齐: 活跃时间过滤
        if (active_minutes > 0 && session.last_active_at < active_threshold) {
            continue;
        }
        
        // 🎯 OpenClaw 对齐: 搜索过滤 (key, label, displayName)
        if (!search.empty()) {
            std::string search_lower = search;
            std::transform(search_lower.begin(), search_lower.end(), 
                         search_lower.begin(), ::tolower);
            
            std::string key_lower = key;
            std::transform(key_lower.begin(), key_lower.end(), 
                         key_lower.begin(), ::tolower);
            
            if (key_lower.find(search_lower) == std::string::npos) {
                // TODO: 也搜索 label 和 displayName
                continue;
            }
        }
        
        // 🎯 OpenClaw 对齐: 构建 session entry
        json session_json;
        session_json["key"] = key;
        session_json["agentId"] = extract_agent_id(key);
        session_json["kind"] = classify_session_kind(key);
        
        // 🎯 OpenClaw 对齐: 时间戳 (毫秒)
        session_json["updatedAt"] = std::chrono::duration_cast<std::chrono::milliseconds>(
            session.last_active_at.time_since_epoch()
        ).count();
        
        // 🎯 OpenClaw 对齐: Token 统计
        auto tokens_it = session.metadata.find("totalTokens");
        int total_tokens = tokens_it != session.metadata.end() 
            ? std::stoi(tokens_it->second) : 0;
        session_json["totalTokens"] = total_tokens;
        session_json["totalTokensFresh"] = false;  // TODO: 实现 token 刷新逻辑
        session_json["contextTokens"] = nullptr;  // null 表示未知
        
        // 🎯 OpenClaw 对齐: 模型信息
        auto model_it = session.metadata.find("model");
        session_json["model"] = model_it != session.metadata.end() 
            ? model_it->second : "unknown";
        
        auto provider_it = session.metadata.find("modelProvider");
        if (provider_it != session.metadata.end()) {
            session_json["modelProvider"] = provider_it->second;
        }
        
        // 🎯 OpenClaw 对齐: label (可选)
        auto label_it = session.metadata.find("label");
        if (label_it != session.metadata.end()) {
            session_json["label"] = label_it->second;
        }
        
        // 🎯 OpenClaw 对齐: displayName (可选)
        auto display_it = session.metadata.find("displayName");
        if (display_it != session.metadata.end()) {
            session_json["displayName"] = display_it->second;
        }
        
        // 🎯 OpenClaw 对齐: spawnedBy (可选)
        auto spawned_it = session.metadata.find("spawnedBy");
        if (spawned_it != session.metadata.end()) {
            session_json["spawnedBy"] = spawned_it->second;
        }
        
        // 🎯 OpenClaw 对齐: Flags
        json flags;
        flags["locked"] = false;  // TODO: 从 metadata 读取
        flags["systemPrompt"] = session.metadata.count("systemPrompt") > 0;
        flags["elevated"] = session.metadata.count("elevated") > 0;
        session_json["flags"] = flags;
        
        // 🎯 OpenClaw 对齐: 传递策略 (deliveryPolicy)
        auto policy_it = session.metadata.find("deliveryPolicy");
        if (policy_it != session.metadata.end()) {
            session_json["deliveryPolicy"] = policy_it->second;
        }
        
        sessions_array.push_back(session_json);
    }
    
    // 🎯 OpenClaw 对齐: 按 updatedAt 降序排序
    std::sort(sessions_array.begin(), sessions_array.end(), 
        [](const json& a, const json& b) {
            return a["updatedAt"].get<int64_t>() > b["updatedAt"].get<int64_t>();
        });
    
    // 🎯 OpenClaw 对齐: 应用 limit
    if (limit > 0 && sessions_array.size() > static_cast<size_t>(limit)) {
        sessions_array = json::array(sessions_array.begin(), 
                                    sessions_array.begin() + limit);
    }
    
    // 🎯 OpenClaw 对齐: 构建结果
    json result;
    result["sessions"] = sessions_array;
    result["count"] = sessions_array.size();
    
    // 🎯 OpenClaw 对齐: 可选字段
    if (active_minutes > 0) {
        result["activeMinutes"] = active_minutes;
    }
    if (limit > 0) {
        result["limit"] = limit;
    }
    if (include_global) {
        result["includeGlobal"] = true;
    }
    if (include_unknown) {
        result["includeUnknown"] = true;
    }
    
    return core::Result<json>::Ok(result);
}

// ============================================================================
// sessions.preview - 预览会话消息 (严格对齐 OpenClaw)
// 参考: openclaw-analysis/src/gateway/server-methods/sessions.ts:111-172
// ============================================================================
core::Result<json> method_session_preview(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    if (!params.contains("keys") || !params["keys"].is_array()) {
        return core::Result<json>::Err("Missing or invalid 'keys' parameter");
    }
    
    // 🎯 OpenClaw 对齐: 参数提取
    std::vector<std::string> keys;
    for (const auto& key_json : params["keys"]) {
        if (key_json.is_string()) {
            std::string key = key_json.get<std::string>();
            if (!key.empty()) {
                keys.push_back(key);
            }
        }
    }
    
    // 🎯 OpenClaw 对齐: 最多处理 64 个 key
    if (keys.size() > 64) {
        keys.resize(64);
    }
    
    int limit = params.value("limit", 12);  // 默认 12 条消息
    int max_chars = params.value("maxChars", 240);  // 默认 240 字符
    
    limit = std::max(1, limit);
    max_chars = std::max(20, max_chars);
    
    auto& session_mgr = server->runtime_state()->session_manager;
    if (!session_mgr) {
        return core::Result<json>::Err("Session manager not initialized");
    }
    
    // 🎯 OpenClaw 对齐: 为每个 key 生成预览
    json previews = json::array();
    
    for (const auto& key : keys) {
        json preview_entry;
        preview_entry["key"] = key;
        
        try {
            // 加载 session
            auto session = session_mgr->get_session(key);
            if (!session.ok) {
                preview_entry["status"] = "missing";
                preview_entry["items"] = json::array();
                previews.push_back(preview_entry);
                continue;
            }
            
            // 读取最近的消息
            auto messages = session_mgr->get_messages(key, limit);
            
            json items = json::array();
            for (const auto& msg : messages) {
                json item;
                item["role"] = msg.role;
                
                // 🎯 OpenClaw 对齐: 截断内容到 maxChars
                std::string content = msg.content;
                if (content.size() > static_cast<size_t>(max_chars)) {
                    content = content.substr(0, max_chars) + "…";
                }
                item["content"] = content;
                
                // 🎯 OpenClaw 对齐: 时间戳 (可选)
                if (msg.metadata.count("timestamp")) {
                    item["ts"] = std::stoll(msg.metadata.at("timestamp"));
                }
                
                items.push_back(item);
            }
            
            preview_entry["status"] = items.empty() ? "empty" : "ok";
            preview_entry["items"] = items;
            
        } catch (...) {
            preview_entry["status"] = "error";
            preview_entry["items"] = json::array();
        }
        
        previews.push_back(preview_entry);
    }
    
    // 🎯 OpenClaw 对齐: 构建结果
    json result;
    result["ts"] = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
    result["previews"] = previews;
    
    return core::Result<json>::Ok(result);
}

// ============================================================================
// sessions.patch - 更新会话元数据 (严格对齐 OpenClaw)
// 参考: openclaw-analysis/src/gateway/server-methods/sessions.ts:187-229
// ============================================================================
core::Result<json> method_session_patch(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    if (!params.contains("key")) {
        return core::Result<json>::Err("Missing 'key' parameter");
    }
    
    std::string key = params["key"].get<std::string>();
    if (key.empty()) {
        return core::Result<json>::Err("key required");
    }
    
    auto& session_mgr = server->runtime_state()->session_manager;
    if (!session_mgr) {
        return core::Result<json>::Err("Session manager not initialized");
    }
    
    // 🎯 OpenClaw 对齐: 加载现有 session
    auto session_result = session_mgr->get_session(key);
    if (!session_result.ok) {
        // 如果 session 不存在,创建新的
        sessions::SessionOptions opts;
        opts.agent_id = extract_agent_id(key);
        session_mgr->create_session(key, opts);
    }
    
    // 🎯 OpenClaw 对齐: 应用 patch 到 metadata
    std::map<std::string, std::string> updates;
    
    // OpenClaw 支持的 patch 字段:
    // - label: 会话标签
    // - agentId: agent ID (不建议修改)
    // - model: 模型名称
    // - thinking: "none" | "low" | "medium" | "high"
    // - toolPolicy: 工具策略
    // - externalContentHandling: "default" | "unsafe"
    // - thinkingLevel: thinking 的别名
    // - verboseLevel: 详细级别
    // - reasoningLevel: 推理级别
    // - fastMode: 快速模式
    // - deliveryPolicy: 传递策略
    // - systemPrompt: 系统提示
    // - elevated: 提升权限模式
    
    if (params.contains("label")) {
        updates["label"] = params["label"].is_null() 
            ? "" : params["label"].get<std::string>();
    }
    
    if (params.contains("model")) {
        updates["model"] = params["model"].is_null() 
            ? "" : params["model"].get<std::string>();
    }
    
    if (params.contains("thinking")) {
        updates["thinking"] = params["thinking"].get<std::string>();
    }
    
    if (params.contains("thinkingLevel")) {
        updates["thinkingLevel"] = params["thinkingLevel"].is_null() 
            ? "" : params["thinkingLevel"].get<std::string>();
    }
    
    if (params.contains("verboseLevel")) {
        updates["verboseLevel"] = params["verboseLevel"].is_null() 
            ? "" : params["verboseLevel"].dump();
    }
    
    if (params.contains("reasoningLevel")) {
        updates["reasoningLevel"] = params["reasoningLevel"].is_null() 
            ? "" : params["reasoningLevel"].dump();
    }
    
    if (params.contains("fastMode")) {
        updates["fastMode"] = params["fastMode"].get<bool>() ? "true" : "false";
    }
    
    if (params.contains("toolPolicy")) {
        updates["toolPolicy"] = params["toolPolicy"].get<std::string>();
    }
    
    if (params.contains("externalContentHandling")) {
        updates["externalContentHandling"] = params["externalContentHandling"].get<std::string>();
    }
    
    if (params.contains("deliveryPolicy")) {
        updates["deliveryPolicy"] = params["deliveryPolicy"].get<std::string>();
    }
    
    if (params.contains("systemPrompt")) {
        updates["systemPrompt"] = params["systemPrompt"].is_null() 
            ? "" : params["systemPrompt"].get<std::string>();
    }
    
    if (params.contains("elevated")) {
        updates["elevated"] = params["elevated"].get<bool>() ? "true" : "false";
    }
    
    // 🎯 OpenClaw 对齐: 应用更新
    for (const auto& [key_str, value] : updates) {
        session_mgr->update_session_metadata(key, key_str, value);
    }
    
    // 🎯 OpenClaw 对齐: 返回更新后的 entry
    auto updated_session = session_mgr->get_session(key);
    if (!updated_session.ok) {
        return core::Result<json>::Err("Failed to retrieve updated session");
    }
    
    json entry;
    entry["sessionId"] = updated_session.value.session_id;
    entry["updatedAt"] = std::chrono::duration_cast<std::chrono::milliseconds>(
        updated_session.value.last_active_at.time_since_epoch()
    ).count();
    
    // 🎯 OpenClaw 对齐: 包含所有 metadata
    for (const auto& [k, v] : updated_session.value.metadata) {
        entry[k] = v;
    }
    
    // 🎯 OpenClaw 对齐: resolved 模型信息
    json resolved;
    auto model_it = updated_session.value.metadata.find("model");
    resolved["model"] = model_it != updated_session.value.metadata.end() 
        ? model_it->second : "unknown";
    
    auto provider_it = updated_session.value.metadata.find("modelProvider");
    if (provider_it != updated_session.value.metadata.end()) {
        resolved["modelProvider"] = provider_it->second;
    }
    
    json result;
    result["ok"] = true;
    result["key"] = key;
    result["entry"] = entry;
    result["resolved"] = resolved;
    
    return core::Result<json>::Ok(result);
}

// ============================================================================
// sessions.reset - 重置会话 (严格对齐 OpenClaw)
// 参考: openclaw-analysis/src/gateway/server-methods/sessions.ts:230-251
// ============================================================================
core::Result<json> method_session_reset(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    if (!params.contains("key")) {
        return core::Result<json>::Err("Missing 'key' parameter");
    }
    
    std::string key = params["key"].get<std::string>();
    if (key.empty()) {
        return core::Result<json>::Err("key required");
    }
    
    // 🎯 OpenClaw 对齐: reason 字段
    std::string reason = params.value("reason", "reset");  // "new" or "reset"
    
    auto& session_mgr = server->runtime_state()->session_manager;
    if (!session_mgr) {
        return core::Result<json>::Err("Session manager not initialized");
    }
    
    // 🎯 OpenClaw 对齐: 执行 reset (归档旧 transcript, 创建新的)
    auto reset_result = session_mgr->reset_session(key, reason);
    if (!reset_result.ok) {
        return core::Result<json>::Err(reset_result.error);
    }
    
    auto updated_session = session_mgr->get_session(key);
    if (!updated_session.ok) {
        return core::Result<json>::Err("Failed to retrieve reset session");
    }
    
    json entry;
    entry["sessionId"] = updated_session.value.session_id;
    entry["updatedAt"] = std::chrono::duration_cast<std::chrono::milliseconds>(
        updated_session.value.last_active_at.time_since_epoch()
    ).count();
    
    json result;
    result["ok"] = true;
    result["key"] = key;
    result["entry"] = entry;
    
    return core::Result<json>::Ok(result);
}

// ============================================================================
// sessions.delete - 删除会话 (严格对齐 OpenClaw)
// 参考: openclaw-analysis/src/gateway/server-methods/sessions.ts:252-322
// ============================================================================
core::Result<json> method_session_delete(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    if (!params.contains("key")) {
        return core::Result<json>::Err("Missing 'key' parameter");
    }
    
    std::string key = params["key"].get<std::string>();
    if (key.empty()) {
        return core::Result<json>::Err("key required");
    }
    
    // 🎯 OpenClaw 对齐: 不允许删除 main session
    if (key == "agent:main:main" || key == "global") {
        return core::Result<json>::Err("Cannot delete the main session");
    }
    
    // 🎯 OpenClaw 对齐: deleteTranscript 参数 (默认 true)
    bool delete_transcript = params.value("deleteTranscript", true);
    
    // 🎯 OpenClaw 对齐: emitLifecycleHooks 参数 (默认 true)
    bool emit_lifecycle_hooks = params.value("emitLifecycleHooks", true);
    
    auto& session_mgr = server->runtime_state()->session_manager;
    if (!session_mgr) {
        return core::Result<json>::Err("Session manager not initialized");
    }
    
    // 🎯 OpenClaw 对齐: 执行删除
    std::vector<std::string> archived;
    auto delete_result = session_mgr->delete_session(key, delete_transcript, &archived);
    if (!delete_result.ok) {
        return core::Result<json>::Err(delete_result.error);
    }
    
    // 🎯 OpenClaw 对齐: 触发生命周期事件
    if (emit_lifecycle_hooks) {
        // TODO: 触发 session-unbound 钩子
    }
    
    // 🎯 OpenClaw 对齐: 清理对应的 Agent 实例
    auto agent_pool = server->agent_pool();
    if (agent_pool) {
        std::string agent_id = extract_agent_id(key);
        agent_pool->remove_agent(agent_id);
    }
    
    json result;
    result["ok"] = true;
    result["key"] = key;
    result["deleted"] = true;
    result["archived"] = archived;
    
    return core::Result<json>::Ok(result);
}

// ============================================================================
// sessions.get - 获取会话消息 (严格对齐 OpenClaw)
// 参考: openclaw-analysis/src/gateway/server-methods/sessions.ts:323-344
// ============================================================================
core::Result<json> method_session_get(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    // 🎯 OpenClaw 对齐: 支持 "key" 或 "sessionKey" 参数
    std::string key;
    if (params.contains("key")) {
        key = params["key"].get<std::string>();
    } else if (params.contains("sessionKey")) {
        key = params["sessionKey"].get<std::string>();
    }
    
    if (key.empty()) {
        return core::Result<json>::Err("key or sessionKey required");
    }
    
    // 🎯 OpenClaw 对齐: limit 参数 (默认 200)
    int limit = params.value("limit", 200);
    limit = std::max(1, limit);
    
    auto& session_mgr = server->runtime_state()->session_manager;
    if (!session_mgr) {
        return core::Result<json>::Err("Session manager not initialized");
    }
    
    // 🎯 OpenClaw 对齐: 读取消息
    auto session_result = session_mgr->get_session(key);
    if (!session_result.ok) {
        // 🎯 OpenClaw 对齐: session 不存在时返回空数组 (不是错误)
        json result;
        result["messages"] = json::array();
        return core::Result<json>::Ok(result);
    }
    
    auto messages = session_mgr->get_messages(key, limit);
    
    json messages_array = json::array();
    for (const auto& msg : messages) {
        json msg_json;
        msg_json["role"] = msg.role;
        msg_json["content"] = msg.content;
        
        // 🎯 OpenClaw 对齐: 可选字段
        if (!msg.name.empty()) {
            msg_json["name"] = msg.name;
        }
        if (!msg.tool_call_id.empty()) {
            msg_json["tool_call_id"] = msg.tool_call_id;
        }
        if (!msg.metadata.empty()) {
            msg_json["metadata"] = msg.metadata;
        }
        
        messages_array.push_back(msg_json);
    }
    
    json result;
    result["messages"] = messages_array;
    
    return core::Result<json>::Ok(result);
}

// ============================================================================
// sessions.compact - 压缩会话 (严格对齐 OpenClaw)
// 参考: openclaw-analysis/src/gateway/server-methods/sessions.ts:345+
// ============================================================================
core::Result<json> method_session_compact(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    if (!params.contains("key")) {
        return core::Result<json>::Err("Missing 'key' parameter");
    }
    
    std::string key = params["key"].get<std::string>();
    if (key.empty()) {
        return core::Result<json>::Err("key required");
    }
    
    auto& session_mgr = server->runtime_state()->session_manager;
    if (!session_mgr) {
        return core::Result<json>::Err("Session manager not initialized");
    }
    
    // 🎯 OpenClaw 对齐: 执行压缩
    // TODO: 调用 memory compactor 进行压缩
    
    json result;
    result["ok"] = true;
    result["key"] = key;
    result["compacted"] = false;  // Placeholder
    result["reason"] = "Not implemented yet";
    
    return core::Result<json>::Ok(result);
}

// ============================================================================
// sessions.resolve - 解析 session key (严格对齐 OpenClaw)
// 用于从 label 或其他标识符解析到实际的 session key
// ============================================================================
core::Result<json> method_session_resolve(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    // 🎯 OpenClaw 对齐: 支持多种解析方式
    std::string session_label = params.value("sessionLabel", "");
    std::string session_key = params.value("sessionKey", "");
    bool require_existing = params.value("requireExisting", false);
    bool reset_session = params.value("resetSession", false);
    
    auto& session_mgr = server->runtime_state()->session_manager;
    if (!session_mgr) {
        return core::Result<json>::Err("Session manager not initialized");
    }
    
    std::string resolved_key;
    
    // 🎯 OpenClaw 对齐: 优先使用 sessionKey
    if (!session_key.empty()) {
        resolved_key = session_key;
    } else if (!session_label.empty()) {
        // 🎯 OpenClaw 对齐: 通过 label 查找
        auto all_sessions = session_mgr->list_all_sessions();
        for (const auto& session : all_sessions) {
            auto label_it = session.metadata.find("label");
            if (label_it != session.metadata.end() && label_it->second == session_label) {
                resolved_key = session.session_id;
                break;
            }
        }
        
        if (resolved_key.empty()) {
            if (require_existing) {
                return core::Result<json>::Err("Session with label not found: " + session_label);
            }
            // 创建新 session
            resolved_key = "agent:main:" + session_label;
            sessions::SessionOptions opts;
            opts.sender_name = session_label;
            session_mgr->create_session(resolved_key, opts);
            session_mgr->update_session_metadata(resolved_key, "label", session_label);
        }
    } else {
        return core::Result<json>::Err("sessionKey or sessionLabel required");
    }
    
    // 🎯 OpenClaw 对齐: requireExisting 检查
    if (require_existing) {
        auto session_result = session_mgr->get_session(resolved_key);
        if (!session_result.ok) {
            return core::Result<json>::Err("Session does not exist: " + resolved_key);
        }
    }
    
    // 🎯 OpenClaw 对齐: resetSession
    if (reset_session) {
        session_mgr->reset_session(resolved_key, "new");
    }
    
    json result;
    result["ok"] = true;
    result["key"] = resolved_key;
    
    return core::Result<json>::Ok(result);
}

}  // namespace methods
}  // namespace gateway
}  // namespace hunterclaw
