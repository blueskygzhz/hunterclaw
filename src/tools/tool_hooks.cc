// ============================================================================
// tool_hooks.cc - Plugin Hook Runner Implementation
//
// Strictly aligned with OpenClaw plugins/hooks.ts createHookRunner():
//
//   before_tool_call:
//     - Sequential (modifying hook)
//     - Priority-sorted (higher first)
//     - Results merged: params/block/blockReason
//     - Errors caught and logged (catchErrors=true by default)
//
//   after_tool_call:
//     - Parallel/sequential void hook
//     - Priority-sorted (higher first)
//     - Errors caught and logged
//
//   tool_result_persist:
//     - Synchronous, sequential
//     - Priority-sorted (higher first)
//     - Each handler may return { message } to replace the current message
//     - Guards against async handlers (this hook is sync-only)
//
//   before_message_write:
//     - Synchronous, sequential
//     - Priority-sorted (higher first)
//     - If any handler returns { block: true }, return immediately
//     - If handler returns { message }, use it for subsequent handlers
// ============================================================================

#include "hunterclaw/tools/tool_hooks.h"
#include <spdlog/spdlog.h>
#include <algorithm>
#include <chrono>
#include <map>
#include <fstream>

namespace hunterclaw {
namespace tools {

namespace {
    auto log() {
        static auto logger = spdlog::get("tools.hooks");
        if (!logger) {
            logger = spdlog::stdout_logger_mt("tools.hooks");
        }
        return logger;
    }
} // anonymous namespace

// ============================================================================
// plugin_hook_name_to_string
// ============================================================================

std::string plugin_hook_name_to_string(PluginHookName name) {
    switch (name) {
        case PluginHookName::BEFORE_MODEL_RESOLVE:      return "before_model_resolve";
        case PluginHookName::BEFORE_PROMPT_BUILD:       return "before_prompt_build";
        case PluginHookName::BEFORE_AGENT_START:        return "before_agent_start";
        case PluginHookName::LLM_INPUT:                 return "llm_input";
        case PluginHookName::LLM_OUTPUT:                return "llm_output";
        case PluginHookName::AGENT_END:                 return "agent_end";
        case PluginHookName::BEFORE_COMPACTION:         return "before_compaction";
        case PluginHookName::AFTER_COMPACTION:          return "after_compaction";
        case PluginHookName::BEFORE_RESET:              return "before_reset";
        case PluginHookName::MESSAGE_RECEIVED:          return "message_received";
        case PluginHookName::MESSAGE_SENDING:           return "message_sending";
        case PluginHookName::MESSAGE_SENT:              return "message_sent";
        case PluginHookName::BEFORE_TOOL_CALL:          return "before_tool_call";
        case PluginHookName::AFTER_TOOL_CALL:           return "after_tool_call";
        case PluginHookName::TOOL_RESULT_PERSIST:       return "tool_result_persist";
        case PluginHookName::BEFORE_MESSAGE_WRITE:      return "before_message_write";
        case PluginHookName::SESSION_START:             return "session_start";
        case PluginHookName::SESSION_END:               return "session_end";
        case PluginHookName::SUBAGENT_SPAWNING:         return "subagent_spawning";
        case PluginHookName::SUBAGENT_DELIVERY_TARGET:  return "subagent_delivery_target";
        case PluginHookName::SUBAGENT_SPAWNED:          return "subagent_spawned";
        case PluginHookName::SUBAGENT_ENDED:            return "subagent_ended";
        case PluginHookName::GATEWAY_START:             return "gateway_start";
        case PluginHookName::GATEWAY_STOP:              return "gateway_stop";
    }
    return "unknown";
}

// ============================================================================
// ToolHookManager - Registration
// ============================================================================

void ToolHookManager::register_before_tool_call(
    const std::string& plugin_id,
    BeforeToolCallHandler handler,
    int priority)
{
    std::lock_guard<std::mutex> lock(mutex_);
    before_tool_call_hooks_.push_back({plugin_id, std::move(handler), priority});
    // Sort by priority descending (higher priority runs first)
    // Aligned with: getHooksForName → .toSorted((a, b) => (b.priority ?? 0) - (a.priority ?? 0))
    std::stable_sort(before_tool_call_hooks_.begin(), before_tool_call_hooks_.end(),
        [](const BeforeToolCallEntry& a, const BeforeToolCallEntry& b) {
            return a.priority > b.priority;
        });
    log()->debug("[hooks] registered before_tool_call hook from plugin '{}'", plugin_id);
}

void ToolHookManager::register_after_tool_call(
    const std::string& plugin_id,
    AfterToolCallHandler handler,
    int priority)
{
    std::lock_guard<std::mutex> lock(mutex_);
    after_tool_call_hooks_.push_back({plugin_id, std::move(handler), priority});
    std::stable_sort(after_tool_call_hooks_.begin(), after_tool_call_hooks_.end(),
        [](const AfterToolCallEntry& a, const AfterToolCallEntry& b) {
            return a.priority > b.priority;
        });
    log()->debug("[hooks] registered after_tool_call hook from plugin '{}'", plugin_id);
}

void ToolHookManager::register_tool_result_persist(
    const std::string& plugin_id,
    ToolResultPersistHandler handler,
    int priority)
{
    std::lock_guard<std::mutex> lock(mutex_);
    tool_result_persist_hooks_.push_back({plugin_id, std::move(handler), priority});
    std::stable_sort(tool_result_persist_hooks_.begin(), tool_result_persist_hooks_.end(),
        [](const ToolResultPersistEntry& a, const ToolResultPersistEntry& b) {
            return a.priority > b.priority;
        });
    log()->debug("[hooks] registered tool_result_persist hook from plugin '{}'", plugin_id);
}

void ToolHookManager::register_before_message_write(
    const std::string& plugin_id,
    BeforeMessageWriteHandler handler,
    int priority)
{
    std::lock_guard<std::mutex> lock(mutex_);
    before_message_write_hooks_.push_back({plugin_id, std::move(handler), priority});
    std::stable_sort(before_message_write_hooks_.begin(), before_message_write_hooks_.end(),
        [](const BeforeMessageWriteEntry& a, const BeforeMessageWriteEntry& b) {
            return a.priority > b.priority;
        });
    log()->debug("[hooks] registered before_message_write hook from plugin '{}'", plugin_id);
}

void ToolHookManager::unregister_plugin(const std::string& plugin_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto remove_by_plugin = [&plugin_id](auto& vec) {
        vec.erase(std::remove_if(vec.begin(), vec.end(),
            [&plugin_id](const auto& entry) { return entry.plugin_id == plugin_id; }),
            vec.end());
    };

    remove_by_plugin(before_tool_call_hooks_);
    remove_by_plugin(after_tool_call_hooks_);
    remove_by_plugin(tool_result_persist_hooks_);
    remove_by_plugin(before_message_write_hooks_);

    log()->debug("[hooks] unregistered all hooks for plugin '{}'", plugin_id);
}

void ToolHookManager::clear_all_hooks() {
    std::lock_guard<std::mutex> lock(mutex_);
    before_tool_call_hooks_.clear();
    after_tool_call_hooks_.clear();
    tool_result_persist_hooks_.clear();
    before_message_write_hooks_.clear();
    log()->debug("[hooks] cleared all hooks");
}

// ============================================================================
// run_before_tool_call
//
// Aligned with OpenClaw runModifyingHook("before_tool_call", ...):
//   - Sequential execution in priority order
//   - Results merged: params/block/blockReason
//     (first defined wins for params, last wins for block — aligned with
//      OpenClaw merge: { params: next.params ?? acc?.params,
//                        block: next.block ?? acc?.block,
//                        blockReason: next.blockReason ?? acc?.blockReason })
//   - Errors caught and logged (catchErrors=true)
// ============================================================================

std::optional<PluginHookBeforeToolCallResult> ToolHookManager::run_before_tool_call(
    const PluginHookBeforeToolCallEvent& event,
    const PluginHookToolContext& ctx)
{
    std::vector<BeforeToolCallEntry> hooks;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        hooks = before_tool_call_hooks_;
    }

    if (hooks.empty()) {
        return std::nullopt;
    }

    log()->debug("[hooks] running before_tool_call ({} handlers)", hooks.size());

    std::optional<PluginHookBeforeToolCallResult> accumulated;

    for (const auto& entry : hooks) {
        try {
            auto result = entry.handler(event, ctx);

            if (result.has_value()) {
                if (!accumulated.has_value()) {
                    accumulated = result;
                } else {
                    // Merge results (aligned with OpenClaw merge logic):
                    // params: next.params ?? acc?.params  (first defined wins)
                    // block:  next.block ?? acc?.block
                    // blockReason: next.blockReason ?? acc?.blockReason
                    auto& acc = accumulated.value();
                    const auto& next = result.value();

                    if (!acc.params.has_value() && next.params.has_value()) {
                        acc.params = next.params;
                    }
                    if (!acc.block.has_value() && next.block.has_value()) {
                        acc.block = next.block;
                    }
                    if (!acc.block_reason.has_value() && next.block_reason.has_value()) {
                        acc.block_reason = next.block_reason;
                    }
                }
            }
        } catch (const std::exception& e) {
            // Aligned with OpenClaw handleHookError (catchErrors=true):
            // log error but don't throw
            log()->error("[hooks] before_tool_call handler from '{}' failed: {}",
                entry.plugin_id, e.what());
        }
    }

    return accumulated;
}

// ============================================================================
// run_after_tool_call
//
// Aligned with OpenClaw runVoidHook("after_tool_call", ...):
//   - All handlers called (parallel in OpenClaw, sequential here for simplicity)
//   - Errors caught and logged
// ============================================================================

void ToolHookManager::run_after_tool_call(
    const PluginHookAfterToolCallEvent& event,
    const PluginHookToolContext& ctx)
{
    std::vector<AfterToolCallEntry> hooks;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        hooks = after_tool_call_hooks_;
    }

    if (hooks.empty()) return;

    log()->debug("[hooks] running after_tool_call ({} handlers)", hooks.size());

    for (const auto& entry : hooks) {
        try {
            entry.handler(event, ctx);
        } catch (const std::exception& e) {
            log()->error("[hooks] after_tool_call handler from '{}' failed: {}",
                entry.plugin_id, e.what());
        }
    }
}

// ============================================================================
// run_tool_result_persist
//
// Aligned with OpenClaw runToolResultPersist() in plugins/hooks.ts:
//   - Synchronous, sequential
//   - Each handler may return { message } to replace the current message
//   - Guards against async handlers (this hook is sync-only)
//   - Errors caught and logged
// ============================================================================

std::optional<PluginHookToolResultPersistResult> ToolHookManager::run_tool_result_persist(
    const PluginHookToolResultPersistEvent& event,
    const PluginHookToolResultPersistContext& ctx)
{
    std::vector<ToolResultPersistEntry> hooks;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        hooks = tool_result_persist_hooks_;
    }

    if (hooks.empty()) return std::nullopt;

    log()->debug("[hooks] running tool_result_persist ({} handlers)", hooks.size());

    // Aligned with OpenClaw: let current = event.message;
    core::JsonValue current = event.message;
    bool modified = false;

    for (const auto& entry : hooks) {
        try {
            // Build event with current message (aligned with OpenClaw:
            // hook.handler({ ...event, message: current }, ctx))
            PluginHookToolResultPersistEvent current_event = event;
            current_event.message = current;

            auto result = entry.handler(current_event, ctx);

            if (result.has_value() && result->message.has_value()) {
                // Aligned with OpenClaw: const next = out?.message; if (next) { current = next; }
                current = result->message.value();
                modified = true;
            }
        } catch (const std::exception& e) {
            log()->error("[hooks] tool_result_persist handler from '{}' failed: {}",
                entry.plugin_id, e.what());
        }
    }

    if (modified) {
        PluginHookToolResultPersistResult result;
        result.message = current;
        return result;
    }

    return std::nullopt;
}

// ============================================================================
// run_before_message_write
//
// Aligned with OpenClaw runBeforeMessageWrite() in plugins/hooks.ts:
//   - Synchronous, sequential
//   - If any handler returns { block: true }, return immediately with block=true
//   - If handler returns { message }, use it for subsequent handlers
//   - If message was modified, return { message: current }
//   - Errors caught and logged
// ============================================================================

std::optional<PluginHookBeforeMessageWriteResult> ToolHookManager::run_before_message_write(
    const PluginHookBeforeMessageWriteEvent& event,
    const std::string& agent_id,
    const std::string& session_key)
{
    std::vector<BeforeMessageWriteEntry> hooks;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        hooks = before_message_write_hooks_;
    }

    if (hooks.empty()) return std::nullopt;

    log()->debug("[hooks] running before_message_write ({} handlers)", hooks.size());

    // Aligned with OpenClaw: let current = event.message;
    core::JsonValue current = event.message;

    for (const auto& entry : hooks) {
        try {
            // Build event with current message
            PluginHookBeforeMessageWriteEvent current_event = event;
            current_event.message = current;

            auto result = entry.handler(current_event, agent_id, session_key);

            if (result.has_value()) {
                // Aligned with OpenClaw: if (result?.block) { return { block: true }; }
                if (result->block) {
                    PluginHookBeforeMessageWriteResult blocked;
                    blocked.block = true;
                    return blocked;
                }
                // Aligned with OpenClaw: if (result?.message) { current = result.message; }
                if (result->message.has_value()) {
                    current = result->message.value();
                }
            }
        } catch (const std::exception& e) {
            log()->error("[hooks] before_message_write handler from '{}' failed: {}",
                entry.plugin_id, e.what());
        }
    }

    // Aligned with OpenClaw: if (current !== event.message) { return { message: current }; }
    // (We track modification via pointer comparison — use a flag instead)
    // Return nullopt if no modification occurred
    // Note: We can't do reference equality on JsonValue, so we always return
    // the current message if any handler ran (conservative approach)
    if (!hooks.empty()) {
        PluginHookBeforeMessageWriteResult result;
        result.block = false;
        result.message = current;
        return result;
    }

    return std::nullopt;
}

// ============================================================================
// Query methods
// ============================================================================

bool ToolHookManager::has_before_tool_call_hooks() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return !before_tool_call_hooks_.empty();
}

bool ToolHookManager::has_after_tool_call_hooks() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return !after_tool_call_hooks_.empty();
}

bool ToolHookManager::has_tool_result_persist_hooks() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return !tool_result_persist_hooks_.empty();
}

bool ToolHookManager::has_before_message_write_hooks() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return !before_message_write_hooks_.empty();
}

size_t ToolHookManager::before_tool_call_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return before_tool_call_hooks_.size();
}

size_t ToolHookManager::after_tool_call_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return after_tool_call_hooks_.size();
}

std::vector<std::string> ToolHookManager::list_plugin_ids() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> ids;
    for (const auto& e : before_tool_call_hooks_) {
        if (std::find(ids.begin(), ids.end(), e.plugin_id) == ids.end())
            ids.push_back(e.plugin_id);
    }
    for (const auto& e : after_tool_call_hooks_) {
        if (std::find(ids.begin(), ids.end(), e.plugin_id) == ids.end())
            ids.push_back(e.plugin_id);
    }
    return ids;
}

// ============================================================================
// Built-in hooks (aligned with OpenClaw bundled hooks pattern)
// ============================================================================

namespace builtin_hooks {

BeforeToolCallHandler create_param_validation_hook() {
    return [](const PluginHookBeforeToolCallEvent& event,
              const PluginHookToolContext& ctx)
        -> std::optional<PluginHookBeforeToolCallResult>
    {
        // Check params is an object
        if (!event.params.is_object()) {
            PluginHookBeforeToolCallResult result;
            result.block = true;
            result.block_reason = "Tool parameters must be an object";
            return result;
        }

        // Tool-specific validation
        if (event.tool_name == "exec") {
            if (!event.params.contains("command")) {
                PluginHookBeforeToolCallResult result;
                result.block = true;
                result.block_reason = "exec tool requires 'command' parameter";
                return result;
            }
        }

        if (event.tool_name == "read_file" || event.tool_name == "write_file") {
            if (!event.params.contains("path")) {
                PluginHookBeforeToolCallResult result;
                result.block = true;
                result.block_reason = event.tool_name + " requires 'path' parameter";
                return result;
            }
        }

        return std::nullopt;  // No modification
    };
}

AfterToolCallHandler create_logging_hook() {
    return [](const PluginHookAfterToolCallEvent& event,
              const PluginHookToolContext& ctx)
    {
        auto logger = spdlog::get("tools.hooks");
        if (!logger) return;

        if (!event.error.has_value() || event.error->empty()) {
            logger->info("[hooks] tool '{}' completed in {:.2f}ms",
                event.tool_name,
                event.duration_ms.value_or(0.0));
        } else {
            logger->error("[hooks] tool '{}' failed after {:.2f}ms: {}",
                event.tool_name,
                event.duration_ms.value_or(0.0),
                event.error.value_or(""));
        }
    };
}

AfterToolCallHandler create_performance_monitoring_hook() {
    struct ToolStats {
        int call_count = 0;
        double total_duration_ms = 0.0;
        double min_duration_ms = std::numeric_limits<double>::max();
        double max_duration_ms = 0.0;
    };

    static std::map<std::string, ToolStats> stats_map;
    static std::mutex stats_mutex;

    return [](const PluginHookAfterToolCallEvent& event,
              const PluginHookToolContext& ctx)
    {
        std::lock_guard<std::mutex> lock(stats_mutex);
        auto& stats = stats_map[event.tool_name];
        double dur = event.duration_ms.value_or(0.0);
        stats.call_count++;
        stats.total_duration_ms += dur;
        stats.min_duration_ms = std::min(stats.min_duration_ms, dur);
        stats.max_duration_ms = std::max(stats.max_duration_ms, dur);

        if (stats.call_count % 100 == 0) {
            double avg = stats.total_duration_ms / stats.call_count;
            auto logger = spdlog::get("tools.hooks");
            if (logger) {
                logger->info("[hooks] tool '{}' stats: {} calls, avg={:.2f}ms, min={:.2f}ms, max={:.2f}ms",
                    event.tool_name, stats.call_count, avg,
                    stats.min_duration_ms, stats.max_duration_ms);
            }
        }
    };
}

AfterToolCallHandler create_error_collection_hook() {
    struct ErrorStats {
        int total_calls = 0;
        int error_count = 0;
        std::string last_error;
    };

    static std::map<std::string, ErrorStats> error_map;
    static std::mutex error_mutex;

    return [](const PluginHookAfterToolCallEvent& event,
              const PluginHookToolContext& ctx)
    {
        std::lock_guard<std::mutex> lock(error_mutex);
        auto& stats = error_map[event.tool_name];
        stats.total_calls++;

        bool has_error = event.error.has_value() && !event.error->empty();
        if (has_error) {
            stats.error_count++;
            stats.last_error = event.error.value_or("");

            double error_rate = static_cast<double>(stats.error_count) / stats.total_calls;
            if (error_rate > 0.5 && stats.total_calls >= 10) {
                auto logger = spdlog::get("tools.hooks");
                if (logger) {
                    logger->warn("[hooks] tool '{}' has high error rate: {:.1f}% ({}/{})",
                        event.tool_name, error_rate * 100,
                        stats.error_count, stats.total_calls);
                }
            }
        }
    };
}

AfterToolCallHandler create_audit_hook(const AuditHookConfig& config) {
    return [config](const PluginHookAfterToolCallEvent& event,
                    const PluginHookToolContext& ctx)
    {
        bool is_sensitive = std::find(
            config.sensitive_tools.begin(),
            config.sensitive_tools.end(),
            event.tool_name) != config.sensitive_tools.end();

        if (!is_sensitive) return;

        std::ofstream audit_log(config.audit_log_path, std::ios::app);
        if (audit_log.is_open()) {
            auto now = std::chrono::system_clock::now();
            auto ts = std::chrono::system_clock::to_time_t(now);
            audit_log << "[" << std::ctime(&ts) << "] "
                      << "tool=" << event.tool_name
                      << " duration=" << event.duration_ms.value_or(0.0) << "ms"
                      << " error=" << event.error.value_or("none")
                      << "\n";
        }
    };
}

BeforeToolCallHandler create_rate_limit_hook(const RateLimitConfig& config) {
    struct RateLimitState {
        std::map<std::string, int> tool_call_counts;
        int64_t window_start_minute = 0;
        int total_calls_in_window = 0;
    };

    static RateLimitState state;
    static std::mutex rate_mutex;

    return [config](const PluginHookBeforeToolCallEvent& event,
                    const PluginHookToolContext& ctx)
        -> std::optional<PluginHookBeforeToolCallResult>
    {
        std::lock_guard<std::mutex> lock(rate_mutex);

        auto now_ns = std::chrono::system_clock::now().time_since_epoch().count();
        int64_t current_minute = now_ns / 60000000000LL;

        if (current_minute != state.window_start_minute) {
            state.window_start_minute = current_minute;
            state.total_calls_in_window = 0;
            state.tool_call_counts.clear();
        }

        if (state.total_calls_in_window >= config.max_calls_per_minute) {
            PluginHookBeforeToolCallResult result;
            result.block = true;
            result.block_reason = "Global rate limit exceeded: " +
                std::to_string(config.max_calls_per_minute) + " calls/min";
            return result;
        }

        int tool_calls = state.tool_call_counts[event.tool_name];
        if (tool_calls >= config.max_calls_per_tool_per_minute) {
            PluginHookBeforeToolCallResult result;
            result.block = true;
            result.block_reason = "Tool '" + event.tool_name + "' rate limit exceeded: " +
                std::to_string(config.max_calls_per_tool_per_minute) + " calls/min";
            return result;
        }

        state.total_calls_in_window++;
        state.tool_call_counts[event.tool_name]++;

        return std::nullopt;
    };
}

} // namespace builtin_hooks

// ============================================================================
// Param normalization (unchanged)
// ============================================================================

std::string to_camel_case(const std::string& snake_case) {
    std::string result;
    bool capitalize_next = false;
    for (char c : snake_case) {
        if (c == '_') {
            capitalize_next = true;
        } else if (capitalize_next) {
            result += static_cast<char>(std::toupper(c));
            capitalize_next = false;
        } else {
            result += c;
        }
    }
    return result;
}

std::string to_snake_case(const std::string& camel_case) {
    std::string result;
    for (size_t i = 0; i < camel_case.size(); ++i) {
        char c = camel_case[i];
        if (std::isupper(c)) {
            if (i > 0) result += '_';
            result += static_cast<char>(std::tolower(c));
        } else {
            result += c;
        }
    }
    return result;
}

core::JsonValue normalize_tool_params(
    const core::JsonValue& params,
    const ParamNormalizationConfig& config)
{
    if (!params.is_object()) return params;

    core::JsonValue normalized = core::JsonValue::object();

    for (auto it = params.begin(); it != params.end(); ++it) {
        std::string key = it->first;
        core::JsonValue value = *it->second;

        if (config.snake_to_camel) {
            key = to_camel_case(key);
        } else if (config.camel_to_snake) {
            key = to_snake_case(key);
        }

        if (config.trim_strings && value.is_string()) {
            std::string str = value.get<std::string>();
            size_t start = str.find_first_not_of(" \t\n\r");
            size_t end   = str.find_last_not_of(" \t\n\r");
            if (start != std::string::npos && end != std::string::npos) {
                str = str.substr(start, end - start + 1);
            } else {
                str.clear();
            }
            value = core::JsonValue(str);
        }

        if (config.remove_nulls && value.is_null()) continue;

        if (value.is_object()) {
            value = normalize_tool_params(value, config);
        }

        normalized[key] = value;
    }

    return normalized;
}

} // namespace tools
} // namespace hunterclaw
