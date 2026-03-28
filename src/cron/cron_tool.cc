// 🎯 严格对齐 OpenClaw cron-tool.ts
// Reference: openclaw-analysis/src/agents/tools/cron-tool.ts
#include "hunterclaw/cron/cron_tool.h"
#include "hunterclaw/tools/tool.h"
#include "hunterclaw/cron/gateway_integration.h"
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <set>
#include <string>
#include <algorithm>

namespace hunterclaw {
namespace cron {

using json = nlohmann::json;

namespace {
    auto log() {
        static auto logger = spdlog::get("cron_tool");
        if (!logger) {
            logger = spdlog::stdout_logger_mt("cron_tool");
        }
        return logger;
    }

    // 🎯 对齐 OpenClaw: CRON_ACTIONS
    const std::set<std::string> CRON_ACTIONS = {
        "status", "list", "add", "update", "remove", "run", "runs", "wake"
    };

    // 🎯 对齐 OpenClaw: JOB_KEYS (for flat-params recovery in add)
    const std::set<std::string> JOB_KEYS = {
        "name", "schedule", "sessionTarget", "wakeMode", "payload", "delivery",
        "enabled", "description", "deleteAfterRun", "agentId", "sessionKey",
        "message", "text", "model", "thinking", "timeoutSeconds",
        "allowUnsafeExternalContent"
    };

    // 🎯 对齐 OpenClaw: PATCH_KEYS (for flat-params recovery in update)
    const std::set<std::string> PATCH_KEYS = {
        "name", "schedule", "payload", "delivery", "enabled", "description",
        "deleteAfterRun", "agentId", "sessionKey", "sessionTarget", "wakeMode",
        "failureAlert", "allowUnsafeExternalContent"
    };

    // 🎯 对齐 OpenClaw: readStringParam(params, key)
    std::string read_string_param(const json& params, const std::string& key) {
        auto it = params.find(key);
        if (it == params.end() || !it->is_string()) return "";
        std::string s = it->get<std::string>();
        auto start = s.find_first_not_of(" \t\r\n");
        if (start == std::string::npos) return "";
        auto end = s.find_last_not_of(" \t\r\n");
        return s.substr(start, end - start + 1);
    }

    // 🎯 对齐 OpenClaw: flat-params recovery for add action
    // Non-frontier models (e.g. Grok) sometimes flatten job properties to the
    // top level alongside `action` instead of nesting them inside `job`.
    // See: https://github.com/openclaw/openclaw/issues/11310
    json recover_flat_job_params(const json& params) {
        json result = params;

        bool job_missing_or_empty =
            !result.contains("job") ||
            (result["job"].is_object() && result["job"].empty());

        if (job_missing_or_empty) {
            json synthetic = json::object();
            bool found = false;
            for (auto it = result.begin(); it != result.end(); ++it) {
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
                result["job"] = synthetic;
            }
        }
        return result;
    }

    // 🎯 对齐 OpenClaw: flat-params recovery for update action
    json recover_flat_patch_params(const json& params) {
        json result = params;

        bool patch_missing_or_empty =
            !result.contains("patch") ||
            (result["patch"].is_object() && result["patch"].empty());

        if (patch_missing_or_empty) {
            json synthetic = json::object();
            bool found = false;
            for (auto it = result.begin(); it != result.end(); ++it) {
                if (PATCH_KEYS.count(it.key()) && !it.value().is_null()) {
                    synthetic[it.key()] = it.value();
                    found = true;
                }
            }
            if (found) {
                result["patch"] = synthetic;
            }
        }
        return result;
    }

} // namespace

// ============================================================================
// CronTool (对齐 OpenClaw createCronTool)
// ============================================================================

class CronTool : public tools::Tool {
public:
    explicit CronTool(std::shared_ptr<GatewayCronIntegration> integration,
                      const std::string& agent_session_key = "")
        : integration_(std::move(integration))
        , agent_session_key_(agent_session_key) {
        if (!integration_) {
            throw std::invalid_argument("GatewayCronIntegration cannot be null");
        }
    }

    std::string name() const override {
        return "cron";
    }

    // 🎯 对齐 OpenClaw: description (完整 JOB SCHEMA 文档)
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
            "  - If the task needs to send to a specific chat/recipient, set announce delivery.channel/to;"
            " do not call messaging tools inside the run.\n"
            "\n"
            "CRITICAL CONSTRAINTS:\n"
            "- sessionTarget=\"main\" REQUIRES payload.kind=\"systemEvent\"\n"
            "- sessionTarget=\"isolated\" REQUIRES payload.kind=\"agentTurn\"\n"
            "- For webhook callbacks, use delivery.mode=\"webhook\" with delivery.to set to a URL.\n"
            "Default: prefer isolated agentTurn jobs unless the user explicitly wants a main-session"
            " system event.\n"
            "\n"
            "WAKE MODES (for wake action):\n"
            "- \"next-heartbeat\" (default): Wake on next heartbeat\n"
            "- \"now\": Wake immediately\n"
            "\n"
            "Use jobId as the canonical identifier; id is accepted for compatibility."
            " Use contextMessages (0-10) to add previous messages as context to the job text.";
    }

    // 🎯 对齐 OpenClaw: ownerOnly: true
    bool owner_only() const override { return true; }

    // 🎯 对齐 OpenClaw: CronToolSchema
    tools::Tool::Schema parameters() const override {
        tools::Tool::Schema schema;
        schema.type = "object";

        // action (required)
        auto action_prop = core::JsonValue::object();
        action_prop["type"] = "string";
        action_prop["description"] = "Action to perform";
        auto enum_arr = core::JsonValue::array();
        for (const auto& a : {"status","list","add","update","remove","run","runs","wake"}) {
            enum_arr.push_back(std::string(a));
        }
        action_prop["enum"] = enum_arr;
        schema.properties["action"] = action_prop;

        // gatewayUrl (optional)
        auto gw_url_prop = core::JsonValue::object();
        gw_url_prop["type"] = "string";
        gw_url_prop["description"] = "Gateway URL override";
        schema.properties["gatewayUrl"] = gw_url_prop;

        // gatewayToken (optional)
        auto gw_token_prop = core::JsonValue::object();
        gw_token_prop["type"] = "string";
        gw_token_prop["description"] = "Gateway token override";
        schema.properties["gatewayToken"] = gw_token_prop;

        // timeoutMs (optional)
        auto timeout_prop = core::JsonValue::object();
        timeout_prop["type"] = "number";
        timeout_prop["description"] = "Request timeout in milliseconds";
        schema.properties["timeoutMs"] = timeout_prop;

        // includeDisabled (optional, for list)
        auto incl_disabled_prop = core::JsonValue::object();
        incl_disabled_prop["type"] = "boolean";
        incl_disabled_prop["description"] = "Include disabled jobs in list (for list action)";
        schema.properties["includeDisabled"] = incl_disabled_prop;

        // job (optional, required for add)
        auto job_prop = core::JsonValue::object();
        job_prop["type"] = "object";
        job_prop["description"] = "Job configuration (required for add action)";
        schema.properties["job"] = job_prop;

        // jobId (optional, required for update/remove/run/runs)
        auto job_id_prop = core::JsonValue::object();
        job_id_prop["type"] = "string";
        job_id_prop["description"] = "Job ID (required for update, remove, run, runs actions)";
        schema.properties["jobId"] = job_id_prop;

        // id (optional, backward compat alias for jobId)
        auto id_prop = core::JsonValue::object();
        id_prop["type"] = "string";
        id_prop["description"] = "Job ID alias (use jobId; id accepted for backward compatibility)";
        schema.properties["id"] = id_prop;

        // patch (optional, required for update)
        auto patch_prop = core::JsonValue::object();
        patch_prop["type"] = "object";
        patch_prop["description"] = "Job patch (required for update action)";
        schema.properties["patch"] = patch_prop;

        // text (optional, required for wake)
        auto text_prop = core::JsonValue::object();
        text_prop["type"] = "string";
        text_prop["description"] = "Wake event text (required for wake action)";
        schema.properties["text"] = text_prop;

        // mode (optional, for wake: "now" | "next-heartbeat")
        auto mode_prop = core::JsonValue::object();
        mode_prop["type"] = "string";
        mode_prop["description"] = "Wake mode: \"now\" or \"next-heartbeat\" (default: next-heartbeat)";
        auto mode_enum = core::JsonValue::array();
        mode_enum.push_back(std::string("now"));
        mode_enum.push_back(std::string("next-heartbeat"));
        mode_prop["enum"] = mode_enum;
        schema.properties["mode"] = mode_prop;

        // runMode (optional, for run: "due" | "force")
        auto run_mode_prop = core::JsonValue::object();
        run_mode_prop["type"] = "string";
        run_mode_prop["description"] = "Run mode: \"due\" or \"force\" (default: force)";
        auto run_mode_enum = core::JsonValue::array();
        run_mode_enum.push_back(std::string("due"));
        run_mode_enum.push_back(std::string("force"));
        run_mode_prop["enum"] = run_mode_enum;
        schema.properties["runMode"] = run_mode_prop;

        // contextMessages (optional, 0-10)
        auto ctx_msg_prop = core::JsonValue::object();
        ctx_msg_prop["type"] = "number";
        ctx_msg_prop["description"] = "Number of recent context messages to include (0-10)";
        ctx_msg_prop["minimum"] = 0;
        ctx_msg_prop["maximum"] = 10;
        schema.properties["contextMessages"] = ctx_msg_prop;

        schema.required = {"action"};
        return schema;
    }

    // 🎯 对齐 OpenClaw: execute
    tools::ToolResult execute(const core::JsonValue& params) override {
        try {
            json args = params.to_json();
            log()->debug("CronTool execute: {}", args.dump());

            std::string action = read_string_param(args, "action");
            if (action.empty()) {
                return make_error("action is required");
            }
            if (!CRON_ACTIONS.count(action)) {
                return make_error("Unknown action: " + action);
            }

            json result;

            if (action == "status") {
                // 🎯 对齐 OpenClaw: case "status"
                result = integration_->handle_status({});

            } else if (action == "list") {
                // 🎯 对齐 OpenClaw: case "list"
                bool include_disabled = false;
                if (args.contains("includeDisabled") && args["includeDisabled"].is_boolean()) {
                    include_disabled = args["includeDisabled"].get<bool>();
                }
                result = integration_->handle_list({{"includeDisabled", include_disabled}});

            } else if (action == "add") {
                // 🎯 对齐 OpenClaw: case "add" with flat-params recovery
                json recovered = recover_flat_job_params(args);

                if (!recovered.contains("job") || !recovered["job"].is_object()) {
                    return make_error("job required");
                }
                json job = recovered["job"];

                // 🎯 对齐 OpenClaw: inject agentId and sessionKey if not present
                if (!job.contains("agentId") && !agent_session_key_.empty()) {
                    // agentId is derived from session key in OpenClaw via resolveSessionAgentId
                    // In HunterClaw we pass it through integration context
                }
                if (!job.contains("sessionKey") && !agent_session_key_.empty()) {
                    job["sessionKey"] = agent_session_key_;
                }

                result = integration_->handle_add(job);

            } else if (action == "update") {
                // 🎯 对齐 OpenClaw: case "update" with flat-params recovery
                // jobId is canonical; id is accepted for backward compatibility
                std::string id = read_string_param(args, "jobId");
                if (id.empty()) id = read_string_param(args, "id");
                if (id.empty()) {
                    return make_error("jobId required (id accepted for backward compatibility)");
                }

                json recovered = recover_flat_patch_params(args);
                if (!recovered.contains("patch") || !recovered["patch"].is_object()) {
                    return make_error("patch required");
                }
                json patch = recovered["patch"];

                result = integration_->handle_update({{"id", id}, {"patch", patch}});

            } else if (action == "remove") {
                // 🎯 对齐 OpenClaw: case "remove"
                std::string id = read_string_param(args, "jobId");
                if (id.empty()) id = read_string_param(args, "id");
                if (id.empty()) {
                    return make_error("jobId required (id accepted for backward compatibility)");
                }
                result = integration_->handle_remove({{"id", id}});

            } else if (action == "run") {
                // 🎯 对齐 OpenClaw: case "run"
                std::string id = read_string_param(args, "jobId");
                if (id.empty()) id = read_string_param(args, "id");
                if (id.empty()) {
                    return make_error("jobId required (id accepted for backward compatibility)");
                }
                // 🎯 对齐 OpenClaw: runMode defaults to "force"
                std::string run_mode = read_string_param(args, "runMode");
                if (run_mode != "due" && run_mode != "force") run_mode = "force";
                result = integration_->handle_run({{"id", id}, {"mode", run_mode}});

            } else if (action == "runs") {
                // 🎯 对齐 OpenClaw: case "runs"
                std::string id = read_string_param(args, "jobId");
                if (id.empty()) id = read_string_param(args, "id");
                if (id.empty()) {
                    return make_error("jobId required (id accepted for backward compatibility)");
                }
                result = integration_->handle_runs({{"id", id}});

            } else if (action == "wake") {
                // 🎯 对齐 OpenClaw: case "wake"
                std::string text = read_string_param(args, "text");
                if (text.empty()) {
                    return make_error("text required for wake action");
                }
                // 🎯 对齐 OpenClaw: mode defaults to "next-heartbeat"
                std::string mode = read_string_param(args, "mode");
                if (mode != "now" && mode != "next-heartbeat") mode = "next-heartbeat";
                result = integration_->handle_wake({{"mode", mode}, {"text", text}});
            }

            // Check for error in result
            if (result.contains("error")) {
                std::string err_msg = result["error"].is_string()
                    ? result["error"].get<std::string>()
                    : result["error"].dump();
                log()->error("CronTool error: {}", err_msg);
                return make_error(err_msg);
            }

            // Success
            tools::ToolResult tool_result;
            tool_result.success = true;
            tool_result.details = core::JsonValue(result);

            tools::ToolResultContent content;
            content.type = tools::ToolResultContent::Type::Json;
            content.text = result.dump();
            tool_result.content.push_back(content);

            return tool_result;

        } catch (const std::exception& e) {
            log()->error("CronTool exception: {}", e.what());
            return make_error(std::string(e.what()));
        }
    }

    // 🎯 对齐 OpenClaw: ownerOnly = true → no user approval needed
    bool requires_approval() const override { return false; }

private:
    std::shared_ptr<GatewayCronIntegration> integration_;
    std::string agent_session_key_;

    tools::ToolResult make_error(const std::string& msg) const {
        tools::ToolResult result;
        result.success = false;
        result.error = msg;
        return result;
    }
};

// ============================================================================
// Factory Function
// ============================================================================

std::shared_ptr<tools::Tool> create_cron_tool(
    std::shared_ptr<GatewayCronIntegration> integration
) {
    return std::make_shared<CronTool>(std::move(integration));
}

} // namespace cron
} // namespace hunterclaw
