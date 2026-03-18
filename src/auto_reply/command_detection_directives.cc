#include "hunterclaw/auto_reply/command_detection.h"
#include <algorithm>
#include <cctype>
#include <spdlog/spdlog.h>

namespace hunterclaw {
namespace auto_reply {

// ✅ 静态正则表达式 (编译期优化)
// OpenClaw 参考: src/auto-reply/reply/get-reply-directives.ts:105-130

const std::regex CommandDetection::verbose_regex_(
    R"(/verbose\s+(off|on|full))", 
    std::regex_constants::icase
);

const std::regex CommandDetection::reasoning_regex_(
    R"(/reasoning\s+(off|on|stream))", 
    std::regex_constants::icase
);

const std::regex CommandDetection::thinking_regex_(
    R"(/think(?:ing)?\s+(off|on|verbose))", 
    std::regex_constants::icase
);

MessageDirectives CommandDetection::parse_directives(const std::string& text) {
    MessageDirectives directives;
    std::smatch match;
    
    // ========== 1. 解析 /verbose 指令 ==========
    // OpenClaw: directives.verboseLevel
    if (std::regex_search(text, match, verbose_regex_)) {
        std::string level = match[1].str();
        std::transform(level.begin(), level.end(), level.begin(), ::tolower);
        
        if (level == "off") {
            directives.verbose_level = react::VerboseLevel::Off;
            //spdlog::debug("[CommandDetection] Parsed /verbose off");
        } else if (level == "on") {
            directives.verbose_level = react::VerboseLevel::On;
            //spdlog::debug("[CommandDetection] Parsed /verbose on");
        } else if (level == "full") {
            directives.verbose_level = react::VerboseLevel::Full;
            //spdlog::debug("[CommandDetection] Parsed /verbose full");
        }
    }
    
    // ========== 2. 解析 /reasoning 指令 ==========
    // OpenClaw: directives.reasoningLevel
    if (std::regex_search(text, match, reasoning_regex_)) {
        std::string level = match[1].str();
        std::transform(level.begin(), level.end(), level.begin(), ::tolower);
        
        if (level == "off") {
            directives.reasoning_level = react::ReasoningLevel::Off;
            //spdlog::debug("[CommandDetection] Parsed /reasoning off");
        } else if (level == "on") {
            directives.reasoning_level = react::ReasoningLevel::On;
            //spdlog::debug("[CommandDetection] Parsed /reasoning on");
        } else if (level == "stream") {
            directives.reasoning_level = react::ReasoningLevel::Stream;
            //spdlog::debug("[CommandDetection] Parsed /reasoning stream");
        }
    }
    
    // ========== 3. 解析 /thinking 指令 ==========
    // OpenClaw: directives.thinkLevel
    if (std::regex_search(text, match, thinking_regex_)) {
        std::string level = match[1].str();
        std::transform(level.begin(), level.end(), level.begin(), ::tolower);
        
        // Note: thinking_level is not in current MessageDirectives struct
        // If needed, add it to the struct definition
        /*
        if (level == "off") {
            directives.thinking_level = react::ThinkingLevel::Off;
            //spdlog::debug("[CommandDetection] Parsed /thinking off");
        } else if (level == "on") {
            directives.thinking_level = react::ThinkingLevel::On;
            //spdlog::debug("[CommandDetection] Parsed /thinking on");
        } else if (level == "verbose") {
            directives.thinking_level = react::ThinkingLevel::Verbose;
            //spdlog::debug("[CommandDetection] Parsed /thinking verbose");
        }
        */
    }
    
    // ========== 4. 判断是否为纯指令消息 ==========
    // OpenClaw: 如果消息只包含指令,不发送给 LLM
    directives.is_directive_only = is_directive_only_message(text);
    
    // ========== 5. 生成移除指令后的文本 ==========
    directives.text_without_directives = strip_directives(text);
    
    return directives;
}

bool CommandDetection::is_directive_only_message(const std::string& text) {
    // 移除所有指令
    std::string stripped = strip_directives(text);
    
    // 移除空白字符
    stripped.erase(
        std::remove_if(stripped.begin(), stripped.end(), 
            [](char c) { return std::isspace(static_cast<unsigned char>(c)); }),
        stripped.end()
    );
    
    // 如果移除指令和空白后为空，说明是纯指令消息
    return stripped.empty();
}

std::string CommandDetection::strip_directives(const std::string& text) {
    std::string result = text;
    
    // 依次移除所有指令 (按顺序替换为空字符串)
    result = std::regex_replace(result, verbose_regex_, "");
    result = std::regex_replace(result, reasoning_regex_, "");
    result = std::regex_replace(result, thinking_regex_, "");
    
    // 清理多余的空白 (连续空格 -> 单个空格)
    std::regex whitespace_re(R"(\s+)");
    result = std::regex_replace(result, whitespace_re, " ");
    
    // 去除首尾空白
    size_t start = result.find_first_not_of(" \t\n\r");
    size_t end = result.find_last_not_of(" \t\n\r");
    
    if (start == std::string::npos) {
        return "";
    }
    
    return result.substr(start, end - start + 1);
}

} // namespace auto_reply
} // namespace hunterclaw
