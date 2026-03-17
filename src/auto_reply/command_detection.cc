#include "hunterclaw/auto_reply/command_detection.h"
#include <algorithm>
#include <cctype>

namespace hunterclaw {
namespace auto_reply {

// Static regex patterns
const std::regex CommandDetection::verbose_regex_(
    R"(/verbose\s+(off|on|full))",
    std::regex_constants::icase
);

const std::regex CommandDetection::reasoning_regex_(
    R"(/reasoning\s+(off|on|stream))",
    std::regex_constants::icase
);

const std::regex CommandDetection::thinking_regex_(
    R"(/think(?:ing)?\s+(off|on))",
    std::regex_constants::icase
);

// Parse directives from message text
MessageDirectives CommandDetection::parse_directives(const std::string& text) {
    MessageDirectives directives;
    directives.text_without_directives = text;
    
    std::smatch match;
    
    // Parse verbose directive
    if (std::regex_search(text, match, verbose_regex_)) {
        std::string level = match[1].str();
        std::transform(level.begin(), level.end(), level.begin(),
                      [](unsigned char c) { return std::tolower(c); });
        
        if (level == "off") {
            directives.verbose_level = react::VerboseLevel::Off;
        } else if (level == "on") {
            directives.verbose_level = react::VerboseLevel::On;
        } else if (level == "full") {
            directives.verbose_level = react::VerboseLevel::Full;
        }
        
        // Remove directive from text
        directives.text_without_directives = std::regex_replace(
            directives.text_without_directives, verbose_regex_, "");
    }
    
    // Parse reasoning directive
    if (std::regex_search(text, match, reasoning_regex_)) {
        std::string level = match[1].str();
        std::transform(level.begin(), level.end(), level.begin(),
                      [](unsigned char c) { return std::tolower(c); });
        
        if (level == "off") {
            directives.reasoning_level = react::ReasoningLevel::Off;
        } else if (level == "on") {
            directives.reasoning_level = react::ReasoningLevel::On;
        } else if (level == "stream") {
            directives.reasoning_level = react::ReasoningLevel::Stream;
        }
        
        // Remove directive from text
        directives.text_without_directives = std::regex_replace(
            directives.text_without_directives, reasoning_regex_, "");
    }
    
    // Trim whitespace
    auto& result = directives.text_without_directives;
    result.erase(0, result.find_first_not_of(" \t\n\r"));
    result.erase(result.find_last_not_of(" \t\n\r") + 1);
    
    // Check if directive-only
    directives.is_directive_only = result.empty();
    
    return directives;
}

// Check if message contains only directives
bool CommandDetection::is_directive_only_message(const std::string& text) {
    auto directives = parse_directives(text);
    return directives.is_directive_only;
}

// Strip all directives from text
std::string CommandDetection::strip_directives(const std::string& text) {
    auto directives = parse_directives(text);
    return directives.text_without_directives;
}

} // namespace auto_reply
} // namespace hunterclaw
