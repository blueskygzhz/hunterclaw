#include "hunterclaw/auto_reply/heartbeat.h"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <regex>

namespace hunterclaw {
namespace auto_reply {

// ═══════════════════════════════════════════════════════════════
// ✅ Heartbeat 实现 - 严格对齐 OpenClaw
// ═══════════════════════════════════════════════════════════════

// 对齐 OpenClaw: stripHeartbeatToken (line 66-73)
std::string Heartbeat::strip_heartbeat_token(const std::string& text) {
    // 查找 HEARTBEAT_OK 令牌
    size_t pos = text.find(HEARTBEAT_TOKEN);
    if (pos == std::string::npos) {
        return text;  // 未找到令牌,返回原文
    }
    
    // 移除令牌及其周围的空白
    std::string result = text;
    result.erase(pos, std::string(HEARTBEAT_TOKEN).length());
    
    // 去除首尾空白
    return trim(result);
}

// 对齐 OpenClaw: resolveHeartbeatPrompt (line 82-100)
std::string Heartbeat::resolve_heartbeat_prompt(
    const std::string& workspace_path,
    const std::optional<std::string>& config_prompt
) {
    // 1. 如果配置了 prompt,直接使用
    if (config_prompt.has_value() && !config_prompt->empty()) {
        return *config_prompt;
    }
    
    // 2. 尝试读取 HEARTBEAT.md
    std::string heartbeat_file = workspace_path + "/HEARTBEAT.md";
    std::ifstream file(heartbeat_file);
    
    if (file.is_open()) {
        std::stringstream buffer;
        buffer << file.rdbuf();
        std::string content = buffer.str();
        
        // 检查内容是否有效
        if (!is_heartbeat_content_effectively_empty(content)) {
            return content;
        }
    }
    
    // 3. 使用默认提示词
    return DEFAULT_HEARTBEAT_PROMPT;
}

// 对齐 OpenClaw: isHeartbeatContentEffectivelyEmpty (line 107-122)
bool Heartbeat::is_heartbeat_content_effectively_empty(
    const std::string& content
) {
    if (content.empty()) {
        return true;
    }
    
    // 去除空白
    std::string trimmed = trim(content);
    if (trimmed.empty()) {
        return true;
    }
    
    // 检查是否只包含注释 (以 # 开头的行)
    std::istringstream stream(trimmed);
    std::string line;
    bool has_non_comment = false;
    
    while (std::getline(stream, line)) {
        std::string trimmed_line = trim(line);
        if (!trimmed_line.empty() && trimmed_line[0] != '#') {
            has_non_comment = true;
            break;
        }
    }
    
    return !has_non_comment;
}

// ✅ 处理心跳响应 (增强版)
Heartbeat::Result Heartbeat::process_heartbeat_response(
    const std::string& response,
    const Config& config
) {
    Result result;
    result.response = response;
    result.timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
    
    // 1. 检测是否包含 HEARTBEAT_OK 令牌
    result.is_ok = contains_heartbeat_token(response);
    
    // 2. 移除令牌
    result.processed_response = strip_heartbeat_token(response);
    
    // 3. 判断是否为警报
    result.is_alert = !result.is_ok && !result.processed_response.empty();
    
    // 4. 根据配置决定是否应该发送
    if (result.is_ok) {
        // HEARTBEAT_OK 情况
        if (config.use_indicator) {
            // 使用指示器而非发送消息
            result.should_send = false;
        } else if (config.show_ok) {
            // 发送 OK 消息
            result.should_send = true;
        } else {
            // 抑制发送
            result.should_send = false;
        }
    } else {
        // 警报情况
        if (config.show_alerts) {
            result.should_send = true;
        } else {
            result.should_send = false;
        }
    }
    
    // 5. 检查长度限制
    if (result.should_send && 
        result.processed_response.length() > static_cast<size_t>(config.ack_max_chars)) {
        result.processed_response = result.processed_response.substr(
            0, config.ack_max_chars
        ) + "...";
    }
    
    return result;
}

// ✅ 检测是否包含心跳令牌
bool Heartbeat::contains_heartbeat_token(const std::string& text) {
    return text.find(HEARTBEAT_TOKEN) != std::string::npos;
}

// ✅ 辅助函数: 去除空白
std::string Heartbeat::trim(const std::string& text) {
    size_t start = text.find_first_not_of(" \t\n\r");
    if (start == std::string::npos) {
        return "";
    }
    
    size_t end = text.find_last_not_of(" \t\n\r");
    return text.substr(start, end - start + 1);
}

// ✅ 辅助函数: 检查是否为空或只含空白
bool Heartbeat::is_empty_or_whitespace(const std::string& text) {
    return trim(text).empty();
}

// ═══════════════════════════════════════════════════════════════
// ✅ HeartbeatRunner 实现 - 严格对齐 OpenClaw
// ═══════════════════════════════════════════════════════════════

// 对齐 OpenClaw: runWebHeartbeatOnce (line 53-317)
HeartbeatRunner::RunResult HeartbeatRunner::run_once(
    const RunOptions& options
) {
    RunResult result;
    
    // TODO: 完整实现需要集成以下组件:
    // 1. SessionManager - 加载/更新会话
    // 2. LLMClient - 调用 LLM 生成响应
    // 3. ChannelManager - 发送消息
    // 4. MemoryManager - 加载 Memory 上下文
    
    // 这是框架代码,展示完整流程:
    
    // 步骤 1: 加载会话快照
    // auto session = session_manager->get_session(options.session_key);
    // int64_t original_updated_at = session.updated_at;
    
    // 步骤 2: 解析心跳提示词
    std::string prompt = Heartbeat::resolve_heartbeat_prompt(
        options.workspace_path,
        options.config.prompt.empty() ? 
            std::nullopt : std::make_optional(options.config.prompt)
    );
    
    // 步骤 3: 调用 LLM (带 isHeartbeat: true 标记)
    // auto llm_response = llm_client->generate({
    //     .prompt = prompt,
    //     .model = options.config.model,
    //     .is_heartbeat = true
    // });
    
    // 步骤 4: 处理心跳响应
    // result.heartbeat_result = Heartbeat::process_heartbeat_response(
    //     llm_response.text,
    //     options.config
    // );
    
    // 步骤 5: 根据结果决定是否发送
    // if (result.heartbeat_result.should_send) {
    //     // 发送消息到接收者
    //     auto recipients = resolve_heartbeat_recipients(
    //         options.agent_id,
    //         options.recipients
    //     );
    //     
    //     for (const auto& recipient : recipients) {
    //         channel_manager->send_message(recipient, 
    //             result.heartbeat_result.processed_response);
    //     }
    //     
    //     result.sent_message = true;
    // } else if (options.config.use_indicator) {
    //     // 使用指示器
    //     result.used_indicator = true;
    // }
    
    // 步骤 6: 恢复会话 updatedAt (防止心跳保持会话活跃)
    // restore_session_updated_at(options.session_key, original_updated_at);
    
    result.success = true;
    return result;
}

// 对齐 OpenClaw: resolveHeartbeatRecipients (line 36-50)
std::vector<std::string> HeartbeatRunner::resolve_heartbeat_recipients(
    const std::string& channel,
    const std::vector<std::string>& configured_recipients
) {
    // 如果配置了接收者,使用配置
    if (!configured_recipients.empty()) {
        return configured_recipients;
    }
    
    // 否则,返回默认接收者 (通常是 agent 所有者)
    // TODO: 从配置或数据库读取默认接收者
    return {};
}

// 恢复会话 updatedAt
void HeartbeatRunner::restore_session_updated_at(
    const std::string& session_key,
    int64_t original_updated_at
) {
    // TODO: 调用 SessionManager 恢复 updatedAt
    // session_manager->update_session_timestamp(session_key, original_updated_at);
}

} // namespace auto_reply
} // namespace hunterclaw
