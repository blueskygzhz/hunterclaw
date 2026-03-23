#include "hunterclaw/gateway/gateway.h"
#include "hunterclaw/gateway/method_guard.h"
#include "hunterclaw/core/utils.h"
#include "hunterclaw/core/logging.h"
#include "hunterclaw/agents/agent_pool.h"
#include "hunterclaw/agents/agent_instance.h"
#include "hunterclaw/sessions/session_manager.h"
#include "hunterclaw/sessions/transcript_io.h"
#include "hunterclaw/tools/tool_hooks.h"   // get_global_hook_manager(), PluginHook* types
#include <nlohmann/json.hpp>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <chrono>

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace hunterclaw {
namespace gateway {
namespace methods {

// ============================================================================
// Session Methods - 严格对齐 OpenClaw
// 参考: openclaw-analysis/src/gateway/server-methods/sessions.ts
//       openclaw-analysis/src/gateway/session-utils.ts
//       openclaw-analysis/src/gateway/session-reset-service.ts
// ============================================================================

namespace {

// ── 辅助: 获取当前时间戳 (ms) ──
static int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

// ── 辅助: 判断 session kind ──
// 对齐 OpenClaw: classifySessionKey(key, entry)
static std::string classify_session_kind(
    const std::string& key,
    const sessions::SessionEntry* entry = nullptr
) {
    if (key == "global") return "global";
    if (key == "unknown") return "unknown";
    if (entry && entry->chat_type) {
        if (*entry->chat_type == "group" || *entry->chat_type == "channel") return "group";
    }
    if (key.find(":group:") != std::string::npos ||
        key.find(":channel:") != std::string::npos) return "group";
    return "direct";
}

// ── 辅助: 解析 sessions_dir ──
// 对齐 chat.cc 中的逻辑: parent_path() / "sessions"
static std::string resolve_sessions_dir(const std::string& workspace_dir) {
    return fs::path(workspace_dir).parent_path() / "sessions";
}

// ── 辅助: 归档 transcript 文件 ──
// 对齐 OpenClaw: archiveFileOnDisk(filePath, suffix)
static std::string archive_file_on_disk(const std::string& file_path, const std::string& suffix) {
    if (!fs::exists(file_path)) return "";
    std::string archived = file_path + "." + suffix + "." + std::to_string(now_ms());
    try {
        fs::rename(file_path, archived);
        return archived;
    } catch (...) {
        return "";
    }
}

// ── 辅助: 归档 session 的所有 transcript 文件 ──
// 对齐 OpenClaw: archiveSessionTranscriptsForSession
static std::vector<std::string> archive_session_transcripts(
    const std::string& session_id,
    const std::string& agent_id,
    const std::string& sessions_dir,
    const std::string& reason  // "reset" | "deleted"
) {
    std::vector<std::string> archived;
    if (session_id.empty() || sessions_dir.empty()) return archived;

    // 对齐 OpenClaw: resolveSessionTranscriptCandidates
    // 候选路径: {sessions_dir}/{agent_id}/{session_id}.jsonl
    //           {sessions_dir}/{session_id}.jsonl
    std::vector<std::string> candidates = {
        (fs::path(sessions_dir) / agent_id / (session_id + ".jsonl")).string(),
        (fs::path(sessions_dir) / (session_id + ".jsonl")).string(),
    };

    for (const auto& path : candidates) {
        if (fs::exists(path)) {
            std::string a = archive_file_on_disk(path, reason);
            if (!a.empty()) archived.push_back(a);
        }
    }
    return archived;
}

// ── 辅助: 检查是否为 main session key ──
// 对齐 OpenClaw: target.canonicalKey === mainKey
// OpenClaw mainKey 默认为 "agent:main:work" (resolveMainSessionKey)
static bool is_main_session_key(const std::string& key) {
    // 对齐 OpenClaw: resolveMainSessionKey(cfg) 默认返回 "agent:main:work"
    if (key == "agent:main:work") return true;
    if (key == "agent:main:main") return true;
    return false;
}

// ── 辅助: 从 session key 解析 agent ID ──
// 对齐 OpenClaw: parseAgentSessionKey(key)?.agentId
static std::string extract_agent_id_from_key(const std::string& key) {
    // key 格式: "agent:<agentId>:<rest>"
    if (key.substr(0, 6) != "agent:") return "main";
    size_t first = key.find(':');
    size_t second = key.find(':', first + 1);
    if (second == std::string::npos) return "main";
    return key.substr(first + 1, second - first - 1);
}

// ── 辅助: 从 server config 推导 sessions_dir ──
// 对齐 chat.cc: workspace_dir/../sessions
static std::string resolve_sessions_dir_from_server(
    std::shared_ptr<GatewayServer> server
) {
    auto cfg = server->config();
    if (cfg && !cfg->workspace_dir.empty()) {
        return (fs::path(cfg->workspace_dir).parent_path() / "sessions").string();
    }
    const char* home = std::getenv("HOME");
    return std::string(home ? home : "/tmp") + "/.hunterclaw/sessions";
}

// ── 辅助: 获取 SessionManager 实例 ──
// 对齐 OpenClaw: loadSessionStore(storePath)
static std::shared_ptr<sessions::SessionManager> get_session_manager(
    std::shared_ptr<GatewayServer> server
) {
    std::string sessions_dir = resolve_sessions_dir_from_server(server);
    sessions::SessionConfig session_cfg;
    return sessions::SessionManager::create(sessions_dir, session_cfg);
}

// ── 辅助: 构建 GatewaySessionRow JSON ──
// 对齐 OpenClaw: GatewaySessionRow (session-utils.types.ts)
static json build_session_row(
    const std::string& key,
    const sessions::SessionEntry& entry,
    bool include_derived_titles,
    bool include_last_message,
    const std::string& sessions_dir
) {
    json row;
    row["key"]       = key;
    row["sessionId"] = entry.session_id;
    row["agentId"]   = entry.agent_id;
    row["kind"]      = classify_session_kind(key, &entry);
    row["updatedAt"] = (int64_t)entry.updated_at;
    row["createdAt"] = (int64_t)entry.created_at;

    // 🎯 OpenClaw 对齐: label / displayName / subject
    if (entry.label)        row["label"]       = *entry.label;
    if (entry.display_name) row["displayName"]  = *entry.display_name;
    if (entry.chat_type)    row["chatType"]     = *entry.chat_type;

    // 🎯 OpenClaw 对齐: token 统计
    row["inputTokens"]      = entry.input_tokens;
    row["outputTokens"]     = entry.output_tokens;
    row["totalTokens"]      = entry.total_tokens;
    row["totalTokensFresh"] = (entry.total_tokens > 0);
    row["systemSent"]       = entry.system_sent;
    row["abortedLastRun"]   = entry.aborted_last_run;

    // 🎯 OpenClaw 对齐: model 信息 (从 entry 读取)
    row["model"]         = entry.model.value_or("");
    row["modelProvider"] = entry.model_provider.value_or("");

    // 🎯 OpenClaw 对齐: 额外字段
    if (entry.spawned_by)        row["spawnedBy"]      = *entry.spawned_by;
    if (entry.channel)           row["channel"]        = *entry.channel;
    if (entry.subject)           row["subject"]        = *entry.subject;
    if (entry.group_channel)     row["groupChannel"]   = *entry.group_channel;
    if (entry.space)             row["space"]          = *entry.space;
    if (entry.send_policy)       row["sendPolicy"]     = *entry.send_policy;
    if (entry.thinking_level)    row["thinkingLevel"]  = *entry.thinking_level;
    if (entry.fast_mode)         row["fastMode"]       = *entry.fast_mode;
    if (entry.verbose_level)     row["verboseLevel"]   = *entry.verbose_level;
    if (entry.reasoning_level)   row["reasoningLevel"] = *entry.reasoning_level;
    if (entry.elevated_level)    row["elevatedLevel"]  = *entry.elevated_level;
    if (entry.response_usage)    row["responseUsage"]  = *entry.response_usage;
    if (entry.context_tokens)    row["contextTokens"]  = *entry.context_tokens;
    if (entry.last_channel)      row["lastChannel"]    = *entry.last_channel;
    if (entry.last_to)           row["lastTo"]         = *entry.last_to;
    if (entry.last_account_id)   row["lastAccountId"]  = *entry.last_account_id;

    // 🎯 OpenClaw 对齐: derivedTitle / lastMessagePreview
    if (include_derived_titles || include_last_message) {
        std::string agent_id = entry.agent_id;
        std::string transcript_path = sessions::resolve_transcript_path(
            entry.session_id, agent_id, sessions_dir
        );
        if (include_derived_titles) {
            // 读取第一条 user 消息作为 derivedTitle
            auto msgs = sessions::read_transcript_messages(transcript_path, 1);
            if (!msgs.empty()) {
                std::string title = msgs[0].content;
                if (title.size() > 60) title = title.substr(0, 59) + "…";
                row["derivedTitle"] = title;
            }
        }
        if (include_last_message) {
            // 读取最后一条消息作为 lastMessagePreview
            auto msgs = sessions::read_transcript_messages(transcript_path, 200);
            if (!msgs.empty()) {
                std::string preview = msgs.back().content;
                if (preview.size() > 240) preview = preview.substr(0, 239) + "…";
                row["lastMessagePreview"] = preview;
            }
        }
    }

    return row;
}

// ── 辅助: 执行 session cleanup (abort run + clear queues) ──
// 对齐 OpenClaw: cleanupSessionBeforeMutation
static void cleanup_session_before_mutation(
    std::shared_ptr<GatewayServer> server,
    const std::string& key
) {
    auto agent_pool = server->agent_pool();
    if (!agent_pool) return;

    auto agent = agent_pool->get_agent(key);
    if (!agent) return;

    // 🎯 OpenClaw 对齐: abortEmbeddedPiRun → cancel react engine
    auto react_engine = agent->react_engine();
    if (react_engine) {
        react_engine->cancel();
    }
}

// ── 辅助: 广播 session lifecycle 事件 ──
// 对齐 OpenClaw: emitSessionUnboundLifecycleEvent
static void emit_session_lifecycle_event(
    std::shared_ptr<GatewayServer> server,
    const std::string& session_key,
    const std::string& reason  // "session-reset" | "session-delete"
) {
    json payload;
    payload["type"]       = "session.unbound";
    payload["sessionKey"] = session_key;
    payload["reason"]     = reason;
    payload["timestamp"]  = now_ms();

    core::EventFrame frame;
    frame.event   = "session";
    frame.payload = core::JsonValue(payload);
    server->broadcast(frame);
}

// ── 辅助: 从 requireSessionKey 提取 key ──
// 对齐 OpenClaw: requireSessionKey(key, respond)
static std::string require_session_key(const json& params, const std::string& field = "key") {
    // 支持 "key" 或 "sessionKey"
    if (params.contains(field) && params[field].is_string()) {
        std::string k = params[field].get<std::string>();
        // trim
        k.erase(0, k.find_first_not_of(" \t\r\n"));
        k.erase(k.find_last_not_of(" \t\r\n") + 1);
        return k;
    }
    return "";
}

} // anonymous namespace

// ============================================================================
// sessions.list - 列出会话 (严格对齐 OpenClaw)
// 参考: openclaw-analysis/src/gateway/server-methods/sessions.ts:96-110
//       openclaw-analysis/src/gateway/session-utils.ts:listSessionsFromStore
// ============================================================================
core::Result<json> method_session_list(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)method_guard;
    (void)client_ctx;

    // 🎯 OpenClaw 对齐: validateSessionsListParams
    bool include_global         = params.value("includeGlobal", false);
    bool include_unknown        = params.value("includeUnknown", false);
    bool include_derived_titles = params.value("includeDerivedTitles", false);
    bool include_last_message   = params.value("includeLastMessage", false);
    std::string spawned_by      = params.value("spawnedBy", "");
    std::string label_filter    = params.value("label", "");
    std::string agent_id_filter = params.value("agentId", "");
    std::string search          = params.value("search", "");
    int active_minutes          = params.value("activeMinutes", 0);
    int limit_val               = params.value("limit", 0);

    int64_t ts = now_ms();

    // 🎯 OpenClaw 对齐: loadCombinedSessionStoreForGateway
    // → 从 FileSessionManager 读取所有持久化 session
    auto session_mgr = get_session_manager(server);
    std::string sessions_dir;
    {
        auto cfg = server->config();
        if (cfg && !cfg->workspace_dir.empty()) sessions_dir = (std::filesystem::path(cfg->workspace_dir).parent_path() / "sessions").string();
        if (sessions_dir.empty()) {
            const char* home = std::getenv("HOME");
            sessions_dir = std::string(home ? home : "/tmp") + "/.hunterclaw/sessions";
        }
    }

    std::optional<std::string> agent_id_opt;
    if (!agent_id_filter.empty()) agent_id_opt = agent_id_filter;

    auto list_result = session_mgr->list_sessions(agent_id_opt);
    std::vector<sessions::SessionEntry> all_sessions;
    if (list_result.is_ok()) {
        all_sessions = list_result.get();
    }

    // 🎯 OpenClaw 对齐: .toSorted((a,b) => (b.updatedAt ?? 0) - (a.updatedAt ?? 0))
    std::sort(all_sessions.begin(), all_sessions.end(),
        [](const sessions::SessionEntry& a, const sessions::SessionEntry& b) {
            return a.updated_at > b.updated_at;
        });

    // 🎯 OpenClaw 对齐: 过滤逻辑 (listSessionsFromStore)
    json sessions_array = json::array();
    for (const auto& entry : all_sessions) {
        const std::string& key = entry.session_key;

        // 🎯 OpenClaw 对齐: isCronRunSessionKey 过滤 (cron run key 包含 ":cron:")
        if (key.find(":cron:") != std::string::npos) continue;

        // 🎯 OpenClaw 对齐: includeGlobal / includeUnknown
        if (!include_global && key == "global") continue;
        if (!include_unknown && key == "unknown") continue;

        // 🎯 OpenClaw 对齐: agentId 过滤
        // OpenClaw: if (!parsed) return false (非 agent: 前缀的 key 也过滤掉)
        if (!agent_id_filter.empty()) {
            if (key == "global" || key == "unknown") continue;
            // 必须是 agent: 前缀
            if (key.substr(0, 6) != "agent:") continue;
            std::string key_agent = extract_agent_id_from_key(key);
            if (key_agent != agent_id_filter) continue;
        }

        // 🎯 OpenClaw 对齐: spawnedBy 过滤
        if (!spawned_by.empty()) {
            if (key == "unknown" || key == "global") continue;
            if (!entry.spawned_by || *entry.spawned_by != spawned_by) continue;
        }

        // 🎯 OpenClaw 对齐: label 过滤
        if (!label_filter.empty()) {
            if (!entry.label || *entry.label != label_filter) continue;
        }

        sessions_array.push_back(
            build_session_row(key, entry, include_derived_titles, include_last_message, sessions_dir)
        );
    }

    // 🎯 OpenClaw 对齐: search 过滤 (在 sort 之后)
    if (!search.empty()) {
        std::string lower_search = search;
        std::transform(lower_search.begin(), lower_search.end(), lower_search.begin(), ::tolower);

        json filtered = json::array();
        for (const auto& row : sessions_array) {
            // 对齐 OpenClaw: [displayName, label, subject, sessionId, key]
            std::vector<std::string> fields = {
                row.value("displayName", ""),
                row.value("label", ""),
                row.value("sessionId", ""),
                row.value("key", ""),
            };
            bool match = false;
            for (const auto& f : fields) {
                std::string lower_f = f;
                std::transform(lower_f.begin(), lower_f.end(), lower_f.begin(), ::tolower);
                if (lower_f.find(lower_search) != std::string::npos) {
                    match = true;
                    break;
                }
            }
            if (match) filtered.push_back(row);
        }
        sessions_array = filtered;
    }

    // 🎯 OpenClaw 对齐: activeMinutes 过滤
    if (active_minutes > 0) {
        int64_t cutoff = ts - (int64_t)active_minutes * 60 * 1000;
        json filtered = json::array();
        for (const auto& row : sessions_array) {
            int64_t updated = row.value("updatedAt", (int64_t)0);
            if (updated >= cutoff) filtered.push_back(row);
        }
        sessions_array = filtered;
    }

    // 🎯 OpenClaw 对齐: limit 截断
    if (limit_val > 0 && (int)sessions_array.size() > limit_val) {
        sessions_array = json(sessions_array.begin(), sessions_array.begin() + limit_val);
    }

    // 🎯 OpenClaw 对齐: SessionsListResult
    json result;
    result["ts"]       = ts;
    result["path"]     = sessions_dir;
    result["count"]    = sessions_array.size();
    result["sessions"] = sessions_array;

    // ✅ 对齐 OpenClaw: getSessionDefaults 从配置读取 contextTokens
    // OpenClaw: { modelProvider, model, contextTokens }
    json defaults;
    defaults["modelProvider"] = nullptr;
    defaults["model"]         = nullptr;
    // 从配置读取 contextTokens
    {
        auto cfg = server->config();
        if (cfg && cfg->agents.context_tokens > 0) {
            defaults["contextTokens"] = cfg->agents.context_tokens;
        } else {
            defaults["contextTokens"] = nullptr;
        }
    }
    result["defaults"] = defaults;

    return core::Result<json>::Ok(result);
}

// ============================================================================
// sessions.preview - 预览会话消息 (严格对齐 OpenClaw)
// 参考: openclaw-analysis/src/gateway/server-methods/sessions.ts:111-172
//       readSessionPreviewItemsFromTranscript
// ============================================================================
core::Result<json> method_session_preview(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)method_guard;
    (void)client_ctx;

    if (!params.contains("keys") || !params["keys"].is_array()) {
        return core::Result<json>::Err("Missing or invalid 'keys' parameter");
    }

    // 🎯 OpenClaw 对齐: 最多 64 个 key
    std::vector<std::string> keys;
    for (const auto& k : params["keys"]) {
        if (k.is_string()) {
            std::string s = k.get<std::string>();
            s.erase(0, s.find_first_not_of(" \t\r\n"));
            s.erase(s.find_last_not_of(" \t\r\n") + 1);
            if (!s.empty()) keys.push_back(s);
        }
    }
    if (keys.size() > 64) keys.resize(64);

    if (keys.empty()) {
        json result;
        result["ts"]       = now_ms();
        result["previews"] = json::array();
        return core::Result<json>::Ok(result);
    }

    // 🎯 OpenClaw 对齐: limit / maxChars 参数
    int limit     = std::max(1, params.value("limit", 12));
    int max_chars = std::max(20, params.value("maxChars", 240));

    auto session_mgr = get_session_manager(server);
    std::string sessions_dir;
    {
        auto cfg = server->config();
        if (cfg && !cfg->workspace_dir.empty()) sessions_dir = (std::filesystem::path(cfg->workspace_dir).parent_path() / "sessions").string();
        if (sessions_dir.empty()) {
            const char* home = std::getenv("HOME");
            sessions_dir = std::string(home ? home : "/tmp") + "/.hunterclaw/sessions";
        }
    }

    json previews = json::array();

    for (const auto& key : keys) {
        json preview_entry;
        preview_entry["key"] = key;

        // 🎯 OpenClaw 对齐: 从 SessionManager 读取 entry
        auto entry_result = session_mgr->get_session(key);
        if (entry_result.is_err()) {
            preview_entry["status"] = "missing";
            preview_entry["items"]  = json::array();
            previews.push_back(preview_entry);
            continue;
        }

        const auto& entry = entry_result.get();
        if (entry.session_id.empty()) {
            preview_entry["status"] = "missing";
            preview_entry["items"]  = json::array();
            previews.push_back(preview_entry);
            continue;
        }

        // 🎯 OpenClaw 对齐: readSessionPreviewItemsFromTranscript
        std::string transcript_path = sessions::resolve_transcript_path(
            entry.session_id, entry.agent_id, sessions_dir
        );

        auto messages = sessions::read_transcript_messages(transcript_path, limit);

        if (messages.empty()) {
            preview_entry["status"] = "empty";
            preview_entry["items"]  = json::array();
        } else {
            json items = json::array();
            for (const auto& msg : messages) {
                std::string text = msg.content;
                if ((int)text.size() > max_chars) {
                    text = text.substr(0, max_chars - 1) + "…";
                }
                json item;
                item["role"] = (msg.role == llm::LLMMessageRole::User) ? "user" : "assistant";
                item["text"] = text;
                items.push_back(item);
            }
            preview_entry["status"] = "ok";
            preview_entry["items"]  = items;
        }
        previews.push_back(preview_entry);
    }

    json result;
    result["ts"]       = now_ms();
    result["previews"] = previews;

    return core::Result<json>::Ok(result);
}

// ============================================================================
// sessions.resolve - 解析 session key (严格对齐 OpenClaw)
// 参考: openclaw-analysis/src/gateway/server-methods/sessions.ts:173-186
//       resolveSessionKeyFromResolveParams
// ============================================================================
core::Result<json> method_session_resolve(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)server;
    (void)method_guard;
    (void)client_ctx;

    // 🎯 OpenClaw 对齐: validateSessionsResolveParams
    std::string session_label = params.value("sessionLabel", "");
    std::string session_key   = params.value("sessionKey", "");
    bool require_existing     = params.value("requireExisting", false);
    bool reset_session = params.value("resetSession", false);

    // ✅ 对齐 OpenClaw: resetSession 逻辑
    // OpenClaw: if (resetSession) { await resetSessionByKey(resolvedKey); }
    // 如果请求重置 session，删除 transcript 并重创建
    if (reset_session && !session_key.empty()) {
        auto agent_pool = server->agent_pool();
        if (agent_pool) {
            auto agent = agent_pool->get_agent(session_key);
            if (agent) {
                // 对齐 OpenClaw: 重置 session 就是删除 transcript 并重新初始化
                agent->reset_transcript();
            }
        }
    }

    if (session_key.empty() && session_label.empty()) {
        return core::Result<json>::Err("sessionKey or sessionLabel required");
    }

    // 🎯 OpenClaw 对齐: resolveSessionKeyFromResolveParams
    std::string input = !session_key.empty() ? session_key : ("label:" + session_label);
    auto key_result = sessions::SessionMapper::resolve_session_key(input, require_existing);
    if (key_result.is_err()) {
        return core::Result<json>::Err(key_result.error);
    }

    json result;
    result["ok"]  = true;
    result["key"] = key_result.get();

    return core::Result<json>::Ok(result);
}

// ============================================================================
// sessions.patch - 更新会话元数据 (严格对齐 OpenClaw)
// 参考: openclaw-analysis/src/gateway/server-methods/sessions.ts:187-229
//       applySessionsPatchToStore
// ============================================================================
core::Result<json> method_session_patch(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)method_guard;
    (void)client_ctx;

    // 🎯 OpenClaw 对齐: validateSessionsPatchParams + requireSessionKey
    std::string key = require_session_key(params);
    if (key.empty()) {
        return core::Result<json>::Err("key required");
    }

    // 🎯 OpenClaw 对齐: resolveGatewaySessionTargetFromKey → updateSessionStore
    auto session_mgr = get_session_manager(server);
    std::string sessions_dir;
    {
        auto cfg = server->config();
        if (cfg && !cfg->workspace_dir.empty()) sessions_dir = (std::filesystem::path(cfg->workspace_dir).parent_path() / "sessions").string();
        if (sessions_dir.empty()) {
            const char* home = std::getenv("HOME");
            sessions_dir = std::string(home ? home : "/tmp") + "/.hunterclaw/sessions";
        }
    }

    // 读取现有 entry (或创建新的)
    sessions::SessionEntry entry;
    auto existing = session_mgr->get_session(key);
    if (existing.is_ok()) {
        entry = existing.get();
    } else {
        // 🎯 OpenClaw 对齐: 如果 entry 不存在，创建新的
        entry.session_id  = core::utils::generate_uuid();
        entry.session_key = key;
        entry.agent_id    = extract_agent_id_from_key(key);
        entry.created_at  = (uint64_t)now_ms();
        entry.updated_at  = entry.created_at;
    }

    // 🎯 OpenClaw 对齐: applySessionsPatchToStore
    // label: 支持 null（清除）或 string（设置，需唯一性检查）
    if (params.contains("label")) {
        if (params["label"].is_null()) {
            entry.label = std::nullopt;
        } else if (params["label"].is_string()) {
            std::string new_label = params["label"].get<std::string>();
            new_label.erase(0, new_label.find_first_not_of(" \t"));
            new_label.erase(new_label.find_last_not_of(" \t") + 1);
            if (!new_label.empty()) {
                // 🎯 OpenClaw 对齐: label 唯一性检查
                auto all_sessions_result = session_mgr->list_sessions(std::nullopt);
                if (all_sessions_result.is_ok()) {
                    for (const auto& other : all_sessions_result.get()) {
                        if (other.session_key != key && other.label && *other.label == new_label) {
                            return core::Result<json>::Err("label already in use: " + new_label);
                        }
                    }
                }
                entry.label = new_label;
            }
        }
    }

    if (params.contains("displayName")) {
        if (params["displayName"].is_null()) {
            entry.display_name = std::nullopt;
        } else if (params["displayName"].is_string()) {
            entry.display_name = params["displayName"].get<std::string>();
        }
    }

    // 🎯 OpenClaw 对齐: thinkingLevel
    if (params.contains("thinkingLevel")) {
        if (params["thinkingLevel"].is_null()) {
            entry.thinking_level = std::nullopt;
        } else if (params["thinkingLevel"].is_string()) {
            std::string v = params["thinkingLevel"].get<std::string>();
            static const std::set<std::string> valid_levels = {"low", "medium", "high", "xhigh"};
            if (valid_levels.count(v)) entry.thinking_level = v;
            else return core::Result<json>::Err("invalid thinkingLevel");
        }
    }

    // 🎯 OpenClaw 对齐: fastMode
    if (params.contains("fastMode")) {
        if (params["fastMode"].is_null()) {
            entry.fast_mode = std::nullopt;
        } else if (params["fastMode"].is_boolean()) {
            entry.fast_mode = params["fastMode"].get<bool>();
        }
    }

    // 🎯 OpenClaw 对齐: verboseLevel
    if (params.contains("verboseLevel")) {
        if (params["verboseLevel"].is_null()) {
            entry.verbose_level = std::nullopt;
        } else if (params["verboseLevel"].is_string()) {
            entry.verbose_level = params["verboseLevel"].get<std::string>();
        }
    }

    // 🎯 OpenClaw 对齐: reasoningLevel
    if (params.contains("reasoningLevel")) {
        if (params["reasoningLevel"].is_null()) {
            entry.reasoning_level = std::nullopt;
        } else if (params["reasoningLevel"].is_string()) {
            std::string v = params["reasoningLevel"].get<std::string>();
            static const std::set<std::string> valid_rl = {"on", "off", "stream"};
            if (valid_rl.count(v)) entry.reasoning_level = v;
            else return core::Result<json>::Err("invalid reasoningLevel (use \"on\"|\"off\"|\"stream\")");
        }
    }

    // 🎯 OpenClaw 对齐: elevatedLevel
    if (params.contains("elevatedLevel")) {
        if (params["elevatedLevel"].is_null()) {
            entry.elevated_level = std::nullopt;
        } else if (params["elevatedLevel"].is_string()) {
            std::string v = params["elevatedLevel"].get<std::string>();
            static const std::set<std::string> valid_el = {"on", "off", "ask", "full"};
            if (valid_el.count(v)) entry.elevated_level = v;
            else return core::Result<json>::Err("invalid elevatedLevel (use \"on\"|\"off\"|\"ask\"|\"full\")");
        }
    }

    // 🎯 OpenClaw 对齐: sendPolicy
    if (params.contains("sendPolicy")) {
        if (params["sendPolicy"].is_null()) {
            entry.send_policy = std::nullopt;
        } else if (params["sendPolicy"].is_string()) {
            std::string v = params["sendPolicy"].get<std::string>();
            if (v == "allow" || v == "deny") entry.send_policy = v;
            else return core::Result<json>::Err("invalid sendPolicy (use \"allow\"|\"deny\")");
        }
    }

    // 🎯 OpenClaw 对齐: responseUsage
    if (params.contains("responseUsage")) {
        if (params["responseUsage"].is_null()) {
            entry.response_usage = std::nullopt;
        } else if (params["responseUsage"].is_string()) {
            std::string v = params["responseUsage"].get<std::string>();
            static const std::set<std::string> valid_ru = {"off", "tokens", "full"};
            if (valid_ru.count(v)) {
                if (v == "off") entry.response_usage = std::nullopt;
                else entry.response_usage = v;
            } else {
                return core::Result<json>::Err("invalid responseUsage (use \"off\"|\"tokens\"|\"full\")");
            }
        }
    }

    // 🎯 OpenClaw 对齐: model (简化版，不做 catalog 验证)
    if (params.contains("model")) {
        if (params["model"].is_null()) {
            entry.model = std::nullopt;
            entry.model_provider = std::nullopt;
        } else if (params["model"].is_string()) {
            std::string v = params["model"].get<std::string>();
            v.erase(0, v.find_first_not_of(" \t"));
            v.erase(v.find_last_not_of(" \t") + 1);
            if (v.empty()) return core::Result<json>::Err("invalid model: empty");
            entry.model = v;
        }
    }

    entry.updated_at = (uint64_t)now_ms();

    // 🎯 OpenClaw 对齐: 持久化到 SessionManager
    session_mgr->update_session(entry);

    // 🎯 OpenClaw 对齐: 构建 SessionsPatchResult
    json entry_json;
    entry_json["sessionId"] = entry.session_id;
    entry_json["updatedAt"] = (int64_t)entry.updated_at;
    if (entry.label)           entry_json["label"]          = *entry.label;
    if (entry.display_name)    entry_json["displayName"]    = *entry.display_name;
    if (entry.model)           entry_json["model"]           = *entry.model;
    if (entry.model_provider)  entry_json["modelProvider"]  = *entry.model_provider;
    if (entry.thinking_level)  entry_json["thinkingLevel"]  = *entry.thinking_level;
    if (entry.fast_mode)       entry_json["fastMode"]        = *entry.fast_mode;
    if (entry.verbose_level)   entry_json["verboseLevel"]   = *entry.verbose_level;
    if (entry.reasoning_level) entry_json["reasoningLevel"] = *entry.reasoning_level;
    if (entry.elevated_level)  entry_json["elevatedLevel"]  = *entry.elevated_level;
    if (entry.send_policy)     entry_json["sendPolicy"]     = *entry.send_policy;
    if (entry.response_usage)  entry_json["responseUsage"]  = *entry.response_usage;

    // 🎯 OpenClaw 对齐: resolveSessionModelRef
    json resolved;
    resolved["modelProvider"] = entry.model_provider ? json(*entry.model_provider) : json(nullptr);
    resolved["model"]         = entry.model ? json(*entry.model) : json("");

    json result;
    result["ok"]       = true;
    result["path"]     = sessions_dir;
    result["key"]      = key;
    result["entry"]    = entry_json;
    result["resolved"] = resolved;

    return core::Result<json>::Ok(result);
}

// ============================================================================
// sessions.reset - 重置会话 (严格对齐 OpenClaw)
// 参考: openclaw-analysis/src/gateway/server-methods/sessions.ts:230-251
//       performGatewaySessionReset
//
// OpenClaw 核心逻辑:
//   1. cleanupSessionBeforeMutation: abort run, 清理队列, 关闭 browser tabs
//   2. updateSessionStore: 生成新 sessionId, 保留 label/model 等元数据
//   3. archiveSessionTranscripts: 归档旧 transcript 文件
//   4. emitSessionUnboundLifecycleEvent: 广播 session-reset 事件
// ============================================================================
core::Result<json> method_session_reset(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)method_guard;
    (void)client_ctx;

    // 🎯 OpenClaw 对齐: validateSessionsResetParams + requireSessionKey
    std::string key = require_session_key(params);
    if (key.empty()) {
        return core::Result<json>::Err("key required");
    }

    // 🎯 OpenClaw 对齐: reason "new" | "reset"
    std::string reason = params.value("reason", "reset");
    if (reason != "new") reason = "reset";

    auto session_mgr = get_session_manager(server);
    std::string sessions_dir;
    {
        auto cfg = server->config();
        if (cfg && !cfg->workspace_dir.empty()) sessions_dir = (std::filesystem::path(cfg->workspace_dir).parent_path() / "sessions").string();
        if (sessions_dir.empty()) {
            const char* home = std::getenv("HOME");
            sessions_dir = std::string(home ? home : "/tmp") + "/.hunterclaw/sessions";
        }
    }

    // 读取现有 entry
    sessions::SessionEntry old_entry;
    bool had_existing_entry = false;
    auto existing = session_mgr->get_session(key);
    if (existing.is_ok()) {
        old_entry = existing.get();
        had_existing_entry = true;
    }

    // =========================================================================
    // Hook: before_reset
    // Aligned with OpenClaw session-reset-service.ts — void hook, fire-and-forget
    // Fires before the session is reset; plugins observe but cannot cancel.
    // Called BEFORE cleanupSessionBeforeMutation so plugins see the old state.
    // =========================================================================
    {
        auto* hook_mgr = tools::get_global_hook_manager();
        if (hook_mgr && hook_mgr->has_hooks(plugins::PluginHookName::BEFORE_RESET)) {
            tools::PluginHookBeforeResetEvent br_event;
            br_event.session_key    = key;
            br_event.reason         = reason;
            br_event.old_session_id = had_existing_entry
                ? std::make_optional(old_entry.session_id) : std::nullopt;
            tools::PluginHookAgentContext br_ctx;
            br_ctx.session_id = had_existing_entry ? old_entry.session_id : key;
            try {
                hook_mgr->run_before_reset(br_event, br_ctx);
            } catch (const std::exception& e) {
                // before_reset hook failure is non-fatal — log and continue
                hunterclaw::core::get_logger()->warn("[sessions.reset] before_reset hook failed: {}", e.what());
            }
        }
    }

    // 🎯 OpenClaw 对齐: Step 1 - cleanupSessionBeforeMutation
    // abort 当前 run, 清理队列
    cleanup_session_before_mutation(server, key);

    // 🎯 OpenClaw 对齐: Step 2 - updateSessionStore
    // 生成新 sessionId，保留 label/thinkingLevel 等元数据
    // 对应 performGatewaySessionReset 中的 nextEntry 构造
    sessions::SessionEntry next_entry;
    next_entry.session_id       = core::utils::generate_uuid();
    next_entry.session_key      = key;
    next_entry.agent_id         = had_existing_entry ? old_entry.agent_id : extract_agent_id_from_key(key);
    next_entry.created_at       = had_existing_entry ? old_entry.created_at : (uint64_t)now_ms();
    next_entry.updated_at       = (uint64_t)now_ms();
    next_entry.system_sent      = false;
    next_entry.aborted_last_run = false;
    next_entry.input_tokens     = 0;
    next_entry.output_tokens    = 0;
    next_entry.total_tokens     = 0;
    next_entry.total_tokens_fresh = true;
    // 🎯 OpenClaw 对齐: 保留 label
    if (had_existing_entry && old_entry.label) {
        next_entry.label = old_entry.label;
    }
    // 🎯 OpenClaw 对齐: 保留 chat_type
    if (had_existing_entry && old_entry.chat_type) {
        next_entry.chat_type = old_entry.chat_type;
    }
    // 🎯 OpenClaw 对齐: 保留 thinkingLevel / fastMode / verboseLevel / reasoningLevel
    if (had_existing_entry && old_entry.thinking_level) {
        next_entry.thinking_level = old_entry.thinking_level;
    }
    if (had_existing_entry && old_entry.fast_mode) {
        next_entry.fast_mode = old_entry.fast_mode;
    }
    if (had_existing_entry && old_entry.verbose_level) {
        next_entry.verbose_level = old_entry.verbose_level;
    }
    if (had_existing_entry && old_entry.reasoning_level) {
        next_entry.reasoning_level = old_entry.reasoning_level;
    }
    if (had_existing_entry && old_entry.response_usage) {
        next_entry.response_usage = old_entry.response_usage;
    }
    if (had_existing_entry && old_entry.send_policy) {
        next_entry.send_policy = old_entry.send_policy;
    }
    // 🎯 OpenClaw 对齐: 保留 lastChannel / lastTo / lastAccountId
    if (had_existing_entry && old_entry.last_channel) {
        next_entry.last_channel = old_entry.last_channel;
    }
    if (had_existing_entry && old_entry.last_to) {
        next_entry.last_to = old_entry.last_to;
    }
    if (had_existing_entry && old_entry.last_account_id) {
        next_entry.last_account_id = old_entry.last_account_id;
    }
    // 🎯 OpenClaw 对齐: stripRuntimeModelState - reset 时清除 runtime model state
    // (model/modelProvider/contextTokens 不保留，让下次运行重新解析)

    // 持久化新 entry
    session_mgr->update_session(next_entry);

    // 🎯 OpenClaw 对齐: Step 3 - archiveSessionTranscripts
    if (had_existing_entry && !old_entry.session_id.empty()) {
        archive_session_transcripts(
            old_entry.session_id,
            old_entry.agent_id,
            sessions_dir,
            "reset"
        );
    }

    // 🎯 OpenClaw 对齐: Step 4 - emitSessionUnboundLifecycleEvent
    if (had_existing_entry) {
        emit_session_lifecycle_event(server, key, "session-reset");
    }

    // 🎯 OpenClaw 对齐: 同时移除 AgentPool 中的旧实例
    auto agent_pool = server->agent_pool();
    if (agent_pool) {
        agent_pool->remove_agent(key);
    }

    // 🎯 OpenClaw 对齐: 构建返回值 (对齐 performGatewaySessionReset 返回)
    json entry_json;
    entry_json["sessionId"]        = next_entry.session_id;
    entry_json["updatedAt"]        = (int64_t)next_entry.updated_at;
    entry_json["systemSent"]       = false;
    entry_json["abortedLastRun"]   = false;
    entry_json["inputTokens"]      = 0;
    entry_json["outputTokens"]     = 0;
    entry_json["totalTokens"]      = 0;
    entry_json["totalTokensFresh"] = true;
    if (next_entry.label)           entry_json["label"]          = *next_entry.label;
    if (next_entry.thinking_level)  entry_json["thinkingLevel"]  = *next_entry.thinking_level;
    if (next_entry.fast_mode)       entry_json["fastMode"]       = *next_entry.fast_mode;
    if (next_entry.verbose_level)   entry_json["verboseLevel"]   = *next_entry.verbose_level;
    if (next_entry.reasoning_level) entry_json["reasoningLevel"] = *next_entry.reasoning_level;
    if (next_entry.send_policy)     entry_json["sendPolicy"]     = *next_entry.send_policy;

    json result;
    result["ok"]    = true;
    result["key"]   = key;
    result["entry"] = entry_json;

    return core::Result<json>::Ok(result);
}

// ============================================================================
// sessions.delete - 删除会话 (严格对齐 OpenClaw)
// 参考: openclaw-analysis/src/gateway/server-methods/sessions.ts:252-322
//
// OpenClaw 核心逻辑:
//   1. 不允许删除 main session
//   2. cleanupSessionBeforeMutation: abort run, 清理队列
//   3. updateSessionStore: 从 store 删除 entry
//   4. archiveSessionTranscripts: 归档 transcript (if deleteTranscript=true)
//   5. emitSessionUnboundLifecycleEvent: 广播 session-delete 事件
// ============================================================================
core::Result<json> method_session_delete(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)method_guard;
    (void)client_ctx;

    // 🎯 OpenClaw 对齐: validateSessionsDeleteParams + requireSessionKey
    std::string key = require_session_key(params);
    if (key.empty()) {
        return core::Result<json>::Err("key required");
    }

    // 🎯 OpenClaw 对齐: 不允许删除 main session
    // 对应: if (target.canonicalKey === mainKey) respond(false, ..., "Cannot delete the main session")
    if (is_main_session_key(key)) {
        return core::Result<json>::Err("Cannot delete the main session (" + key + ").");
    }

    // 🎯 OpenClaw 对齐: deleteTranscript 参数 (默认 true)
    bool delete_transcript    = params.value("deleteTranscript", true);
    bool emit_lifecycle_hooks = params.value("emitLifecycleHooks", true);

    auto session_mgr = get_session_manager(server);
    std::string sessions_dir;
    {
        auto cfg = server->config();
        if (cfg && !cfg->workspace_dir.empty()) sessions_dir = (std::filesystem::path(cfg->workspace_dir).parent_path() / "sessions").string();
        if (sessions_dir.empty()) {
            const char* home = std::getenv("HOME");
            sessions_dir = std::string(home ? home : "/tmp") + "/.hunterclaw/sessions";
        }
    }

    // 读取现有 entry (用于 archive transcript)
    sessions::SessionEntry entry;
    bool had_entry = false;
    auto existing = session_mgr->get_session(key);
    if (existing.is_ok()) {
        entry = existing.get();
        had_entry = true;
    }

    // 🎯 OpenClaw 对齐: Step 1 - cleanupSessionBeforeMutation
    // OpenClaw: if (mutationCleanupError) respond(false, ...) and return
    {
        auto agent_pool = server->agent_pool();
        if (agent_pool) {
            auto agent = agent_pool->get_agent(key);
            if (agent) {
                auto react_engine = agent->react_engine();
                if (react_engine) {
                    react_engine->cancel();
                }
            }
        }
    }
    // 🎯 OpenClaw 对齐: clearSessionQueues + stopSubagentsForRequester
    // (简化版：直接 cancel，不等待 run 结束)

    // 🎯 OpenClaw 对齐: Step 2 - updateSessionStore (delete entry)
    bool deleted = false;
    if (had_entry) {
        auto del_result = session_mgr->delete_session(key);
        deleted = del_result.is_ok() && del_result.get();
    }

    // 🎯 OpenClaw 对齐: Step 3 - archiveSessionTranscripts (if deleteTranscript=true)
    json archived_array = json::array();
    if (deleted && delete_transcript && !entry.session_id.empty()) {
        auto archived = archive_session_transcripts(
            entry.session_id,
            entry.agent_id,
            sessions_dir,
            "deleted"
        );
        for (const auto& a : archived) {
            archived_array.push_back(a);
        }
    }

    // 🎯 OpenClaw 对齐: Step 4 - emitSessionUnboundLifecycleEvent
    if (deleted && emit_lifecycle_hooks) {
        emit_session_lifecycle_event(server, key, "session-delete");
    }

    // 🎯 OpenClaw 对齐: 同时移除 AgentPool 中的实例
    auto agent_pool = server->agent_pool();
    if (agent_pool) {
        agent_pool->remove_agent(key);
    }

    json result;
    result["ok"]       = true;
    result["key"]      = key;
    result["deleted"]  = deleted;
    result["archived"] = archived_array;

    return core::Result<json>::Ok(result);
}

// ============================================================================
// sessions.get - 获取会话消息 (严格对齐 OpenClaw)
// 参考: openclaw-analysis/src/gateway/server-methods/sessions.ts:323-344
//       readSessionMessages
// ============================================================================
core::Result<json> method_session_get(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)method_guard;
    (void)client_ctx;

    // 🎯 OpenClaw 对齐: requireSessionKey(p.key ?? p.sessionKey, respond)
    std::string key = require_session_key(params, "key");
    if (key.empty()) {
        key = require_session_key(params, "sessionKey");
    }
    if (key.empty()) {
        return core::Result<json>::Err("key or sessionKey required");
    }

    // 🎯 OpenClaw 对齐: limit 参数 (默认 200, 最小 1)
    int limit = 200;
    if (params.contains("limit") && params["limit"].is_number()) {
        limit = std::max(1, (int)std::floor(params["limit"].get<double>()));
    }

    auto session_mgr = get_session_manager(server);
    std::string sessions_dir;
    {
        auto cfg = server->config();
        if (cfg && !cfg->workspace_dir.empty()) sessions_dir = (std::filesystem::path(cfg->workspace_dir).parent_path() / "sessions").string();
        if (sessions_dir.empty()) {
            const char* home = std::getenv("HOME");
            sessions_dir = std::string(home ? home : "/tmp") + "/.hunterclaw/sessions";
        }
    }

    // 🎯 OpenClaw 对齐: 从 SessionStore 读取 entry
    auto entry_result = session_mgr->get_session(key);
    if (entry_result.is_err() || entry_result.get().session_id.empty()) {
        // 🎯 OpenClaw 对齐: 没有 entry 时返回空 messages
        json result;
        result["messages"] = json::array();
        return core::Result<json>::Ok(result);
    }

    const auto& entry = entry_result.get();

    // 🎯 OpenClaw 对齐: readSessionMessages(entry.sessionId, storePath, entry.sessionFile)
    std::string transcript_path = sessions::resolve_transcript_path(
        entry.session_id, entry.agent_id, sessions_dir
    );

    auto all_messages = sessions::read_transcript_messages(transcript_path, 0);  // 读取全部

    // 🎯 OpenClaw 对齐: limit < allMessages.length ? allMessages.slice(-limit) : allMessages
    if (limit < (int)all_messages.size()) {
        all_messages = std::vector<llm::LLMMessage>(
            all_messages.end() - limit, all_messages.end()
        );
    }

    // 🎯 OpenClaw 对齐: 转换为 JSON (对齐 readSessionMessages 返回格式)
    json messages_array = json::array();
    for (const auto& msg : all_messages) {
        json msg_json;
        switch (msg.role) {
            case llm::LLMMessageRole::User:      msg_json["role"] = "user";      break;
            case llm::LLMMessageRole::Assistant: msg_json["role"] = "assistant"; break;
            case llm::LLMMessageRole::System:    msg_json["role"] = "system";    break;
            case llm::LLMMessageRole::Tool:      msg_json["role"] = "tool";      break;
        }
        // 🎯 OpenClaw 对齐: content 为 array of content blocks
        json content_block;
        content_block["type"] = "text";
        content_block["text"] = msg.content;
        msg_json["content"] = json::array({ content_block });
        messages_array.push_back(msg_json);
    }

    json result;
    result["messages"] = messages_array;

    return core::Result<json>::Ok(result);
}

// ============================================================================
// sessions.compact - 压缩会话 transcript (严格对齐 OpenClaw)
// 参考: openclaw-analysis/src/gateway/server-methods/sessions.ts:345-430
//
// OpenClaw 核心逻辑:
//   1. updateSessionStore: 读取 entry (Lock + read in critical section)
//   2. 没有 sessionId → 返回 compacted=false, reason="no sessionId"
//   3. resolveSessionTranscriptCandidates: 找到 transcript 文件
//   4. 没有文件 → 返回 compacted=false, reason="no transcript"
//   5. 读取所有行，过滤空行
//   6. lines.length <= maxLines → 返回 compacted=false, kept=lines.length
//   7. archiveFileOnDisk(filePath, "bak")
//   8. 写入最后 maxLines 行
//   9. updateSessionStore: 清除 inputTokens/outputTokens/totalTokens/totalTokensFresh
// ============================================================================
core::Result<json> method_session_compact(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)method_guard;
    (void)client_ctx;

    // 🎯 OpenClaw 对齐: validateSessionsCompactParams + requireSessionKey
    std::string key = require_session_key(params);
    if (key.empty()) {
        return core::Result<json>::Err("key required");
    }

    // 🎯 OpenClaw 对齐: maxLines 参数 (default 400, min 1)
    int max_lines = 400;
    if (params.contains("maxLines") && params["maxLines"].is_number()) {
        max_lines = std::max(1, (int)std::floor(params["maxLines"].get<double>()));
    }

    auto session_mgr = get_session_manager(server);
    std::string sessions_dir;
    {
        auto cfg = server->config();
        if (cfg && !cfg->workspace_dir.empty()) sessions_dir = (std::filesystem::path(cfg->workspace_dir).parent_path() / "sessions").string();
        if (sessions_dir.empty()) {
            const char* home = std::getenv("HOME");
            sessions_dir = std::string(home ? home : "/tmp") + "/.hunterclaw/sessions";
        }
    }

    // 🎯 OpenClaw 对齐: updateSessionStore → migrateAndPruneGatewaySessionStoreKey
    // 读取 entry
    auto entry_result = session_mgr->get_session(key);

    // 🎯 OpenClaw 对齐: 没有 sessionId 时返回 compacted=false
    if (entry_result.is_err() || entry_result.get().session_id.empty()) {
        json result;
        result["ok"]        = true;
        result["key"]       = key;
        result["compacted"] = false;
        result["reason"]    = "no sessionId";
        return core::Result<json>::Ok(result);
    }

    const auto& entry = entry_result.get();

    // 🎯 OpenClaw 对齐: resolveSessionTranscriptCandidates → 找到 transcript 文件
    std::vector<std::string> candidates = {
        (fs::path(sessions_dir) / entry.agent_id / (entry.session_id + ".jsonl")).string(),
        (fs::path(sessions_dir) / (entry.session_id + ".jsonl")).string(),
    };

    std::string file_path;
    for (const auto& c : candidates) {
        if (fs::exists(c)) {
            file_path = c;
            break;
        }
    }

    if (file_path.empty()) {
        json result;
        result["ok"]        = true;
        result["key"]       = key;
        result["compacted"] = false;
        result["reason"]    = "no transcript";
        return core::Result<json>::Ok(result);
    }

    // 🎯 OpenClaw 对齐: 读取所有行，过滤空行
    std::vector<std::string> lines;
    {
        std::ifstream ifs(file_path);
        std::string line;
        while (std::getline(ifs, line)) {
            // 对齐 OpenClaw: .filter(l => l.trim().length > 0)
            std::string trimmed = line;
            trimmed.erase(0, trimmed.find_first_not_of(" \t\r\n"));
            trimmed.erase(trimmed.find_last_not_of(" \t\r\n") + 1);
            if (!trimmed.empty()) {
                lines.push_back(line);
            }
        }
    }

    // 🎯 OpenClaw 对齐: lines.length <= maxLines → 不压缩
    if ((int)lines.size() <= max_lines) {
        json result;
        result["ok"]        = true;
        result["key"]       = key;
        result["compacted"] = false;
        result["kept"]      = (int)lines.size();
        return core::Result<json>::Ok(result);
    }

    // 🎯 OpenClaw 对齐: archiveFileOnDisk(filePath, "bak")
    std::string archived = archive_file_on_disk(file_path, "bak");

    // 🎯 OpenClaw 对齐: 写入最后 maxLines 行
    std::vector<std::string> kept_lines(lines.end() - max_lines, lines.end());
    {
        std::ofstream ofs(file_path);
        for (const auto& l : kept_lines) {
            ofs << l << "\n";
        }
    }

    // 🎯 OpenClaw 对齐: updateSessionStore → 清除 token 计数器
    // delete entryToUpdate.inputTokens / outputTokens / totalTokens / totalTokensFresh
    {
        sessions::SessionEntry updated_entry = entry;
        updated_entry.input_tokens  = 0;
        updated_entry.output_tokens = 0;
        updated_entry.total_tokens  = 0;
        updated_entry.updated_at    = (uint64_t)now_ms();
        session_mgr->update_session(updated_entry);
    }

    json result;
    result["ok"]        = true;
    result["key"]       = key;
    result["compacted"] = true;
    result["archived"]  = archived;
    result["kept"]      = (int)kept_lines.size();

    return core::Result<json>::Ok(result);
}

}  // namespace methods
}  // namespace gateway
}  // namespace hunterclaw
