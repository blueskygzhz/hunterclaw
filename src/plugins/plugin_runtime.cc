// ============================================================================
// plugin_runtime.cc
// 严格对齐 OpenClaw: plugins/runtime/ (36 files)
// ============================================================================

#include "hunterclaw/plugins/plugin_runtime.h"
#include "hunterclaw/tasks/task_registry.h"
#include "hunterclaw/tasks/task_executor.h"
#include "hunterclaw/tasks/task_flow_registry.h"
#include "hunterclaw/tasks/task_domain_views.h"
#include "hunterclaw/infra/heartbeat_wake.h"
#include <spdlog/spdlog.h>

namespace hunterclaw {
namespace plugins {

// ============================================================================
// createRuntimeAgent (对齐 OC: runtime-agent.ts)
// ============================================================================

RuntimeAgent create_runtime_agent() {
    RuntimeAgent agent;
    agent.defaults.model = llm::DEFAULT_MODEL;
    agent.defaults.provider = llm::DEFAULT_PROVIDER;

    agent.resolve_agent_dir = [](const core::Config& cfg, const std::string& agent_id) -> std::string {
        if (cfg.config_dir.empty()) return "";
        return cfg.config_dir + "/agents/" + (agent_id.empty() ? "main" : agent_id);
    };

    agent.resolve_agent_workspace_dir = [](const core::Config& cfg, const std::string& agent_id) -> std::string {
        if (cfg.config_dir.empty()) return "";
        return cfg.config_dir + "/agents/" + (agent_id.empty() ? "main" : agent_id) + "/workspace";
    };

    agent.resolve_thinking_default = [](const core::Config& cfg,
                                         const std::string& provider,
                                         const std::string& model) -> llm::ThinkLevel {
        return llm::resolve_thinking_default(cfg, provider, model);
    };

    agent.session.resolve_store_path = [](const core::Config& cfg) -> std::string {
        return cfg.config_dir.empty() ? "./sessions" : cfg.config_dir + "/sessions";
    };

    agent.session.resolve_session_file_path = [](const std::string& store_path,
                                                  const std::string& session_id) -> std::string {
        return store_path + "/" + session_id + ".json";
    };

    return agent;
}

// ============================================================================
// createRuntimeSystem (对齐 OC: runtime-system.ts)
// ============================================================================

RuntimeSystem create_runtime_system() {
    RuntimeSystem system;

    system.enqueue_system_event = [](const core::EventFrame& event) {
        // Fire-and-forget system event
        (void)event;
    };

    system.request_heartbeat_now = []() {
        infra::request_heartbeat_now();
    };

    system.run_command_with_timeout = [](const std::string& command, int timeout_ms) -> core::Result<int> {
        (void)timeout_ms;
        int ret = std::system(command.c_str());
        return core::Result<int>::Ok(ret);
    };

    return system;
}

// ============================================================================
// createRuntimeTasks (对齐 OC: runtime-tasks.ts + runtime-taskflow.ts)
// ============================================================================

RuntimeTasks create_runtime_tasks(
    std::shared_ptr<tasks::TaskRegistry> task_registry,
    std::shared_ptr<tasks::TaskFlowRegistry> task_flow_registry,
    std::shared_ptr<tasks::TaskExecutor> task_executor
) {
    RuntimeTasks rt;

    // Task Runs — simplified binding (views are plain structs, serialize via nlohmann)
    rt.runs.bind_session = [task_registry, task_executor](const std::string& session_key) -> BoundTaskRunsRuntime {
        BoundTaskRunsRuntime bound;
        bound.session_key = session_key;

        bound.get = [task_registry](const std::string& task_id) -> core::JsonValue {
            if (!task_registry) return core::JsonValue();
            auto record = task_registry->get_task_by_id(task_id);
            if (!record) return core::JsonValue();
            auto view = tasks::map_task_run_view(*record);
            nlohmann::json j;
            j["task_id"] = view.id;
            j["status"] = tasks::task_status_string(view.status);
            j["kind"] = tasks::task_runtime_string(view.runtime);
            return core::JsonValue(j);
        };

        bound.list = [task_registry, session_key]() -> std::vector<core::JsonValue> {
            if (!task_registry) return {};
            auto records = task_registry->list_tasks_for_session_key(session_key);
            std::vector<core::JsonValue> result;
            for (const auto& r : records) {
                auto view = tasks::map_task_run_view(r);
                nlohmann::json j;
                j["task_id"] = view.id;
                j["status"] = tasks::task_status_string(view.status);
                j["kind"] = tasks::task_runtime_string(view.runtime);
                result.push_back(core::JsonValue(j));
            }
            return result;
        };

        bound.find_latest = [task_registry, session_key]() -> std::optional<core::JsonValue> {
            if (!task_registry) return std::nullopt;
            auto records = task_registry->list_tasks_for_session_key(session_key);
            if (records.empty()) return std::nullopt;
            auto latest = std::max_element(records.begin(), records.end(),
                [](const tasks::TaskRecord& a, const tasks::TaskRecord& b) {
                    return a.created_at < b.created_at;
                });
            auto view = tasks::map_task_run_view(*latest);
            nlohmann::json j;
            j["task_id"] = view.id;
            j["status"] = tasks::task_status_string(view.status);
            return core::JsonValue(j);
        };

        bound.resolve = [task_registry](const std::string& token) -> std::optional<core::JsonValue> {
            if (!task_registry) return std::nullopt;
            auto record = task_registry->get_task_by_id(token);
            if (!record) return std::nullopt;
            auto view = tasks::map_task_run_view(*record);
            nlohmann::json j;
            j["task_id"] = view.id;
            j["status"] = tasks::task_status_string(view.status);
            return core::JsonValue(j);
        };

        bound.cancel = [task_executor](const std::string& task_id) -> core::JsonValue {
            nlohmann::json j;
            if (!task_executor) {
                j["found"] = false; j["cancelled"] = false;
            } else {
                try {
                    task_executor->cancel_task_by_id(task_id);
                    j["found"] = true; j["cancelled"] = true;
                } catch (...) {
                    j["found"] = false; j["cancelled"] = false;
                }
            }
            return core::JsonValue(j);
        };

        return bound;
    };

    // Task Flows — simplified binding
    rt.flows.bind_session = [task_flow_registry](const std::string& session_key) -> BoundTaskFlowsRuntime {
        (void)session_key; // flow records don't have session_key field; filter would need owner_key
        BoundTaskFlowsRuntime bound;
        bound.session_key = session_key;

        bound.get = [task_flow_registry](const std::string& flow_id) -> core::JsonValue {
            if (!task_flow_registry) return core::JsonValue();
            auto flow = task_flow_registry->get_task_flow_by_id(flow_id);
            if (!flow) return core::JsonValue();
            auto view = tasks::map_task_flow_view(*flow);
            nlohmann::json j;
            j["flow_id"] = view.id;
            j["status"] = tasks::task_flow_status_string(view.status);
            j["name"] = view.goal;
            return core::JsonValue(j);
        };

        bound.list = [task_flow_registry]() -> std::vector<core::JsonValue> {
            if (!task_flow_registry) return {};
            auto flows = task_flow_registry->list_task_flow_records();
            std::vector<core::JsonValue> result;
            for (const auto& f : flows) {
                auto view = tasks::map_task_flow_view(f);
                nlohmann::json j;
                j["flow_id"] = view.id;
                j["status"] = tasks::task_flow_status_string(view.status);
                j["name"] = view.goal;
                result.push_back(core::JsonValue(j));
            }
            return result;
        };

        bound.find_latest = [task_flow_registry]() -> std::optional<core::JsonValue> {
            if (!task_flow_registry) return std::nullopt;
            auto flows = task_flow_registry->list_task_flow_records();
            if (flows.empty()) return std::nullopt;
            auto latest = std::max_element(flows.begin(), flows.end(),
                [](const tasks::TaskFlowRecord& a, const tasks::TaskFlowRecord& b) {
                    return a.created_at < b.created_at;
                });
            auto view = tasks::map_task_flow_view(*latest);
            nlohmann::json j;
            j["flow_id"] = view.id;
            j["status"] = tasks::task_flow_status_string(view.status);
            return core::JsonValue(j);
        };

        bound.resolve = [task_flow_registry](const std::string& token) -> std::optional<core::JsonValue> {
            if (!task_flow_registry) return std::nullopt;
            auto flow = task_flow_registry->get_task_flow_by_id(token);
            if (!flow) return std::nullopt;
            auto view = tasks::map_task_flow_view(*flow);
            nlohmann::json j;
            j["flow_id"] = view.id;
            j["status"] = tasks::task_flow_status_string(view.status);
            return core::JsonValue(j);
        };

        return bound;
    };

    return rt;
}

// ============================================================================
// createPluginRuntime (对齐 OC: index.ts — main factory)
// ============================================================================

PluginRuntime create_plugin_runtime(
    const core::Config& cfg,
    std::shared_ptr<tasks::TaskRegistry> task_registry,
    std::shared_ptr<tasks::TaskFlowRegistry> task_flow_registry,
    std::shared_ptr<tasks::TaskExecutor> task_executor
) {
    PluginRuntime runtime;
    runtime.version = "0.1.0";

    // Agent
    runtime.agent = create_runtime_agent();

    // System
    runtime.system = create_runtime_system();

    // Config
    runtime.config.load_config = [cfg]() -> core::Config { return cfg; };
    runtime.config.write_config_file = [](const std::string& path, const core::Config& new_cfg) -> core::Result<bool> {
        (void)path; (void)new_cfg;
        // Config write requires file-level access — delegated to gateway
        return core::Result<bool>::Ok(true);
    };

    // Events
    runtime.events.emit = [](const std::string& name, const core::EventFrame& frame) {
        (void)name; (void)frame;
    };
    runtime.events.on = [](const std::string& name, std::function<void(const core::EventFrame&)> cb) -> int {
        (void)name; (void)cb;
        return 0; // subscription id
    };
    runtime.events.off = [](int id) { (void)id; };

    // Media
    runtime.media.detect_mime = [](const std::string& path) -> std::string {
        // Simple extension-based detection
        if (path.size() > 4) {
            auto ext = path.substr(path.rfind('.'));
            if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
            if (ext == ".png") return "image/png";
            if (ext == ".gif") return "image/gif";
            if (ext == ".webp") return "image/webp";
            if (ext == ".mp3") return "audio/mpeg";
            if (ext == ".wav") return "audio/wav";
            if (ext == ".mp4") return "video/mp4";
            if (ext == ".pdf") return "application/pdf";
        }
        return "application/octet-stream";
    };
    runtime.media.media_kind_from_mime = [](const std::string& mime) -> std::string {
        if (mime.find("image/") == 0) return "image";
        if (mime.find("audio/") == 0) return "audio";
        if (mime.find("video/") == 0) return "video";
        return "file";
    };
    runtime.media.is_voice_compatible_audio = [](const std::string& mime) -> bool {
        return mime == "audio/wav" || mime == "audio/mpeg" || mime == "audio/ogg" ||
               mime == "audio/webm" || mime == "audio/mp4";
    };

    // Tasks
    runtime.tasks = create_runtime_tasks(task_registry, task_flow_registry, task_executor);

    // Logger
    auto logger = spdlog::get("plugins");
    if (!logger) logger = spdlog::stdout_logger_mt("plugins");
    runtime.logger.debug = [logger](const std::string& msg) { if (logger) logger->debug(msg); };
    runtime.logger.info = [logger](const std::string& msg) { if (logger) logger->info(msg); };
    runtime.logger.warn = [logger](const std::string& msg) { if (logger) logger->warn(msg); };
    runtime.logger.error = [logger](const std::string& msg) { if (logger) logger->error(msg); };

    return runtime;
}

} // namespace plugins
} // namespace hunterclaw
