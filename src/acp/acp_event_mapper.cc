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
// ✅ 严格对齐 OC event-mapper.ts:278-295 extractAttachmentsFromPrompt(prompt)
//   - 仅处理 type === "image"
//   - 若 !data || !mimeType → 跳过（不 fallback 默认 mime）
//   - 输出 { type: "image", mimeType, content: data }
std::vector<GatewayAttachment> extract_attachments_from_prompt(const nlohmann::json& prompt) {
    std::vector<GatewayAttachment> attachments;
    if (!prompt.is_array()) return attachments;

    for (auto& block : prompt) {
        if (!block.is_object()) continue;
        if (block.value("type", "") != "image") continue;

        // ✅ 对齐 OC: 缺 data 或 mimeType 则跳过，不 fallback
        if (!block.contains("data") || !block["data"].is_string() ||
            !block.contains("mimeType") || !block["mimeType"].is_string()) {
            continue;
        }
        GatewayAttachment att;
        att.type = "image";
        att.mime_type = block["mimeType"].get<std::string>();
        att.content   = block["data"].get<std::string>();
        if (att.mime_type.empty() || att.content.empty()) continue;
        attachments.push_back(std::move(att));
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
// ✅ 严格对齐 OC event-mapper.ts:315-342 inferToolKind(name)
//
// ACP spec ToolKind 枚举:
//   "read" | "edit" | "delete" | "move" | "search" | "execute" | "fetch" | "other"
//
// OC 实现使用 substring 匹配（而非精确 whitelist），优先级顺序:
//   1. read
//   2. write|edit  → "edit"
//   3. delete|remove → "delete"
//   4. move|rename → "move"
//   5. search|find → "search"
//   6. exec|run|bash → "execute"
//   7. fetch|http → "fetch"
//   8. default: "other"
//
// 旧 HC 实现使用精确 whitelist，且返回 "write"/"command" — 违反 ACP spec 枚举。
std::string infer_tool_kind(const std::string& tool_name) {
    if (tool_name.empty()) return "other";

    std::string n = tool_name;
    std::transform(n.begin(), n.end(), n.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    auto contains = [&n](const char* sub) {
        return n.find(sub) != std::string::npos;
    };

    // 对齐 OC 匹配顺序（不可换顺序，比如 "rewrite" 同时含 write 和 re）
    if (contains("read"))                        return "read";
    if (contains("write") || contains("edit"))   return "edit";
    if (contains("delete") || contains("remove"))return "delete";
    if (contains("move")   || contains("rename"))return "move";
    if (contains("search") || contains("find"))  return "search";
    if (contains("exec")   || contains("run") || contains("bash")) return "execute";
    if (contains("fetch")  || contains("http"))  return "fetch";
    return "other";
}

// ── formatToolTitle ──────────────────────────────────────────────────────────
// ✅ 严格对齐 OC event-mapper.ts:297-313 formatToolTitle(name, args)
//
// OC 实现:
//   base = name ?? "tool"
//   if (!args || Object.keys(args).length === 0) return base;
//   parts = Object.entries(args).map(([k, v]) => {
//     raw = typeof v === "string" ? v : JSON.stringify(v);
//     safe = raw.length > 100 ? raw.slice(0,100)+"..." : raw;
//     return `${k}: ${safe}`;
//   });
//   return escapeInlineControlChars(`${base}: ${parts.join(", ")}`);
//
// 旧 HC 实现只挑 path/command 展示，违反对齐语义（丢失其他 args）
std::string format_tool_title(const std::string& tool_name,
                                const nlohmann::json& raw_input) {
    const std::string base = tool_name.empty() ? "tool" : tool_name;

    if (!raw_input.is_object() || raw_input.empty()) {
        return base;
    }

    static constexpr size_t MAX_PART_VALUE_LEN = 100;
    std::string parts;
    bool first = true;
    for (auto& [key, value] : raw_input.items()) {
        std::string raw;
        if (value.is_string()) {
            raw = value.get<std::string>();
        } else {
            raw = value.dump();
        }
        std::string safe = (raw.size() > MAX_PART_VALUE_LEN)
            ? raw.substr(0, MAX_PART_VALUE_LEN) + "..."
            : raw;
        if (!first) parts += ", ";
        parts += key;
        parts += ": ";
        parts += safe;
        first = false;
    }
    // ✅ 对齐 OC: 最外层过 escapeInlineControlChars
    return escape_inline_control_chars(base + ": " + parts);
}

} // namespace acp
} // namespace hunterclaw
