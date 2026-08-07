#include "hunterclaw/memory/active_memory_hooks.h"
#include "hunterclaw/memory/active_memory_lane.h"
#include "hunterclaw/memory/memory_index_manager.h"
#include "hunterclaw/tools/tool_hooks.h"

// 🎯 对齐 OpenClaw extensions/active-memory/index.ts 的 register()：
//   将 lane-1 编排接线到 before_prompt_build / agent_end hook。

namespace hunterclaw {
namespace memory {

void register_active_memory_hooks(
    tools::ToolHookManager& hook_manager,
    const ActiveMemoryHookConfig& config) {

    // ------------------------------------------------------------------
    // before_prompt_build: 运行 lane-1 召回，命中强trigger 时注入 prepend_context。
    //   对齐 OC api.on("before_prompt_build", ...)。
    // ------------------------------------------------------------------
    hook_manager.register_before_prompt_build(
        config.plugin_id,
        [config](const tools::PluginHookBeforePromptBuildEvent& event,
                 const tools::PluginHookAgentContext& ctx)
            -> std::optional<tools::PluginHookBeforePromptBuildResult> {
            // 必要上下文缺失 →跳过（对齐 OC 对缺失 ctx 的安全降级）。
            if (!ctx.workspace_dir.has_value() || ctx.workspace_dir->empty()) {
                return std::nullopt;
            }
            const std::string workspace_dir = *ctx.workspace_dir;
            const std::string agent_id = ctx.agent_id.value_or("");

            // 1. 解析 memory 配置（对齐 resolveMemoryBackendConfig）。未配置 → 跳过。
            if (!config.resolve_settings) {
                return std::nullopt;
            }
            std::optional<ResolvedMemorySearchConfig> settings =
                config.resolve_settings(workspace_dir);
            if (!settings.has_value()) {
                return std::nullopt;
            }

            // 2. 获取 MemoryIndexManager（带缓存）。nullptr → 未配置，跳过。
            MemoryIndexManager::Params mp;
            mp.agent_id = agent_id;
            mp.workspace_dir = workspace_dir;
            mp.settings = *settings;
            mp.purpose = "default";
            auto mgr_res = MemoryIndexManager::get(mp);
            if (!mgr_res.is_ok()) {
                return std::nullopt;
            }
            std::shared_ptr<MemoryIndexManager> manager = mgr_res.get();
            if (!manager) {
                return std::nullopt;
            }

            // 3. 组装 lane-1 参数。
            ActiveMemoryLaneOneParams params;
            params.message = event.prompt;
            params.search_query = event.prompt;  // 对齐 OC buildSearchQuery 的简化：直接用消息
            params.agent_id = agent_id;
            if (config.resolve_active_project_keys) {
                params.active_project_keys =
                    config.resolve_active_project_keys(agent_id, workspace_dir);
            }
            params.run_id = ctx.session_id;  // session_id 作 run 级 prewarm 键（同一 run 稳定）
            // provider/model：优先 config 回调，回退 settings。
            params.provider = config.resolve_provider ? config.resolve_provider()
                                                      : std::optional<std::string>(settings->provider);
            params.model = config.resolve_model ? config.resolve_model() : settings->model;
            params.mode = config.mode;
            params.circuit_breaker_max_timeouts = config.circuit_breaker_max_timeouts;
            params.circuit_breaker_cooldown_ms = config.circuit_breaker_cooldown_ms;
            // cfg_token：workspace + provider + model（同一 run 内稳定，对齐 OC cfg 引用相等）。
            params.cfg_token = workspace_dir + "|" + params.provider.value_or("") + "|" +
                               params.model.value_or("");

            // 4. 运行 lane-1。
            ActiveMemoryLaneOneResult lane = run_active_memory_lane_one(manager.get(), params);

            // 5. 无注入 → 不修改 prompt。
            if (!lane.prepend_context.has_value() || lane.prepend_context->empty()) {
                return std::nullopt;
            }
            tools::PluginHookBeforePromptBuildResult result;
            result.prepend_context = *lane.prepend_context;
            return result;
        });

    // ------------------------------------------------------------------
    // agent_end:清理该 run 的 prewarm 缓存（对齐 forgetTriggerRecallPrewarm）。
    // ------------------------------------------------------------------
    hook_manager.register_agent_end(
        config.plugin_id,
        [](const tools::PluginHookAgentEndEvent& /*event*/,
           const tools::PluginHookAgentContext& ctx) {
            active_memory_forget_run(ctx.session_id);
        });
}

void register_active_memory_hooks_global(ActiveMemoryHookConfig config) {
    // 默认 resolve_settings：返回默认 ResolvedMemorySearchConfig（builtin 本地 memory）。
    //   对齐 OC "memory 已配置即启用" —— builtin 本地后端始终可用。
    if (!config.resolve_settings) {
        config.resolve_settings =
            [](const std::string& /*workspace_dir*/)
            -> std::optional<ResolvedMemorySearchConfig> {
            return ResolvedMemorySearchConfig{};
        };
    }
    register_active_memory_hooks(tools::require_global_hook_manager(), config);
}

} // namespace memory
} // namespace hunterclaw
