#include "hunterclaw/auto_reply/command_detection.h"
#include <algorithm>
#include <cctype>

namespace hunterclaw {
namespace auto_reply {

// ═══════════════════════════════════════════════════════════════
// ✅ 静态成员初始化
// ═══════════════════════════════════════════════════════════════

// 控制命令正则: /^\s*[!/]([a-z]+)/i
std::regex CommandDetector::control_command_regex_(
    R"(^\s*[!/]([a-z]+))",
    std::regex_constants::icase
);

// 内联命令正则: /[!/][a-z]/i  
std::regex CommandDetector::inline_command_regex_(
    R"([!/][a-z])",
    std::regex_constants::icase
);

// owner-only 命令列表
const std::vector<std::string> CommandDetector::owner_only_commands_ = {
    "config",
    "logs",
    "debug",
    "stop",
    "restart"
};

// ═══════════════════════════════════════════════════════════════
// ✅ 实现 - 严格对齐 OpenClaw
// ═══════════════════════════════════════════════════════════════

// 对齐 OpenClaw: hasControlCommand (line 15-18)
bool CommandDetector::has_control_command(const std::string& text) {
    std::smatch match;
    return std::regex_search(text, match, control_command_regex_);
}

// 对齐 OpenClaw: isControlCommandMessage (line 24-27)
bool CommandDetector::is_control_command_message(const std::string& text) {
    // 与 hasControlCommand 相同的实现
    return has_control_command(text);
}

// 对齐 OpenClaw: hasInlineCommandTokens (line 33-36)
bool CommandDetector::has_inline_command_tokens(const std::string& text) {
    std::smatch match;
    return std::regex_search(text, match, inline_command_regex_);
}

// 对齐 OpenClaw: shouldComputeCommandAuthorized (line 45-70)
bool CommandDetector::should_compute_command_authorized(
    const std::string& text,
    bool is_owner
) {
    // 1. 如果是 owner,不需要检查
    if (is_owner) {
        return false;
    }
    
    // 2. 检测是否有控制命令
    if (!has_control_command(text)) {
        return false;
    }
    
    // 3. 提取命令名称
    std::smatch match;
    if (!std::regex_search(text, match, control_command_regex_)) {
        return false;
    }
    
    std::string command = match[1].str();
    
    // 4. 转换为小写
    std::transform(command.begin(), command.end(), command.begin(),
        [](unsigned char c) { return std::tolower(c); });
    
    // 5. 检查是否为 owner-only 命令
    return is_owner_only_command(command);
}

// ✅ 完整的命令检测 (增强版)
CommandDetector::DetectionResult CommandDetector::detect_command(
    const std::string& text
) {
    DetectionResult result;
    
    // 1. 尝试匹配控制命令
    std::smatch match;
    if (std::regex_search(text, match, control_command_regex_)) {
        result.type = CommandType::Slash;
        result.command = match[1].str();
        result.is_control_command = true;
        
        // 转换为小写
        std::transform(result.command.begin(), result.command.end(), 
                      result.command.begin(),
                      [](unsigned char c) { return std::tolower(c); });
        
        // 提取参数 (命令后的所有内容)
        size_t command_end = match.position(0) + match.length(0);
        if (command_end < text.size()) {
            result.args = text.substr(command_end);
            
            // 去除前导空白
            size_t first_non_space = result.args.find_first_not_of(" \t\n\r");
            if (first_non_space != std::string::npos) {
                result.args = result.args.substr(first_non_space);
            } else {
                result.args.clear();
            }
        }
        
        // 检查是否需要授权
        result.needs_authorization = is_owner_only_command(result.command);
        
        return result;
    }
    
    // 2. 检测内联命令 (但不提取,只标记)
    if (has_inline_command_tokens(text)) {
        result.type = CommandType::Inline;
        result.is_control_command = false;
        result.needs_authorization = false;
    }
    
    return result;
}

// ✅ 检测是否为 owner-only 命令
bool CommandDetector::is_owner_only_command(const std::string& command) {
    // 转换为小写进行比较
    std::string lower_command = command;
    std::transform(lower_command.begin(), lower_command.end(), 
                  lower_command.begin(),
                  [](unsigned char c) { return std::tolower(c); });
    
    // 查找命令
    return std::find(owner_only_commands_.begin(), 
                    owner_only_commands_.end(), 
                    lower_command) != owner_only_commands_.end();
}

} // namespace auto_reply
} // namespace hunterclaw
