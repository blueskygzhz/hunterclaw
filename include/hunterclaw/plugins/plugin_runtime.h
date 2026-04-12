#pragma once
// ============================================================================
// plugin_runtime.h
// 严格对齐 OpenClaw: plugins/runtime/ (36 files)
//   index.ts + types-core.ts + types-channel.ts + types.ts
//   runtime-agent.ts + runtime-system.ts + runtime-config.ts
//   runtime-events.ts + runtime-media.ts + runtime-logging.ts
//   runtime-tasks.ts + runtime-taskflow.ts + runtime-channel.ts
//   runtime-cache.ts + runtime-plugin-boundary.ts
// ============================================================================

#include "hunterclaw/core/types.h"
#include "hunterclaw/llm/model_selection.h"
#include <string>
#include <vector>
#include <map>
#include <optional>
#include <functional>
#include <memory>

namespace hunterclaw {

// Forward declarations
namespace tasks { class TaskRegistry; class TaskFlowRegistry; class TaskExecutor; }

namespace plugins {

// ============================================================================
// RuntimeLogger (对齐 OC: types-core.ts RuntimeLogger)
// ============================================================================

struct RuntimeLogger {
    std::function<void(const std::string&)> debug;
    std::function<void(const std::string&)> info;
    std::function<void(const std::string&)> warn;
    std::function<void(const std::string&)> error;
};

// ============================================================================
// RuntimeAgent (对齐 OC: runtime-agent.ts createRuntimeAgent)
// ============================================================================

struct RuntimeAgentDefaults {
    std::string model;     // DEFAULT_MODEL
    std::string provider;  // DEFAULT_PROVIDER
};

struct RuntimeAgentSession {
    std::function<std::string(const core::Config&)> resolve_store_path;
    std::function<std::string(const std::string&, const std::string&)> resolve_session_file_path;
};

struct RuntimeAgent {
    RuntimeAgentDefaults defaults;
    std::function<std::string(const core::Config&, const std::string&)> resolve_agent_dir;
    std::function<std::string(const core::Config&, const std::string&)> resolve_agent_workspace_dir;
    std::function<llm::ThinkLevel(const core::Config&, const std::string&, const std::string&)> resolve_thinking_default;
    RuntimeAgentSession session;
};

// ============================================================================
// RuntimeSystem (对齐 OC: runtime-system.ts)
// ============================================================================

struct RuntimeSystem {
    std::function<void(const core::EventFrame&)> enqueue_system_event;
    std::function<void()> request_heartbeat_now;
    std::function<core::Result<int>(const std::string&, int)> run_command_with_timeout;
};

// ============================================================================
// RuntimeConfig (对齐 OC: runtime-config.ts)
// ============================================================================

struct RuntimeConfig {
    std::function<core::Config()> load_config;
    std::function<core::Result<bool>(const std::string&, const core::Config&)> write_config_file;
};

// ============================================================================
// RuntimeEvents (对齐 OC: runtime-events.ts)
// ============================================================================

struct RuntimeEvents {
    std::function<void(const std::string&, const core::EventFrame&)> emit;
    std::function<int(const std::string&, std::function<void(const core::EventFrame&)>)> on;
    std::function<void(int)> off;
};

// ============================================================================
// RuntimeMedia (对齐 OC: runtime-media.ts)
// ============================================================================

struct RuntimeMedia {
    std::function<std::string(const std::string&)> detect_mime;
    std::function<std::string(const std::string&)> media_kind_from_mime;
    std::function<bool(const std::string&)> is_voice_compatible_audio;
};

// ============================================================================
// RuntimeTaskRuns (对齐 OC: runtime-tasks.ts)
// ============================================================================

struct BoundTaskRunsRuntime {
    std::string session_key;
    std::function<core::JsonValue(const std::string&)> get;
    std::function<std::vector<core::JsonValue>()> list;
    std::function<std::optional<core::JsonValue>()> find_latest;
    std::function<std::optional<core::JsonValue>(const std::string&)> resolve;
    std::function<core::JsonValue(const std::string&)> cancel;
};

struct RuntimeTaskRuns {
    std::function<BoundTaskRunsRuntime(const std::string&)> bind_session;
};

// ============================================================================
// RuntimeTaskFlows (对齐 OC: runtime-taskflow.ts)
// ============================================================================

struct BoundTaskFlowsRuntime {
    std::string session_key;
    std::function<core::JsonValue(const std::string&)> get;
    std::function<std::vector<core::JsonValue>()> list;
    std::function<std::optional<core::JsonValue>()> find_latest;
    std::function<std::optional<core::JsonValue>(const std::string&)> resolve;
};

struct RuntimeTaskFlows {
    std::function<BoundTaskFlowsRuntime(const std::string&)> bind_session;
};

// ============================================================================
// RuntimeTasks (对齐 OC: runtime-tasks.ts PluginRuntimeTasks)
// ============================================================================

struct RuntimeTasks {
    RuntimeTaskRuns runs;
    RuntimeTaskFlows flows;
};

// ============================================================================
// PluginRuntime (对齐 OC: types.ts PluginRuntime)
// ============================================================================

struct PluginRuntime {
    std::string version;
    RuntimeAgent agent;
    RuntimeSystem system;
    RuntimeConfig config;
    RuntimeEvents events;
    RuntimeMedia media;
    RuntimeTasks tasks;
    RuntimeLogger logger;
};

// ============================================================================
// Factory Functions (对齐 OC: index.ts createRuntime*)
// ============================================================================

/// Create a default plugin runtime wired to a gateway server's live state
/// 对齐 OC: createPluginRuntime (index.ts)
PluginRuntime create_plugin_runtime(
    const core::Config& cfg,
    std::shared_ptr<tasks::TaskRegistry> task_registry = nullptr,
    std::shared_ptr<tasks::TaskFlowRegistry> task_flow_registry = nullptr,
    std::shared_ptr<tasks::TaskExecutor> task_executor = nullptr);

/// Create the agent runtime subset
/// 对齐 OC: createRuntimeAgent
RuntimeAgent create_runtime_agent();

/// Create the system runtime subset
/// 对齐 OC: createRuntimeSystem
RuntimeSystem create_runtime_system();

/// Create the tasks runtime subset wired to task registries
/// 对齐 OC: createRuntimeTasks
RuntimeTasks create_runtime_tasks(
    std::shared_ptr<tasks::TaskRegistry> task_registry,
    std::shared_ptr<tasks::TaskFlowRegistry> task_flow_registry,
    std::shared_ptr<tasks::TaskExecutor> task_executor);

} // namespace plugins
} // namespace hunterclaw
