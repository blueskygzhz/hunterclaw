// HunterClaw - Tool 循环检测器实现

#include "hunterclaw/tools/tool_loop_detector.h"
#include "hunterclaw/tools/tool_hooks.h"
#include "hunterclaw/tools/tool.h"
#include <spdlog/spdlog.h>
#include <algorithm>
#include <sstream>
#include <iomanip>

// 简单 hash 实现 (生产环境建议用 SHA256)
#include <functional>

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
// ToolLoopDetector 实现
// ============================================================================

ToolLoopDetector::ToolLoopDetector(const LoopDetectorConfig& config)
    : config_(config) {
}

void ToolLoopDetector::record_call(
    const std::string& tool_name,
    const core::JsonValue& params,
    bool success,
    const std::string& result
) {
    cleanup_old_records();
    
    ToolCallRecord record;
    record.tool_name = tool_name;
    record.params = params;
    record.timestamp = std::chrono::system_clock::now();
    record.success = success;
    record.result_hash = compute_hash(result);
    
    call_history_.push_back(record);
    
    // 限制窗口大小
    if (call_history_.size() > config_.window_size) {
        call_history_.pop_front();
    }
}

LoopDetectionResult ToolLoopDetector::detect_loop() {
    cleanup_old_records();
    
    if (call_history_.size() < 3) {
        return LoopDetectionResult{false, "", 0, {}, ""};
    }
    
    // 1. 检测完全相同的重复
    auto exact_result = detect_exact_repeats();
    if (exact_result.is_looping) {
        return exact_result;
    }
    
    // 2. 检测乒乓循环
    auto ping_pong_result = detect_ping_pong_loops();
    if (ping_pong_result.is_looping) {
        return ping_pong_result;
    }
    
    // 3. 检测模式循环
    auto pattern_result = detect_pattern_loops();
    if (pattern_result.is_looping) {
        return pattern_result;
    }
    
    return LoopDetectionResult{false, "", 0, {}, ""};
}

LoopDetectionResult ToolLoopDetector::detect_exact_repeats() {
    if (call_history_.size() < config_.exact_repeat_threshold) {
        return LoopDetectionResult{false, "", 0, {}, ""};
    }
    
    // 检查最后 N 次调用是否完全相同
    const auto& last_call = call_history_.back();
    int repeat_count = 1;
    
    for (auto it = call_history_.rbegin() + 1; it != call_history_.rend(); ++it) {
        if (it->tool_name != last_call.tool_name) {
            break;
        }
        
        if (config_.compare_params && !params_similar(it->params, last_call.params)) {
            break;
        }
        
        if (config_.compare_results && it->result_hash != last_call.result_hash) {
            break;
        }
        
        repeat_count++;
    }
    
    if (repeat_count >= config_.exact_repeat_threshold) {
        LoopDetectionResult result;
        result.is_looping = true;
        result.pattern = "exact_repeat";
        result.loop_count = repeat_count;
        result.loop_sequence = {last_call.tool_name};
        result.suggestion = "Tool '" + last_call.tool_name + "' called " + 
                           std::to_string(repeat_count) + 
                           " times with same parameters. Consider different approach.";
        return result;
    }
    
    return LoopDetectionResult{false, "", 0, {}, ""};
}

LoopDetectionResult ToolLoopDetector::detect_ping_pong_loops() {
    if (call_history_.size() < 4) {
        return LoopDetectionResult{false, "", 0, {}, ""};
    }
    
    // 检测 A->B->A->B 模式
    auto it = call_history_.rbegin();
    const std::string& t1 = it->tool_name;
    const std::string& t2 = (it + 1)->tool_name;
    
    if (t1 == t2) {
        return LoopDetectionResult{false, "", 0, {}, ""};
    }
    
    int ping_pong_count = 1;
    for (size_t i = 2; i + 1 < call_history_.size(); i += 2) {
        auto a = call_history_.rbegin() + i;
        auto b = call_history_.rbegin() + i + 1;
        
        if (a->tool_name != t1 || b->tool_name != t2) {
            break;
        }
        
        ping_pong_count++;
    }
    
    if (ping_pong_count >= config_.pattern_repeat_threshold) {
        LoopDetectionResult result;
        result.is_looping = true;
        result.pattern = "ping_pong";
        result.loop_count = ping_pong_count;
        result.loop_sequence = {t1, t2};
        result.suggestion = "Detected ping-pong between '" + t1 + "' and '" + t2 + 
                           "' (" + std::to_string(ping_pong_count) + " cycles). " +
                           "Try a different strategy.";
        return result;
    }
    
    return LoopDetectionResult{false, "", 0, {}, ""};
}

LoopDetectionResult ToolLoopDetector::detect_pattern_loops() {
    if (call_history_.size() < 6) {
        return LoopDetectionResult{false, "", 0, {}, ""};
    }
    
    // 尝试不同的模式长度 (3-8)
    for (size_t pattern_len = 3; pattern_len <= 8 && pattern_len * 2 <= call_history_.size(); pattern_len++) {
        std::vector<std::string> pattern;
        for (size_t i = 0; i < pattern_len; i++) {
            pattern.push_back((call_history_.rbegin() + i)->tool_name);
        }
        
        // 检查模式是否重复
        int repeat_count = 1;
        for (size_t offset = pattern_len; offset + pattern_len <= call_history_.size(); offset += pattern_len) {
            bool match = true;
            for (size_t i = 0; i < pattern_len; i++) {
                if ((call_history_.rbegin() + offset + i)->tool_name != pattern[i]) {
                    match = false;
                    break;
                }
            }
            
            if (!match) {
                break;
            }
            
            repeat_count++;
        }
        
        if (repeat_count >= config_.pattern_repeat_threshold) {
            LoopDetectionResult result;
            result.is_looping = true;
            result.pattern = "cyclic_pattern";
            result.loop_count = repeat_count;
            result.loop_sequence = pattern;
            
            std::ostringstream ss;
            ss << "Detected cyclic pattern: ";
            for (size_t i = 0; i < pattern.size(); i++) {
                if (i > 0) ss << " -> ";
                ss << pattern[i];
            }
            ss << " (repeated " << repeat_count << " times). Consider breaking the cycle.";
            result.suggestion = ss.str();
            
            return result;
        }
    }
    
    return LoopDetectionResult{false, "", 0, {}, ""};
}

// Helper function to serialize JsonValue to string for comparison
namespace {
std::string json_value_to_string(const core::JsonValue& val) {
    if (val.is_null()) return "null";
    if (val.is_bool()) return val.as_bool() ? "true" : "false";
    if (val.is_number()) return std::to_string(val.as_number());
    if (val.is_string()) return "\"" + val.as_string() + "\"";
    if (val.is_array()) {
        // Simplified array representation
        return "[array]";
    }
    if (val.is_object()) {
        // Simplified object representation
        return "{object}";
    }
    return "unknown";
}
}

bool ToolLoopDetector::params_similar(const core::JsonValue& p1, const core::JsonValue& p2) {
    // 简单比较: 转为字符串后比较
    // 生产环境可以用更精细的 JSON diff
    return json_value_to_string(p1) == json_value_to_string(p2);
}

std::string ToolLoopDetector::compute_hash(const std::string& content) {
    if (content.empty()) {
        return "";
    }
    
    // 简单 hash (生产环境用 SHA256)
    std::hash<std::string> hasher;
    auto hash_value = hasher(content);
    
    std::ostringstream ss;
    ss << std::hex << std::setfill('0') << std::setw(16) << hash_value;
    return ss.str();
}

void ToolLoopDetector::cleanup_old_records() {
    if (config_.time_window_seconds <= 0) {
        return;
    }
    
    auto now = std::chrono::system_clock::now();
    auto cutoff = now - std::chrono::seconds(config_.time_window_seconds);
    
    while (!call_history_.empty() && call_history_.front().timestamp < cutoff) {
        call_history_.pop_front();
    }
}

void ToolLoopDetector::clear_history() {
    call_history_.clear();
}

const std::deque<ToolCallRecord>& ToolLoopDetector::get_history() const {
    return call_history_;
}

void ToolLoopDetector::set_config(const LoopDetectorConfig& config) {
    config_ = config;
}

const LoopDetectorConfig& ToolLoopDetector::get_config() const {
    return config_;
}

// ============================================================================
// 内置循环检测 Hooks
// ============================================================================

namespace builtin_loop_detectors {

std::function<BeforeHookOutcome(const std::string&, const core::JsonValue&, const ToolContext&)>
create_loop_detection_hook(std::shared_ptr<ToolLoopDetector> detector) {
    return [detector](const std::string& tool_name, 
                      const core::JsonValue& params,
                      const ToolContext& ctx) -> BeforeHookOutcome {
        BeforeHookOutcome outcome;
        outcome.blocked = false;
        outcome.params = params;
        
        if (!detector) {
            return outcome;
        }
        
        // 检测循环
        auto loop_result = detector->detect_loop();
        if (loop_result.is_looping) {
            log()->warn("Loop detected: {}", loop_result.suggestion);
            
            outcome.blocked = true;
            outcome.reason = "Tool loop detected: " + loop_result.suggestion;
        }
        
        return outcome;
    };
}

std::function<void(const std::string&, const core::JsonValue&, const ToolResult&, double, const std::string&)>
create_loop_recording_hook(std::shared_ptr<ToolLoopDetector> detector) {
    return [detector](const std::string& tool_name,
                      const core::JsonValue& params,
                      const ToolResult& result,
                      double duration_ms,
                      const std::string& error_msg) {
        if (!detector) {
            return;
        }
        
        // 记录调用
        bool success = result.success;
        std::string result_str;
        if (!result.content.empty() && result.content[0].type == ToolResultContent::Type::Text) {
            result_str = result.content[0].text;
        }
        
        detector->record_call(tool_name, params, success, result_str);
    };
}

}  // namespace builtin_loop_detectors

}  // namespace tools
}  // namespace hunterclaw
