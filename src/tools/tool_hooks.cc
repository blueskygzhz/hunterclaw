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
        static auto logger = spdlog::get("tools");
        if (!logger) {
            logger = spdlog::stdout_logger_mt("tools");
        }
        return logger;
    }
}

// ============================================================================
// ToolHookManager Implementation
// ============================================================================

void ToolHookManager::register_before_hook(const std::string& name, BeforeToolCallHook hook) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    // 检查是否已存在
    auto it = std::find_if(before_hooks_.begin(), before_hooks_.end(),
        [&name](const HookEntry& entry) { return entry.name == name; }
    );
    
    if (it != before_hooks_.end()) {
        log()->warn("Overwriting existing before hook: {}", name);
        it->before_hook = hook;
    } else {
        HookEntry entry;
        entry.name = name;
        entry.before_hook = hook;
        before_hooks_.push_back(entry);
        log()->debug("Registered before hook: {}", name);
    }
}

void ToolHookManager::register_after_hook(const std::string& name, AfterToolCallHook hook) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = std::find_if(after_hooks_.begin(), after_hooks_.end(),
        [&name](const HookEntry& entry) { return entry.name == name; }
    );
    
    if (it != after_hooks_.end()) {
        log()->warn("Overwriting existing after hook: {}", name);
        it->after_hook = hook;
    } else {
        HookEntry entry;
        entry.name = name;
        entry.after_hook = hook;
        after_hooks_.push_back(entry);
        log()->debug("Registered after hook: {}", name);
    }
}

void ToolHookManager::unregister_before_hook(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = std::remove_if(before_hooks_.begin(), before_hooks_.end(),
        [&name](const HookEntry& entry) { return entry.name == name; }
    );
    
    if (it != before_hooks_.end()) {
        before_hooks_.erase(it, before_hooks_.end());
        log()->debug("Unregistered before hook: {}", name);
    }
}

void ToolHookManager::unregister_after_hook(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = std::remove_if(after_hooks_.begin(), after_hooks_.end(),
        [&name](const HookEntry& entry) { return entry.name == name; }
    );
    
    if (it != after_hooks_.end()) {
        after_hooks_.erase(it, after_hooks_.end());
        log()->debug("Unregistered after hook: {}", name);
    }
}

void ToolHookManager::clear_all_hooks() {
    std::lock_guard<std::mutex> lock(mutex_);
    before_hooks_.clear();
    after_hooks_.clear();
    log()->debug("Cleared all hooks");
}

BeforeHookOutcome ToolHookManager::run_before_hooks(
    const std::string& tool_name,
    const core::JsonValue& params,
    const ToolContext& ctx
) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    BeforeHookOutcome outcome;
    outcome.blocked = false;
    outcome.params = params;  // 初始参数
    
    for (const auto& entry : before_hooks_) {
        if (!entry.before_hook) continue;
        
        try {
            // 运行 Hook (使用上一个 Hook 修改后的参数)
            auto hook_outcome = entry.before_hook(tool_name, outcome.params, ctx);
            
            if (hook_outcome.blocked) {
                log()->warn("Tool {} blocked by before hook '{}': {}", 
                    tool_name, entry.name, hook_outcome.reason);
                return hook_outcome;  // 立即返回,不执行后续 Hooks
            }
            
            // 更新参数 (允许 Hook 修改参数)
            outcome.params = hook_outcome.params;
            
        } catch (const std::exception& e) {
            log()->error("Before hook '{}' failed for tool {}: {}", 
                entry.name, tool_name, e.what());
            // 继续执行其他 Hooks (不因单个 Hook 失败而中断)
        }
    }
    
    return outcome;
}

void ToolHookManager::run_after_hooks(
    const std::string& tool_name,
    const core::JsonValue& params,
    const ToolResult& result,
    double duration_ms,
    const std::string& error
) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    for (const auto& entry : after_hooks_) {
        if (!entry.after_hook) continue;
        
        try {
            entry.after_hook(tool_name, params, result, duration_ms, error);
        } catch (const std::exception& e) {
            log()->error("After hook '{}' failed for tool {}: {}", 
                entry.name, tool_name, e.what());
            // 继续执行其他 Hooks
        }
    }
}

size_t ToolHookManager::before_hooks_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return before_hooks_.size();
}

size_t ToolHookManager::after_hooks_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return after_hooks_.size();
}

std::vector<std::string> ToolHookManager::list_before_hooks() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> names;
    for (const auto& entry : before_hooks_) {
        names.push_back(entry.name);
    }
    return names;
}

std::vector<std::string> ToolHookManager::list_after_hooks() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> names;
    for (const auto& entry : after_hooks_) {
        names.push_back(entry.name);
    }
    return names;
}

// ============================================================================
// 内置 Hook 实现
// ============================================================================

namespace builtin_hooks {

BeforeToolCallHook create_param_validation_hook() {
    return [](const std::string& tool_name, const core::JsonValue& params, const ToolContext& ctx) {
        BeforeHookOutcome outcome;
        outcome.blocked = false;
        outcome.params = params;
        
        // 检查参数是否为对象
        if (!params.is_object()) {
            outcome.blocked = true;
            outcome.reason = "Tool parameters must be an object";
            return outcome;
        }
        
        // 工具特定验证
        if (tool_name == "exec") {
            if (!params.contains("command")) {
                outcome.blocked = true;
                outcome.reason = "exec tool requires 'command' parameter";
                return outcome;
            }
        }
        
        if (tool_name == "read_file" || tool_name == "write_file") {
            if (!params.contains("path")) {
                outcome.blocked = true;
                outcome.reason = tool_name + " requires 'path' parameter";
                return outcome;
            }
        }
        
        return outcome;
    };
}

AfterToolCallHook create_logging_hook() {
    return [](const std::string& tool_name, const core::JsonValue& params,
              const ToolResult& result, double duration_ms, const std::string& error) {
        if (error.empty() && result.success) {
            log()->info("Tool '{}' executed successfully in {:.2f}ms", tool_name, duration_ms);
        } else {
            log()->error("Tool '{}' failed after {:.2f}ms: {}", 
                tool_name, duration_ms, error.empty() ? result.error : error);
        }
    };
}

AfterToolCallHook create_performance_monitoring_hook() {
    // 静态性能统计
    struct ToolStats {
        int call_count = 0;
        double total_duration_ms = 0.0;
        double min_duration_ms = std::numeric_limits<double>::max();
        double max_duration_ms = 0.0;
    };
    
    static std::map<std::string, ToolStats> stats_map;
    static std::mutex stats_mutex;
    
    return [](const std::string& tool_name, const core::JsonValue& params,
              const ToolResult& result, double duration_ms, const std::string& error) {
        std::lock_guard<std::mutex> lock(stats_mutex);
        
        auto& stats = stats_map[tool_name];
        stats.call_count++;
        stats.total_duration_ms += duration_ms;
        stats.min_duration_ms = std::min(stats.min_duration_ms, duration_ms);
        stats.max_duration_ms = std::max(stats.max_duration_ms, duration_ms);
        
        // 每 100 次调用打印统计
        if (stats.call_count % 100 == 0) {
            double avg = stats.total_duration_ms / stats.call_count;
            log()->info("Tool '{}' stats: {} calls, avg={:.2f}ms, min={:.2f}ms, max={:.2f}ms",
                tool_name, stats.call_count, avg, stats.min_duration_ms, stats.max_duration_ms);
        }
    };
}

AfterToolCallHook create_error_collection_hook() {
    // 静态错误统计
    struct ErrorStats {
        int total_calls = 0;
        int error_count = 0;
        std::string last_error;
        int64_t last_error_time = 0;
    };
    
    static std::map<std::string, ErrorStats> error_map;
    static std::mutex error_mutex;
    
    return [](const std::string& tool_name, const core::JsonValue& params,
              const ToolResult& result, double duration_ms, const std::string& error) {
        std::lock_guard<std::mutex> lock(error_mutex);
        
        auto& stats = error_map[tool_name];
        stats.total_calls++;
        
        if (!error.empty() || !result.success) {
            stats.error_count++;
            stats.last_error = error.empty() ? result.error : error;
            stats.last_error_time = std::chrono::system_clock::now().time_since_epoch().count();
            
            double error_rate = static_cast<double>(stats.error_count) / stats.total_calls;
            if (error_rate > 0.5 && stats.total_calls >= 10) {
                log()->warn("Tool '{}' has high error rate: {:.1f}% ({}/{})",
                    tool_name, error_rate * 100, stats.error_count, stats.total_calls);
            }
        }
    };
}

AfterToolCallHook create_audit_hook(const AuditHookConfig& config) {
    return [config](const std::string& tool_name, const core::JsonValue& params,
                    const ToolResult& result, double duration_ms, const std::string& error) {
        // 检查是否是敏感工具
        bool is_sensitive = std::find(config.sensitive_tools.begin(), 
                                     config.sensitive_tools.end(), 
                                     tool_name) != config.sensitive_tools.end();
        
        if (!is_sensitive) return;
        
        // 写入审计日志
        std::ofstream audit_log(config.audit_log_path, std::ios::app);
        if (audit_log.is_open()) {
            auto now = std::chrono::system_clock::now();
            auto timestamp = std::chrono::system_clock::to_time_t(now);
            
            audit_log << "[" << std::ctime(&timestamp) << "] "
                     << "Tool: " << tool_name << ", "
                     << "Duration: " << duration_ms << "ms, "
                     << "Success: " << (result.success ? "true" : "false");
            
            if (!error.empty()) {
                audit_log << ", Error: " << error;
            }
            
            audit_log << "\n";
            audit_log.close();
        }
    };
}

BeforeToolCallHook create_rate_limit_hook(const RateLimitConfig& config) {
    // 静态速率限制状态
    struct RateLimitState {
        std::map<std::string, int> tool_call_counts;  // tool_name => count
        int64_t window_start_time = 0;
        int total_calls_in_window = 0;
    };
    
    static RateLimitState state;
    static std::mutex rate_mutex;
    
    return [config](const std::string& tool_name, const core::JsonValue& params, const ToolContext& ctx) {
        std::lock_guard<std::mutex> lock(rate_mutex);
        
        BeforeHookOutcome outcome;
        outcome.blocked = false;
        outcome.params = params;
        
        auto now = std::chrono::system_clock::now().time_since_epoch().count();
        int64_t current_minute = now / 60000000000;  // 纳秒 => 分钟
        
        // 检查是否需要重置窗口
        if (current_minute != state.window_start_time) {
            state.window_start_time = current_minute;
            state.total_calls_in_window = 0;
            state.tool_call_counts.clear();
        }
        
        // 检查全局速率限制
        if (state.total_calls_in_window >= config.max_calls_per_minute) {
            outcome.blocked = true;
            outcome.reason = "Global rate limit exceeded: " + 
                           std::to_string(config.max_calls_per_minute) + " calls/min";
            return outcome;
        }
        
        // 检查单个工具速率限制
        int tool_calls = state.tool_call_counts[tool_name];
        if (tool_calls >= config.max_calls_per_tool_per_minute) {
            outcome.blocked = true;
            outcome.reason = "Tool '" + tool_name + "' rate limit exceeded: " +
                           std::to_string(config.max_calls_per_tool_per_minute) + " calls/min";
            return outcome;
        }
        
        // 记录调用
        state.total_calls_in_window++;
        state.tool_call_counts[tool_name]++;
        
        return outcome;
    };
}

} // namespace builtin_hooks

// ============================================================================
// 参数规范化
// ============================================================================

std::string to_camel_case(const std::string& snake_case) {
    std::string result;
    bool capitalize_next = false;
    
    for (char c : snake_case) {
        if (c == '_') {
            capitalize_next = true;
        } else if (capitalize_next) {
            result += std::toupper(c);
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
            result += std::tolower(c);
        } else {
            result += c;
        }
    }
    
    return result;
}

core::JsonValue normalize_tool_params(
    const core::JsonValue& params,
    const ParamNormalizationConfig& config
) {
    if (!params.is_object()) {
        return params;  // 非对象直接返回
    }
    
    core::JsonValue normalized = core::JsonValue::object();
    
    for (auto it = params.begin(); it != params.end(); ++it) {
        std::string key = it->first;
        core::JsonValue value = *it->second;
        
        // 键名转换
        if (config.snake_to_camel) {
            key = to_camel_case(key);
        } else if (config.camel_to_snake) {
            key = to_snake_case(key);
        }
        
        // 字符串值处理
        if (config.trim_strings && value.is_string()) {
            std::string str = value.get<std::string>();
            // 去除前后空白
            size_t start = str.find_first_not_of(" \t\n\r");
            size_t end = str.find_last_not_of(" \t\n\r");
            if (start != std::string::npos && end != std::string::npos) {
                str = str.substr(start, end - start + 1);
            }
            value = core::JsonValue(str);
        }
        
        // Null 值处理
        if (config.remove_nulls && value.is_null()) {
            continue;  // 跳过 null 值
        }
        
        // 递归处理嵌套对象
        if (value.is_object()) {
            value = normalize_tool_params(value, config);
        }
        
        normalized[key] = value;
    }
    
    return normalized;
}

} // namespace tools
} // namespace hunterclaw
