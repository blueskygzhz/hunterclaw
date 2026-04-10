#include "hunterclaw/react/reply_media_paths.h"
#include <algorithm>
#include <regex>
#include <set>
#include <spdlog/spdlog.h>

namespace hunterclaw {
namespace react {

namespace {
auto log() {
    static auto logger = spdlog::get("react");
    if (!logger) logger = spdlog::stdout_logger_mt("react");
    return logger;
}

std::string trim(const std::string& s) {
    auto start = s.find_first_not_of(" \t\n\r");
    auto end = s.find_last_not_of(" \t\n\r");
    if (start == std::string::npos) return "";
    return s.substr(start, end - start + 1);
}

std::string to_lower(const std::string& s) {
    std::string r = s;
    std::transform(r.begin(), r.end(), r.begin(), ::tolower);
    return r;
}

const std::string AGENT_STATE_MEDIA_DIRNAME = ".hunterclaw/media";
} // anon

// 对齐 reply-media-paths.ts:14-18
bool is_http_url(const std::string& media) {
    auto lower = to_lower(media);
    return lower.find("http://") == 0 || lower.find("https://") == 0;
}

bool is_file_url(const std::string& media) {
    return to_lower(media).find("file://") == 0;
}

bool is_windows_drive_path(const std::string& media) {
    if (media.size() < 3) return false;
    return std::isalpha(media[0]) && media[1] == ':' && (media[2] == '/' || media[2] == '\\');
}

bool has_scheme(const std::string& media) {
    static const std::regex scheme_re("^[a-zA-Z][a-zA-Z0-9+.-]*:");
    return std::regex_search(media, scheme_re);
}

bool has_file_extension(const std::string& media) {
    static const std::regex ext_re(R"(\.\w{1,10}$)");
    return std::regex_search(media, ext_re);
}

// 对齐 reply-media-paths.ts:22-25
bool is_path_inside(const std::string& root, const std::string& candidate) {
    namespace fs = std::filesystem;
    try {
        auto rel = fs::relative(fs::absolute(candidate), fs::absolute(root));
        auto rel_str = rel.string();
        return rel_str.empty() || (rel_str.find("..") != 0 && !fs::path(rel_str).is_absolute()) == 0;
    } catch (...) {
        return false;
    }
}

// 对齐 reply-media-paths.ts:51-63
bool is_likely_local_media_source(const std::string& media) {
    if (is_file_url(media)) return true;
    if (media.find("/") == 0) return true;
    if (media.find("./") == 0) return true;
    if (media.find("../") == 0) return true;
    if (media.find("~") == 0) return true;
    if (is_windows_drive_path(media)) return true;
    if (media.find("\\\\") == 0) return true;
    if (!has_scheme(media) &&
        (media.find('/') != std::string::npos ||
         media.find('\\') != std::string::npos ||
         has_file_extension(media))) {
        return true;
    }
    return false;
}

// 对齐 reply-media-paths.ts:37-49
bool is_allowed_absolute_reply_media_path(
    const std::string& candidate,
    const std::string& workspace_dir,
    const std::string& config_dir,
    const std::optional<std::string>& sandbox_root
) {
    namespace fs = std::filesystem;
    // Check managed global media path
    if (!config_dir.empty()) {
        auto global_media_root = fs::path(config_dir) / "media";
        try {
            auto rel = fs::relative(fs::absolute(candidate), fs::absolute(global_media_root));
            auto rel_str = rel.string();
            if (!rel_str.empty() && rel_str.find("..") != 0 && !fs::path(rel_str).is_absolute()) {
                auto first_segment = fs::path(rel_str).begin()->string();
                if (first_segment == "outbound" || first_segment.find("tool-") == 0) {
                    return true;
                }
            }
        } catch (...) {}
    }

    // Check volatile roots (.hunterclaw/media under workspace or sandbox)
    std::vector<std::string> volatile_roots;
    volatile_roots.push_back((fs::path(workspace_dir) / AGENT_STATE_MEDIA_DIRNAME).string());
    if (sandbox_root.has_value()) {
        volatile_roots.push_back(
            (fs::path(*sandbox_root) / AGENT_STATE_MEDIA_DIRNAME).string());
    }
    for (const auto& root : volatile_roots) {
        if (is_path_inside(root, candidate)) return true;
    }
    return false;
}

// 对齐 reply-media-paths.ts:125-189
std::string normalize_media_source(
    const std::string& raw,
    const std::string& workspace_dir,
    bool workspace_only,
    const std::optional<std::string>& sandbox_root,
    const std::string& config_dir
) {
    auto media = trim(raw);
    if (media.empty()) return media;

    // Block data: URLs
    if (to_lower(media).find("data:") == 0) {
        throw std::runtime_error("Data URLs are not allowed in reply media");
    }

    // HTTP URL → passthrough
    if (is_http_url(media)) return media;

    // file:// URL → block
    if (is_file_url(media)) {
        throw std::runtime_error(
            "Absolute host-local MEDIA file URLs are blocked in normal replies. "
            "Use a safe relative path or the message tool.");
    }

    // Not likely local → passthrough
    if (!is_likely_local_media_source(media)) return media;

    // Relative path → resolve against workspace
    namespace fs = std::filesystem;
    if (!fs::path(media).is_absolute()) {
        return (fs::path(workspace_dir) / media).string();
    }

    // Absolute path — check if allowed
    if (is_allowed_absolute_reply_media_path(media, workspace_dir, config_dir, sandbox_root)) {
        return media;
    }

    if (workspace_only) {
        throw std::runtime_error(
            "Absolute host-local MEDIA paths are blocked in normal replies. "
            "Use a safe relative path or the message tool.");
    }

    // Default: allow (non-workspace-only mode)
    return media;
}

// 对齐 reply-media-paths.ts:69-228
std::function<ReplyPayload(const ReplyPayload&)> create_reply_media_path_normalizer(
    const ReplyMediaPathNormalizerParams& params
) {
    // Persistent cache for dedup
    auto persisted_cache = std::make_shared<std::map<std::string, std::string>>();

    return [params, persisted_cache](const ReplyPayload& payload) -> ReplyPayload {
        // Collect all media URLs
        std::vector<std::string> media_list;
        if (payload.media_url.has_value()) media_list.push_back(*payload.media_url);
        for (const auto& url : payload.media_urls) {
            media_list.push_back(url);
        }
        if (media_list.empty()) return payload;

        std::vector<std::string> normalized_media;
        std::set<std::string> seen;

        for (const auto& media : media_list) {
            std::string normalized;
            try {
                normalized = normalize_media_source(
                    media, params.workspace_dir, params.workspace_only,
                    params.sandbox_root, params.config_dir);

                // Persist volatile agent media (dedup via cache)
                if (params.persist_volatile_media && !normalized.empty() &&
                    std::filesystem::path(normalized).is_absolute()) {
                    auto it = persisted_cache->find(normalized);
                    if (it != persisted_cache->end()) {
                        normalized = it->second;
                    } else {
                        try {
                            auto persisted = params.persist_volatile_media(normalized);
                            (*persisted_cache)[normalized] = persisted;
                            normalized = persisted;
                        } catch (const std::exception& e) {
                            log()->debug("failed to persist volatile reply media {}: {}",
                                        media, e.what());
                        }
                    }
                }
            } catch (const std::exception& e) {
                log()->debug("dropping blocked reply media {}: {}", media, e.what());
                continue;
            }

            if (normalized.empty() || seen.count(normalized)) continue;
            seen.insert(normalized);
            normalized_media.push_back(normalized);
        }

        if (normalized_media.empty()) {
            ReplyPayload result = payload;
            result.media_url = std::nullopt;
            result.media_urls.clear();
            return result;
        }

        ReplyPayload result = payload;
        result.media_url = normalized_media[0];
        result.media_urls = normalized_media;
        return result;
    };
}

} // namespace react
} // namespace hunterclaw
