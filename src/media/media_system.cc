// Aligned with openclaw: src/media/ (store.ts + parse.ts + local-roots.ts + inbound-path-policy.ts)
#include "hunterclaw/media/media_system.h"
#include <spdlog/spdlog.h>
#include <fstream>
#include <chrono>
#include <regex>
#include <uuid/uuid.h>

namespace hunterclaw {
namespace media {

namespace {
auto log() {
    static auto logger = []() {
        auto l = spdlog::get("media");
        if (!l) l = spdlog::stdout_logger_mt("media");
        return l;
    }();
    return logger;
}

std::string generate_uuid() {
    uuid_t uuid;
    uuid_generate(uuid);
    char str[37];
    uuid_unparse(uuid, str);
    return std::string(str);
}

int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

bool looks_like_url(const std::string& src) {
    return src.find("http://") == 0 || src.find("https://") == 0;
}

} // namespace

// ============================================================================
// MediaStore
// ============================================================================
MediaStore::MediaStore(const std::string& state_dir)
    : state_dir_(state_dir)
    , media_dir_(state_dir + "/media") {}

std::string MediaStore::get_media_dir() const { return media_dir_; }

void MediaStore::ensure_media_dir() {
    std::filesystem::create_directories(media_dir_);
    std::filesystem::permissions(media_dir_, std::filesystem::perms::owner_all);
}

void MediaStore::clean_old_media(int64_t ttl_ms, bool recursive) {
    try {
        auto now = now_ms();
        for (auto& entry : std::filesystem::directory_iterator(media_dir_)) {
            if (entry.is_symlink()) continue;
            if (entry.is_directory() && recursive) {
                // Recurse into subdirectories
                for (auto& sub : std::filesystem::directory_iterator(entry.path())) {
                    if (sub.is_regular_file()) {
                        auto mtime = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::filesystem::last_write_time(sub).time_since_epoch()
                        ).count();
                        if (now - mtime > ttl_ms) {
                            std::filesystem::remove(sub);
                        }
                    }
                }
                continue;
            }
            if (entry.is_regular_file()) {
                auto mtime = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::filesystem::last_write_time(entry).time_since_epoch()
                ).count();
                if (now - mtime > ttl_ms) {
                    std::filesystem::remove(entry);
                }
            }
        }
    } catch (const std::exception& e) {
        log()->debug("clean_old_media: {}", e.what());
    }
}

core::Result<SavedMedia> MediaStore::save_media_source(const std::string& source,
                                                         const std::string& subdir) {
    ensure_media_dir();
    auto dir = subdir.empty() ? media_dir_ : media_dir_ + "/" + subdir;
    std::filesystem::create_directories(dir);
    clean_old_media(DEFAULT_TTL_MS, false);

    std::string base_id = generate_uuid();

    if (looks_like_url(source)) {
        // URL download - placeholder (requires libcurl integration)
        return core::Result<SavedMedia>::Err("URL download not yet implemented in media store");
    }

    // Local file copy
    try {
        auto file_size = std::filesystem::file_size(source);
        if (static_cast<int64_t>(file_size) > MEDIA_MAX_BYTES) {
            return core::Result<SavedMedia>::Err("Media exceeds 5MB limit");
        }
        auto mime = detect_mime(source);
        auto ext = mime ? extension_for_mime(*mime) : get_file_extension(source);
        std::string id = ext ? (base_id + *ext) : base_id;
        std::string dest = dir + "/" + id;
        std::filesystem::copy_file(source, dest);
        SavedMedia result;
        result.id = id;
        result.path = dest;
        result.size = file_size;
        result.content_type = mime;
        return core::Result<SavedMedia>::Ok(result);
    } catch (const std::exception& e) {
        return core::Result<SavedMedia>::Err(std::string("save_media_source: ") + e.what());
    }
}

core::Result<SavedMedia> MediaStore::save_media_buffer(const std::string& data,
                                                         const std::string& content_type,
                                                         const std::string& subdir,
                                                         const std::string& original_filename) {
    if (static_cast<int64_t>(data.size()) > MEDIA_MAX_BYTES) {
        return core::Result<SavedMedia>::Err("Media exceeds 5MB limit");
    }
    auto dir = media_dir_ + "/" + subdir;
    std::filesystem::create_directories(dir);
    std::string uuid = generate_uuid();
    auto ext = extension_for_mime(content_type);
    std::string id;
    if (!original_filename.empty()) {
        auto sanitized = sanitize_filename(std::filesystem::path(original_filename).stem().string());
        id = sanitized.empty() ? (uuid + (ext ? *ext : "")) : (sanitized + "---" + uuid + (ext ? *ext : ""));
    } else {
        id = uuid + (ext ? *ext : "");
    }
    std::string dest = dir + "/" + id;
    std::ofstream out(dest, std::ios::binary);
    out.write(data.data(), data.size());
    out.close();
    SavedMedia result;
    result.id = id;
    result.path = dest;
    result.size = data.size();
    result.content_type = normalize_mime_type(content_type);
    return core::Result<SavedMedia>::Ok(result);
}

// ============================================================================
// Media Parse (对齐 parse.ts)
// ============================================================================
bool is_valid_media(const std::string& candidate, bool allow_spaces, bool allow_bare_filename) {
    if (candidate.empty() || candidate.size() > 4096) return false;
    if (!allow_spaces) {
        for (char c : candidate) if (std::isspace(c)) return false;
    }
    if (candidate.find("http://") == 0 || candidate.find("https://") == 0) return true;
    if (candidate[0] == '/' || candidate.find("./") == 0 || candidate.find("../") == 0 || candidate[0] == '~') return true;
    if (allow_bare_filename) {
        static const std::regex has_ext_re("\\.\\w{1,10}$");
        if (std::regex_search(candidate, has_ext_re)) return true;
    }
    return false;
}

MediaParseResult split_media_from_output(const std::string& raw) {
    MediaParseResult result;
    std::string trimmed = raw;
    while (!trimmed.empty() && (trimmed.back() == ' ' || trimmed.back() == '\n' || trimmed.back() == '\r' || trimmed.back() == '\t'))
        trimmed.pop_back();
    if (trimmed.empty()) { result.text = ""; return result; }

    // Quick check
    bool has_media_token = false;
    {
        std::string lower = trimmed;
        for (auto& c : lower) c = std::tolower(c);
        has_media_token = lower.find("media:") != std::string::npos;
    }
    if (!has_media_token) { result.text = trimmed; return result; }

    // Line-by-line extraction
    std::vector<std::string> media;
    std::vector<std::string> kept_lines;
    std::istringstream stream(trimmed);
    std::string line;
    static const std::regex media_re("\\bMEDIA:\\s*`?([^\\n]+)`?", std::regex::icase);

    while (std::getline(stream, line)) {
        std::smatch match;
        std::string line_trimmed = line;
        auto s = line_trimmed.find_first_not_of(" \t");
        if (s != std::string::npos) line_trimmed = line_trimmed.substr(s);

        if (line_trimmed.find("MEDIA:") != 0) {
            kept_lines.push_back(line);
            continue;
        }

        if (std::regex_search(line, match, media_re)) {
            std::string payload = match[1].str();
            // Trim backticks, quotes
            while (!payload.empty() && (payload.front() == '`' || payload.front() == '"' || payload.front() == '\''))
                payload.erase(0, 1);
            while (!payload.empty() && (payload.back() == '`' || payload.back() == '"' || payload.back() == '\''))
                payload.pop_back();

            auto candidate = normalize_media_source(payload);
            // Trim
            auto cs = candidate.find_first_not_of(" \t");
            if (cs != std::string::npos) candidate = candidate.substr(cs);
            auto ce = candidate.find_last_not_of(" \t");
            if (ce != std::string::npos) candidate = candidate.substr(0, ce + 1);

            if (is_valid_media(candidate, true)) {
                media.push_back(candidate);
            }
            // Don't keep the MEDIA: line
        } else {
            kept_lines.push_back(line);
        }
    }

    // Build cleaned text
    std::string cleaned;
    for (const auto& l : kept_lines) {
        if (!cleaned.empty()) cleaned += "\n";
        cleaned += l;
    }
    // Trim trailing whitespace
    while (!cleaned.empty() && (cleaned.back() == '\n' || cleaned.back() == ' ')) cleaned.pop_back();

    result.text = cleaned;
    if (!media.empty()) {
        result.media_urls = media;
        result.media_url = media[0];
    }
    return result;
}

// ============================================================================
// Inbound Path Policy (对齐 inbound-path-policy.ts)
// ============================================================================
static std::optional<std::string> normalize_posix_absolute_path(const std::string& value) {
    std::string trimmed = value;
    auto s = trimmed.find_first_not_of(" \t");
    if (s == std::string::npos) return std::nullopt;
    trimmed = trimmed.substr(s);
    auto e = trimmed.find_last_not_of(" \t");
    trimmed = trimmed.substr(0, e + 1);
    if (trimmed.empty() || trimmed.find('\0') != std::string::npos) return std::nullopt;

    // Normalize path
    auto p = std::filesystem::path(trimmed).lexically_normal().string();
    if (p.empty() || p[0] != '/') return std::nullopt;
    if (p == "/") return std::nullopt;
    if (p.back() == '/') p.pop_back();
    return p;
}

static bool matches_root_pattern(const std::string& candidate, const std::string& root_pattern) {
    auto cand_parts = std::filesystem::path(candidate);
    auto root_parts = std::filesystem::path(root_pattern);

    auto cit = cand_parts.begin();
    auto rit = root_parts.begin();
    while (rit != root_parts.end()) {
        if (cit == cand_parts.end()) return false;
        if (*rit != "*" && *rit != *cit) return false;
        ++cit; ++rit;
    }
    return true;
}

std::vector<std::string> normalize_inbound_path_roots(const std::vector<std::string>& roots) {
    std::vector<std::string> result;
    std::unordered_set<std::string> seen;
    for (const auto& root : roots) {
        auto norm = normalize_posix_absolute_path(root);
        if (!norm || seen.count(*norm)) continue;
        seen.insert(*norm);
        result.push_back(*norm);
    }
    return result;
}

std::vector<std::string> merge_inbound_path_roots(
    const std::vector<std::string>& roots1,
    const std::vector<std::string>& roots2) {
    auto merged = normalize_inbound_path_roots(roots1);
    auto norm2 = normalize_inbound_path_roots(roots2);
    std::unordered_set<std::string> seen(merged.begin(), merged.end());
    for (const auto& r : norm2) {
        if (!seen.count(r)) { merged.push_back(r); seen.insert(r); }
    }
    return merged;
}

bool is_inbound_path_allowed(const std::string& file_path,
                              const std::vector<std::string>& roots,
                              const std::vector<std::string>& fallback_roots) {
    auto candidate = normalize_posix_absolute_path(file_path);
    if (!candidate) return false;
    auto norm_roots = normalize_inbound_path_roots(roots);
    auto effective = norm_roots.empty() ? normalize_inbound_path_roots(fallback_roots) : norm_roots;
    if (effective.empty()) return false;
    for (const auto& root : effective) {
        if (matches_root_pattern(*candidate, root)) return true;
    }
    return false;
}

// ============================================================================
// Local Roots (对齐 local-roots.ts)
// ============================================================================
std::vector<std::string> get_default_media_local_roots(const std::string& state_dir) {
    auto resolved = std::filesystem::absolute(state_dir).string();
    return {
        "/tmp",
        resolved + "/media",
        resolved + "/agents",
        resolved + "/workspace",
        resolved + "/sandboxes",
    };
}

std::vector<std::string> get_agent_scoped_media_local_roots(const std::string& state_dir,
                                                              const std::string& workspace_dir) {
    auto roots = get_default_media_local_roots(state_dir);
    if (!workspace_dir.empty()) {
        auto resolved = std::filesystem::absolute(workspace_dir).string();
        bool found = false;
        for (const auto& r : roots) if (r == resolved) { found = true; break; }
        if (!found) roots.push_back(resolved);
    }
    return roots;
}

} // namespace media
} // namespace hunterclaw
