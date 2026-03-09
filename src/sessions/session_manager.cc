#include "hunterclaw/sessions/session_manager.h"
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
    
    j["inputTokens"] = session.input_tokens;
    j["outputTokens"] = session.output_tokens;
    j["totalTokens"] = session.total_tokens;
    j["contextTokens"] = session.context_tokens;
    
    j["metadata"] = to_json(session.metadata);
    j["messages"] = session.messages;
    
    return j;
}

static SessionEntry session_from_json(const json& j) {
    SessionEntry session;
    session.session_id = j.value("sessionId", "");
    session.session_key = j.value("sessionKey", "");
    session.agent_id = j.value("agentId", "");
    session.created_at = j.value("createdAt", 0ULL);
    session.updated_at = j.value("updatedAt", 0ULL);
    
    session.input_tokens = j.value("inputTokens", 0);
    session.output_tokens = j.value("outputTokens", 0);
    session.total_tokens = j.value("totalTokens", 0);
    session.context_tokens = j.value("contextTokens", 0);
    
    if (j.contains("metadata")) {
        session.metadata = metadata_from_json(j["metadata"]);
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

std::string FileSessionManager::get_session_file_path(const std::string& session_key) const {
    // URL-safe encode session_key
    std::string encoded;
    for (char c : session_key) {
        if (std::isalnum(c) || c == '-' || c == '_') {
            encoded += c;
        } else {
            encoded += '_';
        }
    }
    return storage_dir_ + "/" + encoded + ".json";
}

core::Result<bool> FileSessionManager::write_session(const SessionEntry& session) {
    try {
        std::string file_path = get_session_file_path(session.session_key);
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
    const std::string& session_key,
    const std::string& agent_id,
    const SessionMetadata& metadata
) {
    std::string file_path = get_session_file_path(session_key);
    
    // Try to load existing session
    if (fs::exists(file_path)) {
        auto result = read_session(file_path);
        if (result.is_ok()) {
            auto session = result.value;
            
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
    
    auto write_result = write_session(session);
    if (write_result.is_err()) {
        return core::Result<SessionEntry>::Err(write_result.error);
    }
    
    return core::Result<SessionEntry>::Ok(session);
}

core::Result<bool> FileSessionManager::update_session(const SessionEntry& session) {
    return write_session(session);
}

core::Result<bool> FileSessionManager::delete_session(const std::string& session_key) {
    try {
        std::string file_path = get_session_file_path(session_key);
        
        if (fs::exists(file_path)) {
            fs::remove(file_path);
        }
        
        return core::Result<bool>::Ok(true);
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
        
        for (const auto& entry : fs::directory_iterator(storage_dir_)) {
            if (entry.is_regular_file() && entry.path().extension() == ".json") {
                auto result = read_session(entry.path().string());
                if (result.is_ok()) {
                    auto session = result.value;
                    
                    // Filter by agent_id if provided
                    if (!agent_id || session.agent_id == *agent_id) {
                        sessions.push_back(session);
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
    return is_idle(session) || is_past_daily_reset(session);
}

core::Result<int> FileSessionManager::prune_sessions() {
    auto sessions_result = list_sessions();
    if (sessions_result.is_err()) {
        return core::Result<int>::Err(sessions_result.error);
    }
    
    auto sessions = sessions_result.value;
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

} // namespace sessions
} // namespace hunterclaw
