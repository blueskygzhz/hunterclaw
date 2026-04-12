// ============================================================================
// acp_event_mapper.cc
// 🎯 严格对齐 OpenClaw: src/acp/event-mapper.ts
// ACP 事件映射: Gateway EventFrame → ACP ContentBlock 转换
// ============================================================================

#include "hunterclaw/acp/acp_event_mapper.h"
#include <algorithm>
#include <regex>
#include <sstream>

namespace hunterclaw {
namespace acp {

// ── Constants ────────────────────────────────────────────────────────────────
// 🎯 对齐 event-mapper.ts TOOL_LOCATION_PATH_KEYS, TOOL_LOCATION_LINE_KEYS

static const std::vector<std::string> TOOL_LOCATION_PATH_KEYS = {
    "path", "filePath", "file_path",
    "targetPath", "target_path", "targetFile", "target_file",
    "sourcePath", "source_path", "destinationPath", "destination_path",
    "oldPath", "old_path", "newPath", "new_path",
    "outputPath", "output_path", "inputPath", "input_path"
};

static const std::vector<std::string> TOOL_LOCATION_LINE_KEYS = {
    "line", "lineNumber", "line_number",
    "startLine", "start_line"
};

static const std::regex TOOL_RESULT_PATH_MARKER_RE(R"(^(?:FILE|MEDIA):(.+)$)");
static constexpr int TOOL_LOCATION_MAX_DEPTH = 4;
static constexpr int TOOL_LOCATION_MAX_NODES = 100;

// ── escapeInlineControlChars ─────────────────────────────────────────────────
// 🎯 对齐 event-mapper.ts escapeInlineControlChars(value)

static std::string escape_inline_control_chars(const std::string& value) {
    std::string result;
    result.reserve(value.size());
    for (char c : value) {
        unsigned char uc = static_cast<unsigned char>(c);
        if (uc <= 0x1f || (uc >= 0x7f && uc <= 0x9f)) {
            switch (c) {
                case '\0': result += "\\0"; break;
                case '\r': result += "\\r"; break;
                case '\n': result += "\\n"; break;
                case '\t': result += "\\t"; break;
                case '\v': result += "\\v"; break;
                case '\f': result += "\\f"; break;
                default: {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\x%02x", uc);
                    result += buf;
                }
            }
        } else {
            result += c;
        }
    }
    return result;
}

// ── extractToolCallLocations ─────────────────────────────────────────────────
// 🎯 对齐 event-mapper.ts extractToolCallLocations(rawInput)

static void collect_locations_recursive(
    const nlohmann::json& node,
    std::vector<ToolCallLocation>& out,
    int depth, int& node_count) {

    if (depth > TOOL_LOCATION_MAX_DEPTH || node_count >= TOOL_LOCATION_MAX_NODES) return;
    node_count++;

    if (node.is_object()) {
        // Check for path keys
        std::string path_value;
        for (auto& key : TOOL_LOCATION_PATH_KEYS) {
            if (node.contains(key) && node[key].is_string()) {
                path_value = node[key].get<std::string>();
                break;
            }
        }

        if (!path_value.empty()) {
            ToolCallLocation loc;
            loc.path = path_value;

            // Check for line keys
            for (auto& key : TOOL_LOCATION_LINE_KEYS) {
                if (node.contains(key) && node[key].is_number_integer()) {
                    loc.line = node[key].get<int>();
                    break;
                }
            }
            out.push_back(std::move(loc));
        }

        // Recurse into children
        for (auto& [k, v] : node.items()) {
            collect_locations_recursive(v, out, depth + 1, node_count);
        }
    }
    else if (node.is_array()) {
        for (auto& elem : node) {
            collect_locations_recursive(elem, out, depth + 1, node_count);
        }
    }
}

std::vector<ToolCallLocation> extract_tool_call_locations(const nlohmann::json& raw_input) {
    std::vector<ToolCallLocation> locations;
    if (raw_input.is_null()) return locations;
    int node_count = 0;
    collect_locations_recursive(raw_input, locations, 0, node_count);
    return locations;
}

// ── extractToolCallLocationsFromResult ────────────────────────────────────────
// 🎯 对齐 event-mapper.ts — extract FILE: / MEDIA: markers from tool results

std::vector<ToolCallLocation> extract_tool_call_locations_from_result(
    const std::string& result_text) {
    std::vector<ToolCallLocation> locations;
    std::istringstream stream(result_text);
    std::string line;
    while (std::getline(stream, line)) {
        std::smatch match;
        if (std::regex_match(line, match, TOOL_RESULT_PATH_MARKER_RE)) {
            ToolCallLocation loc;
            loc.path = match[1].str();
            locations.push_back(std::move(loc));
        }
    }
    return locations;
}

// ── extractTextFromPrompt ────────────────────────────────────────────────────
// 🎯 对齐 event-mapper.ts extractTextFromPrompt(prompt)

std::string extract_text_from_prompt(const nlohmann::json& prompt) {
    if (prompt.is_string()) {
        return prompt.get<std::string>();
    }
    if (prompt.is_array()) {
        std::string combined;
        for (auto& block : prompt) {
            if (block.is_object() && block.contains("type") &&
                block["type"].get<std::string>() == "text" &&
                block.contains("text") && block["text"].is_string()) {
                if (!combined.empty()) combined += "\n";
                combined += block["text"].get<std::string>();
            }
        }
        return combined;
    }
    return "";
}

// ── extractAttachmentsFromPrompt ─────────────────────────────────────────────
// 🎯 对齐 event-mapper.ts extractAttachmentsFromPrompt(prompt)

std::vector<GatewayAttachment> extract_attachments_from_prompt(const nlohmann::json& prompt) {
    std::vector<GatewayAttachment> attachments;
    if (!prompt.is_array()) return attachments;

    for (auto& block : prompt) {
        if (!block.is_object()) continue;
        std::string type = block.value("type", "");
        if (type == "image" && block.contains("data")) {
            GatewayAttachment att;
            att.type = "image";
            att.mime_type = block.value("mimeType", "image/png");
            att.content = block["data"].get<std::string>();
            attachments.push_back(std::move(att));
        }
    }
    return attachments;
}

// ── extractToolCallContent ───────────────────────────────────────────────────
// 🎯 对齐 event-mapper.ts extractToolCallContent(event)

ToolCallContent extract_tool_call_content(const std::string& tool_name,
                                            const nlohmann::json& tool_input,
                                            const std::string& tool_call_id) {
    ToolCallContent content;
    content.tool_call_id = tool_call_id;
    content.tool_name = tool_name;
    content.kind = infer_tool_kind(tool_name);
    content.title = format_tool_title(tool_name, tool_input);
    content.locations = extract_tool_call_locations(tool_input);
    return content;
}

// ── inferToolKind ────────────────────────────────────────────────────────────
// 🎯 对齐 event-mapper.ts inferToolKind(toolName)

std::string infer_tool_kind(const std::string& tool_name) {
    if (tool_name.empty()) return "other";

    std::string lower = tool_name;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

    // Read-only tools
    if (lower == "read" || lower == "read_file" || lower == "search" ||
        lower == "list_dir" || lower == "find" || lower == "grep" ||
        lower == "web_search" || lower == "memory_search" ||
        lower == "web_fetch" || lower == "list_files") {
        return "read";
    }

    // Write tools
    if (lower == "write" || lower == "write_file" || lower == "edit" ||
        lower == "replace" || lower == "insert" || lower == "delete" ||
        lower == "create" || lower == "move" || lower == "rename") {
        return "write";
    }

    // Exec tools
    if (lower == "exec" || lower == "bash" || lower == "shell" ||
        lower == "spawn" || lower == "process" || lower == "terminal" ||
        lower == "code_execution") {
        return "command";
    }

    return "other";
}

// ── formatToolTitle ──────────────────────────────────────────────────────────
// 🎯 对齐 event-mapper.ts formatToolTitle(toolName, rawInput)

std::string format_tool_title(const std::string& tool_name,
                                const nlohmann::json& raw_input) {
    if (tool_name.empty()) return "Tool call";

    // Try to extract a short descriptive title
    std::string path;
    for (auto& key : TOOL_LOCATION_PATH_KEYS) {
        if (raw_input.is_object() && raw_input.contains(key) && raw_input[key].is_string()) {
            path = raw_input[key].get<std::string>();
            break;
        }
    }

    if (!path.empty()) {
        // Shorten the path for display
        if (path.size() > 60) {
            path = "..." + path.substr(path.size() - 57);
        }
        return tool_name + ": " + path;
    }

    // For exec-like tools, try command
    if (raw_input.is_object()) {
        for (auto& key : {"command", "cmd", "script"}) {
            if (raw_input.contains(key) && raw_input[key].is_string()) {
                std::string cmd = raw_input[key].get<std::string>();
                if (cmd.size() > 60) cmd = cmd.substr(0, 57) + "...";
                return tool_name + ": " + escape_inline_control_chars(cmd);
            }
        }
    }

    return tool_name;
}

} // namespace acp
} // namespace hunterclaw
