#include "hunterclaw/sessions/transcript_repairer.h"
#include "hunterclaw/sessions/transcript_sanitizer.h"
#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <set>

using json = nlohmann::json;

namespace hunterclaw {
namespace sessions {
namespace repair {

namespace {
    auto log() {
        static auto logger = spdlog::get("transcript");
        if (!logger) {
            logger = spdlog::stdout_logger_mt("transcript");
        }
        return logger;
    }
}

// ============================================================================
// Tool Call/Result 配对修复
// ============================================================================

RepairReport TranscriptRepairer::repair_tool_use_pairing(
    const std::vector<llm::LLMMessage>& messages,
    bool allow_synthetic_results
) {
    RepairReport report;
    report.messages = messages;
    
    // 1. 构建 Tool Call 索引 (tool_call_id -> message_index)
    auto tool_call_index = build_tool_call_index(messages);
    
    // 2. 查找重复的 tool results
    auto duplicates = find_duplicate_tool_results(messages);
    
    std::vector<RepairAction> actions;
    std::set<std::string> seen_tool_results;  // 已处理的 tool_call_id
    
    // 第一遍: 检测需要修复的 tool results
    for (size_t i = 0; i < messages.size(); ++i) {
        const auto& msg = messages[i];
        
        if (msg.role == llm::LLMMessageRole::Tool && !msg.tool_call_id.empty()) {
            std::string tool_call_id = msg.tool_call_id;
            
            // 检查是否是重复
            if (seen_tool_results.count(tool_call_id)) {
                // 重复的 tool result，删除
                actions.push_back({
                    RepairActionType::Delete,
                    static_cast<int>(i),
                    std::nullopt,
                    std::nullopt,
                    "duplicate_result"
                });
                report.dropped_duplicate_results++;
                log()->debug("Found duplicate tool result: {}", tool_call_id);
                continue;
            }
            
            // 检查是否有对应的 tool call
            auto it = tool_call_index.find(tool_call_id);
            if (it == tool_call_index.end()) {
                // 孤立的 tool result，删除
                actions.push_back({
                    RepairActionType::Delete,
                    static_cast<int>(i),
                    std::nullopt,
                    std::nullopt,
                    "orphan_result"
                });
                report.dropped_orphan_results++;
                log()->debug("Found orphan tool result: {}", tool_call_id);
                continue;
            }
            
            // 检查位置是否正确（应该紧跟在 tool call 后面）
            int expected_index = it->second + 1;
            if (static_cast<int>(i) != expected_index) {
                // 位置错误，移动
                actions.push_back({
                    RepairActionType::Move,
                    static_cast<int>(i),
                    expected_index,
                    std::nullopt,
                    "misplaced_result"
                });
                report.messages_moved = true;
                log()->debug("Tool result misplaced: {} (expected: {}, actual: {})", 
                            tool_call_id, expected_index, i);
            }
            
            seen_tool_results.insert(tool_call_id);
        }
    }
    
    // 第二遍: 插入合成的 tool results（缺失的情况）
    if (allow_synthetic_results) {
        for (const auto& [tool_call_id, call_index] : tool_call_index) {
            if (!seen_tool_results.count(tool_call_id)) {
                // 缺失 tool result，插入合成的错误结果
                auto synthetic_msg = create_synthetic_tool_result(
                    tool_call_id,
                    "",  // tool_name 从 call 中提取
                    "Error: tool result missing (synthetic)"
                );
                
                actions.push_back({
                    RepairActionType::Insert,
                    call_index + 1,
                    std::nullopt,
                    synthetic_msg,
                    "missing_result"
                });
                report.added_synthetic_results++;
                log()->debug("Inserting synthetic tool result for: {}", tool_call_id);
            }
        }
    }
    
    // 应用修复操作
    report.messages = apply_repairs(messages, actions);
    report.actions = actions;
    
    log()->info("Tool use pairing repair: +{} synthetic, -{} duplicates, -{} orphans, moved: {}", 
                report.added_synthetic_results,
                report.dropped_duplicate_results,
                report.dropped_orphan_results,
                report.messages_moved);
    
    return report;
}

// ============================================================================
// Tool Call 输入验证
// ============================================================================

RepairReport TranscriptRepairer::repair_tool_call_inputs(
    const std::vector<llm::LLMMessage>& messages,
    const std::vector<std::string>& tool_whitelist
) {
    RepairReport report;
    report.messages = messages;
    
    for (auto& msg : report.messages) {
        if (msg.role == llm::LLMMessageRole::Assistant && !msg.tool_calls.empty()) {
            std::vector<core::JsonValue> filtered_calls;
            
            for (auto& call_val : msg.tool_calls) {
                json call = call_val.to_json();
                
                // 检查 name
                if (!call.contains("name") || !call["name"].is_string()) {
                    report.dropped_invalid_tool_calls++;
                    log()->debug("Dropped tool call: missing name");
                    continue;
                }
                
                std::string name = call["name"].get<std::string>();
                
                // 检查白名单
                if (!tool_whitelist.empty() && 
                    std::find(tool_whitelist.begin(), tool_whitelist.end(), name) == tool_whitelist.end()) {
                    report.dropped_invalid_tool_calls++;
                    log()->debug("Dropped tool call: {} not in whitelist", name);
                    continue;
                }
                
                // 检查 id
                if (!call.contains("id") || !call["id"].is_string()) {
                    report.dropped_invalid_tool_calls++;
                    log()->debug("Dropped tool call: missing id");
                    continue;
                }
                
                // 检查 input/arguments
                if (!call.contains("input") && !call.contains("arguments")) {
                    report.dropped_invalid_tool_calls++;
                    log()->debug("Dropped tool call: missing input/arguments");
                    continue;
                }
                
                // sessions_spawn: 脱敏 attachments
                if (name == "sessions_spawn") {
                    if (call.contains("arguments")) {
                        auto args = core::JsonValue(call["arguments"]);
                        call["arguments"] = sanitizer::TranscriptSanitizer::redact_sessions_spawn_args(args).to_json();
                    }
                    if (call.contains("input")) {
                        auto args = core::JsonValue(call["input"]);
                        call["input"] = sanitizer::TranscriptSanitizer::redact_sessions_spawn_args(args).to_json();
                    }
                }
                
                filtered_calls.push_back(core::JsonValue(call));
            }
            
            msg.tool_calls = filtered_calls;
        }
    }
    
    log()->info("Tool call inputs repair: dropped {} invalid calls", 
                report.dropped_invalid_tool_calls);
    
    return report;
}

// ============================================================================
// Google Turn Ordering
// ============================================================================

std::vector<llm::LLMMessage> TranscriptRepairer::apply_google_turn_ordering(
    const std::vector<llm::LLMMessage>& messages
) {
    std::vector<llm::LLMMessage> cleaned;
    
    for (size_t i = 0; i < messages.size(); ++i) {
        const auto& curr = messages[i];
        const auto* next = (i + 1 < messages.size()) ? &messages[i + 1] : nullptr;
        
        // 移除连续的 assistant 消息（只保留最后一条）
        if (curr.role == llm::LLMMessageRole::Assistant && 
            next && next->role == llm::LLMMessageRole::Assistant) {
            log()->debug("Skipping duplicate assistant message at index {}", i);
            continue;
        }
        
        // 移除连续的 user 消息（只保留最后一条）
        if (curr.role == llm::LLMMessageRole::User && 
            next && next->role == llm::LLMMessageRole::User) {
            log()->debug("Skipping duplicate user message at index {}", i);
            continue;
        }
        
        cleaned.push_back(curr);
    }
    
    log()->info("Google turn ordering: {} -> {} messages", messages.size(), cleaned.size());
    
    return cleaned;
}

// ============================================================================
// Anthropic 消息验证
// ============================================================================

std::vector<llm::LLMMessage> TranscriptRepairer::validate_anthropic_turns(
    const std::vector<llm::LLMMessage>& messages
) {
    if (messages.empty()) {
        return messages;
    }
    
    std::vector<llm::LLMMessage> validated;
    
    // 确保第一条消息是 user
    if (messages[0].role != llm::LLMMessageRole::User) {
        log()->warn("First message is not user role, prepending empty user message");
        llm::LLMMessage user_msg;
        user_msg.role = llm::LLMMessageRole::User;
        user_msg.content = "[Context start]";
        validated.push_back(user_msg);
    }
    
    // 确保 user/assistant 交替
    llm::LLMMessageRole expected_role = llm::LLMMessageRole::User;
    
    for (const auto& msg : messages) {
        // 跳过 system 和 tool 消息（保持原样）
        if (msg.role == llm::LLMMessageRole::System || 
            msg.role == llm::LLMMessageRole::Tool) {
            validated.push_back(msg);
            continue;
        }
        
        if (msg.role != expected_role) {
            log()->warn("Unexpected message role: expected {}, got {}", 
                       (int)expected_role, (int)msg.role);
            // 插入占位消息
            llm::LLMMessage placeholder;
            placeholder.role = expected_role;
            placeholder.content = "[Placeholder]";
            validated.push_back(placeholder);
        }
        
        validated.push_back(msg);
        
        // 切换期望角色
        expected_role = (expected_role == llm::LLMMessageRole::User) 
            ? llm::LLMMessageRole::Assistant 
            : llm::LLMMessageRole::User;
    }
    
    log()->info("Anthropic validation: {} -> {} messages", messages.size(), validated.size());
    
    return validated;
}

// ============================================================================
// Gemini 消息验证
// ============================================================================

std::vector<llm::LLMMessage> TranscriptRepairer::validate_gemini_turns(
    const std::vector<llm::LLMMessage>& messages
) {
    // Gemini 验证规则类似 Google Turn Ordering
    return apply_google_turn_ordering(messages);
}

// ============================================================================
// 合成 Tool Result
// ============================================================================

llm::LLMMessage TranscriptRepairer::create_synthetic_tool_result(
    const std::string& tool_call_id,
    const std::string& tool_name,
    const std::string& error_message
) {
    llm::LLMMessage msg;
    msg.role = llm::LLMMessageRole::Tool;
    msg.tool_call_id = tool_call_id;
    msg.name = tool_name;
    msg.content = error_message;
    
    return msg;
}

// ============================================================================
// 辅助函数
// ============================================================================

std::map<std::string, int> TranscriptRepairer::build_tool_call_index(
    const std::vector<llm::LLMMessage>& messages
) {
    std::map<std::string, int> index;
    
    for (size_t i = 0; i < messages.size(); ++i) {
        const auto& msg = messages[i];
        
        if (msg.role == llm::LLMMessageRole::Assistant && !msg.tool_calls.empty()) {
            for (const auto& call_val : msg.tool_calls) {
                json call = call_val.to_json();
                if (call.contains("id") && call["id"].is_string()) {
                    std::string id = call["id"].get<std::string>();
                    index[id] = static_cast<int>(i);
                }
            }
        }
    }
    
    return index;
}

std::map<std::string, std::vector<int>> TranscriptRepairer::find_duplicate_tool_results(
    const std::vector<llm::LLMMessage>& messages
) {
    std::map<std::string, std::vector<int>> duplicates;
    
    for (size_t i = 0; i < messages.size(); ++i) {
        const auto& msg = messages[i];
        
        if (msg.role == llm::LLMMessageRole::Tool && !msg.tool_call_id.empty()) {
            std::string id = msg.tool_call_id;
            duplicates[id].push_back(static_cast<int>(i));
        }
    }
    
    // 移除非重复项
    for (auto it = duplicates.begin(); it != duplicates.end(); ) {
        if (it->second.size() <= 1) {
            it = duplicates.erase(it);
        } else {
            ++it;
        }
    }
    
    return duplicates;
}

std::vector<llm::LLMMessage> TranscriptRepairer::apply_repairs(
    const std::vector<llm::LLMMessage>& messages,
    const std::vector<RepairAction>& actions
) {
    auto result = messages;
    
    // 按索引倒序排序（避免索引偏移问题）
    auto sorted_actions = actions;
    std::sort(sorted_actions.begin(), sorted_actions.end(),
             [](const RepairAction& a, const RepairAction& b) {
                 return a.index > b.index;
             });
    
    for (const auto& action : sorted_actions) {
        switch (action.type) {
            case RepairActionType::Delete:
                if (action.index >= 0 && action.index < static_cast<int>(result.size())) {
                    result.erase(result.begin() + action.index);
                }
                break;
                
            case RepairActionType::Insert:
                if (action.message && action.index >= 0 && action.index <= static_cast<int>(result.size())) {
                    result.insert(result.begin() + action.index, action.message.value());
                }
                break;
                
            case RepairActionType::Move:
                if (action.target_index && 
                    action.index >= 0 && action.index < static_cast<int>(result.size()) &&
                    action.target_index.value() >= 0 && action.target_index.value() <= static_cast<int>(result.size())) {
                    auto msg = result[action.index];
                    result.erase(result.begin() + action.index);
                    
                    int target = action.target_index.value();
                    if (target > action.index) {
                        target--;  // 调整索引偏移
                    }
                    
                    result.insert(result.begin() + target, msg);
                }
                break;
        }
    }
    
    return result;
}

bool TranscriptRepairer::is_valid_tool_call(
    const core::JsonValue& tool_call,
    const std::vector<std::string>& tool_whitelist
) {
    auto j = tool_call.to_json();
    
    // 检查必需字段
    if (!j.contains("name") || !j.contains("id")) {
        return false;
    }
    
    if (!j.contains("input") && !j.contains("arguments")) {
        return false;
    }
    
    // 检查白名单
    if (!tool_whitelist.empty()) {
        std::string name = j["name"].get<std::string>();
        return std::find(tool_whitelist.begin(), tool_whitelist.end(), name) != tool_whitelist.end();
    }
    
    return true;
}

} // namespace repair
} // namespace sessions
} // namespace hunterclaw
