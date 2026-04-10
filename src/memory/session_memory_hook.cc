#include "hunterclaw/memory/session_memory_hook.h"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <chrono>
#include <iomanip>
#include <set>
#include <nlohmann/json.hpp>

// 严格对齐 OpenClaw src/hooks/bundled/session-memory/handler.ts + transcript.ts

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace hunterclaw {
namespace memory {

// ============================================================================
// Internal helpers
// ============================================================================

static std::string extract_text_message_content(const json& content) {
    // (对齐 transcript.ts:5-22 extractTextMessageContent)
    if (content.is_string()) return content.get<std::string>();
    if (!content.is_array()) return "";
    for (const auto& block : content) {
        if (!block.is_object()) continue;
        if (block.value("type", "") == "text" && block.contains("text") && block["text"].is_string()) {
            return block["text"].get<std::string>();
        }
    }
    return "";
}

static bool has_inter_session_provenance(const json& msg) {
    // (对齐 hasInterSessionUserProvenance)
    if (!msg.contains("provenance") || !msg["provenance"].is_object()) return false;
    return msg["provenance"].value("kind", "") == "inter_session";
}

// ============================================================================
// getRecentSessionContent (对齐 transcript.ts:24-62)
// ============================================================================

std::optional<std::string> get_recent_session_content(
    const std::string& session_file_path,
    int message_count
) {
    try {
        std::ifstream ifs(session_file_path);
        if (!ifs.is_open()) return std::nullopt;

        std::string content((std::istreambuf_iterator<char>(ifs)),
                            std::istreambuf_iterator<char>());

        // Trim trailing whitespace
        auto end = content.find_last_not_of(" \t\n\r");
        if (end == std::string::npos) return std::nullopt;
        content = content.substr(0, end + 1);
        if (content.empty()) return std::nullopt;

        // Split into lines
        std::vector<std::string> lines;
        std::istringstream stream(content);
        std::string line;
        while (std::getline(stream, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            lines.push_back(line);
        }

        // Parse JSONL and extract user/assistant messages
        std::vector<std::string> all_messages;
        for (const auto& l : lines) {
            try {
                auto entry = json::parse(l);
                if (entry.value("type", "") != "message") continue;
                if (!entry.contains("message") || !entry["message"].is_object()) continue;

                const auto& msg = entry["message"];
                std::string role = msg.value("role", "");
                if (role != "user" && role != "assistant") continue;
                if (!msg.contains("content")) continue;

                // Filter inter-session user messages
                if (role == "user" && has_inter_session_provenance(msg)) continue;

                std::string text = extract_text_message_content(msg["content"]);
                if (text.empty()) continue;

                // Filter command messages starting with /
                if (text.front() == '/') continue;

                all_messages.push_back(role + ": " + text);
            } catch (...) {
                // Skip invalid JSON lines
            }
        }

        if (all_messages.empty()) return std::nullopt;

        // Take last N messages
        int start = std::max(0, static_cast<int>(all_messages.size()) - message_count);
        std::ostringstream result;
        for (size_t i = start; i < all_messages.size(); ++i) {
            if (i > static_cast<size_t>(start)) result << "\n";
            result << all_messages[i];
        }

        std::string res = result.str();
        return res.empty() ? std::nullopt : std::make_optional(res);
    } catch (...) {
        return std::nullopt;
    }
}

// ============================================================================
// getRecentSessionContentWithResetFallback (对齐 transcript.ts:64-89)
// ============================================================================

std::optional<std::string> get_recent_session_content_with_reset_fallback(
    const std::string& session_file_path,
    int message_count
) {
    auto primary = get_recent_session_content(session_file_path, message_count);
    if (primary && !primary->empty()) return primary;

    try {
        fs::path dir = fs::path(session_file_path).parent_path();
        std::string base = fs::path(session_file_path).filename().string();
        std::string reset_prefix = base + ".reset.";

        std::vector<std::string> reset_candidates;
        for (auto& entry : fs::directory_iterator(dir)) {
            if (!entry.is_regular_file()) continue;
            std::string name = entry.path().filename().string();
            if (name.compare(0, reset_prefix.size(), reset_prefix) == 0) {
                reset_candidates.push_back(name);
            }
        }

        if (reset_candidates.empty()) return primary;

        std::sort(reset_candidates.begin(), reset_candidates.end());
        std::string latest_reset = (dir / reset_candidates.back()).string();

        auto fallback = get_recent_session_content(latest_reset, message_count);
        return (fallback && !fallback->empty()) ? fallback : primary;
    } catch (...) {
        return primary;
    }
}

// ============================================================================
// stripResetSuffix (对齐 transcript.ts:91-94)
// ============================================================================

std::string strip_reset_suffix(const std::string& file_name) {
    auto pos = file_name.find(".reset.");
    return pos == std::string::npos ? file_name : file_name.substr(0, pos);
}

// ============================================================================
// findPreviousSessionFile (对齐 transcript.ts:96-148)
// ============================================================================

std::optional<std::string> find_previous_session_file(
    const std::string& sessions_dir,
    const std::optional<std::string>& current_session_file,
    const std::optional<std::string>& session_id
) {
    try {
        std::vector<std::string> files;
        std::set<std::string> file_set;
        for (auto& entry : fs::directory_iterator(sessions_dir)) {
            if (!entry.is_regular_file()) continue;
            std::string name = entry.path().filename().string();
            files.push_back(name);
            file_set.insert(name);
        }

        // Try strip-reset base
        if (current_session_file) {
            std::string base_from_reset = strip_reset_suffix(fs::path(*current_session_file).filename().string());
            if (file_set.count(base_from_reset)) {
                return (fs::path(sessions_dir) / base_from_reset).string();
            }
        }

        // Try sessionId.jsonl
        if (session_id) {
            auto trimmed = *session_id;
            auto s = trimmed.find_first_not_of(" \t");
            if (s != std::string::npos) trimmed = trimmed.substr(s);
            auto e = trimmed.find_last_not_of(" \t");
            if (e != std::string::npos) trimmed = trimmed.substr(0, e + 1);

            if (!trimmed.empty()) {
                std::string canonical = trimmed + ".jsonl";
                if (file_set.count(canonical)) {
                    return (fs::path(sessions_dir) / canonical).string();
                }

                // Topic variants
                std::string topic_prefix = trimmed + "-topic-";
                std::vector<std::string> topic_variants;
                for (const auto& f : files) {
                    if (f.compare(0, topic_prefix.size(), topic_prefix) == 0 &&
                        f.size() > 6 && f.substr(f.size() - 6) == ".jsonl" &&
                        f.find(".reset.") == std::string::npos) {
                        topic_variants.push_back(f);
                    }
                }
                std::sort(topic_variants.begin(), topic_variants.end(), std::greater<>());
                if (!topic_variants.empty()) {
                    return (fs::path(sessions_dir) / topic_variants[0]).string();
                }
            }
        }

        if (!current_session_file) return std::nullopt;

        // Newest non-reset jsonl
        std::vector<std::string> non_reset;
        for (const auto& f : files) {
            if (f.size() > 6 && f.substr(f.size() - 6) == ".jsonl" &&
                f.find(".reset.") == std::string::npos) {
                non_reset.push_back(f);
            }
        }
        std::sort(non_reset.begin(), non_reset.end(), std::greater<>());
        if (!non_reset.empty()) {
            return (fs::path(sessions_dir) / non_reset[0]).string();
        }
    } catch (...) {}
    return std::nullopt;
}

// ============================================================================
// saveSessionToMemory (对齐 handler.ts:53-223)
// ============================================================================

std::optional<std::string> save_session_to_memory(
    const SessionMemoryHookEvent& event,
    std::function<std::string(const std::string& content)> slug_generator
) {
    // Only trigger on command:new or command:reset
    if (event.type != "command") return std::nullopt;
    if (event.action != "new" && event.action != "reset") return std::nullopt;

    try {
        std::string workspace_dir = event.workspace_dir;
        if (workspace_dir.empty()) return std::nullopt;

        std::string memory_dir = (fs::path(workspace_dir) / "memory").string();
        fs::create_directories(memory_dir);

        // Date string
        auto tp = std::chrono::system_clock::from_time_t(event.timestamp / 1000);
        auto time_t_val = std::chrono::system_clock::to_time_t(tp);
        std::tm tm_val{};
        gmtime_r(&time_t_val, &tm_val);

        std::ostringstream date_oss;
        date_oss << std::put_time(&tm_val, "%Y-%m-%d");
        std::string date_str = date_oss.str();

        std::ostringstream time_oss;
        time_oss << std::put_time(&tm_val, "%H:%M:%S");
        std::string time_str = time_oss.str();

        // Read session content
        std::string session_file = event.session_file;
        std::optional<std::string> session_content;
        std::string slug;

        if (!session_file.empty()) {
            session_content = get_recent_session_content_with_reset_fallback(
                session_file, event.message_count);

            if (session_content && !session_content->empty() && slug_generator) {
                slug = slug_generator(*session_content);
            }
        }

        // Fallback slug: HHMM
        if (slug.empty()) {
            std::ostringstream slug_oss;
            slug_oss << std::put_time(&tm_val, "%H%M");
            slug = slug_oss.str();
        }

        // Create filename
        std::string filename = date_str + "-" + slug + ".md";
        std::string memory_file_path = (fs::path(memory_dir) / filename).string();

        // Build markdown entry
        std::ostringstream entry;
        entry << "# Session: " << date_str << " " << time_str << " UTC\n\n";
        entry << "- **Session Key**: " << event.session_key << "\n";
        entry << "- **Session ID**: " << (event.session_id.empty() ? "unknown" : event.session_id) << "\n";
        entry << "- **Source**: " << (event.command_source.empty() ? "unknown" : event.command_source) << "\n\n";

        if (session_content && !session_content->empty()) {
            entry << "## Conversation Summary\n\n" << *session_content << "\n";
        }

        // Write file
        std::ofstream ofs(memory_file_path);
        if (!ofs.is_open()) return std::nullopt;
        ofs << entry.str();
        ofs.close();

        return memory_file_path;
    } catch (...) {
        return std::nullopt;
    }
}

}  // namespace memory
}  // namespace hunterclaw
