#include "hunterclaw/gateway/gateway.h"
#include "hunterclaw/core/utils.h"
#include "hunterclaw/core/json.h"

using json = nlohmann::json;

namespace hunterclaw {
namespace gateway {

// ============================================================================
// Gateway Methods Implementation (对齐 OpenClaw server-methods/)
// ============================================================================

class GatewayMethodsImpl {
public:
    explicit GatewayMethodsImpl(std::shared_ptr<GatewayServer> server)
        : server_(server) {}
    
    // ========== Core Methods ==========
    
    // 1. ping - Health check
    core::Result<json> method_ping(const json& params) {
        json result;
        result["pong"] = true;
        result["timestamp"] = core::utils::now().time_since_epoch().count();
        result["server"] = "HunterClaw";
        result["version"] = "0.1.0";
        return core::Result<json>::Ok(result);
    }
    
    // 2. echo - Echo back params (debug)
    core::Result<json> method_echo(const json& params) {
        return core::Result<json>::Ok(params);
    }
    
    // ========== Session Methods ==========
    
    // 3. session.list - List all sessions
    core::Result<json> method_session_list(const json& params) {
        auto sessions = server_->runtime_state()->sessions;
        
        json result = json::array();
        for (const auto& [session_id, session_info] : sessions) {
            json session_json;
            session_json["session_id"] = session_id;
            session_json["channel_id"] = session_info.key.channel_id;
            session_json["account_id"] = session_info.key.account_id;
            session_json["conversation_id"] = session_info.key.conversation_id;
            session_json["created_at"] = session_info.created_at.time_since_epoch().count();
            session_json["last_active_at"] = session_info.last_active_at.time_since_epoch().count();
            result.push_back(session_json);
        }
        
        return core::Result<json>::Ok(result);
    }
    
    // 4. session.get - Get session by ID
    core::Result<json> method_session_get(const json& params) {
        if (!params.contains("session_id")) {
            return core::Result<json>::Err("Missing session_id");
        }
        
        std::string session_id = params["session_id"];
        auto& sessions = server_->runtime_state()->sessions;
        
        auto it = sessions.find(session_id);
        if (it == sessions.end()) {
            return core::Result<json>::Err("Session not found");
        }
        
        const auto& session_info = it->second;
        json result;
        result["session_id"] = session_id;
        result["channel_id"] = session_info.key.channel_id;
        result["account_id"] = session_info.key.account_id;
        result["conversation_id"] = session_info.key.conversation_id;
        result["created_at"] = session_info.created_at.time_since_epoch().count();
        result["last_active_at"] = session_info.last_active_at.time_since_epoch().count();
        
        return core::Result<json>::Ok(result);
    }
    
    // 5. session.delete - Delete session
    core::Result<json> method_session_delete(const json& params) {
        if (!params.contains("session_id")) {
            return core::Result<json>::Err("Missing session_id");
        }
        
        std::string session_id = params["session_id"];
        auto& sessions = server_->runtime_state()->sessions;
        
        auto it = sessions.find(session_id);
        if (it == sessions.end()) {
            return core::Result<json>::Err("Session not found");
        }
        
        sessions.erase(it);
        
        json result;
        result["ok"] = true;
        return core::Result<json>::Ok(result);
    }
    
    // ========== Channel Methods ==========
    
    // 6. channel.list - List all channels
    core::Result<json> method_channel_list(const json& params) {
        auto channel_mgr = server_->channel_manager();
        auto channel_ids = channel_mgr->list_channels();
        
        json result = json::array();
        for (const auto& channel_id : channel_ids) {
            auto meta = channel_mgr->get_channel_meta(channel_id);
            if (meta.has_value()) {
                json channel_json;
                channel_json["id"] = meta->id;
                channel_json["name"] = meta->name;
                channel_json["description"] = meta->description;
                result.push_back(channel_json);
            }
        }
        
        return core::Result<json>::Ok(result);
    }
    
    // 7. channel.start - Start a channel
    core::Result<json> method_channel_start(const json& params) {
        if (!params.contains("channel_id")) {
            return core::Result<json>::Err("Missing channel_id");
        }
        
        std::string channel_id = params["channel_id"];
        std::optional<std::string> account_id;
        
        if (params.contains("account_id")) {
            account_id = params["account_id"].get<std::string>();
        }
        
        auto channel_mgr = server_->channel_manager();
        auto result = channel_mgr->start_channel(channel_id, account_id);
        
        if (result.is_err()) {
            return core::Result<json>::Err(result.error);
        }
        
        json response;
        response["ok"] = true;
        response["channel_id"] = channel_id;
        response["account_id"] = account_id.value_or("default");
        
        return core::Result<json>::Ok(response);
    }
    
    // 8. channel.stop - Stop a channel
    core::Result<json> method_channel_stop(const json& params) {
        if (!params.contains("channel_id")) {
            return core::Result<json>::Err("Missing channel_id");
        }
        
        std::string channel_id = params["channel_id"];
        std::optional<std::string> account_id;
        
        if (params.contains("account_id")) {
            account_id = params["account_id"].get<std::string>();
        }
        
        auto channel_mgr = server_->channel_manager();
        auto result = channel_mgr->stop_channel(channel_id, account_id);
        
        if (result.is_err()) {
            return core::Result<json>::Err(result.error);
        }
        
        json response;
        response["ok"] = true;
        response["channel_id"] = channel_id;
        
        return core::Result<json>::Ok(response);
    }
    
    // 9. channel.send - Send message through channel
    core::Result<json> method_channel_send(const json& params) {
        // Required fields
        if (!params.contains("channel_id") || !params.contains("to") || !params.contains("text")) {
            return core::Result<json>::Err("Missing required fields: channel_id, to, text");
        }
        
        std::string channel_id = params["channel_id"];
        std::string account_id = params.value("account_id", "default");
        std::string to = params["to"];
        std::string text = params["text"];
        
        auto channel_mgr = server_->channel_manager();
        auto result = channel_mgr->deliver_message(channel_id, account_id, to, text);
        
        if (result.is_err()) {
            return core::Result<json>::Err(result.error);
        }
        
        json response;
        response["ok"] = true;
        response["message_id"] = core::utils::generate_uuid();
        
        return core::Result<json>::Ok(response);
    }
    
    // ========== Skill Methods ==========
    
    // 10. skill.list - List all skills
    core::Result<json> method_skill_list(const json& params) {
        auto skill_mgr = server_->skill_manager();
        auto skills = skill_mgr->list_skills();
        
        json result = json::array();
        for (const auto& skill_meta : skills) {
            json skill_json;
            skill_json["name"] = skill_meta.name;
            skill_json["description"] = skill_meta.description;
            skill_json["path"] = skill_meta.path;
            result.push_back(skill_json);
        }
        
        return core::Result<json>::Ok(result);
    }
    
    // 11. skill.get - Get skill by name
    core::Result<json> method_skill_get(const json& params) {
        if (!params.contains("name")) {
            return core::Result<json>::Err("Missing name");
        }
        
        std::string name = params["name"];
        auto skill_mgr = server_->skill_manager();
        auto skill_opt = skill_mgr->get_skill(name);
        
        if (!skill_opt.has_value()) {
            return core::Result<json>::Err("Skill not found: " + name);
        }
        
        const auto& skill_meta = skill_opt.value();
        json result;
        result["name"] = skill_meta.name;
        result["description"] = skill_meta.description;
        result["path"] = skill_meta.path;
        result["content"] = skill_meta.content;
        
        return core::Result<json>::Ok(result);
    }
    
    // ========== Plugin Methods ==========
    
    // 12. plugin.list - List all plugins
    core::Result<json> method_plugin_list(const json& params) {
        auto plugin_mgr = server_->plugin_manager();
        auto plugins = plugin_mgr->list_plugins();
        
        json result = json::array();
        for (const auto& plugin_info : plugins) {
            json plugin_json;
            plugin_json["name"] = plugin_info.name;
            plugin_json["version"] = plugin_info.version;
            plugin_json["path"] = plugin_info.path;
            plugin_json["loaded"] = plugin_info.loaded;
            result.push_back(plugin_json);
        }
        
        return core::Result<json>::Ok(result);
    }
    
    // ========== Status Methods ==========
    
    // 13. status - Get server status
    core::Result<json> method_status(const json& params) {
        auto state = server_->runtime_state();
        
        json result;
        result["running"] = server_->is_running();
        result["connections"] = state->clients.size();
        result["sessions"] = state->sessions.size();
        
        // Channel status
        auto channel_mgr = server_->channel_manager();
        result["channels"] = channel_mgr->list_channels().size();
        
        // Skill status
        auto skill_mgr = server_->skill_manager();
        result["skills"] = skill_mgr->list_skills().size();
        
        // Plugin status
        auto plugin_mgr = server_->plugin_manager();
        result["plugins"] = plugin_mgr->list_plugins().size();
        
        result["uptime"] = 0;  // TODO: Track uptime
        result["version"] = "0.1.0";
        
        return core::Result<json>::Ok(result);
    }
    
    // ========== Method Router ==========
    
    core::Result<json> invoke(const std::string& method, const json& params) {
        // Core
        if (method == "ping") return method_ping(params);
        if (method == "echo") return method_echo(params);
        if (method == "status") return method_status(params);
        
        // Session
        if (method == "session.list") return method_session_list(params);
        if (method == "session.get") return method_session_get(params);
        if (method == "session.delete") return method_session_delete(params);
        
        // Channel
        if (method == "channel.list") return method_channel_list(params);
        if (method == "channel.start") return method_channel_start(params);
        if (method == "channel.stop") return method_channel_stop(params);
        if (method == "channel.send") return method_channel_send(params);
        
        // Skill
        if (method == "skill.list") return method_skill_list(params);
        if (method == "skill.get") return method_skill_get(params);
        
        // Plugin
        if (method == "plugin.list") return method_plugin_list(params);
        
        return core::Result<json>::Err("Unknown method: " + method);
    }
    
private:
    std::shared_ptr<GatewayServer> server_;
};

// Factory function for method handler
std::function<core::Result<json>(const std::string&, const json&)> 
create_method_handler(std::shared_ptr<GatewayServer> server) {
    auto impl = std::make_shared<GatewayMethodsImpl>(server);
    
    return [impl](const std::string& method, const json& params) -> core::Result<json> {
        return impl->invoke(method, params);
    };
}

} // namespace gateway
} // namespace hunterclaw
