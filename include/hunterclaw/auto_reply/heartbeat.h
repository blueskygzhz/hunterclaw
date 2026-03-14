#pragma once

#include <string>
#include <chrono>
#include <optional>
#include <vector>
#include <memory>

namespace hunterclaw {
namespace auto_reply {

// ═══════════════════════════════════════════════════════════════
// ✅ 严格对齐 OpenClaw: src/auto-reply/heartbeat.ts
// 心跳机制 (Heartbeat)
// ═══════════════════════════════════════════════════════════════

class Heartbeat {
public:
    // ✅ 对齐 OpenClaw: 心跳常量 (line 8-16)
    
    // 默认心跳提示词
    static constexpr const char* DEFAULT_HEARTBEAT_PROMPT = 
        "Check for any important alerts, notifications, or updates that need attention. "
        "If everything is okay, simply respond with 'HEARTBEAT_OK'. "
        "Only send a message if there's something important to report.";
    
    // 心跳令牌 (用于识别 "一切正常" 的响应)
    static constexpr const char* HEARTBEAT_TOKEN = "HEARTBEAT_OK";
    
    // 默认心跳间隔
    static constexpr std::chrono::seconds DEFAULT_HEARTBEAT_EVERY{1800};  // 30 分钟
    
    // 默认最大确认字符数
    static constexpr int DEFAULT_HEARTBEAT_ACK_MAX_CHARS = 300;
    
    // ✅ 心跳配置
    struct Config {
        std::string prompt = DEFAULT_HEARTBEAT_PROMPT;
        std::optional<std::string> model;  // 可覆盖 agent 默认模型
        int ack_max_chars = DEFAULT_HEARTBEAT_ACK_MAX_CHARS;
        
        // 可见性配置
        bool show_alerts = true;   // 显示警报 (默认: true)
        bool show_ok = false;      // 显示 OK 消息 (默认: false)
        bool use_indicator = true; // 使用指示器而非发送消息 (默认: true)
        
        // 直接消息策略
        enum class DirectPolicy {
            Allow,
            Block
        };
        DirectPolicy direct_policy = DirectPolicy::Allow;
    };
    
    // ✅ 心跳结果
    struct Result {
        bool is_ok = false;              // 是否为 HEARTBEAT_OK
        bool should_send = false;        // 是否应该发送给用户
        std::string response;            // LLM 响应内容
        std::string processed_response;  // 处理后的响应 (移除 token)
        bool is_alert = false;           // 是否为警报
        int64_t timestamp_ms = 0;        // 时间戳
    };
    
    // ✅ 对齐 OpenClaw: stripHeartbeatToken (line 66-73)
    // 移除心跳令牌
    static std::string strip_heartbeat_token(const std::string& text);
    
    // ✅ 对齐 OpenClaw: resolveHeartbeatPrompt (line 82-100)
    // 解析心跳提示词
    static std::string resolve_heartbeat_prompt(
        const std::string& workspace_path,
        const std::optional<std::string>& config_prompt
    );
    
    // ✅ 对齐 OpenClaw: isHeartbeatContentEffectivelyEmpty (line 107-122)
    // 检查 HEARTBEAT.md 是否为空
    static bool is_heartbeat_content_effectively_empty(
        const std::string& content
    );
    
    // ✅ 处理心跳响应 (增强版)
    static Result process_heartbeat_response(
        const std::string& response,
        const Config& config
    );
    
    // ✅ 检测是否包含心跳令牌
    static bool contains_heartbeat_token(const std::string& text);
    
private:
    // 辅助函数: 去除空白
    static std::string trim(const std::string& text);
    
    // 辅助函数: 检查是否为空或只含空白
    static bool is_empty_or_whitespace(const std::string& text);
};

// ═══════════════════════════════════════════════════════════════
// ✅ 严格对齐 OpenClaw: src/web/auto-reply/heartbeat-runner.ts
// 心跳执行器 (Heartbeat Runner)
// ═══════════════════════════════════════════════════════════════

class HeartbeatRunner {
public:
    // ✅ 执行一次心跳检查
    // 对齐 OpenClaw: runWebHeartbeatOnce (line 53-317)
    struct RunOptions {
        std::string agent_id;
        std::string workspace_path;
        std::string session_key;
        Heartbeat::Config config;
        
        // 接收者列表 (可选)
        std::vector<std::string> recipients;
    };
    
    struct RunResult {
        bool success = false;
        bool sent_message = false;      // 是否发送了消息
        bool used_indicator = false;    // 是否使用了指示器
        Heartbeat::Result heartbeat_result;
        std::string error;
    };
    
    static RunResult run_once(const RunOptions& options);
    
    // ✅ 解析心跳接收者
    // 对齐 OpenClaw: resolveHeartbeatRecipients (line 36-50)
    static std::vector<std::string> resolve_heartbeat_recipients(
        const std::string& channel,
        const std::vector<std::string>& configured_recipients
    );
    
private:
    // 恢复会话 updatedAt (防止心跳保持会话活跃)
    // 对齐 OpenClaw: line 289-300
    static void restore_session_updated_at(
        const std::string& session_key,
        int64_t original_updated_at
    );
};

} // namespace auto_reply
} // namespace hunterclaw
