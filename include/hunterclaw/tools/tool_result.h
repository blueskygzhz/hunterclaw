#pragma once
#include "hunterclaw/core/result.h"
#include <nlohmann/json.hpp>
#include <string>

namespace hunterclaw {

// Tool execution result
struct ToolResult {
    bool success{false};
    std::string content;
    nlohmann::json metadata;
    std::string error_message;
};

} // namespace hunterclaw
