#include "hunterclaw/tools/tool.h"
#include "hunterclaw/agents/subagent.h"
#include "hunterclaw/agents/types.h"
#include "hunterclaw/core/logging.h"
#include <sstream>
#include <nlohmann/json.hpp>

namespace hunterclaw {
namespace tools {

using json = nlohmann::json;

// ============================================================================
// sessions_spawn Tool (OpenClaw 对齐)
// 参考: openclaw-analysis/src/agents/tools/sessions-spawn-tool.ts
// ============================================================================

/**
 * sessions_spawn Tool
 * 
 * 功能: 生成隔离的子 Agent 会话
 * 
 * OpenClaw 对齐:
 * - src/agents/tools/sessions-spawn-tool.ts:68-212
 * - src/agents/subagent-spawn.ts (spawnSubagentDirect)
 * 
 * 参数:
 * - task: 任务描述 (必需)
 * - label: 简短标签 (可选)
 * - runtime: "subagent" | "acp" (默认 "subagent")
 * - agentId: Agent ID 覆盖 (可选)
 * - model: 模型覆盖 (可选)
 * - thinking: 思考级别覆盖 (可选)
 * - runTimeoutSeconds: 超时秒数 (可选)
 * - thread: 是否线程绑定 (可选)
 * - mode: "run" | "session" (可选)
 * - cleanup: "keep" | "delete" (默认 "keep")
 * - sandbox: "inherit" | "require" (默认 "inherit")
 */
class SessionsSpawnTool : public Tool {
public:
    SessionsSpawnTool(
        std::shared_ptr<agents::SubagentManager> subagent_mgr,
        const ToolContext& ctx
    ) : subagent_mgr_(subagent_mgr), context_(ctx) {}
    
    std::string name() const override {
        return "sessions_spawn";
    }
    
    std::string description() const override {
        return "Spawn an isolated session (runtime=\"subagent\" or runtime=\"acp\"). "
               "mode=\"run\" is one-shot and mode=\"session\" is persistent/thread-bound. "
               "Subagents inherit the parent workspace directory automatically.";
    }
    
    core::JsonValue input_schema() const override {
        return {
            {"type", "object"},
            {"properties", {
                {"task", {
                    {"type", "string"},
                    {"description", "Task description for the spawned session"}
                }},
                {"label", {
                    {"type", "string"},
                    {"description", "Short label (3-5 words) for the task"}
                }},
                {"runtime", {
                    {"type", "string"},
                    {"enum", json::array({"subagent", "acp"})},
                    {"description", "Runtime type (default: subagent)"}
                }},
                {"agentId", {
                    {"type", "string"},
                    {"description", "Override agent ID"}
                }},
                {"model", {
                    {"type", "string"},
                    {"description", "Override model"}
                }},
                {"thinking", {
                    {"type", "string"},
                    {"description", "Override thinking level"}
                }},
                {"runTimeoutSeconds", {
                    {"type", "number"},
                    {"minimum", 0},
                    {"description", "Timeout in seconds"}
                }},
                {"thread", {
                    {"type", "boolean"},
                    {"description", "Thread-bound session"}
                }},
                {"mode", {
                    {"type", "string"},
                    {"enum", json::array({"run", "session"})},
                    {"description", "Execution mode"}
                }},
                {"cleanup", {
                    {"type", "string"},
                    {"enum", json::array({"keep", "delete"})},
                    {"description", "Cleanup policy (default: keep)"}
                }},
                {"sandbox", {
                    {"type", "string"},
                    {"enum", json::array({"inherit", "require"})},
                    {"description", "Sandbox mode (default: inherit)"}
                }}
            }},
            {"required", json::array({"task"})}
        };
    }
    
    core::Result<ToolResult> execute(
        const std::string& /* tool_call_id */,
        const core::JsonValue& args
    ) override {
        auto log = core::get_logger("sessions_spawn");
        
        // ✅ 1. 解析参数
        if (!args.contains("task") || !args["task"].is_string()) {
            return core::Result<ToolResult>::Err("Missing required parameter: task");
        }
        
        std::string task = args["task"];
        std::string label = args.value("label", "");
        std::string runtime = args.value("runtime", "subagent");
        
        if (runtime != "subagent" && runtime != "acp") {
            return core::Result<ToolResult>::Err("Invalid runtime: " + runtime);
        }
        
        // ✅ 2. 构造 SubagentSpawnOptions
        agents::SubagentSpawnOptions options;
        options.subagent_name = args.value("agentId", "default");
        options.description = label.empty() ? "Subagent task" : label;
        options.prompt = task;
        
        if (args.contains("model") && args["model"].is_string()) {
            options.model = args["model"];
        }
        
        if (args.contains("thinking") && args["thinking"].is_string()) {
            options.thinking_level = args["thinking"];
        }
        
        if (args.contains("runTimeoutSeconds") && args["runTimeoutSeconds"].is_number()) {
            options.timeout_seconds = args["runTimeoutSeconds"];
        }
        
        // mode: "run" → 不等待, "session" → 等待
        std::string mode = args.value("mode", "run");
        options.wait_for_completion = (mode == "session");
        
        log->info("[sessions_spawn] Spawning subagent: task='{}', runtime={}, mode={}",
                 task.substr(0, 50), runtime, mode);
        
        // ✅ 3. 调用 SubagentManager::spawn
        if (!subagent_mgr_) {
            return core::Result<ToolResult>::Err("SubagentManager not available");
        }
        
        // TODO: 需要 context 中的 requester_run_id 和 requester_agent_id
        std::string requester_run_id = "TODO_GET_FROM_CONTEXT";
        std::string requester_agent_id = "TODO_GET_FROM_CONTEXT";
        
        auto spawn_result = subagent_mgr_->spawn(
            requester_run_id,
            requester_agent_id,
            options
        );
        
        if (!spawn_result.is_ok()) {
            log->error("[sessions_spawn] Spawn failed: {}", spawn_result.error());
            
            ToolResult result;
            result.content = json{
                {"status", "error"},
                {"error", spawn_result.error()}
            }.dump();
            result.is_error = true;
            return core::Result<ToolResult>::ok(result);
        }
        
        std::string subagent_run_id = spawn_result.unwrap();
        
        log->info("[sessions_spawn] Spawned subagent: run_id={}", subagent_run_id);
        
        // ✅ 4. 返回结果 (OpenClaw 格式)
        ToolResult result;
        result.content = json{
            {"status", "spawned"},
            {"sessionId", subagent_run_id},
            {"label", label},
            {"runtime", runtime},
            {"mode", mode}
        }.dump();
        
        return core::Result<ToolResult>::ok(result);
    }
    
private:
    std::shared_ptr<agents::SubagentManager> subagent_mgr_;
    ToolContext context_;
};

// ============================================================================
// 注册函数
// ============================================================================

void register_subagent_tools(
    ToolManager& manager,
    std::shared_ptr<agents::SubagentManager> subagent_mgr,
    std::shared_ptr<agents::AgentManager> /* agent_mgr */
) {
    if (!subagent_mgr) {
        auto log = core::get_logger("subagent_tools");
        log->warn("SubagentManager is null, skipping subagent tools registration");
        return;
    }
    
    // ✅ 注册 sessions_spawn 工具
    ToolContext ctx;  // TODO: 从外部传入完整 context
    
    auto sessions_spawn_tool = std::make_shared<SessionsSpawnTool>(subagent_mgr, ctx);
    manager.register_tool(sessions_spawn_tool);
    
    auto log = core::get_logger("subagent_tools");
    log->info("Registered subagent tools: sessions_spawn");
}

} // namespace tools
} // namespace hunterclaw
