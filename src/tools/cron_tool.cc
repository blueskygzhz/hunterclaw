// 🎯 严格对齐 OpenClaw cron-tool.ts (src/agents/tools/cron-tool.ts)
// Single-file implementation — mirrors the structure of OpenClaw exactly.
// OpenClaw has ONE cron-tool.ts; HunterClaw has ONE cron_tool.cc here.
//
// The src/cron/ directory is the cron *scheduler subsystem* (service, storage,
// schedule, delivery, gateway_integration). This file is the *agent tool* that
// sits in src/tools/ alongside all other agent tools, and calls into the cron
// subsystem via GatewayCronIntegration — exactly as OpenClaw calls callGatewayTool.

#include "hunterclaw/tools/cron_tool.h"
#include "hunterclaw/cron/gateway_integration.h"
#include "hunterclaw/cron/delivery.h"
#include "hunterclaw/cron/types.h"
#include "hunterclaw/tools/tool.h"
#include "hunterclaw/sessions/session_key_utils.h"
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <set>
#include <string>
#include <algorithm>
#include <cmath>

namespace hunterclaw {
namespace tools {

using json = nlohmann::json;

namespace {

// ============================================================================
// 🎯 对齐 OpenClaw: constants
// ============================================================================

const std::set<std::string> CRON_ACTIONS = {
    "status", "list", "add", "update", "remove", "run", "runs", "wake"
};

// 🎯 对齐 OpenClaw: REMINDER_CONTEXT_* constants
constexpr int REMINDER_CONTEXT_MESSAGES_MAX   = 10;
constexpr int REMINDER_CONTEXT_PER_MESSAGE_MAX = 220;
constexpr int REMINDER_CONTEXT_TOTAL_MAX       = 700;
const std::string REMINDER_CONTEXT_MARKER      = "\n\nRecent context:\n";

// 🎯 对齐 OpenClaw: JOB_KEYS (flat-params recovery for add)
const std::set<std::string> JOB_KEYS = {
    "name", "schedule", "sessionTarget", "wakeMode", "payload", "delivery",
    "enabled", "description", "deleteAfterRun", "agentId", "sessionKey",
    "message", "text", "model", "thinking", "timeoutSeconds",
    "allowUnsafeExternalContent"
};

// 🎯 对齐 OpenClaw: PATCH_KEYS (flat-params recovery for update)
const std::set<std::string> PATCH_KEYS = {
    "name", "schedule", "payload", "delivery", "enabled", "description",
    "deleteAfterRun", "agentId", "sessionKey", "sessionTarget", "wakeMode",
    "failureAlert", "allowUnsafeExternalContent"
};

// ============================================================================
// 🎯 对齐 OpenClaw: readStringParam
// ============================================================================
std::string read_string_param(const json& params, const std::string& key) {
    auto it = params.find(key);
    if (it == params.end() || !it->is_string()) return "";
    std::string s = it->get<std::string>();
    auto start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

// ============================================================================
// 🎯 对齐 OpenClaw: stripExistingContext
// ============================================================================
std::string strip_existing_context(const std::string& text) {
    auto idx = text.find(REMINDER_CONTEXT_MARKER);
    if (idx == std::string::npos) return text;
    std::string result = text.substr(0, idx);
    // trim trailing whitespace
    auto end = result.find_last_not_of(" \t\r\n");
    return end == std::string::npos ? "" : result.substr(0, end + 1);
}

// ============================================================================
// 🎯 对齐 OpenClaw: truncateText
// ============================================================================
std::string truncate_text(const std::string& input, int max_len) {
    if ((int)input.size() <= max_len) return input;
    std::string truncated = input.substr(0, std::max(0, max_len - 3));
    // trim trailing whitespace
    auto end = truncated.find_last_not_of(" \t\r\n");
    if (end != std::string::npos) truncated = truncated.substr(0, end + 1);
    return truncated + "...";
}

// ============================================================================
// 🎯 对齐 OpenClaw: flat-params recovery for add action
// Non-frontier models (e.g. Grok) sometimes flatten job properties to the
// top level alongside `action` instead of nesting them inside `job`.
// See: https://github.com/openclaw/openclaw/issues/11310
// ============================================================================
void recover_flat_job_params(json& params) {
    bool job_missing_or_empty =
        !params.contains("job") ||
        (params["job"].is_object() && params["job"].empty());

    if (!job_missing_or_empty) return;

    json synthetic = json::object();
    bool found = false;
    for (auto it = params.begin(); it != params.end(); ++it) {
        if (JOB_KEYS.count(it.key()) && !it.value().is_null()) {
            synthetic[it.key()] = it.value();
            found = true;
        }
    }
    // Only use synthetic job if at least one meaningful field is present
    // (schedule, payload, message, or text are the minimum signals)
    if (found && (synthetic.contains("schedule") ||
                  synthetic.contains("payload") ||
                  synthetic.contains("message") ||
                  synthetic.contains("text"))) {
        params["job"] = synthetic;
    }
}

// ============================================================================
// 🎯 对齐 OpenClaw: flat-params recovery for update action
// ============================================================================
void recover_flat_patch_params(json& params) {
    bool patch_missing_or_empty =
        !params.contains("patch") ||
        (params["patch"].is_object() && params["patch"].empty());

    if (!patch_missing_or_empty) return;

    json synthetic = json::object();
    bool found = false;
    for (auto it = params.begin(); it != params.end(); ++it) {
        if (PATCH_KEYS.count(it.key()) && !it.value().is_null()) {
            synthetic[it.key()] = it.value();
            found = true;
        }
    }
    if (found) {
        params["patch"] = synthetic;
    }
}

// ============================================================================
// 🎯 对齐 OpenClaw: stripThreadSuffixFromSessionKey
// ============================================================================
std::string strip_thread_suffix(const std::string& session_key) {
    std::string normalized = session_key;
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), ::tolower);
    auto idx = normalized.rfind(":thread:");
    if (idx == std::string::npos || idx == 0) return session_key;
    std::string parent = session_key.substr(0, idx);
    // trim
    auto end = parent.find_last_not_of(" \t\r\n");
    return end == std::string::npos ? session_key : parent.substr(0, end + 1);
}

} // namespace

// ============================================================================
// CronTool — 🎯 对齐 OpenClaw createCronTool
// ============================================================================

class CronTool : public Tool {
public:
    explicit CronTool(std::shared_ptr<cron::GatewayCronIntegration> integration,
                      const std::string& agent_session_key = "")
        : integration_(std::move(integration))
        , agent_session_key_(agent_session_key) {
        if (!integration_) {
            throw std::invalid_argument("GatewayCronIntegration cannot be null");
        }
    }

    std::string name() const override { return "cron"; }

    // 🎯 对齐 OpenClaw: label: "Cron"
    std::string label() const override { return "Cron"; }

    // 🎯 对齐 OpenClaw: ownerOnly: true
    bool owner_only() const override { return true; }

    // 🎯 对齐 OpenClaw: description (verbatim from cron-tool.ts)
    std::string description() const override {
        return
            "Manage Gateway cron jobs (status/list/add/update/remove/run/runs) and send wake events.\n"
            "\n"
            "ACTIONS:\n"
            "- status: Check cron scheduler status\n"
            "- list: List jobs (use includeDisabled:true to include disabled)\n"
            "- add: Create job (requires job object, see schema below)\n"
            "- update: Modify job (requires jobId + patch object)\n"
            "- remove: Delete job (requires jobId)\n"
            "- run: Trigger job immediately (requires jobId)\n"
            "- runs: Get job run history (requires jobId)\n"
            "- wake: Send wake event (requires text, optional mode)\n"
            "\n"
            "JOB SCHEMA (for add action):\n"
            "{\n"
            "  \"name\": \"string (optional)\",\n"
            "  \"schedule\": { ... },      // Required: when to run\n"
            "  \"payload\": { ... },       // Required: what to execute\n"
            "  \"delivery\": { ... },      // Optional: announce summary or webhook POST\n"
            "  \"sessionTarget\": \"main\" | \"isolated\",  // Required\n"
            "  \"enabled\": true | false   // Optional, default true\n"
            "}\n"
            "\n"
            "SCHEDULE TYPES (schedule.kind):\n"
            "- \"at\": One-shot at absolute time\n"
            "  { \"kind\": \"at\", \"at\": \"<ISO-8601 timestamp>\" }\n"
            "- \"every\": Recurring interval\n"
            "  { \"kind\": \"every\", \"everyMs\": <interval-ms>, \"anchorMs\": <optional-start-ms> }\n"
            "- \"cron\": Cron expression\n"
            "  { \"kind\": \"cron\", \"expr\": \"<cron-expression>\", \"tz\": \"<optional-timezone>\" }\n"
            "\n"
            "ISO timestamps without an explicit timezone are treated as UTC.\n"
            "\n"
            "PAYLOAD TYPES (payload.kind):\n"
            "- \"systemEvent\": Injects text as system event into session\n"
            "  { \"kind\": \"systemEvent\", \"text\": \"<message>\" }\n"
            "- \"agentTurn\": Runs agent with message (isolated sessions only)\n"
            "  { \"kind\": \"agentTurn\", \"message\": \"<prompt>\", \"model\": \"<optional>\","
            " \"thinking\": \"<optional>\", \"timeoutSeconds\": <optional, 0 means no timeout> }\n"
            "\n"
            "DELIVERY (top-level):\n"
            "  { \"mode\": \"none|announce|webhook\", \"channel\": \"<optional>\","
            " \"to\": \"<optional>\", \"bestEffort\": <optional-bool> }\n"
            "  - Default for isolated agentTurn jobs (when delivery omitted): \"announce\"\n"
            "  - announce: send to chat channel (optional channel/to target)\n"
            "  - webhook: send finished-run event as HTTP POST to delivery.to (URL required)\n"
            "  - If the task needs to send to a specific chat/recipient, set announce"
            " delivery.channel/to; do not call messaging tools inside the run.\n"
            "\n"
            "CRITICAL CONSTRAINTS:\n"
            "- sessionTarget=\"main\" REQUIRES payload.kind=\"systemEvent\"\n"
            "- sessionTarget=\"isolated\" REQUIRES payload.kind=\"agentTurn\"\n"
            "- For webhook callbacks, use delivery.mode=\"webhook\" with delivery.to set to a URL.\n"
            "Default: prefer isolated agentTurn jobs unless the user explicitly wants a"
            " main-session system event.\n"
            "\n"
            "WAKE MODES (for wake action):\n"
            "- \"next-heartbeat\" (default): Wake on next heartbeat\n"
            "- \"now\": Wake immediately\n"
            "\n"
            "Use jobId as the canonical identifier; id is accepted for compatibility."
            " Use contextMessages (0-10) to add previous messages as context to the job text.";
    }

    // 🎯 对齐 OpenClaw: CronToolSchema
    Tool::Schema parameters() const override {
        Tool::Schema schema;
        schema.type = "object";

        auto make_str = [](const std::string& desc) {
            core::JsonValue p = core::JsonValue::object();
            p["type"] = "string";
            p["description"] = desc;
            return p;
        };
        auto make_bool = [](const std::string& desc) {
            core::JsonValue p = core::JsonValue::object();
            p["type"] = "boolean";
            p["description"] = desc;
            return p;
        };
        auto make_num = [](const std::string& desc) {
            core::JsonValue p = core::JsonValue::object();
            p["type"] = "number";
            p["description"] = desc;
            return p;
        };
        auto make_obj = [](const std::string& desc) {
            core::JsonValue p = core::JsonValue::object();
            p["type"] = "object";
            p["description"] = desc;
            p["additionalProperties"] = true;
            return p;
        };

        // action (required)
        {
            core::JsonValue p = core::JsonValue::object();
            p["type"] = "string";
            p["description"] = "Action to perform";
            core::JsonValue e = core::JsonValue::array();
            for (const auto& a : {"status","list","add","update","remove","run","runs","wake"})
                e.push_back(std::string(a));
            p["enum"] = e;
            schema.properties["action"] = p;
        }

        schema.properties["gatewayUrl"]      = make_str("Gateway URL override");
        schema.properties["gatewayToken"]    = make_str("Gateway token override");
        schema.properties["timeoutMs"]       = make_num("Request timeout in milliseconds");
        schema.properties["includeDisabled"] = make_bool("Include disabled jobs (for list action)");
        schema.properties["job"]             = make_obj("Job configuration (required for add)");
        schema.properties["jobId"]           = make_str("Job ID (required for update/remove/run/runs)");
        schema.properties["id"]              = make_str("Job ID alias (use jobId; id accepted for backward compatibility)");
        schema.properties["patch"]           = make_obj("Job patch (required for update)");
        schema.properties["text"]            = make_str("Wake event text (required for wake)");

        // mode enum
        {
            core::JsonValue p = core::JsonValue::object();
            p["type"] = "string";
            p["description"] = "Wake mode: \"now\" or \"next-heartbeat\" (default: next-heartbeat)";
            core::JsonValue e = core::JsonValue::array();
            e.push_back(std::string("now"));
            e.push_back(std::string("next-heartbeat"));
            p["enum"] = e;
            schema.properties["mode"] = p;
        }

        // runMode enum
        {
            core::JsonValue p = core::JsonValue::object();
            p["type"] = "string";
            p["description"] = "Run mode: \"due\" or \"force\" (default: force)";
            core::JsonValue e = core::JsonValue::array();
            e.push_back(std::string("due"));
            e.push_back(std::string("force"));
            p["enum"] = e;
            schema.properties["runMode"] = p;
        }

        // contextMessages
        {
            core::JsonValue p = core::JsonValue::object();
            p["type"] = "number";
            p["description"] = "Number of recent context messages to include (0-10)";
            p["minimum"] = 0;
            p["maximum"] = REMINDER_CONTEXT_MESSAGES_MAX;
            schema.properties["contextMessages"] = p;
        }

        schema.required = {"action"};
        return schema;
    }

    // 🎯 对齐 OpenClaw: execute
    ToolResult execute(const core::JsonValue& params_val) override {
        try {
            json params = params_val.to_json();
            spdlog::debug("[CronTool] execute: {}", params.dump());

            std::string action = read_string_param(params, "action");
            if (action.empty())
                return make_error("action is required");
            if (!CRON_ACTIONS.count(action))
                return make_error("Unknown action: " + action);

            // 🎯 对齐 OpenClaw: gatewayOpts (timeoutMs defaults to 60000)
            cron::GatewayCallOptions gateway_opts;
            gateway_opts.gateway_url   = read_string_param(params, "gatewayUrl");
            gateway_opts.gateway_token = read_string_param(params, "gatewayToken");
            gateway_opts.timeout_ms    = 60000;
            if (params.contains("timeoutMs") && params["timeoutMs"].is_number()) {
                double t = params["timeoutMs"].get<double>();
                if (std::isfinite(t)) gateway_opts.timeout_ms = (int64_t)t;
            }

            json result;

            // ----------------------------------------------------------------
            // 🎯 case "status"
            // ----------------------------------------------------------------
            if (action == "status") {
                result = integration_->handle_status(gateway_opts, {});

            // ----------------------------------------------------------------
            // 🎯 case "list"
            // ----------------------------------------------------------------
            } else if (action == "list") {
                bool include_disabled = false;
                if (params.contains("includeDisabled") && params["includeDisabled"].is_boolean())
                    include_disabled = params["includeDisabled"].get<bool>();
                result = integration_->handle_list(gateway_opts, {{"includeDisabled", include_disabled}});

            // ----------------------------------------------------------------
            // 🎯 case "add" — with flat-params recovery + agentId/sessionKey injection
            //                 + delivery inference + contextMessages
            // ----------------------------------------------------------------
            } else if (action == "add") {
                recover_flat_job_params(params);

                if (!params.contains("job") || !params["job"].is_object())
                    return make_error("job required");

                json job = params["job"];

                // 🎯 对齐 OpenClaw: normalizeCronJobCreate
                job = cron::normalize_cron_job_create(job);

                // 🎯 对齐 OpenClaw: inject agentId if not present
                if (!job.contains("agentId") && !agent_session_key_.empty()) {
                    auto agent_id = sessions::resolve_session_agent_id(agent_session_key_);
                    if (!agent_id.empty()) job["agentId"] = agent_id;
                }

                // 🎯 对齐 OpenClaw: inject sessionKey if not present
                if (!job.contains("sessionKey") && !agent_session_key_.empty()) {
                    auto resolved = sessions::resolve_internal_session_key(agent_session_key_);
                    if (!resolved.empty()) job["sessionKey"] = resolved;
                }

                // 🎯 对齐 OpenClaw: delivery inference for agentTurn jobs
                if (!agent_session_key_.empty() &&
                    job.contains("payload") && job["payload"].is_object() &&
                    job["payload"].value("kind", "") == "agentTurn") {

                    auto delivery_val = job.contains("delivery") ? job["delivery"] : json(nullptr);
                    bool delivery_is_obj = delivery_val.is_object();
                    std::string mode_raw = delivery_is_obj
                        ? delivery_val.value("mode", "") : "";
                    std::string mode = mode_raw;
                    std::transform(mode.begin(), mode.end(), mode.begin(), ::tolower);

                    // 🎯 对齐 OpenClaw: validate webhook URL
                    if (mode == "webhook") {
                        std::string to = delivery_is_obj ? delivery_val.value("to", "") : "";
                        if (!cron::is_valid_http_webhook_url(to))
                            return make_error(
                                "delivery.mode=\"webhook\" requires delivery.to to be a valid http(s) URL");
                    }

                    // 🎯 对齐 OpenClaw: infer delivery from session key
                    bool has_target = delivery_is_obj &&
                        (!delivery_val.value("channel", std::string{}).empty() ||
                         !delivery_val.value("to", std::string{}).empty());
                    bool should_infer = (delivery_val.is_null() || delivery_is_obj) &&
                        (mode.empty() || mode == "announce") && !has_target;

                    if (should_infer) {
                        auto inferred = cron::infer_delivery_from_session_key(
                            strip_thread_suffix(agent_session_key_));
                        if (inferred.has_value()) {
                            json merged = delivery_is_obj ? delivery_val : json::object();
                            merged["mode"] = inferred->mode;
                            merged["to"]   = inferred->to;
                            if (!inferred->channel.empty()) merged["channel"] = inferred->channel;
                            job["delivery"] = merged;
                        }
                    }
                }

                // 🎯 对齐 OpenClaw: contextMessages for systemEvent jobs
                int context_messages = 0;
                if (params.contains("contextMessages") && params["contextMessages"].is_number()) {
                    double v = params["contextMessages"].get<double>();
                    if (std::isfinite(v)) context_messages = (int)std::floor(v);
                }
                context_messages = std::max(0, std::min(REMINDER_CONTEXT_MESSAGES_MAX, context_messages));

                if (context_messages > 0 &&
                    job.contains("payload") && job["payload"].is_object() &&
                    job["payload"].value("kind", "") == "systemEvent") {
                    std::string text = job["payload"].value("text", "");
                    if (!text.empty()) {
                        auto context_lines = build_reminder_context(
                            gateway_opts, context_messages);
                        if (!context_lines.empty()) {
                            std::string base = strip_existing_context(text);
                            std::string appended = base + REMINDER_CONTEXT_MARKER;
                            for (const auto& line : context_lines)
                                appended += line + "\n";
                            // trim trailing newline
                            if (!appended.empty() && appended.back() == '\n')
                                appended.pop_back();
                            job["payload"]["text"] = appended;
                        }
                    }
                }

                result = integration_->handle_add(gateway_opts, job);

            // ----------------------------------------------------------------
            // 🎯 case "update" — with flat-params recovery
            // ----------------------------------------------------------------
            } else if (action == "update") {
                std::string id = read_string_param(params, "jobId");
                if (id.empty()) id = read_string_param(params, "id");
                if (id.empty())
                    return make_error("jobId required (id accepted for backward compatibility)");

                recover_flat_patch_params(params);
                if (!params.contains("patch") || !params["patch"].is_object())
                    return make_error("patch required");

                json patch = cron::normalize_cron_job_patch(params["patch"]);
                result = integration_->handle_update(gateway_opts, {{"id", id}, {"patch", patch}});

            // ----------------------------------------------------------------
            // 🎯 case "remove"
            // ----------------------------------------------------------------
            } else if (action == "remove") {
                std::string id = read_string_param(params, "jobId");
                if (id.empty()) id = read_string_param(params, "id");
                if (id.empty())
                    return make_error("jobId required (id accepted for backward compatibility)");
                result = integration_->handle_remove(gateway_opts, {{"id", id}});

            // ----------------------------------------------------------------
            // 🎯 case "run" — runMode defaults to "force"
            // ----------------------------------------------------------------
            } else if (action == "run") {
                std::string id = read_string_param(params, "jobId");
                if (id.empty()) id = read_string_param(params, "id");
                if (id.empty())
                    return make_error("jobId required (id accepted for backward compatibility)");
                std::string run_mode = read_string_param(params, "runMode");
                if (run_mode != "due" && run_mode != "force") run_mode = "force";
                result = integration_->handle_run(gateway_opts, {{"id", id}, {"mode", run_mode}});

            // ----------------------------------------------------------------
            // 🎯 case "runs"
            // ----------------------------------------------------------------
            } else if (action == "runs") {
                std::string id = read_string_param(params, "jobId");
                if (id.empty()) id = read_string_param(params, "id");
                if (id.empty())
                    return make_error("jobId required (id accepted for backward compatibility)");
                result = integration_->handle_runs(gateway_opts, {{"id", id}});

            // ----------------------------------------------------------------
            // 🎯 case "wake" — mode defaults to "next-heartbeat"
            // ----------------------------------------------------------------
            } else if (action == "wake") {
                std::string text = read_string_param(params, "text");
                if (text.empty())
                    return make_error("text required for wake action");
                std::string mode = read_string_param(params, "mode");
                if (mode != "now" && mode != "next-heartbeat") mode = "next-heartbeat";
                result = integration_->handle_wake(gateway_opts, {{"mode", mode}, {"text", text}});
            }

            // Check for error in result
            if (result.contains("error")) {
                std::string err_msg = result["error"].is_string()
                    ? result["error"].get<std::string>()
                    : result["error"].dump();
                spdlog::error("[CronTool] error: {}", err_msg);
                return make_error(err_msg);
            }

            ToolResult tool_result;
            tool_result.success = true;
            tool_result.details = core::JsonValue(result);
            ToolResultContent content;
            content.type = ToolResultContent::Type::Json;
            content.text = result.dump();
            tool_result.content.push_back(content);
            return tool_result;

        } catch (const std::exception& e) {
            spdlog::error("[CronTool] exception: {}", e.what());
            return make_error(std::string(e.what()));
        }
    }

    // ownerOnly = true → no user approval needed
    bool requires_approval() const override { return false; }

private:
    std::shared_ptr<cron::GatewayCronIntegration> integration_;
    std::string agent_session_key_;

    ToolResult make_error(const std::string& msg) const {
        ToolResult result;
        result.success = false;
        result.error = msg;
        return result;
    }

    // 🎯 对齐 OpenClaw: buildReminderContextLines
    std::vector<std::string> build_reminder_context(
        const cron::GatewayCallOptions& gateway_opts,
        int max_messages
    ) const {
        if (max_messages <= 0 || agent_session_key_.empty()) return {};
        try {
            auto resolved = sessions::resolve_internal_session_key(agent_session_key_);
            if (resolved.empty()) return {};

            auto res = integration_->fetch_chat_history(gateway_opts, resolved, max_messages);
            if (!res.contains("messages") || !res["messages"].is_array()) return {};

            std::vector<std::string> lines;
            int total = 0;
            for (const auto& msg : res["messages"]) {
                std::string role = msg.value("role", "");
                if (role != "user" && role != "assistant") continue;
                std::string text = cron::extract_text_from_chat_content(msg.value("content", json{}));
                if (text.empty()) continue;
                std::string label = (role == "user") ? "User" : "Assistant";
                std::string line = "- " + label + ": " + truncate_text(text, REMINDER_CONTEXT_PER_MESSAGE_MAX);
                total += (int)line.size();
                if (total > REMINDER_CONTEXT_TOTAL_MAX) break;
                lines.push_back(line);
            }
            return lines;
        } catch (...) {
            return {};
        }
    }
};

// ============================================================================
// Factory Function
// ============================================================================

std::shared_ptr<Tool> create_cron_tool(
    std::shared_ptr<cron::GatewayCronIntegration> integration,
    const std::string& agent_session_key
) {
    return std::make_shared<CronTool>(std::move(integration), agent_session_key);
}

} // namespace tools
} // namespace hunterclaw
