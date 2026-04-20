#include "hunterclaw/sessions/session_manager.h"
#include "hunterclaw/observability/diagnostic_events.h"  // ✅ 可观测性集成
#include "hunterclaw/agents/types.h"  // ✅ AgentConfig 定义
#include <fstream>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <random>
#include <chrono>
#include <ctime>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace hunterclaw {
namespace sessions {

// ============================================================================
// Utility Functions
// ============================================================================

static std::string generate_uuid() {
    static std::random_device rd;
    static std::mt19937 gen(rd());
    static std::uniform_int_distribution<> dis(0, 15);
    
    const char* hex = "0123456789abcdef";
    std::string uuid = "xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx";
    
    for (auto& c : uuid) {
        if (c == 'x') {
            c = hex[dis(gen)];
        } else if (c == 'y') {
            c = hex[(dis(gen) & 0x3) | 0x8];
        }
    }
    
    return uuid;
}

static uint64_t current_timestamp_ms() {
    auto now = std::chrono::system_clock::now();
    auto duration = now.time_since_epoch();
    return std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
}

// JSON 序列化

// ACP Identity 序列化
static json to_json(const SessionAcpIdentity& identity) {
    json j;
    j["state"] = (identity.state == AcpIdentityState::Pending) ? "pending" : "resolved";
    if (identity.acp_record_id) j["acpRecordId"] = *identity.acp_record_id;
    if (identity.acp_session_id) j["acpSessionId"] = *identity.acp_session_id;
    if (identity.agent_session_id) j["agentSessionId"] = *identity.agent_session_id;
    j["source"] = identity.source;
    j["lastUpdatedAt"] = identity.last_updated_at;
    return j;
}

static SessionAcpIdentity acp_identity_from_json(const json& j) {
    SessionAcpIdentity identity;
    std::string state_str = j.value("state", "pending");
    identity.state = (state_str == "resolved") ? AcpIdentityState::Resolved : AcpIdentityState::Pending;
    if (j.contains("acpRecordId")) identity.acp_record_id = j["acpRecordId"].get<std::string>();
    if (j.contains("acpSessionId")) identity.acp_session_id = j["acpSessionId"].get<std::string>();
    if (j.contains("agentSessionId")) identity.agent_session_id = j["agentSessionId"].get<std::string>();
    identity.source = j.value("source", "");
    identity.last_updated_at = j.value("lastUpdatedAt", 0ULL);
    return identity;
}

// ACP Runtime Options 序列化
static json to_json(const AcpSessionRuntimeOptions& opts) {
    json j;
    j["mode"] = opts.mode;
    j["model"] = opts.model;
    j["cwd"] = opts.cwd;
    if (opts.max_turns) j["maxTurns"] = *opts.max_turns;
    return j;
}

static AcpSessionRuntimeOptions acp_runtime_options_from_json(const json& j) {
    AcpSessionRuntimeOptions opts;
    opts.mode = j.value("mode", "persistent");
    opts.model = j.value("model", "");
    opts.cwd = j.value("cwd", "");
    if (j.contains("maxTurns")) opts.max_turns = j["maxTurns"].get<int>();
    return opts;
}

// ACP Meta 序列化
static json to_json(const SessionAcpMeta& acp) {
    json j;
    j["backend"] = acp.backend;
    j["agent"] = acp.agent;
    j["runtimeSessionName"] = acp.runtime_session_name;
    if (acp.identity) j["identity"] = to_json(*acp.identity);
    j["runtimeOptions"] = to_json(acp.runtime_options);
    j["state"] = acp.state;
    j["lastActivityAt"] = acp.last_activity_at;
    return j;
}

static SessionAcpMeta acp_meta_from_json(const json& j) {
    SessionAcpMeta acp;
    acp.backend = j.value("backend", "");
    acp.agent = j.value("agent", "");
    acp.runtime_session_name = j.value("runtimeSessionName", "");
    if (j.contains("identity")) {
        acp.identity = acp_identity_from_json(j["identity"]);
    }
    if (j.contains("runtimeOptions")) {
        acp.runtime_options = acp_runtime_options_from_json(j["runtimeOptions"]);
    }
    acp.state = j.value("state", "idle");
    acp.last_activity_at = j.value("lastActivityAt", 0ULL);
    return acp;
}

// SessionMetadata 序列化
static json to_json(const SessionMetadata& meta) {
    json j;
    if (meta.provider) j["provider"] = *meta.provider;
    if (meta.channel) j["channel"] = *meta.channel;
    if (meta.account_id) j["accountId"] = *meta.account_id;
    if (meta.conversation_label) j["conversationLabel"] = *meta.conversation_label;
    if (meta.group_subject) j["groupSubject"] = *meta.group_subject;
    if (meta.sender_name) j["senderName"] = *meta.sender_name;
    if (meta.sender_username) j["senderUsername"] = *meta.sender_username;
    return j;
}

static SessionMetadata metadata_from_json(const json& j) {
    SessionMetadata meta;
    if (j.contains("provider")) meta.provider = j["provider"].get<std::string>();
    if (j.contains("channel")) meta.channel = j["channel"].get<std::string>();
    if (j.contains("accountId")) meta.account_id = j["accountId"].get<std::string>();
    if (j.contains("conversationLabel")) {
        meta.conversation_label = j["conversationLabel"].get<std::string>();
    }
    if (j.contains("groupSubject")) {
        meta.group_subject = j["groupSubject"].get<std::string>();
    }
    if (j.contains("senderName")) meta.sender_name = j["senderName"].get<std::string>();
    if (j.contains("senderUsername")) {
        meta.sender_username = j["senderUsername"].get<std::string>();
    }
    return meta;
}

static json to_json(const SessionEntry& session) {
    json j;
    j["sessionId"] = session.session_id;
    j["sessionKey"] = session.session_key;
    j["agentId"] = session.agent_id;
    j["createdAt"] = session.created_at;
    j["updatedAt"] = session.updated_at;

    j["inputTokens"]      = session.input_tokens;
    j["outputTokens"]     = session.output_tokens;
    j["totalTokens"]      = session.total_tokens;
    j["totalTokensFresh"] = session.total_tokens_fresh;
    j["systemSent"]       = session.system_sent;
    j["abortedLastRun"]   = session.aborted_last_run;

    // ✅ 对齐 OC memory-flush.ts: hasAlreadyFlushedForCurrentCompaction 的持久化字段
    if (session.compaction_count)
        j["compactionCount"] = *session.compaction_count;
    if (session.memory_flush_compaction_count)
        j["memoryFlushCompactionCount"] = *session.memory_flush_compaction_count;
    if (session.memory_flush_at_ms)
        j["memoryFlushAt"] = *session.memory_flush_at_ms;

    if (session.context_tokens) j["contextTokens"] = *session.context_tokens;
    if (session.label)          j["label"]          = *session.label;
    if (session.display_name)   j["displayName"]    = *session.display_name;
    if (session.chat_type)      j["chatType"]       = *session.chat_type;
    if (session.subject)        j["subject"]        = *session.subject;
    if (session.channel)        j["channel"]        = *session.channel;
    if (session.group_channel)  j["groupChannel"]   = *session.group_channel;
    if (session.space)          j["space"]          = *session.space;
    if (session.model)          j["model"]          = *session.model;
    if (session.model_provider) j["modelProvider"]  = *session.model_provider;
    if (session.thinking_level) j["thinkingLevel"]  = *session.thinking_level;
    if (session.fast_mode)      j["fastMode"]       = *session.fast_mode;
    if (session.verbose_level)  j["verboseLevel"]   = *session.verbose_level;
    if (session.reasoning_level) j["reasoningLevel"] = *session.reasoning_level;
    if (session.elevated_level) j["elevatedLevel"]  = *session.elevated_level;
    if (session.send_policy)    j["sendPolicy"]     = *session.send_policy;
    if (session.response_usage) j["responseUsage"]  = *session.response_usage;
    if (session.spawned_by)     j["spawnedBy"]      = *session.spawned_by;
    if (session.last_channel)   j["lastChannel"]    = *session.last_channel;
    if (session.last_to)        j["lastTo"]         = *session.last_to;
    if (session.last_account_id) j["lastAccountId"] = *session.last_account_id;
    if (session.last_thread_id) j["lastThreadId"]   = *session.last_thread_id;
    if (session.tts_auto)       j["ttsAuto"]        = *session.tts_auto;

    j["metadata"] = to_json(session.metadata);
    j["messages"] = session.messages;

    return j;
}

static SessionEntry session_from_json(const json& j) {
    SessionEntry session;
    session.session_id  = j.value("sessionId", "");
    session.session_key = j.value("sessionKey", "");
    session.agent_id    = j.value("agentId", "");
    session.created_at  = j.value("createdAt", 0ULL);
    session.updated_at  = j.value("updatedAt", 0ULL);

    session.input_tokens       = j.value("inputTokens", 0);
    session.output_tokens      = j.value("outputTokens", 0);
    session.total_tokens       = j.value("totalTokens", 0);
    session.total_tokens_fresh = j.value("totalTokensFresh", false);
    session.system_sent        = j.value("systemSent", false);
    session.aborted_last_run   = j.value("abortedLastRun", false);

    // ✅ 对齐 OC memory-flush.ts: hasAlreadyFlushedForCurrentCompaction 的持久化字段
    if (j.contains("compactionCount") && j["compactionCount"].is_number())
        session.compaction_count = j["compactionCount"].get<int>();
    if (j.contains("memoryFlushCompactionCount") && j["memoryFlushCompactionCount"].is_number())
        session.memory_flush_compaction_count = j["memoryFlushCompactionCount"].get<int>();
    if (j.contains("memoryFlushAt") && j["memoryFlushAt"].is_number())
        session.memory_flush_at_ms = j["memoryFlushAt"].get<int64_t>();

    if (j.contains("contextTokens") && j["contextTokens"].is_number())
        session.context_tokens = j["contextTokens"].get<int>();
    if (j.contains("label") && j["label"].is_string())
        session.label = j["label"].get<std::string>();
    if (j.contains("displayName") && j["displayName"].is_string())
        session.display_name = j["displayName"].get<std::string>();
    if (j.contains("chatType") && j["chatType"].is_string())
        session.chat_type = j["chatType"].get<std::string>();
    if (j.contains("subject") && j["subject"].is_string())
        session.subject = j["subject"].get<std::string>();
    if (j.contains("channel") && j["channel"].is_string())
        session.channel = j["channel"].get<std::string>();
    if (j.contains("groupChannel") && j["groupChannel"].is_string())
        session.group_channel = j["groupChannel"].get<std::string>();
    if (j.contains("space") && j["space"].is_string())
        session.space = j["space"].get<std::string>();
    if (j.contains("model") && j["model"].is_string())
        session.model = j["model"].get<std::string>();
    if (j.contains("modelProvider") && j["modelProvider"].is_string())
        session.model_provider = j["modelProvider"].get<std::string>();
    if (j.contains("thinkingLevel") && j["thinkingLevel"].is_string())
        session.thinking_level = j["thinkingLevel"].get<std::string>();
    if (j.contains("fastMode") && j["fastMode"].is_boolean())
        session.fast_mode = j["fastMode"].get<bool>();
    if (j.contains("verboseLevel") && j["verboseLevel"].is_string())
        session.verbose_level = j["verboseLevel"].get<std::string>();
    if (j.contains("reasoningLevel") && j["reasoningLevel"].is_string())
        session.reasoning_level = j["reasoningLevel"].get<std::string>();
    if (j.contains("elevatedLevel") && j["elevatedLevel"].is_string())
        session.elevated_level = j["elevatedLevel"].get<std::string>();
    if (j.contains("sendPolicy") && j["sendPolicy"].is_string())
        session.send_policy = j["sendPolicy"].get<std::string>();
    if (j.contains("responseUsage") && j["responseUsage"].is_string())
        session.response_usage = j["responseUsage"].get<std::string>();
    if (j.contains("spawnedBy") && j["spawnedBy"].is_string())
        session.spawned_by = j["spawnedBy"].get<std::string>();
    if (j.contains("lastChannel") && j["lastChannel"].is_string())
        session.last_channel = j["lastChannel"].get<std::string>();
    if (j.contains("lastTo") && j["lastTo"].is_string())
        session.last_to = j["lastTo"].get<std::string>();
    if (j.contains("lastAccountId") && j["lastAccountId"].is_string())
        session.last_account_id = j["lastAccountId"].get<std::string>();
    if (j.contains("lastThreadId") && j["lastThreadId"].is_string())
        session.last_thread_id = j["lastThreadId"].get<std::string>();
    if (j.contains("ttsAuto") && j["ttsAuto"].is_string())
        session.tts_auto = j["ttsAuto"].get<std::string>();

    if (j.contains("metadata")) {
        session.metadata = metadata_from_json(j["metadata"]);
    }

    // ACP 集成
    if (j.contains("acp")) {
        session.acp = acp_meta_from_json(j["acp"]);
    }

    if (j.contains("messages")) {
        session.messages = j["messages"].get<std::vector<std::string>>();
    }

    return session;
}

// ============================================================================
// FileSessionManager Implementation
// ============================================================================

SessionManager::Ptr SessionManager::create(
    const std::string& storage_dir,
    const SessionConfig& config
) {
    return std::make_shared<FileSessionManager>(storage_dir, config);
}

FileSessionManager::FileSessionManager(
    const std::string& storage_dir,
    const SessionConfig& config
)
    : storage_dir_(storage_dir)
    , config_(config)
{
    // Ensure storage directory exists
    try {
        fs::create_directories(storage_dir_);
        fs::permissions(storage_dir_,
            fs::perms::owner_read | fs::perms::owner_write | fs::perms::owner_exec,
            fs::perm_options::replace);
    } catch (...) {
        // Ignore errors
    }
}

std::string FileSessionManager::resolve_storage_path(const std::string& agent_id) const {
    if (!config_.per_agent_storage || agent_id.empty()) {
        return storage_dir_;
    }
    
    // Per-Agent Storage: {storage_dir}/agents/{agent_id}/
    std::string agent_dir = storage_dir_ + "/agents/" + agent_id;
    
    try {
        fs::create_directories(agent_dir);
        fs::permissions(agent_dir,
            fs::perms::owner_read | fs::perms::owner_write | fs::perms::owner_exec,
            fs::perm_options::replace);
    } catch (...) {
        // Fallback to storage_dir
        return storage_dir_;
    }
    
    return agent_dir;
}

std::string FileSessionManager::get_session_file_path(
    const std::string& session_key,
    const std::string& agent_id
) const {
    // URL-safe encode session_key
    std::string encoded;
    for (char c : session_key) {
        if (std::isalnum(c) || c == '-' || c == '_') {
            encoded += c;
        } else {
            encoded += '_';
        }
    }
    
    std::string base_dir = resolve_storage_path(agent_id);
    return base_dir + "/" + encoded + ".json";
}

core::Result<bool> FileSessionManager::write_session(const SessionEntry& session) {
    try {
        std::string file_path = get_session_file_path(session.session_key, session.agent_id);
        std::string tmp_path = file_path + ".tmp";
        
        json j = to_json(session);
        std::ofstream ofs(tmp_path);
        ofs << j.dump(2);
        ofs.close();
        
        fs::permissions(tmp_path,
            fs::perms::owner_read | fs::perms::owner_write,
            fs::perm_options::replace);
        
        fs::rename(tmp_path, file_path);
        
        return core::Result<bool>::Ok(true);
    } catch (const std::exception& e) {
        return core::Result<bool>::Err("Failed to write session: " + 
                                       std::string(e.what()));
    }
}

core::Result<SessionEntry> FileSessionManager::read_session(const std::string& file_path) {
    try {
        std::ifstream ifs(file_path);
        if (!ifs.is_open()) {
            return core::Result<SessionEntry>::Err("Failed to open file: " + file_path);
        }
        
        json j;
        ifs >> j;
        
        return core::Result<SessionEntry>::Ok(session_from_json(j));
    } catch (const std::exception& e) {
        return core::Result<SessionEntry>::Err("Failed to read session: " + 
                                               std::string(e.what()));
    }
}

core::Result<SessionEntry> FileSessionManager::get_or_create_session(
    const std::string& label_or_key,
    const std::string& agent_id,
    const SessionMetadata& metadata
) {
    // 解析 label → key
    auto key_result = SessionMapper::resolve_session_key(label_or_key, false);
    if (key_result.is_err()) {
        return core::Result<SessionEntry>::Err(key_result.error);
    }
    std::string session_key = key_result.get();
    
    std::string file_path = get_session_file_path(session_key, agent_id);
    
    // Try to load existing session
    if (fs::exists(file_path)) {
        auto result = read_session(file_path);
        if (result.is_ok()) {
            SessionEntry session = result.get();
            
            // Check if should reset
            if (should_reset(session)) {
                // Reset session (keep metadata but clear history)
                session.messages.clear();
                session.input_tokens = 0;
                session.output_tokens = 0;
                session.total_tokens = 0;
                session.context_tokens = 0;
                session.updated_at = current_timestamp_ms();
                
                write_session(session);
            }
            
            return core::Result<SessionEntry>::Ok(session);
        }
    }
    
    // Create new session
    SessionEntry session;
    session.session_id = generate_uuid();
    session.session_key = session_key;
    session.agent_id = agent_id;
    session.created_at = current_timestamp_ms();
    session.updated_at = session.created_at;
    session.metadata = metadata;
    
    // 如果是 label,存储 label
    auto extracted_label = SessionMapper::extract_label(label_or_key);
    if (extracted_label) {
        session.label = *extracted_label;
    }
    
    auto write_result = write_session(session);
    if (write_result.is_err()) {
        return core::Result<SessionEntry>::Err(write_result.error);
    }
    
    // ✅ 可观测性: 发射 SessionState 事件 (对齐 OpenClaw)
    try {
        auto state_event = std::make_shared<observability::SessionStateEvent>();
        state_event->session_id = session.session_id;
        state_event->old_state = observability::SessionState::IDLE;
        state_event->new_state = observability::SessionState::PROCESSING;
        state_event->reason = "created";
        state_event->state_duration_ms = 0;
        state_event->timestamp = std::chrono::system_clock::now();
        
        observability::DiagnosticEventBus::instance().emit(state_event);
    } catch (...) {
        // 故障隔离
    }
    
    return core::Result<SessionEntry>::Ok(session);
}

core::Result<bool> FileSessionManager::update_session(const SessionEntry& session) {
    auto result = write_session(session);
    
    // ✅ 可观测性: 发射 SessionState 事件 (更新)
    if (result.is_ok()) {
        try {
            auto state_event = std::make_shared<observability::SessionStateEvent>();
            state_event->session_id = session.session_id;
            state_event->old_state = observability::SessionState::PROCESSING;
            state_event->new_state = observability::SessionState::IDLE;
            state_event->reason = "active";
            state_event->state_duration_ms = 0;
            state_event->timestamp = std::chrono::system_clock::now();
            
            observability::DiagnosticEventBus::instance().emit(state_event);
        } catch (...) {}
    }
    
    return result;
}

core::Result<SessionEntry> FileSessionManager::get_session(const std::string& label_or_key) {
    // 解析 label → key
    auto key_result = SessionMapper::resolve_session_key(label_or_key, true);
    if (key_result.is_err()) {
        return core::Result<SessionEntry>::Err(key_result.error);
    }
    std::string session_key = key_result.get();
    
    // 需要遍历所有 agent 目录查找 (Per-Agent Storage)
    if (config_.per_agent_storage) {
        std::string agents_dir = storage_dir_ + "/agents";
        if (fs::exists(agents_dir)) {
            for (const auto& agent_entry : fs::directory_iterator(agents_dir)) {
                if (agent_entry.is_directory()) {
                    std::string agent_id = agent_entry.path().filename().string();
                    std::string file_path = get_session_file_path(session_key, agent_id);
                    
                    if (fs::exists(file_path)) {
                        return read_session(file_path);
                    }
                }
            }
        }
    }
    
    // Fallback: 尝试直接路径 (不带 agent_id)
    std::string file_path = get_session_file_path(session_key, "");
    if (fs::exists(file_path)) {
        return read_session(file_path);
    }
    
    return core::Result<SessionEntry>::Err("Session not found: " + label_or_key);
}

core::Result<bool> FileSessionManager::delete_session(const std::string& label_or_key) {
    try {
        // 解析 label → key
        auto key_result = SessionMapper::resolve_session_key(label_or_key, true);
        if (key_result.is_err()) {
            return core::Result<bool>::Err(key_result.error);
        }
        std::string session_key = key_result.get();
        
        bool deleted = false;
        std::string deleted_session_id;
        
        // 尝试删除所有可能的路径 (Per-Agent Storage)
        if (config_.per_agent_storage) {
            std::string agents_dir = storage_dir_ + "/agents";
            if (fs::exists(agents_dir)) {
                for (const auto& agent_entry : fs::directory_iterator(agents_dir)) {
                    if (agent_entry.is_directory()) {
                        std::string agent_id = agent_entry.path().filename().string();
                        std::string file_path = get_session_file_path(session_key, agent_id);
                        
                        if (fs::exists(file_path)) {
                            // 读取 session_id (用于可观测性)
                            auto session_result = read_session(file_path);
                            if (session_result.is_ok()) {
                                deleted_session_id = session_result.get().session_id;
                            }
                            
                            fs::remove(file_path);
                            deleted = true;
                        }
                    }
                }
            }
        }
        
        // Fallback: 尝试直接路径
        std::string file_path = get_session_file_path(session_key, "");
        if (fs::exists(file_path)) {
            auto session_result = read_session(file_path);
            if (session_result.is_ok()) {
                deleted_session_id = session_result.get().session_id;
            }
            
            fs::remove(file_path);
            deleted = true;
        }
        
        // ✅ 可观测性: 发射 SessionState 事件 (结束)
        if (deleted && !deleted_session_id.empty()) {
            try {
                auto state_event = std::make_shared<observability::SessionStateEvent>();
                state_event->session_id = deleted_session_id;
                state_event->old_state = observability::SessionState::IDLE;
                state_event->new_state = observability::SessionState::IDLE;
                state_event->reason = "ended";
                state_event->state_duration_ms = 0;
                state_event->timestamp = std::chrono::system_clock::now();
                
                observability::DiagnosticEventBus::instance().emit(state_event);
            } catch (...) {}
        }
        
        return core::Result<bool>::Ok(deleted);
    } catch (const std::exception& e) {
        return core::Result<bool>::Err("Failed to delete session: " + 
                                       std::string(e.what()));
    }
}

core::Result<std::vector<SessionEntry>> FileSessionManager::list_sessions(
    const std::optional<std::string>& agent_id
) {
    try {
        std::vector<SessionEntry> sessions;
        
        if (config_.per_agent_storage) {
            // Per-Agent Storage: 遍历 agents 子目录
            std::string agents_dir = storage_dir_ + "/agents";
            
            if (fs::exists(agents_dir)) {
                for (const auto& agent_entry : fs::directory_iterator(agents_dir)) {
                    if (!agent_entry.is_directory()) continue;
                    
                    std::string current_agent_id = agent_entry.path().filename().string();
                    
                    // 过滤 agent_id
                    if (agent_id && current_agent_id != *agent_id) {
                        continue;
                    }
                    
                    // 遍历该 agent 的所有 session 文件
                    for (const auto& entry : fs::directory_iterator(agent_entry.path())) {
                        if (entry.is_regular_file() && entry.path().extension() == ".json") {
                            auto result = read_session(entry.path().string());
                            if (result.is_ok()) {
                                sessions.push_back(result.get());
                            }
                        }
                    }
                }
            }
        } else {
            // 统一存储: 直接遍历 storage_dir
            for (const auto& entry : fs::directory_iterator(storage_dir_)) {
                if (entry.is_regular_file() && entry.path().extension() == ".json") {
                    auto result = read_session(entry.path().string());
                    if (result.is_ok()) {
                        SessionEntry session = result.get();
                        
                        // Filter by agent_id if provided
                        if (!agent_id || session.agent_id == *agent_id) {
                            sessions.push_back(session);
                        }
                    }
                }
            }
        }
        
        return core::Result<std::vector<SessionEntry>>::Ok(sessions);
    } catch (const std::exception& e) {
        return core::Result<std::vector<SessionEntry>>::Err(
            "Failed to list sessions: " + std::string(e.what()));
    }
}

bool FileSessionManager::is_idle(const SessionEntry& session) const {
    if (!config_.idle_minutes) {
        return false;
    }
    
    uint64_t now = current_timestamp_ms();
    uint64_t idle_ms = *config_.idle_minutes * 60 * 1000;
    
    return (now - session.updated_at) > idle_ms;
}

bool FileSessionManager::is_past_daily_reset(const SessionEntry& session) const {
    if (!config_.daily_reset) {
        return false;
    }
    
    auto now = std::chrono::system_clock::now();
    auto now_time_t = std::chrono::system_clock::to_time_t(now);
    std::tm now_tm = *std::localtime(&now_time_t);
    
    auto session_time_t = static_cast<std::time_t>(session.updated_at / 1000);
    std::tm session_tm = *std::localtime(&session_time_t);
    
    // Check if crossed reset hour
    if (now_tm.tm_year != session_tm.tm_year ||
        now_tm.tm_mon != session_tm.tm_mon ||
        now_tm.tm_mday != session_tm.tm_mday) {
        
        // Different day - check if past reset hour
        if (now_tm.tm_hour >= config_.daily_reset_hour) {
            return true;
        }
    }
    
    return false;
}

bool FileSessionManager::should_reset(const SessionEntry& session) const {
    // ✅ 对齐 OpenClaw: 完全禁用重置时直接返回 false
    if (!config_.daily_reset && !config_.idle_minutes.has_value()) {
        return false;  // 两种重置机制都禁用 - 永不重置
    }
    
    // 检查空闲超时或每日重置
    return is_idle(session) || is_past_daily_reset(session);
}

core::Result<int> FileSessionManager::prune_sessions() {
    auto sessions_result = list_sessions(std::nullopt);
    if (sessions_result.is_err()) {
        return core::Result<int>::Err(sessions_result.error);
    }
    
    std::vector<SessionEntry> sessions = sessions_result.get();
    int pruned = 0;
    uint64_t now = current_timestamp_ms();
    uint64_t prune_threshold_ms = config_.prune_after_days * 24 * 60 * 60 * 1000;
    
    // Sort by updated_at (oldest first)
    std::sort(sessions.begin(), sessions.end(),
        [](const SessionEntry& a, const SessionEntry& b) {
            return a.updated_at < b.updated_at;
        });
    
    // Prune old sessions
    for (const auto& session : sessions) {
        bool should_prune = false;
        
        // Check age
        if ((now - session.updated_at) > prune_threshold_ms) {
            should_prune = true;
        }
        
        // Check max entries
        if (static_cast<int>(sessions.size()) - pruned > config_.max_entries) {
            should_prune = true;
        }
        
        if (should_prune) {
            if (config_.maintenance_mode == SessionMaintenanceMode::Enforce) {
                delete_session(session.session_key);
                pruned++;
            } else {
                // Just warn (log would go here)
            }
        }
    }
    
    return core::Result<int>::Ok(pruned);
}

// ============================================================================
// SessionMapper Implementation (对齐 OpenClaw)
// ============================================================================

core::Result<std::string> SessionMapper::resolve_session_key(
    const std::string& label_or_key,
    bool require_existing
) {
    std::string input = label_or_key;
    
    // 修剪空白
    input.erase(0, input.find_first_not_of(" \t\n\r"));
    input.erase(input.find_last_not_of(" \t\n\r") + 1);
    
    if (input.empty()) {
        return core::Result<std::string>::Err("Empty session key or label");
    }
    
    // 检查是否是 Label 格式: "label:xxx"
    const std::string label_prefix = "label:";
    if (input.substr(0, label_prefix.size()) == label_prefix) {
        // 这是一个 Label - 需要解析
        std::string label = input.substr(label_prefix.size());
        
        // 验证 label (不能为空，最大长度 64)
        if (label.empty()) {
            return core::Result<std::string>::Err("Empty label");
        }
        if (label.size() > 64) {
            return core::Result<std::string>::Err("Label too long (max 64 characters)");
        }
        
        // 构造 session_key: 转换为规范化的 key
        // 对齐 OpenClaw: label 转换为小写，替换空格为 '-'
        std::string normalized_label = label;
        std::transform(normalized_label.begin(), normalized_label.end(),
                      normalized_label.begin(), ::tolower);
        std::replace(normalized_label.begin(), normalized_label.end(), ' ', '-');
        
        // 构造 session_key: agent:main:{label}
        // 注意: 这里假设使用 "main" agent (对齐 OpenClaw 默认逻辑)
        std::string session_key = "agent:main:label:" + normalized_label;
        
        return core::Result<std::string>::Ok(session_key);
    }
    
    // 不是 label 格式 - 直接作为 session_key
    // 对齐 OpenClaw: session_key 应该是小写
    std::string session_key = input;
    std::transform(session_key.begin(), session_key.end(),
                  session_key.begin(), ::tolower);
    
    // 如果需要验证存在 (require_existing)
    // 注意: 这里简化实现，不做文件存在检查 (由调用方处理)
    
    return core::Result<std::string>::Ok(session_key);
}

std::optional<std::string> SessionMapper::extract_label(const std::string& session_key) {
    // 尝试从 session_key 提取 label
    // 对齐 OpenClaw: 如果 session_key 包含 "label:" 片段，提取它
    
    const std::string label_marker = "label:";
    auto pos = session_key.find(label_marker);
    if (pos == std::string::npos) {
        return std::nullopt;
    }
    
    // 提取 label 部分 (从 "label:" 后面到下一个 ':' 或结束)
    size_t start = pos + label_marker.size();
    size_t end = session_key.find(':', start);
    
    std::string label;
    if (end == std::string::npos) {
        label = session_key.substr(start);
    } else {
        label = session_key.substr(start, end - start);
    }
    
    if (label.empty()) {
        return std::nullopt;
    }
    
    return label;
}

// ============================================================================
// AgentConfig 实现 (临时解决方案)
// ============================================================================

core::Result<hunterclaw::agents::AgentConfig> FileSessionManager::get_agent_config(
    const std::string& agent_id
) {
    // 🎯 对齐 OpenClaw: 从配置文件读取 agent 配置
    // 先尝试从 config.json 读取，失败则返回默认值
    hunterclaw::agents::AgentConfig config;
    config.id = agent_id;
    config.name = agent_id;
    config.is_default = (agent_id == "main" || agent_id == "default");
    
    std::string home = std::getenv("HOME") ? std::getenv("HOME") : "/tmp";
    config.workspace = home + "/.hunterclaw/agents/" + agent_id;
    config.agent_dir = config.workspace;
    config.creator = "";

    // Try to load from config file
    std::string config_path = home + "/.hunterclaw/config.json";
    try {
        if (std::filesystem::exists(config_path)) {
            std::ifstream f(config_path);
            nlohmann::json cfg;
            f >> cfg;
            if (cfg.contains("agents") && cfg["agents"].is_array()) {
                for (const auto& a : cfg["agents"]) {
                    if (a.value("id", "") == agent_id) {
                        config.name = a.value("name", agent_id);
                        // system_prompt is part of skill/prompt config, not AgentConfig
                        if (a.contains("workspace")) config.workspace = a["workspace"];
                        break;
                    }
                }
            }
        }
    } catch (...) {}
    
    // 默认模型配置
    config.model.provider = "anthropic";
    config.model.model = "claude-3-5-sonnet-20241022";
    config.model.max_tokens = 4096;
    config.model.temperature = 1.0;
    config.model.context_tokens = 200000;
    
    // 默认行为配置
    config.thinking_level = "medium";
    config.timeout_seconds = 600;
    
    // 默认 subagent 配置
    config.subagents.enabled = false;
    config.subagents.max_concurrent = 3;
    config.subagents.timeout_seconds = 900;
    config.subagents.max_depth = 5;
    
    return core::Result<hunterclaw::agents::AgentConfig>::Ok(config);
}

} // namespace sessions
} // namespace hunterclaw
