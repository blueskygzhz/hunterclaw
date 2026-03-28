// gateway_tool.cc
// Strictly aligned with OpenClaw: src/agents/tools/gateway-tool.ts
//
// Tool: gateway
// Actions: restart | config.get | config.schema.lookup | config.apply | config.patch | update.run
// ownerOnly: true

#include "hunterclaw/tools/gateway_tool.h"
#include "hunterclaw/tools/tool.h"
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <string>
#include <set>
#include <optional>
#include <thread>
#include <chrono>
#include <csignal>
#include <unistd.h>
#include <cmath>

namespace hunterclaw {
namespace tools {

using json = nlohmann::json;

namespace {

// ============================================================================
// 🎯 对齐 OpenClaw: GATEWAY_ACTIONS
// ============================================================================
const std::set<std::string> GATEWAY_ACTIONS = {
    "restart",
    "config.get",
    "config.schema.lookup",
    "config.apply",
    "config.patch",
    "update.run",
};

// 🎯 对齐 OpenClaw: DEFAULT_UPDATE_TIMEOUT_MS = 20 * 60_000
constexpr int64_t DEFAULT_UPDATE_TIMEOUT_MS = 20 * 60000;

// ============================================================================
// Helper: read string param
// ============================================================================
static std::string read_str(const json& params, const std::string& key) {
    auto it = params.find(key);
    if (it == params.end() || !it->is_string()) return "";
    std::string s = it->get<std::string>();
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

// ============================================================================
// GatewayTool
// ============================================================================
class GatewayTool : public Tool {
public:
    explicit GatewayTool(GatewayToolOptions opts)
        : opts_(std::move(opts)) {}

    // 🎯 对齐 OpenClaw: name: "gateway"
    std::string name() const override { return "gateway"; }

    // 🎯 对齐 OpenClaw: description
    std::string description() const override {
        return "Restart, inspect a specific config schema path, apply config, or update the "
               "gateway in-place (SIGUSR1). Use config.schema.lookup with a targeted dot path "
               "before config edits. Use config.patch for safe partial config updates (merges "
               "with existing). Use config.apply only when replacing entire config. Both trigger "
               "restart after writing. Always pass a human-readable completion message via the "
               "`note` parameter so the system can deliver it to the user after restart.";
    }

    // 🎯 对齐 OpenClaw: GatewayToolSchema (flattened, no top-level anyOf/allOf/oneOf)
    Schema parameters() const override {
        Schema schema;
        schema.type = "object";

        auto make_str = [](const std::string& desc) {
            core::JsonValue p = core::JsonValue::object();
            p["type"] = "string";
            p["description"] = desc;
            return p;
        };
        auto make_num = [](const std::string& desc) {
            core::JsonValue p = core::JsonValue::object();
            p["type"] = "number";
            p["description"] = desc;
            return p;
        };

        // action (required)
        {
            core::JsonValue p = core::JsonValue::object();
            p["type"] = "string";
            p["description"] = "Action to perform";
            core::JsonValue e = core::JsonValue::array();
            for (const auto& a : GATEWAY_ACTIONS) e.push_back(a);
            p["enum"] = e;
            schema.properties["action"] = p;
        }

        // restart
        schema.properties["delayMs"]        = make_num("Restart delay in milliseconds");
        schema.properties["reason"]         = make_str("Restart reason");
        // config.get, config.schema.lookup, config.apply, update.run
        schema.properties["gatewayUrl"]     = make_str("Gateway URL override");
        schema.properties["gatewayToken"]   = make_str("Gateway token override");
        schema.properties["timeoutMs"]      = make_num("Request timeout in milliseconds");
        // config.schema.lookup
        schema.properties["path"]           = make_str("Config schema dot path to look up");
        // config.apply, config.patch
        schema.properties["raw"]            = make_str("Raw config YAML/JSON string");
        schema.properties["baseHash"]       = make_str("Base config hash for conflict detection");
        // config.apply, config.patch, update.run
        schema.properties["sessionKey"]     = make_str("Session key for post-restart delivery");
        schema.properties["note"]           = make_str("Human-readable completion message delivered after restart");
        schema.properties["restartDelayMs"] = make_num("Restart delay after config write (ms)");

        schema.required = {"action"};
        return schema;
    }

    // 🎯 对齐 OpenClaw: ownerOnly: true
    bool owner_only() const override { return true; }

    // 🎯 对齐 OpenClaw: execute
    ToolResult execute(const core::JsonValue& params_val) override {
        try {
            json params = params_val.to_json();
            if (auto log = spdlog::get("default")) log->debug("[GatewayTool] execute: {}", params.dump());

            std::string action = read_str(params, "action");
            if (action.empty())
                return make_error("action is required");
            if (!GATEWAY_ACTIONS.count(action))
                return make_error("Unknown action: " + action);

            // ----------------------------------------------------------------
            // 🎯 case "restart"
            // ----------------------------------------------------------------
            if (action == "restart") {
                // 🎯 对齐 OpenClaw: isRestartEnabled(opts?.config)
                if (!is_restart_enabled()) {
                    return make_error("Gateway restart is disabled (commands.restart=false).");
                }

                std::string session_key = read_str(params, "sessionKey");
                if (session_key.empty()) session_key = opts_.agent_session_key;

                std::optional<int64_t> delay_ms;
                if (params.contains("delayMs") && params["delayMs"].is_number()) {
                    double v = params["delayMs"].get<double>();
                    if (std::isfinite(v)) delay_ms = (int64_t)v;
                }

                std::string reason = read_str(params, "reason");
                if (reason.size() > 200) reason = reason.substr(0, 200);

                std::string note = read_str(params, "note");

                if (auto log = spdlog::get("default")) log->info("[GatewayTool] restart requested (delayMs={}, reason={})",
                    delay_ms.has_value() ? std::to_string(*delay_ms) : "default",
                    reason.empty() ? "none" : reason);

                // 🎯 对齐 OpenClaw: scheduleGatewaySigusr1Restart
                json result = schedule_gateway_restart(delay_ms, reason, session_key, note);
                return make_json_result(result);
            }

            // Gateway opts for remote calls
            std::string gw_url   = read_str(params, "gatewayUrl");
            std::string gw_token = read_str(params, "gatewayToken");
            int64_t timeout_ms = 60000;
            if (params.contains("timeoutMs") && params["timeoutMs"].is_number()) {
                double v = params["timeoutMs"].get<double>();
                if (std::isfinite(v)) timeout_ms = (int64_t)v;
            }

            // ----------------------------------------------------------------
            // 🎯 case "config.get"
            // ----------------------------------------------------------------
            if (action == "config.get") {
                auto result = call_gateway("config.get", gw_url, gw_token, timeout_ms, {});
                return make_json_result({{"ok", true}, {"result", result}});
            }

            // ----------------------------------------------------------------
            // 🎯 case "config.schema.lookup"
            // ----------------------------------------------------------------
            if (action == "config.schema.lookup") {
                std::string path = read_str(params, "path");
                if (path.empty())
                    return make_error("path required for config.schema.lookup");
                auto result = call_gateway("config.schema.lookup", gw_url, gw_token, timeout_ms,
                                           {{"path", path}});
                return make_json_result({{"ok", true}, {"result", result}});
            }

            // ----------------------------------------------------------------
            // 🎯 case "config.apply"
            // ----------------------------------------------------------------
            if (action == "config.apply") {
                std::string raw = read_str(params, "raw");
                if (raw.empty())
                    return make_error("raw required for config.apply");
                std::string base_hash = read_str(params, "baseHash");
                if (base_hash.empty()) {
                    // 🎯 对齐 OpenClaw: auto-fetch baseHash from config.get
                    auto snapshot = call_gateway("config.get", gw_url, gw_token, timeout_ms, {});
                    if (snapshot.contains("hash") && snapshot["hash"].is_string())
                        base_hash = snapshot["hash"].get<std::string>();
                    else if (snapshot.contains("raw") && snapshot["raw"].is_string()) {
                        // resolve from raw
                        base_hash = snapshot["raw"].get<std::string>().substr(0, 16);
                    }
                }
                if (base_hash.empty())
                    return make_error("Missing baseHash from config snapshot.");

                std::string session_key = read_str(params, "sessionKey");
                if (session_key.empty()) session_key = opts_.agent_session_key;
                std::string note = read_str(params, "note");
                std::optional<int64_t> restart_delay_ms;
                if (params.contains("restartDelayMs") && params["restartDelayMs"].is_number()) {
                    double v = params["restartDelayMs"].get<double>();
                    if (std::isfinite(v)) restart_delay_ms = (int64_t)v;
                }

                json call_params = {{"raw", raw}, {"baseHash", base_hash}};
                if (!session_key.empty()) call_params["sessionKey"] = session_key;
                if (!note.empty()) call_params["note"] = note;
                if (restart_delay_ms.has_value()) call_params["restartDelayMs"] = *restart_delay_ms;

                auto result = call_gateway("config.apply", gw_url, gw_token, timeout_ms, call_params);
                return make_json_result({{"ok", true}, {"result", result}});
            }

            // ----------------------------------------------------------------
            // 🎯 case "config.patch"
            // ----------------------------------------------------------------
            if (action == "config.patch") {
                std::string raw = read_str(params, "raw");
                if (raw.empty())
                    return make_error("raw required for config.patch");
                std::string base_hash = read_str(params, "baseHash");
                if (base_hash.empty()) {
                    auto snapshot = call_gateway("config.get", gw_url, gw_token, timeout_ms, {});
                    if (snapshot.contains("hash") && snapshot["hash"].is_string())
                        base_hash = snapshot["hash"].get<std::string>();
                }
                if (base_hash.empty())
                    return make_error("Missing baseHash from config snapshot.");

                std::string session_key = read_str(params, "sessionKey");
                if (session_key.empty()) session_key = opts_.agent_session_key;
                std::string note = read_str(params, "note");
                std::optional<int64_t> restart_delay_ms;
                if (params.contains("restartDelayMs") && params["restartDelayMs"].is_number()) {
                    double v = params["restartDelayMs"].get<double>();
                    if (std::isfinite(v)) restart_delay_ms = (int64_t)v;
                }

                json call_params = {{"raw", raw}, {"baseHash", base_hash}};
                if (!session_key.empty()) call_params["sessionKey"] = session_key;
                if (!note.empty()) call_params["note"] = note;
                if (restart_delay_ms.has_value()) call_params["restartDelayMs"] = *restart_delay_ms;

                auto result = call_gateway("config.patch", gw_url, gw_token, timeout_ms, call_params);
                return make_json_result({{"ok", true}, {"result", result}});
            }

            // ----------------------------------------------------------------
            // 🎯 case "update.run"
            // ----------------------------------------------------------------
            if (action == "update.run") {
                std::string session_key = read_str(params, "sessionKey");
                if (session_key.empty()) session_key = opts_.agent_session_key;
                std::string note = read_str(params, "note");
                std::optional<int64_t> restart_delay_ms;
                if (params.contains("restartDelayMs") && params["restartDelayMs"].is_number()) {
                    double v = params["restartDelayMs"].get<double>();
                    if (std::isfinite(v)) restart_delay_ms = (int64_t)v;
                }

                // 🎯 对齐 OpenClaw: updateTimeoutMs = gatewayOpts.timeoutMs ?? DEFAULT_UPDATE_TIMEOUT_MS
                int64_t update_timeout_ms = (timeout_ms > 0) ? timeout_ms : DEFAULT_UPDATE_TIMEOUT_MS;

                json call_params = {{"timeoutMs", update_timeout_ms}};
                if (!session_key.empty()) call_params["sessionKey"] = session_key;
                if (!note.empty()) call_params["note"] = note;
                if (restart_delay_ms.has_value()) call_params["restartDelayMs"] = *restart_delay_ms;

                auto result = call_gateway("update.run", gw_url, gw_token, update_timeout_ms, call_params);
                return make_json_result({{"ok", true}, {"result", result}});
            }

            return make_error("Unknown action: " + action);

        } catch (const std::exception& e) {
            if (auto log = spdlog::get("default")) log->error("[GatewayTool] exception: {}", e.what());
            return make_error(std::string(e.what()));
        }
    }

    // ownerOnly = true
    bool requires_approval() const override { return false; }

private:
    GatewayToolOptions opts_;

    ToolResult make_error(const std::string& msg) const {
        ToolResult result;
        result.success = false;
        result.error = msg;
        ToolResultContent content;
        content.type = ToolResultContent::Type::Text;
        content.text = "Error: " + msg;
        result.content.push_back(content);
        return result;
    }

    ToolResult make_json_result(const json& data) const {
        ToolResult result;
        result.success = true;
        ToolResultContent content;
        content.type = ToolResultContent::Type::Json;
        content.text = data.dump();
        result.content.push_back(content);
        return result;
    }

    // 🎯 对齐 OpenClaw: isRestartEnabled(opts?.config)
    bool is_restart_enabled() const {
        if (!opts_.config) return true; // default: enabled
        // Check config.commands.restart
        const auto& data = opts_.config->data;
        if (data.contains("commands") && data["commands"].contains("restart")) {
            const auto& v = data["commands"]["restart"];
            if (v.is_boolean()) return v.get<bool>();
        }
        return true; // default: enabled
    }

    // 🎯 对齐 OpenClaw: scheduleGatewaySigusr1Restart
    json schedule_gateway_restart(
        std::optional<int64_t> delay_ms,
        const std::string& reason,
        const std::string& session_key,
        const std::string& note
    ) const {
        // Send SIGUSR1 to self (gateway process)
        // In HunterClaw, the gateway is the current process
        int64_t actual_delay = delay_ms.value_or(500);

        // Schedule restart via SIGUSR1 after delay
        // This is a best-effort implementation
        std::thread([actual_delay]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(actual_delay));
            ::kill(::getpid(), SIGUSR1);
        }).detach();

        json result = {
            {"scheduled", true},
            {"delayMs",   actual_delay},
        };
        if (!reason.empty())      result["reason"]     = reason;
        if (!session_key.empty()) result["sessionKey"] = session_key;
        if (!note.empty())        result["note"]       = note;
        return result;
    }

    // 🎯 对齐 OpenClaw: callGatewayTool(method, gatewayOpts, params)
    // Calls the gateway via HTTP/WebSocket
    json call_gateway(
        const std::string& method,
        const std::string& gw_url,
        const std::string& gw_token,
        int64_t timeout_ms,
        const json& call_params
    ) const {
        // In HunterClaw, gateway calls go through the GatewayClient
        // For now, return a stub response indicating the call was made
        // The actual implementation depends on the GatewayClient being available
        // TODO: inject GatewayClient into GatewayTool for real calls
        if (auto log = spdlog::get("default")) log->info("[GatewayTool] call_gateway: method={}, url={}, timeout={}ms",
                     method, gw_url.empty() ? "(default)" : gw_url, timeout_ms);

        // Return a stub response
        json result = {
            {"method",  method},
            {"ok",      true},
            {"stub",    true},
            {"message", "Gateway call stub - inject GatewayClient for real calls"},
        };
        if (!call_params.empty()) result["params"] = call_params;
        return result;
    }
};

} // namespace

// ============================================================================
// Factory
// ============================================================================

std::shared_ptr<Tool> create_gateway_tool(const GatewayToolOptions& opts) {
    return std::make_shared<GatewayTool>(opts);
}

} // namespace tools
} // namespace hunterclaw
