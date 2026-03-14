#include "hunterclaw/tools/sessions_tool.h"
#include <fstream>
#include <sstream>
#include <filesystem>
#include <chrono>
#include <algorithm>

namespace hunterclaw {
namespace tools {

namespace {

// Helper: Get current timestamp in milliseconds
int64_t get_current_timestamp_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(
        system_clock::now().time_since_epoch()
    ).count();
}

// Helper: Parse session kind from key
std::string classify_session_kind(
    const std::string& key,
    const std::optional<std::string>& gateway_kind,
    const std::string& alias,
    const std::string& main_key
) {
    if (key == alias || key == main_key) return "main";
    if (gateway_kind.has_value()) return gateway_kind.value();
    if (key.find(":cron:") != std::string::npos) return "cron";
    if (key.find(":hook:") != std::string::npos) return "hook";
    if (key.find(":node:") != std::string::npos) return "node";
    if (key.find(":group:") != std::string::npos) return "group";
    return "other";
}

// Helper: Check if session is within active minutes
bool is_session_active(int64_t last_active, int active_minutes) {
    if (active_minutes <= 0) return true;
    int64_t now = get_current_timestamp_ms();
    int64_t threshold = now - (active_minutes * 60 * 1000);
    return last_active >= threshold;
}

// Helper: Redact sensitive information from text
std::string redact_sensitive_text(const std::string& text) {
    // Simple redaction for common patterns
    // In production: Use comprehensive regex patterns
    std::string result = text;
    
    // Redact API keys (e.g., sk-xxx, api_xxx)
    std::regex api_key_pattern(R"(\b(?:sk|api|key)[-_]?[a-zA-Z0-9]{20,}\b)");
    result = std::regex_replace(result, api_key_pattern, "[API_KEY_REDACTED]");
    
    // Redact tokens
    std::regex token_pattern(R"(\b(?:token|bearer|jwt)[:=\s]+[a-zA-Z0-9\-_.]{20,}\b)");
    result = std::regex_replace(result, token_pattern, "[TOKEN_REDACTED]");
    
    return result;
}

} // anonymous namespace

//=============================================================================
// SessionsTool Implementation
//=============================================================================

SessionsTool::SessionsTool(const Config& config)
    : config_(config)
    , cache_timestamp_(0)
{
    // Initialize session storage
    std::filesystem::create_directories(config_.session_storage_path);
}

ToolMetadata SessionsTool::get_metadata() const {
    return ToolMetadata{
        .name = "sessions",
        .description = "Multi-modal session management tool with 5 actions: "
                      "sessions_list, sessions_send, sessions_spawn, "
                      "sessions_history, session_status",
        .version = "1.0.0",
        .author = "HunterClaw Team",
        .actions = {
            "sessions_list",
            "sessions_send", 
            "sessions_spawn",
            "sessions_history",
            "session_status"
        }
    };
}

json SessionsTool::execute(const std::string& action, const json& params) {
    try {
        if (action == "sessions_list") {
            return sessions_list(params);
        } else if (action == "sessions_send") {
            return sessions_send(params);
        } else if (action == "sessions_spawn") {
            return sessions_spawn(params);
        } else if (action == "sessions_history") {
            return sessions_history(params);
        } else if (action == "session_status") {
            return session_status(params);
        } else {
            return json{
                {"status", "error"},
                {"error", "Unknown action: " + action}
            };
        }
    } catch (const std::exception& e) {
        return json{
            {"status", "error"},
            {"error", std::string("SessionsTool error: ") + e.what()}
        };
    }
}

//=============================================================================
// Sessions List
//=============================================================================

json SessionsTool::sessions_list(const json& params) {
    // Parse parameters
    std::optional<std::vector<std::string>> kinds;
    if (params.contains("kinds") && params["kinds"].is_array()) {
        kinds = params["kinds"].get<std::vector<std::string>>();
    }
    
    std::optional<int> active_minutes;
    if (params.contains("active_minutes")) {
        active_minutes = params["active_minutes"].get<int>();
    }
    
    std::optional<int> limit;
    if (params.contains("limit")) {
        limit = params["limit"].get<int>();
    }
    
    int message_limit = params.value("message_limit", 0);
    message_limit = std::min(message_limit, 20); // Cap at 20
    
    // Load sessions from storage/gateway
    auto sessions = load_sessions_from_storage();
    
    // Apply filters
    auto filtered = filter_sessions(sessions, kinds, active_minutes, limit);
    
    // Build result
    json::array_t result_array;
    for (const auto& session : filtered) {
        json session_json = {
            {"key", session.key},
            {"kind", session.kind},
            {"channel", session.channel},
            {"created_at", session.created_at},
            {"last_active_at", session.last_active_at},
            {"is_active", session.is_active},
            {"message_count", session.message_count}
        };
        
        if (session.label.has_value()) {
            session_json["label"] = session.label.value();
        }
        if (session.agent_id.has_value()) {
            session_json["agent_id"] = session.agent_id.value();
        }
        if (session.model.has_value()) {
            session_json["model"] = session.model.value();
        }
        
        // Include last messages if requested
        if (message_limit > 0) {
            auto history = load_session_history(
                session.key,
                message_limit,
                false // Don't include tool messages
            );
            
            json::array_t messages;
            for (const auto& msg : history) {
                messages.push_back(json{
                    {"role", msg.role},
                    {"content", msg.content},
                    {"timestamp", msg.timestamp}
                });
            }
            session_json["last_messages"] = messages;
        }
        
        result_array.push_back(session_json);
    }
    
    return json{
        {"status", "success"},
        {"sessions", result_array},
        {"count", result_array.size()},
        {"visibility", config_.visibility_mode}
    };
}

//=============================================================================
// Sessions Send
//=============================================================================

json SessionsTool::sessions_send(const json& params) {
    // Validate required parameter
    if (!params.contains("message")) {
        return json{
            {"status", "error"},
            {"error", "Missing required parameter: message"}
        };
    }
    
    std::string message = params["message"].get<std::string>();
    if (message.empty()) {
        return json{
            {"status", "error"},
            {"error", "Message cannot be empty"}
        };
    }
    
    // Resolve target session key
    std::string target_key;
    
    if (params.contains("session_key")) {
        target_key = params["session_key"].get<std::string>();
    } else if (params.contains("label")) {
        std::string label = params["label"].get<std::string>();
        std::optional<std::string> agent_id;
        if (params.contains("agent_id")) {
            agent_id = params["agent_id"].get<std::string>();
        }
        
        // Check agent-to-agent policy if cross-agent
        if (agent_id.has_value() && 
            agent_id.value() != config_.requester_agent_id) {
            if (!config_.agent_to_agent_enabled) {
                return json{
                    {"status", "forbidden"},
                    {"error", "Agent-to-agent messaging is disabled"}
                };
            }
            if (!check_agent_to_agent_allowed(
                config_.requester_agent_id, 
                agent_id.value()
            )) {
                return json{
                    {"status", "forbidden"},
                    {"error", "Agent-to-agent messaging denied by policy"}
                };
            }
        }
        
        target_key = resolve_session_key_from_label(label, agent_id);
        if (target_key.empty()) {
            return json{
                {"status", "error"},
                {"error", "No session found with label: " + label}
            };
        }
    } else {
        return json{
            {"status", "error"},
            {"error", "Provide either session_key or label"}
        };
    }
    
    // Parse timeout
    std::optional<int> timeout_seconds;
    if (params.contains("timeout_seconds")) {
        timeout_seconds = params["timeout_seconds"].get<int>();
    }
    
    // Send message
    auto result = send_message_internal(target_key, message, timeout_seconds);
    
    return json{
        {"status", result.status},
        {"run_id", result.run_id},
        {"target_key", target_key},
        {"error", result.error.value_or("")}
    };
}

//=============================================================================
// Sessions Spawn
//=============================================================================

json SessionsTool::sessions_spawn(const json& params) {
    // Validate required parameter
    if (!params.contains("task")) {
        return json{
            {"status", "error"},
            {"error", "Missing required parameter: task"}
        };
    }
    
    std::string task = params["task"].get<std::string>();
    if (task.empty()) {
        return json{
            {"status", "error"},
            {"error", "Task cannot be empty"}
        };
    }
    
    // Parse optional parameters
    std::optional<std::string> label;
    if (params.contains("label")) {
        label = params["label"].get<std::string>();
    }
    
    std::string runtime = params.value("runtime", "subagent");
    if (runtime != "subagent" && runtime != "acp") {
        return json{
            {"status", "error"},
            {"error", "Invalid runtime. Must be 'subagent' or 'acp'"}
        };
    }
    
    std::string mode = params.value("mode", "session");
    if (mode != "run" && mode != "session") {
        return json{
            {"status", "error"},
            {"error", "Invalid mode. Must be 'run' or 'session'"}
        };
    }
    
    std::optional<std::string> model;
    if (params.contains("model")) {
        model = params["model"].get<std::string>();
    }
    
    // Spawn session
    auto result = spawn_session_internal(task, label, runtime, mode, model);
    
    json response = {
        {"status", result.status},
        {"session_key", result.session_key}
    };
    
    if (result.session_id.has_value()) {
        response["session_id"] = result.session_id.value();
    }
    if (result.run_id.has_value()) {
        response["run_id"] = result.run_id.value();
    }
    if (result.error.has_value()) {
        response["error"] = result.error.value();
    }
    
    return response;
}

//=============================================================================
// Sessions History
//=============================================================================

json SessionsTool::sessions_history(const json& params) {
    // Validate required parameter
    if (!params.contains("session_key")) {
        return json{
            {"status", "error"},
            {"error", "Missing required parameter: session_key"}
        };
    }
    
    std::string session_key = params["session_key"].get<std::string>();
    int limit = params.value("limit", 50);
    limit = std::min(limit, 200); // Cap at 200
    
    bool include_tools = params.value("include_tools", false);
    
    // Check visibility
    // (Simplified: In production, check against visibility policy)
    
    // Load history
    auto messages = load_session_history(session_key, limit, include_tools);
    
    // Build result
    json::array_t messages_array;
    for (const auto& msg : messages) {
        json msg_json = {
            {"role", msg.role},
            {"content", msg.content},
            {"timestamp", msg.timestamp}
        };
        
        if (msg.tool_name.has_value()) {
            msg_json["tool_name"] = msg.tool_name.value();
        }
        if (msg.tool_call_id.has_value()) {
            msg_json["tool_call_id"] = msg.tool_call_id.value();
        }
        if (msg.truncated) {
            msg_json["truncated"] = true;
        }
        if (msg.redacted) {
            msg_json["redacted"] = true;
        }
        
        messages_array.push_back(msg_json);
    }
    
    return json{
        {"status", "success"},
        {"session_key", session_key},
        {"messages", messages_array},
        {"count", messages_array.size()},
        {"include_tools", include_tools}
    };
}

//=============================================================================
// Session Status
//=============================================================================

json SessionsTool::session_status(const json& params) {
    std::string session_key = params.value(
        "session_key", 
        config_.requester_session_key
    );
    
    if (session_key.empty()) {
        return json{
            {"status", "error"},
            {"error", "No session key provided and no default available"}
        };
    }
    
    // Get status
    auto status = get_session_status_internal(session_key);
    
    json response = {
        {"status", "success"},
        {"session_key", status.session_key},
        {"kind", status.kind},
        {"model", status.model},
        {"session_status", status.status},
        {"created_at", status.created_at},
        {"last_active_at", status.last_active_at},
        {"message_count", status.message_count},
        {"pending_messages", status.pending_messages}
    };
    
    if (status.label.has_value()) {
        response["label"] = status.label.value();
    }
    if (status.channel.has_value()) {
        response["channel"] = status.channel.value();
    }
    if (status.error.has_value()) {
        response["error_message"] = status.error.value();
    }
    
    return response;
}

//=============================================================================
// Helper Implementations (Stubs)
//=============================================================================

std::vector<SessionInfo> SessionsTool::load_sessions_from_storage() {
    // STUB: In production, load from gateway or local storage
    std::vector<SessionInfo> sessions;
    
    // Example: Load from gateway
    try {
        auto gateway_result = call_gateway("sessions.list", json{
            {"include_global", !config_.sandboxed},
            {"include_unknown", !config_.sandboxed}
        });
        
        if (gateway_result.contains("sessions")) {
            // Parse sessions from gateway response
            // (Implementation would parse JSON into SessionInfo structs)
        }
    } catch (const std::exception& e) {
        // Fallback: Load from local storage
    }
    
    return sessions;
}

std::vector<SessionInfo> SessionsTool::filter_sessions(
    const std::vector<SessionInfo>& sessions,
    const std::optional<std::vector<std::string>>& kinds,
    const std::optional<int>& active_minutes,
    const std::optional<int>& limit
) {
    std::vector<SessionInfo> filtered;
    
    for (const auto& session : sessions) {
        // Filter by kind
        if (kinds.has_value()) {
            bool kind_match = std::find(
                kinds->begin(), 
                kinds->end(), 
                session.kind
            ) != kinds->end();
            if (!kind_match) continue;
        }
        
        // Filter by active minutes
        if (active_minutes.has_value()) {
            if (!is_session_active(session.last_active_at, active_minutes.value())) {
                continue;
            }
        }
        
        // Check visibility
        if (!check_session_visibility(session)) {
            continue;
        }
        
        filtered.push_back(session);
    }
    
    // Apply limit
    if (limit.has_value() && filtered.size() > static_cast<size_t>(limit.value())) {
        filtered.resize(limit.value());
    }
    
    return filtered;
}

bool SessionsTool::check_session_visibility(const SessionInfo& session) {
    // STUB: Implement visibility checks based on config_.visibility_mode
    // - "tree": current session + spawned subagents
    // - "self": only current session
    // - "agent": any session in current agent
    // - "all": any session (requires A2A enabled)
    
    if (config_.visibility_mode == "self") {
        return session.key == config_.requester_session_key;
    }
    
    // Default: allow (implement full logic in production)
    return true;
}

bool SessionsTool::check_agent_to_agent_allowed(
    const std::string& requester_agent,
    const std::string& target_agent
) {
    if (requester_agent == target_agent) return true;
    if (!config_.agent_to_agent_enabled) return false;
    
    // Check allow list
    std::string pair1 = requester_agent + ":" + target_agent;
    std::string pair2 = target_agent + ":" + requester_agent;
    
    for (const auto& allowed : config_.a2a_allow) {
        if (allowed == pair1 || allowed == pair2 || allowed == "*") {
            return true;
        }
    }
    
    return false;
}

std::string SessionsTool::resolve_session_key_from_label(
    const std::string& label,
    const std::optional<std::string>& agent_id
) {
    // STUB: Query gateway to resolve label to session key
    try {
        json params = {{"label", label}};
        if (agent_id.has_value()) {
            params["agent_id"] = agent_id.value();
        }
        
        auto result = call_gateway("sessions.resolve", params);
        if (result.contains("key")) {
            return result["key"].get<std::string>();
        }
    } catch (const std::exception& e) {
        // Label not found
    }
    
    return "";
}

std::vector<SessionMessage> SessionsTool::load_session_history(
    const std::string& session_key,
    int limit,
    bool include_tools
) {
    // STUB: Load history from storage or gateway
    std::vector<SessionMessage> messages;
    
    // Example implementation would:
    // 1. Load session file from storage
    // 2. Parse messages
    // 3. Filter tool messages if !include_tools
    // 4. Redact sensitive data
    // 5. Apply limit
    
    return messages;
}

SessionStatus SessionsTool::get_session_status_internal(
    const std::string& session_key
) {
    // STUB: Query gateway for session status
    SessionStatus status;
    status.session_key = session_key;
    status.kind = "main";
    status.model = "claude-sonnet-4.5";
    status.status = "active";
    status.created_at = get_current_timestamp_ms() - 3600000; // 1 hour ago
    status.last_active_at = get_current_timestamp_ms();
    status.message_count = 10;
    status.pending_messages = 0;
    
    return status;
}

SessionSpawnResult SessionsTool::spawn_session_internal(
    const std::string& task,
    const std::optional<std::string>& label,
    const std::string& runtime,
    const std::string& mode,
    const std::optional<std::string>& model
) {
    // STUB: Call gateway to spawn new session
    SessionSpawnResult result;
    result.status = "success";
    result.session_key = "agent:main:spawn-" + std::to_string(get_current_timestamp_ms());
    result.session_id = "session-" + std::to_string(get_current_timestamp_ms());
    
    return result;
}

SessionSendResult SessionsTool::send_message_internal(
    const std::string& target_key,
    const std::string& message,
    const std::optional<int>& timeout_seconds
) {
    // STUB: Send message via gateway
    SessionSendResult result;
    result.status = "success";
    result.run_id = "run-" + std::to_string(get_current_timestamp_ms());
    
    return result;
}

json SessionsTool::call_gateway(const std::string& method, const json& params) {
    // STUB: HTTP/gRPC call to gateway
    // In production: Implement actual gateway communication
    return json{
        {"status", "success"},
        {"method", method}
    };
}

//=============================================================================
// Factory Function
//=============================================================================

std::unique_ptr<SessionsTool> create_sessions_tool(const SessionsTool::Config& config) {
    return std::make_unique<SessionsTool>(config);
}

} // namespace tools
} // namespace hunterclaw
