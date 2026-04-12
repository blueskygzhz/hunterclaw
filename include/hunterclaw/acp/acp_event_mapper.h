#pragma once

// ============================================================================
// acp_event_mapper.h
// 🎯 对齐 OpenClaw: src/acp/event-mapper.ts
// ============================================================================

#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace hunterclaw {
namespace acp {

struct ToolCallLocation {
    std::string path;
    int line = 0;
};

struct GatewayAttachment {
    std::string type;
    std::string mime_type;
    std::string content;
};

struct ToolCallContent {
    std::string tool_call_id;
    std::string tool_name;
    std::string kind;
    std::string title;
    std::vector<ToolCallLocation> locations;
};

std::vector<ToolCallLocation> extract_tool_call_locations(const nlohmann::json& raw_input);
std::vector<ToolCallLocation> extract_tool_call_locations_from_result(const std::string& result_text);
std::string extract_text_from_prompt(const nlohmann::json& prompt);
std::vector<GatewayAttachment> extract_attachments_from_prompt(const nlohmann::json& prompt);
ToolCallContent extract_tool_call_content(const std::string& tool_name,
                                           const nlohmann::json& tool_input,
                                           const std::string& tool_call_id);
std::string infer_tool_kind(const std::string& tool_name);
std::string format_tool_title(const std::string& tool_name, const nlohmann::json& raw_input);

} // namespace acp
} // namespace hunterclaw
