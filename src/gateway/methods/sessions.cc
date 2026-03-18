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
    
    // TODO: HunterClaw session management not yet implemented
    // Return empty session list for now
    (void)now;  // Suppress unused warning
    (void)active_threshold;
    (void)include_global;
    (void)include_unknown;
    (void)agent_id_filter;
    (void)spawned_by;
    (void)label_filter;
    (void)search;
    (void)limit;
    
    json sessions_array = json::array();
    
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
    
    // TODO: HunterClaw session previews not yet implemented
    (void)limit;
    (void)max_chars;
    
    // 🎯 OpenClaw 对齐: 为每个 key 生成预览
    json previews = json::array();
    
    for (const auto& key : keys) {
        json preview_entry;
        preview_entry["key"] = key;
        preview_entry["status"] = "not_implemented";
        preview_entry["items"] = json::array();
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
    
    // TODO: HunterClaw session patching not yet implemented
    // This would integrate with sessions::SessionManager
    
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
    // TODO: Apply updates when session manager is integrated
    
    // For now, just return success with placeholder data
    json entry;
    entry["sessionId"] = key;
    entry["updatedAt"] = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
    
    // Include the updates as metadata
    for (const auto& [k, v] : updates) {
        entry[k] = v;
    }
    
    json resolved;
    resolved["model"] = "unknown";  // Placeholder
    
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
    (void)reason;
    (void)server;
    (void)method_guard;
    (void)client_ctx;
    
    // TODO: HunterClaw session reset not yet implemented
    
    json entry;
    entry["sessionId"] = key;
    entry["updatedAt"] = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
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
    (void)delete_transcript;
    (void)emit_lifecycle_hooks;
    (void)method_guard;
    (void)client_ctx;
    
    // TODO: HunterClaw session deletion not yet implemented
    
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
    result["archived"] = json::array();  // Empty, as we haven't actually archived anything
    
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
    (void)limit;
    
    // TODO: HunterClaw session messages not yet implemented
    // Return empty messages array
    
    json messages_array = json::array();
    
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
    
    // TODO: HunterClaw session compression not yet implemented
    // This would integrate with memory compactor
    
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
    
    // TODO: HunterClaw session resolution not yet implemented
    // This would integrate with sessions::SessionManager
    
    std::string resolved_key;
    
    // 🎯 OpenClaw 对齐: 优先使用 sessionKey
    if (!session_key.empty()) {
        resolved_key = session_key;
    } else if (!session_label.empty()) {
        // For now, generate a key from label
        resolved_key = "agent:main:" + session_label;
    } else {
        return core::Result<json>::Err("sessionKey or sessionLabel required");
    }
    
    // TODO: requireExisting and resetSession checks would go here
    // when session manager is integrated
    
    json result;
    result["ok"] = true;
    result["key"] = resolved_key;
    
    return core::Result<json>::Ok(result);
}

}  // namespace methods
}  // namespace gateway
}  // namespace hunterclaw
