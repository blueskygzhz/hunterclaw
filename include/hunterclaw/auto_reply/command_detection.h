#pragma once

#include <string>
#include <optional>
#include <regex>
#include "hunterclaw/react/verbose_reasoning_config.h"

namespace hunterclaw {
namespace auto_reply {

// ✅ OpenClaw 对齐: 消息指令解析
// 参考: openclaw-analysis/src/auto-reply/reply/get-reply-directives.ts:97-165

/// 消息中的内联指令
struct MessageDirectives {
    /// Verbose 级别 (来自 /verbose <level> 指令)
    std::optional<react::VerboseLevel> verbose_level;
    
    /// Reasoning 级别 (来自 /reasoning <level> 指令)
    std::optional<react::ReasoningLevel> reasoning_level;
    
    /// 是否为纯指令消息 (没有其他内容)
    bool is_directive_only = false;
    
    /// 原始消息文本 (移除指令后)
    std::string text_without_directives;
};

/// 命令检测器
class CommandDetection {
public:
    /// 从消息文本中解析指令
    /// @param text 原始消息文本
    /// @return 解析出的指令
    static MessageDirectives parse_directives(const std::string& text);
    
    /// 检测是否为纯指令消息 (只包含指令,没有其他内容)
    /// @param text 原始消息文本
    /// @return true 如果是纯指令消息
    static bool is_directive_only_message(const std::string& text);
    
    /// 从文本中移除所有指令
    /// @param text 原始消息文本
    /// @return 移除指令后的文本
    static std::string strip_directives(const std::string& text);
    
private:
    // 正则表达式 (静态编译优化)
    static const std::regex verbose_regex_;
    static const std::regex reasoning_regex_;
    static const std::regex thinking_regex_;
};

} // namespace auto_reply
} // namespace hunterclaw
