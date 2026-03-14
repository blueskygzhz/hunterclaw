#pragma once

#include <string>
#include <regex>
#include <optional>

namespace hunterclaw {
namespace auto_reply {

// ═══════════════════════════════════════════════════════════════
// ✅ 严格对齐 OpenClaw: src/auto-reply/command-detection.ts
// 命令检测器 (Command Detection)
// ═══════════════════════════════════════════════════════════════

class CommandDetector {
public:
    // 命令类型
    enum class CommandType {
        None,
        Slash,      // /command
        Bang,       // !command
        Inline      // 内联命令
    };
    
    // 检测结果
    struct DetectionResult {
        CommandType type = CommandType::None;
        std::string command;         // 命令名称 (例如: "status", "new")
        std::string args;            // 命令参数
        bool is_control_command = false;
        bool needs_authorization = false;
    };
    
    // ✅ 对齐 OpenClaw: hasControlCommand
    // 检测是否包含控制命令
    static bool has_control_command(const std::string& text);
    
    // ✅ 对齐 OpenClaw: isControlCommandMessage
    // 检测是否为控制命令消息
    static bool is_control_command_message(const std::string& text);
    
    // ✅ 对齐 OpenClaw: hasInlineCommandTokens
    // 检测是否包含内联命令标记
    static bool has_inline_command_tokens(const std::string& text);
    
    // ✅ 对齐 OpenClaw: shouldComputeCommandAuthorized
    // 判断是否需要计算命令授权
    static bool should_compute_command_authorized(
        const std::string& text,
        bool is_owner
    );
    
    // ✅ 完整的命令检测 (增强版)
    static DetectionResult detect_command(const std::string& text);
    
    // ✅ 检测是否为 owner-only 命令
    static bool is_owner_only_command(const std::string& command);
    
private:
    // 正则表达式 (对齐 OpenClaw)
    
    // ✅ 控制命令正则: /^\s*[!/]([a-z]+)/i
    static std::regex control_command_regex_;
    
    // ✅ 内联命令正则: /[!/][a-z]/i
    static std::regex inline_command_regex_;
    
    // ✅ owner-only 命令列表
    static const std::vector<std::string> owner_only_commands_;
};

// ═══════════════════════════════════════════════════════════════
// ✅ 常用命令常量 (对齐 OpenClaw)
// ═══════════════════════════════════════════════════════════════

namespace commands {
    constexpr const char* STATUS = "status";
    constexpr const char* NEW = "new";
    constexpr const char* RESET = "reset";
    constexpr const char* HELP = "help";
    constexpr const char* CONFIG = "config";
    constexpr const char* LOGS = "logs";
    constexpr const char* DEBUG = "debug";
    constexpr const char* STOP = "stop";
    constexpr const char* RESTART = "restart";
} // namespace commands

} // namespace auto_reply
} // namespace hunterclaw
