#include "hunterclaw/react/loop_detection.h"
#include <sstream>
#include <algorithm>
#include <openssl/sha.h>
#include <iomanip>

namespace hunterclaw {
namespace react {

namespace {

// SHA256 哈希计算
std::string compute_sha256(const std::string& data) {
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(data.c_str()), data.size(), hash);
    
    std::ostringstream oss;
    for (int i = 0; i < SHA256_DIGEST_LENGTH; i++) {
        oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(hash[i]);
    }
    return oss.str();
}

// JSON 序列化 (对齐 OpenClaw)
std::string serialize_json(const core::JsonValue& json) {
    // ✅ 对齐 OpenClaw: 使用 to_json() 转换为 nlohmann::json 再序列化
    try {
        return json.to_json().dump();
    } catch (...) {
        return "null";
    }
}

} // namespace

// LoopDetectionState 实现
std::shared_ptr<LoopDetectionState> LoopDetectionState::create(
    const LoopDetectionConfig& config
) {
    return std::shared_ptr<LoopDetectionState>(new LoopDetectionState(config));
}

LoopDetectionState::LoopDetectionState(const LoopDetectionConfig& config)
    : config_(config) {
}

LoopDetectionResult LoopDetectionState::detect_tool_call_loop(
    const std::string& tool_name,
    const core::JsonValue& params
) {
    if (!config_.enabled) {
        return LoopDetectionResult{};
    }
    
    // 1. 全局断路器 (最高优先级)
    auto global_result = detect_global_circuit_breaker();
    if (global_result.stuck) {
        return global_result;
    }
    
    std::string params_hash = compute_params_hash(params);
    
    // 2. 通用重复检测
    if (config_.detectors.generic_repeat) {
        auto repeat_result = detect_generic_repeat(tool_name, params_hash);
        if (repeat_result.stuck && repeat_result.level == LoopDetectionResult::Level::Critical) {
            return repeat_result;
        }
    }
    
    // 3. 无进展轮询检测
    if (config_.detectors.known_poll_no_progress) {
        auto poll_result = detect_known_poll_no_progress(tool_name, params_hash);
        if (poll_result.stuck && poll_result.level == LoopDetectionResult::Level::Critical) {
            return poll_result;
        }
    }
    
    // 4. 乒乓循环检测
    if (config_.detectors.ping_pong) {
        auto ping_pong_result = detect_ping_pong(tool_name);
        if (ping_pong_result.stuck) {
            return ping_pong_result;
        }
    }
    
    return LoopDetectionResult{};
}

void LoopDetectionState::record_tool_call(
    const std::string& tool_name,
    const core::JsonValue& params,
    const std::string& tool_call_id
) {
    ToolCallRecord record;
    record.tool_name = tool_name;
    record.params_hash = compute_params_hash(params);
    record.timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
    record.tool_call_id = tool_call_id;
    record.executed = false;
    
    history_.push_back(record);
    
    // 维护滑动窗口
    if (history_.size() > config_.history_size) {
        history_.erase(history_.begin());
    }
    
    // 更新计数
    tool_name_counts_[tool_name]++;
    
    std::string key = tool_name + ":" + record.params_hash;
    tool_params_counts_[key]++;
}

void LoopDetectionState::record_tool_call_outcome(
    const std::string& tool_name,
    const tools::ToolResult& result,
    const std::string& error
) {
    // 查找最近的未执行记录
    for (auto it = history_.rbegin(); it != history_.rend(); ++it) {
        if (it->tool_name == tool_name && !it->executed) {
            it->executed = true;
            it->success = result.success;
            it->result_hash = compute_result_hash(result);
            it->error = error;
            
            // 检查无进展 (相同参数 + 相同结果)
            if (loop_detection::is_known_polling_tool(tool_name)) {
                std::string no_progress_key = tool_name + ":" + it->params_hash + ":" + it->result_hash;
                no_progress_counts_[no_progress_key]++;
            }
            
            break;
        }
    }
}

void LoopDetectionState::reset() {
    history_.clear();
    tool_name_counts_.clear();
    tool_params_counts_.clear();
    no_progress_counts_.clear();
}

std::vector<std::string> LoopDetectionState::recent_tool_names(int count) const {
    std::vector<std::string> result;
    int start = std::max(0, static_cast<int>(history_.size()) - count);
    
    for (size_t i = start; i < history_.size(); i++) {
        result.push_back(history_[i].tool_name);
    }
    
    return result;
}

// 检测器实现
LoopDetectionResult LoopDetectionState::detect_generic_repeat(
    const std::string& tool_name,
    const std::string& params_hash
) {
    std::string key = tool_name + ":" + params_hash;
    int count = tool_params_counts_[key];
    
    LoopDetectionResult result;
    result.detector = LoopDetectorKind::GenericRepeat;
    result.count = count;
    
    if (count >= config_.critical_threshold) {
        result.stuck = true;
        result.level = LoopDetectionResult::Level::Critical;
        result.message = "Tool '" + tool_name + "' called " + std::to_string(count) + 
                        " times with identical parameters. This appears to be a loop.";
        result.warning_key = "generic_repeat:" + key;
    } else if (count >= config_.warning_threshold) {
        result.stuck = true;
        result.level = LoopDetectionResult::Level::Warning;
        result.message = "Tool '" + tool_name + "' has been called " + std::to_string(count) + 
                        " times with the same parameters. Consider a different approach.";
        result.warning_key = "generic_repeat:" + key;
    }
    
    return result;
}

LoopDetectionResult LoopDetectionState::detect_known_poll_no_progress(
    const std::string& tool_name,
    const std::string& params_hash
) {
    if (!loop_detection::is_known_polling_tool(tool_name)) {
        return LoopDetectionResult{};
    }
    
    // 检查最近的调用是否有相同的结果
    int no_progress_count = 0;
    std::string last_result_hash;
    
    for (auto it = history_.rbegin(); it != history_.rend(); ++it) {
        if (it->tool_name == tool_name && 
            it->params_hash == params_hash && 
            it->executed) {
            
            if (last_result_hash.empty()) {
                last_result_hash = it->result_hash;
                no_progress_count = 1;
            } else if (it->result_hash == last_result_hash) {
                no_progress_count++;
            } else {
                break;  // 结果变化了
            }
        }
    }
    
    LoopDetectionResult result;
    result.detector = LoopDetectorKind::KnownPollNoProgress;
    result.count = no_progress_count;
    
    if (no_progress_count >= config_.critical_threshold / 2) {  // 更严格的阈值
        result.stuck = true;
        result.level = LoopDetectionResult::Level::Critical;
        result.message = "Tool '" + tool_name + "' has been polling " + std::to_string(no_progress_count) + 
                        " times with no change in results. This appears to be stuck.";
        result.warning_key = "no_progress:" + tool_name + ":" + params_hash;
    } else if (no_progress_count >= config_.warning_threshold / 2) {
        result.stuck = true;
        result.level = LoopDetectionResult::Level::Warning;
        result.message = "Tool '" + tool_name + "' has been polling " + std::to_string(no_progress_count) + 
                        " times with no progress. Consider a different approach.";
        result.warning_key = "no_progress:" + tool_name + ":" + params_hash;
    }
    
    return result;
}

LoopDetectionResult LoopDetectionState::detect_global_circuit_breaker() {
    int total_calls = history_.size();
    
    LoopDetectionResult result;
    result.detector = LoopDetectorKind::GlobalCircuitBreaker;
    result.count = total_calls;
    
    if (total_calls >= config_.global_circuit_breaker_threshold) {
        result.stuck = true;
        result.level = LoopDetectionResult::Level::Critical;
        result.message = "Global circuit breaker triggered: " + std::to_string(total_calls) + 
                        " tool calls in this session. Stopping to prevent infinite loops.";
        result.warning_key = "global_circuit_breaker";
    }
    
    return result;
}

LoopDetectionResult LoopDetectionState::detect_ping_pong(
    const std::string& tool_name
) {
    // 检查乒乓模式: A→B→A→B→A→B...
    // 至少需要 6 次调用 (3个完整周期)
    if (history_.size() < 6) {
        return LoopDetectionResult{};
    }
    
    // 获取最近的工具调用序列
    std::vector<std::string> recent;
    int look_back = std::min(12, static_cast<int>(history_.size()));
    
    for (int i = history_.size() - look_back; i < history_.size(); i++) {
        recent.push_back(history_[i].tool_name);
    }
    
    // 检查交替模式
    if (recent.size() >= 6) {
        std::string tool_a = recent[recent.size() - 1];
        std::string tool_b;
        
        // 找到配对工具
        for (int i = recent.size() - 2; i >= 0; i--) {
            if (recent[i] != tool_a) {
                tool_b = recent[i];
                break;
            }
        }
        
        if (!tool_b.empty()) {
            // 计算交替次数
            int alternations = 0;
            std::string expected = tool_a;
            
            for (auto it = recent.rbegin(); it != recent.rend(); ++it) {
                if (*it == expected) {
                    alternations++;
                    expected = (expected == tool_a) ? tool_b : tool_a;
                } else {
                    break;
                }
            }
            
            LoopDetectionResult result;
            result.detector = LoopDetectorKind::PingPong;
            result.count = alternations;
            result.paired_tool_name = tool_b;
            
            if (alternations >= 8) {  // 4 个完整周期
                result.stuck = true;
                result.level = LoopDetectionResult::Level::Critical;
                result.message = "Ping-pong loop detected between '" + tool_a + "' and '" + tool_b + 
                                "' (" + std::to_string(alternations) + " alternations). " +
                                "These tools are calling each other repeatedly without progress.";
                result.warning_key = "ping_pong:" + tool_a + ":" + tool_b;
                return result;
            } else if (alternations >= 6) {  // 3 个完整周期
                result.stuck = true;
                result.level = LoopDetectionResult::Level::Warning;
                result.message = "Possible ping-pong loop between '" + tool_a + "' and '" + tool_b + 
                                "' (" + std::to_string(alternations) + " alternations).";
                result.warning_key = "ping_pong:" + tool_a + ":" + tool_b;
                return result;
            }
        }
    }
    
    return LoopDetectionResult{};
}

std::string LoopDetectionState::compute_params_hash(const core::JsonValue& params) {
    std::string serialized = serialize_json(params);
    return compute_sha256(serialized);
}

std::string LoopDetectionState::compute_result_hash(const tools::ToolResult& result) {
    std::ostringstream oss;
    oss << (result.success ? "success" : "failure");
    
    for (const auto& content : result.content) {
        if (content.type == tools::ToolResultContent::Type::Text) {
            oss << content.text;
        } else if (content.type == tools::ToolResultContent::Type::Json) {
            oss << content.text;
        }
    }
    
    if (!result.error.empty()) {
        oss << "error:" << result.error;
    }
    
    return compute_sha256(oss.str());
}

// 辅助函数实现
namespace loop_detection {

std::string create_loop_summary(
    const LoopDetectionResult& result,
    const std::vector<ToolCallRecord>& recent_calls
) {
    std::ostringstream oss;
    oss << "Loop detected (" << result.count << " occurrences):\n";
    oss << "Detector: ";
    
    switch (result.detector) {
        case LoopDetectorKind::GenericRepeat:
            oss << "Generic Repeat\n";
            break;
        case LoopDetectorKind::KnownPollNoProgress:
            oss << "Known Poll (No Progress)\n";
            break;
        case LoopDetectorKind::GlobalCircuitBreaker:
            oss << "Global Circuit Breaker\n";
            break;
        case LoopDetectorKind::PingPong:
            oss << "Ping-Pong (alternating between tools)\n";
            if (!result.paired_tool_name.empty()) {
                oss << "Paired tool: " << result.paired_tool_name << "\n";
            }
            break;
    }
    
    oss << "\nRecent tool calls:\n";
    int count = std::min(10, static_cast<int>(recent_calls.size()));
    for (int i = recent_calls.size() - count; i < recent_calls.size(); i++) {
        oss << "  " << (i + 1) << ". " << recent_calls[i].tool_name;
        if (recent_calls[i].executed) {
            oss << " (" << (recent_calls[i].success ? "success" : "failed") << ")";
        }
        oss << "\n";
    }
    
    return oss.str();
}

bool is_known_polling_tool(const std::string& tool_name) {
    // 对齐 OpenClaw 的已知轮询工具
    static const std::vector<std::string> polling_tools = {
        "read_file",
        "list_dir",
        "search_file",
        "search_content",
        "execute_command",  // 检查命令状态
        "get_process_status",
        "check_file_exists"
    };
    
    return std::find(polling_tools.begin(), polling_tools.end(), tool_name) != polling_tools.end();
}

} // namespace loop_detection

} // namespace react
} // namespace hunterclaw
