// ============================================================================
// gateway_canvas_documents.cc
// ✅ 严格对齐 OpenClaw:
//   - src/gateway/canvas-documents.ts (348行)
//   - src/gateway/cli-session-history.claude.ts (336行)
//
// Canvas 文档 CRUD + CLI 会话历史导入
// ============================================================================

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <optional>
#include <random>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace hunterclaw {
namespace gateway {

// ============================================================================
// Part 1: Canvas Documents (对齐 canvas-documents.ts)
// ============================================================================

static const std::string CANVAS_HOST_PATH = "/_a2ui_/canvas";
static const std::string CANVAS_DOCUMENTS_DIR = "documents";

// 🎯 escapeHtml (对齐 :65-72)
static std::string escape_html(const std::string& s) {
    std::string r;
    for (char c : s) {
        switch (c) {
            case '&': r += "&amp;"; break;
            case '<': r += "&lt;"; break;
            case '>': r += "&gt;"; break;
            case '"': r += "&quot;"; break;
            case '\'': r += "&#39;"; break;
            default: r += c;
        }
    }
    return r;
}

// 🎯 isPdfPathLike (对齐 :56-58)
static bool is_pdf_path_like(const std::string& value) {
    std::regex re(R"(\.pdf(?:[?#].*)?$)", std::regex::icase);
    return std::regex_search(value, re);
}

// 🎯 buildPdfWrapper (对齐 :60-62)
static std::string build_pdf_wrapper(const std::string& url) {
    auto e = escape_html(url);
    return "<!doctype html><html><body style=\"margin:0;background:#e5e7eb;\">"
           "<object data=\"" + e + "\" type=\"application/pdf\" "
           "style=\"width:100%;height:100vh;border:0;\">"
           "<iframe src=\"" + e + "\" style=\"width:100%;height:100vh;border:0;\"></iframe>"
           "<p style=\"padding:16px;font:14px system-ui,sans-serif;\">"
           "Unable to render PDF preview. <a href=\"" + e + "\" target=\"_blank\" "
           "rel=\"noopener noreferrer\">Open PDF</a>.</p></object></body></html>";
}

// 🎯 normalizeLogicalPath (对齐 :74-81)
static std::string normalize_logical_path(const std::string& value) {
    std::string normalized = value;
    std::replace(normalized.begin(), normalized.end(), '\\', '/');
    while (!normalized.empty() && normalized.front() == '/') normalized.erase(0, 1);

    std::vector<std::string> parts;
    std::istringstream ss(normalized);
    std::string part;
    while (std::getline(ss, part, '/')) {
        if (!part.empty()) parts.push_back(part);
    }
    if (parts.empty()) throw std::runtime_error("canvas document logicalPath invalid");
    for (const auto& p : parts) {
        if (p == "." || p == "..") throw std::runtime_error("canvas document logicalPath invalid");
    }
    std::string result;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) result += "/";
        result += parts[i];
    }
    return result;
}

// 🎯 canvasDocumentId (对齐 :83-85)
static std::string canvas_document_id() {
    static std::random_device rd;
    static std::mt19937 gen(rd());
    static std::uniform_int_distribution<uint32_t> dis;
    static const char hex[] = "0123456789abcdef";
    std::string uuid;
    for (int i = 0; i < 16; ++i) {
        uint8_t b = dis(gen) & 0xff;
        uuid += hex[b >> 4];
        uuid += hex[b & 0xf];
    }
    return "cv_" + uuid;
}

// 🎯 normalizeCanvasDocumentId (对齐 :87-98)
static std::string normalize_canvas_document_id(const std::string& value) {
    std::string s = value;
    auto a = s.find_first_not_of(" \t");
    if (a == std::string::npos) throw std::runtime_error("canvas document id invalid");
    s = s.substr(a, s.find_last_not_of(" \t") - a + 1);
    if (s.empty() || s == "." || s == "..") throw std::runtime_error("canvas document id invalid");
    std::regex valid_re(R"(^[A-Za-z0-9._-]+$)");
    if (!std::regex_match(s, valid_re)) throw std::runtime_error("canvas document id invalid");
    return s;
}

// 🎯 resolveCanvasRootDir / resolveCanvasDocumentsDir / resolveCanvasDocumentDir
// (对齐 :100-114)
std::string resolve_canvas_root_dir(const std::string& state_dir) {
    return (fs::path(state_dir) / "canvas").string();
}

std::string resolve_canvas_documents_dir(const std::string& state_dir) {
    return (fs::path(resolve_canvas_root_dir(state_dir)) / CANVAS_DOCUMENTS_DIR).string();
}

std::string resolve_canvas_document_dir(const std::string& doc_id, const std::string& state_dir) {
    return (fs::path(resolve_canvas_documents_dir(state_dir)) / doc_id).string();
}

// 🎯 buildCanvasDocumentEntryUrl (对齐 :116-123)
std::string build_canvas_document_entry_url(const std::string& doc_id, const std::string& entrypoint) {
    auto norm = normalize_logical_path(entrypoint);
    return CANVAS_HOST_PATH + "/" + CANVAS_DOCUMENTS_DIR + "/" + doc_id + "/" + norm;
}

// 🎯 resolveCanvasHttpPathToLocalPath (对齐 :129-173)
// HTTP→本地路径映射 + 目录穿越防护
std::optional<std::string> resolve_canvas_http_path_to_local(
    const std::string& request_path,
    const std::string& state_dir
) {
    std::string prefix = CANVAS_HOST_PATH + "/" + CANVAS_DOCUMENTS_DIR + "/";
    if (request_path.find(prefix) != 0) return std::nullopt;

    // Strip query/hash
    auto path_no_query = request_path.substr(0, request_path.find_first_of("?#"));
    auto relative = path_no_query.substr(prefix.size());

    std::vector<std::string> segments;
    std::istringstream ss(relative);
    std::string seg;
    while (std::getline(ss, seg, '/')) {
        if (!seg.empty()) segments.push_back(seg);
    }
    if (segments.size() < 2) return std::nullopt;

    try {
        auto doc_id = normalize_canvas_document_id(segments[0]);
        std::string entry_path;
        for (size_t i = 1; i < segments.size(); ++i) {
            if (i > 1) entry_path += "/";
            entry_path += segments[i];
        }
        auto norm_entry = normalize_logical_path(entry_path);
        auto docs_dir = fs::absolute(resolve_canvas_documents_dir(state_dir));
        auto candidate = fs::absolute(fs::path(resolve_canvas_document_dir(doc_id, state_dir)) / norm_entry);

        // 目录穿越防护 — 对齐 :164-168
        auto candidate_str = candidate.string();
        auto docs_str = docs_dir.string();
        if (candidate_str != docs_str &&
            candidate_str.find(docs_str + std::string(1, fs::path::preferred_separator)) != 0) {
            return std::nullopt;
        }
        return candidate_str;
    } catch (...) {
        return std::nullopt;
    }
}

// 🎯 createCanvasDocument (对齐 :275-305)
json create_canvas_document(
    const json& input,
    const std::string& state_dir,
    const std::string& workspace_dir
) {
    std::string id = input.contains("id") && input["id"].is_string() && !input["id"].get<std::string>().empty()
        ? normalize_canvas_document_id(input["id"].get<std::string>())
        : canvas_document_id();

    auto root_dir = resolve_canvas_document_dir(id, state_dir);

    // Clean + create dir
    std::error_code ec;
    fs::remove_all(root_dir, ec);
    fs::create_directories(root_dir, ec);

    // Copy assets — 对齐 :287
    std::vector<json> manifest_assets;
    if (input.contains("assets") && input["assets"].is_array()) {
        for (const auto& asset : input["assets"]) {
            auto logical = normalize_logical_path(asset.value("logicalPath", ""));
            std::string source = asset.value("sourcePath", "");
            fs::path src_path = fs::path(source).is_absolute()
                ? fs::path(source)
                : fs::path(workspace_dir) / source;
            auto dest = fs::path(root_dir) / logical;
            fs::create_directories(dest.parent_path(), ec);
            fs::copy_file(src_path, dest, fs::copy_options::overwrite_existing, ec);
            json a = {{"logicalPath", logical}};
            if (asset.contains("contentType")) a["contentType"] = asset["contentType"];
            manifest_assets.push_back(a);
        }
    }

    // Materialize entrypoint — simplified
    std::string entry_url;
    std::optional<std::string> local_entrypoint;
    std::optional<std::string> external_url;
    std::string kind = input.value("kind", "document");

    if (input.contains("entrypoint") && input["entrypoint"].is_object()) {
        auto ep = input["entrypoint"];
        std::string type = ep.value("type", "");
        std::string value = ep.value("value", "");

        if (type == "html") {
            std::ofstream ofs(fs::path(root_dir) / "index.html");
            ofs << value;
            local_entrypoint = "index.html";
            entry_url = build_canvas_document_entry_url(id, "index.html");
        } else if (type == "url") {
            if (kind == "document" && is_pdf_path_like(value)) {
                std::ofstream ofs(fs::path(root_dir) / "index.html");
                ofs << build_pdf_wrapper(value);
                local_entrypoint = "index.html";
                external_url = value;
                entry_url = build_canvas_document_entry_url(id, "index.html");
            } else {
                external_url = value;
                entry_url = value;
            }
        } else if (type == "path") {
            fs::path src = fs::path(value).is_absolute() ? fs::path(value) : fs::path(workspace_dir) / value;
            std::string fname = src.filename().string();

            if (kind == "image" || kind == "video_asset") {
                fs::copy_file(src, fs::path(root_dir) / fname, fs::copy_options::overwrite_existing, ec);
                std::string wrapper = (kind == "image")
                    ? "<!doctype html><html><body style=\"margin:0;background:#0f172a;display:flex;"
                      "align-items:center;justify-content:center;\"><img src=\"" + escape_html(fname) +
                      "\" style=\"max-width:100%;max-height:100vh;object-fit:contain;\" /></body></html>"
                    : "<!doctype html><html><body style=\"margin:0;background:#0f172a;\"><video src=\"" +
                      escape_html(fname) + "\" controls autoplay style=\"width:100%;height:100vh;"
                      "object-fit:contain;background:#000;\"></video></body></html>";
                std::ofstream ofs(fs::path(root_dir) / "index.html");
                ofs << wrapper;
                local_entrypoint = "index.html";
                entry_url = build_canvas_document_entry_url(id, "index.html");
            } else {
                fs::copy_file(src, fs::path(root_dir) / fname, fs::copy_options::overwrite_existing, ec);
                if (kind == "document" && is_pdf_path_like(fname)) {
                    std::ofstream ofs(fs::path(root_dir) / "index.html");
                    ofs << build_pdf_wrapper(fname);
                    local_entrypoint = "index.html";
                    entry_url = build_canvas_document_entry_url(id, "index.html");
                } else {
                    local_entrypoint = fname;
                    entry_url = build_canvas_document_entry_url(id, fname);
                }
            }
        }
    }

    // Write manifest — 对齐 :289-304
    json manifest = {
        {"id", id}, {"kind", kind},
        {"createdAt", ""}, {"entryUrl", entry_url},
        {"assets", manifest_assets}
    };
    if (input.contains("title") && input["title"].is_string()) manifest["title"] = input["title"];
    if (input.contains("preferredHeight")) manifest["preferredHeight"] = input["preferredHeight"];
    if (input.contains("surface")) manifest["surface"] = input["surface"];
    if (local_entrypoint) manifest["localEntrypoint"] = *local_entrypoint;
    if (external_url) manifest["externalUrl"] = *external_url;

    std::ofstream mf(fs::path(root_dir) / "manifest.json");
    mf << manifest.dump(2) << "\n";

    return manifest;
}

// 🎯 loadCanvasDocumentManifest (对齐 :307-328)
std::optional<json> load_canvas_document_manifest(const std::string& doc_id, const std::string& state_dir) {
    try {
        auto id = normalize_canvas_document_id(doc_id);
        auto path = fs::path(resolve_canvas_document_dir(id, state_dir)) / "manifest.json";
        std::ifstream ifs(path);
        if (!ifs.good()) return std::nullopt;
        auto parsed = json::parse(ifs);
        if (!parsed.is_object()) return std::nullopt;
        return parsed;
    } catch (...) { return std::nullopt; }
}

// ============================================================================
// Part 2: CLI Session History Import (对齐 cli-session-history.claude.ts)
// ============================================================================

static const std::string CLI_PROVIDER = "cli-history";

// 🎯 resolveCliBindingSessionId (对齐 :49-64)
std::optional<std::string> resolve_cli_binding_session_id(const json& entry) {
    // 3 legacy format support
    if (entry.contains("cliSessionBindings") && entry["cliSessionBindings"].is_object()) {
        for (auto& [provider, binding] : entry["cliSessionBindings"].items()) {
            if (binding.contains("sessionId") && binding["sessionId"].is_string()) {
                return binding["sessionId"].get<std::string>();
            }
        }
    }
    if (entry.contains("cliSessionIds") && entry["cliSessionIds"].is_object()) {
        for (auto& [provider, sid] : entry["cliSessionIds"].items()) {
            if (sid.is_string()) return sid.get<std::string>();
        }
    }
    if (entry.contains("claudeCliSessionId") && entry["claudeCliSessionId"].is_string()) {
        return entry["claudeCliSessionId"].get<std::string>();
    }
    return std::nullopt;
}

// 🎯 normalizeCliContent (对齐 :106-150)
// tool_use→toolcall 归一化
static json normalize_cli_content(
    const json& content,
    std::unordered_map<std::string, std::string>& tool_name_registry
) {
    if (!content.is_array()) return content;
    json normalized = json::array();
    for (auto block : content) {
        if (!block.is_object()) { normalized.push_back(block); continue; }
        std::string type = block.value("type", "");
        if (type == "tool_use") {
            std::string id = block.value("id", "");
            std::string name = block.value("name", "");
            if (!id.empty() && !name.empty()) tool_name_registry[id] = name;
            if (block.contains("input") && !block.contains("arguments")) {
                block["arguments"] = block["input"];
            }
            block["type"] = "toolcall";
            block.erase("input");
        } else if (type == "tool_result") {
            std::string tool_use_id = block.value("tool_use_id", "");
            if (!block.contains("name") && !tool_use_id.empty()) {
                auto it = tool_name_registry.find(tool_use_id);
                if (it != tool_name_registry.end()) block["name"] = it->second;
            }
        }
        normalized.push_back(block);
    }
    return normalized;
}

// 🎯 coalesceCliToolMessages (对齐 :184-217)
static std::vector<json> coalesce_cli_tool_messages(const std::vector<json>& messages) {
    std::vector<json> coalesced;
    for (size_t i = 0; i < messages.size(); ++i) {
        const auto& current = messages[i];
        bool is_assistant_tc = current.value("role", "") == "assistant" &&
            current.contains("content") && current["content"].is_array();
        bool has_next = (i + 1 < messages.size());

        if (is_assistant_tc && has_next) {
            const auto& next = messages[i + 1];
            bool is_user_tr = next.value("role", "") == "user" &&
                next.contains("content") && next["content"].is_array();
            if (is_user_tr) {
                // Check all tool_results match tool_calls
                std::set<std::string> call_ids;
                for (const auto& b : current["content"]) {
                    if (b.value("type", "") == "toolcall" && b.contains("id"))
                        call_ids.insert(b["id"].get<std::string>());
                }
                bool all_match = !next["content"].empty();
                for (const auto& b : next["content"]) {
                    std::string tid = b.value("tool_use_id", "");
                    if (tid.empty() || !call_ids.count(tid)) { all_match = false; break; }
                }
                if (all_match) {
                    json merged = current;
                    json combined = current["content"];
                    for (const auto& b : next["content"]) combined.push_back(b);
                    merged["content"] = combined;
                    coalesced.push_back(merged);
                    ++i; // skip next
                    continue;
                }
            }
        }
        coalesced.push_back(current);
    }
    return coalesced;
}

// 🎯 readCliSessionMessages (对齐 :302-335)
std::vector<json> read_cli_session_messages(
    const std::string& session_file_path,
    const std::string& cli_session_id
) {
    std::ifstream ifs(session_file_path);
    if (!ifs.good()) return {};

    std::vector<json> messages;
    std::unordered_map<std::string, std::string> tool_name_registry;
    std::string line;

    while (std::getline(ifs, line)) {
        if (line.empty()) continue;
        try {
            auto entry = json::parse(line);
            if (entry.value("isSidechain", false)) continue;
            if (!entry.contains("message") || !entry["message"].is_object()) continue;

            std::string type = entry.value("type", "");
            std::string role = entry["message"].value("role", "");
            if ((type != "user" && type != "assistant") || role != type) continue;

            auto content = entry["message"].value("content", json());
            if (!content.is_string() && !content.is_array()) continue;

            auto normalized = normalize_cli_content(content, tool_name_registry);
            json msg = {{"role", role}, {"content", normalized}};

            // Metadata
            msg["_meta"] = {{"importedFrom", CLI_PROVIDER}, {"cliSessionId", cli_session_id}};
            if (entry.contains("uuid")) msg["_meta"]["externalId"] = entry["uuid"];

            if (role == "assistant") {
                if (entry["message"].contains("model")) msg["model"] = entry["message"]["model"];
                if (entry["message"].contains("stop_reason")) msg["stopReason"] = entry["message"]["stop_reason"];
                if (entry["message"].contains("usage") && entry["message"]["usage"].is_object()) {
                    auto& u = entry["message"]["usage"];
                    json usage;
                    if (u.contains("input_tokens")) usage["input"] = u["input_tokens"];
                    if (u.contains("output_tokens")) usage["output"] = u["output_tokens"];
                    if (u.contains("cache_read_input_tokens")) usage["cacheRead"] = u["cache_read_input_tokens"];
                    if (u.contains("cache_creation_input_tokens")) usage["cacheWrite"] = u["cache_creation_input_tokens"];
                    if (!usage.empty()) msg["usage"] = usage;
                }
            }

            messages.push_back(msg);
        } catch (...) {
            // Ignore malformed entries
        }
    }

    return coalesce_cli_tool_messages(messages);
}

} // namespace gateway
} // namespace hunterclaw
