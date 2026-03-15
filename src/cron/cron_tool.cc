#include "hunterclaw/tools/tool.h"
#include "hunterclaw/cron/gateway_integration.h"
#include <spdlog/spdlog.h>

namespace hunterclaw {
namespace cron {

namespace {
    auto log() {
        static auto logger = spdlog::get("cron_tool");
        if (!logger) {
            logger = spdlog::stdout_logger_mt("cron_tool");
        }
        return logger;
    }
}

// ============================================================================
// Cron Agent Tool (对齐 OpenClaw cron-tool.ts)
// ============================================================================

class CronTool : public tools::Tool {
public:
    explicit CronTool(std::shared_ptr<GatewayCronIntegration> integration)
        : integration_(std::move(integration)) {
        if (!integration_) {
            throw std::invalid_argument("GatewayCronIntegration cannot be null");
        }
    }
    
    std::string name() const override { 
        return "cron"; 
    }
    
    std::string description() const override {
        return "Manage cron jobs (scheduled tasks).\n"
               "Supports:\n"
               "- list: List all cron jobs\n"
               "- add: Create a new cron job\n"
               "- update: Modify an existing cron job\n"
               "- remove: Delete a cron job\n"
               "- run: Manually trigger a cron job\n"
               "- status: Get cron service status\n"
               "\n"
               "Example usage:\n"
               "1. List jobs: {\"action\": \"list\"}\n"
               "2. Add job: {\"action\": \"add\", \"job\": {...}}\n"
               "3. Update: {\"action\": \"update\", \"id\": \"job-123\", \"patch\": {\"enabled\": false}}\n"
               "4. Remove: {\"action\": \"remove\", \"id\": \"job-123\"}\n"
               "5. Run now: {\"action\": \"run\", \"id\": \"job-123\"}";
    }
    
    Tool::Schema parameters() const override {
        Tool::Schema schema;
        schema.type = "object";
        schema.description = "Cron management parameters";
        
        // action (required)
        auto action_prop = core::JsonValue::object();
        action_prop["type"] = "string";
        action_prop["description"] = "Action to perform";
        action_prop["enum"] = core::JsonValue::array({
            "status", "list", "add", "update", "remove", "run"
        });
        schema.properties["action"] = action_prop;
        
        // id (optional, required for update/remove/run)
        auto id_prop = core::JsonValue::object();
        id_prop["type"] = "string";
        id_prop["description"] = "Job ID (required for update, remove, run actions)";
        schema.properties["id"] = id_prop;
        
        // job (optional, required for add)
        auto job_prop = core::JsonValue::object();
        job_prop["type"] = "object";
        job_prop["description"] = "Job configuration (required for add action)";
        schema.properties["job"] = job_prop;
        
        // patch (optional, required for update)
        auto patch_prop = core::JsonValue::object();
        patch_prop["type"] = "object";
        patch_prop["description"] = "Job patch (required for update action)";
        schema.properties["patch"] = patch_prop;
        
        // force (optional, for run)
        auto force_prop = core::JsonValue::object();
        force_prop["type"] = "boolean";
        force_prop["description"] = "Force run even if disabled (for run action)";
        schema.properties["force"] = force_prop;
        
        schema.required = {"action"};
        
        return schema;
    }
    
    tools::ToolResult execute(const core::JsonValue& params) override {
        try {
            // Convert JsonValue to nlohmann::json
            nlohmann::json args = convert_to_json(params);
            
            log()->debug("CronTool execute: {}", args.dump());
            
            // Execute through integration
            auto result = integration_->execute_agent_tool(args);
            
            // Check for errors
            if (result.contains("error")) {
                std::string error = result["error"].get<std::string>();
                log()->error("CronTool error: {}", error);
                
                tools::ToolResult tool_result;
                tool_result.ok = false;
                tool_result.error = error;
                return tool_result;
            }
            
            // Success
            tools::ToolResult tool_result;
            tool_result.success = true;
            tool_result.details = convert_from_json(result);
            
            return tool_result;
            
        } catch (const std::exception& e) {
            log()->error("CronTool exception: {}", e.what());
            
            tools::ToolResult tool_result;
            tool_result.success = false;
            tool_result.error = std::string("Cron tool failed: ") + e.what();
            return tool_result;
        }
    }
    
    bool requires_approval() const override { 
        return false;  // Cron management typically doesn't require user approval
    }
    
private:
    std::shared_ptr<GatewayCronIntegration> integration_;
    
    // Helper: Convert core::JsonValue to nlohmann::json
    // Helper: Convert core::JsonValue to nlohmann::json
    nlohmann::json convert_to_json(const core::JsonValue& value) {
        return value.to_json();
    }
    
    // Helper: Convert nlohmann::json to core::JsonValue
    core::JsonValue convert_from_json(const nlohmann::json& j) {
        // JsonValue has a constructor that accepts nlohmann::json
        return core::JsonValue(j);
    }
};

// ============================================================================
// Factory Function
// ============================================================================

std::shared_ptr<tools::Tool> create_cron_tool(
    std::shared_ptr<GatewayCronIntegration> integration
) {
    return std::make_shared<CronTool>(integration);
}

} // namespace cron
} // namespace hunterclaw
