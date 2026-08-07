#pragma once

// 🎯 对齐 OpenClaw extensions/active-memory/index.ts 的 hook 注册（api.on）：
//   将 lane-1 召回编排注册到 HunterClaw 的 before_prompt_build / agent_end hook。
//
//   - before_prompt_build: 运行 run_active_memory_lane_one，命中强trigger 时
//     通过 PluginHookBeforePromptBuildResult.prepend_context 注入召回上下文。
//   - agent_end: forget 该 run 的 prewarm 缓存。
//
//宿主装配：gateway 启动时调用 register_active_memory_hooks，注入 workspace/
//   settings/mode/provider/model/activeProjectKeys 的取值方式。

#include "hunterclaw/memory/types.h"
#include "hunterclaw/memory/active_memory_escalation.h"
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace hunterclaw {
namespace tools { class ToolHookManager; }

namespace memory {

// lane-1 hook 宿主配置（对齐 OC index.ts 从 cfg/ctx 读取的运行时依赖）。
struct ActiveMemoryHookConfig {
    // 插件 id（用于 hook 注册与反注册，对齐 OC plugin id）。
    std::string plugin_id = "active-memory";

    // memory 检索配置提供者：按 workspace_dir 解析（对齐 OC resolveMemoryBackendConfig）。
    //   返回 std::nullopt 表示该 workspace 未配置 memory →跳过 lane-1。
    std::function<std::optional<ResolvedMemorySearchConfig>(const std::string& workspace_dir)>
        resolve_settings;

    // active_project_keys 提供者（对齐 OC ctx.activeProjectKeys）。默认空。
    std::function<std::vector<std::string>(const std::string& agent_id,
                                           const std::string& workspace_dir)>
        resolve_active_project_keys;

    // 召回模式（对齐 OC invocationConfig.mode）。默认 escalate。
    ActiveMemoryMode mode = ActiveMemoryMode::Escalate;

    // provider/model 提供者（circuit breaker key + cfg_token）。默认取settings.provider/model。
    std::function<std::optional<std::string>()> resolve_provider;
    std::function<std::optional<std::string>()> resolve_model;

    // circuit breaker 参数（对齐 OC config 默认；此处允许宿主覆盖）。
    int circuit_breaker_max_timeouts = 3;
    int64_t circuit_breaker_cooldown_ms = 60000;

    // 是否记录日志（对齐 OC invocationConfig.logging）。
    bool logging = false;
};

//注册 lane-1 hooks 到给定 ToolHookManager（对齐 OC register()）。
void register_active_memory_hooks(
    tools::ToolHookManager& hook_manager,
    const ActiveMemoryHookConfig& config = {});

// 便捷装配：注册到进程全局 ToolHookManager（require_global_hook_manager）。
//   未提供 resolve_settings 时使用默认实现——返回默认 ResolvedMemorySearchConfig
//   （对应始终可用的 builtin 本地 memory），使 lane-1 对每个带 workspace 的 run生效。
//   gateway 启动装配时调用一次。
void register_active_memory_hooks_global(ActiveMemoryHookConfig config = {});

} // namespace memory
} // namespace hunterclaw
