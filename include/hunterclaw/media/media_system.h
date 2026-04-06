#pragma once
// Aligned with openclaw: src/media/ (41 files)
// Complete media processing subsystem: store, server, fetch, parse, mime, path policy

#include "hunterclaw/core/result.h"
#include <string>
#include <vector>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <functional>
#include <filesystem>

namespace hunterclaw {
namespace media {

// ============================================================================
// Constants (对齐 store.ts)
// ============================================================================
static constexpr int64_t MEDIA_MAX_BYTES = 5 * 1024 * 1024;  // 5MB
static constexpr int64_t DEFAULT_TTL_MS = 2 * 60 * 1000;     // 2 minutes
static constexpr int MEDIA_FILE_MODE = 0644;

// ============================================================================
// MIME Types (对齐 mime.ts)
// ============================================================================

// Extension → MIME mapping
inline const std::unordered_map<std::string, std::string>& mime_by_ext() {
    static const std::unordered_map<std::string, std::string> map = {
        {".jpg", "image/jpeg"}, {".jpeg", "image/jpeg"}, {".png", "image/png"},
        {".webp", "image/webp"}, {".gif", "image/gif"}, {".heic", "image/heic"},
        {".heif", "image/heif"}, {".ogg", "audio/ogg"}, {".mp3", "audio/mpeg"},
        {".wav", "audio/wav"}, {".flac", "audio/flac"}, {".aac", "audio/aac"},
        {".opus", "audio/opus"}, {".m4a", "audio/x-m4a"}, {".mp4", "video/mp4"},
        {".mov", "video/quicktime"}, {".pdf", "application/pdf"},
        {".json", "application/json"}, {".zip", "application/zip"},
        {".gz", "application/gzip"}, {".tar", "application/x-tar"},
        {".doc", "application/msword"}, {".docx", "application/vnd.openxmlformats-officedocument.wordprocessingml.document"},
        {".xls", "application/vnd.ms-excel"}, {".xlsx", "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet"},
        {".csv", "text/csv"}, {".txt", "text/plain"}, {".md", "text/markdown"},
    };
    return map;
}

// MIME → Extension mapping
inline const std::unordered_map<std::string, std::string>& ext_by_mime() {
    static const std::unordered_map<std::string, std::string> map = {
        {"image/jpeg", ".jpg"}, {"image/png", ".png"}, {"image/webp", ".webp"},
        {"image/gif", ".gif"}, {"image/heic", ".heic"}, {"image/heif", ".heif"},
        {"audio/ogg", ".ogg"}, {"audio/mpeg", ".mp3"}, {"audio/wav", ".wav"},
        {"audio/flac", ".flac"}, {"audio/aac", ".aac"}, {"audio/opus", ".opus"},
        {"audio/x-m4a", ".m4a"}, {"audio/mp4", ".m4a"}, {"video/mp4", ".mp4"},
        {"video/quicktime", ".mov"}, {"application/pdf", ".pdf"},
        {"application/json", ".json"}, {"application/zip", ".zip"},
        {"application/gzip", ".gz"}, {"application/x-tar", ".tar"},
        {"text/csv", ".csv"}, {"text/plain", ".txt"}, {"text/markdown", ".md"},
    };
    return map;
}

// Audio file extensions
inline const std::unordered_set<std::string>& audio_extensions() {
    static const std::unordered_set<std::string> exts = {
        ".aac", ".caf", ".flac", ".m4a", ".mp3", ".oga", ".ogg", ".opus", ".wav"
    };
    return exts;
}

enum class MediaKind { Image, Audio, Video, Document, Unknown };

// Aligned with openclaw: normalizeMimeType
inline std::optional<std::string> normalize_mime_type(const std::string& mime) {
    if (mime.empty()) return std::nullopt;
    auto pos = mime.find(';');
    std::string cleaned = (pos != std::string::npos) ? mime.substr(0, pos) : mime;
    // trim + lowercase
    auto s = cleaned.find_first_not_of(" \t");
    auto e = cleaned.find_last_not_of(" \t");
    if (s == std::string::npos) return std::nullopt;
    cleaned = cleaned.substr(s, e - s + 1);
    for (auto& c : cleaned) c = std::tolower(c);
    return cleaned.empty() ? std::nullopt : std::optional<std::string>(cleaned);
}

// Aligned with openclaw: extensionForMime
inline std::optional<std::string> extension_for_mime(const std::string& mime) {
    auto norm = normalize_mime_type(mime);
    if (!norm) return std::nullopt;
    auto it = ext_by_mime().find(*norm);
    return (it != ext_by_mime().end()) ? std::optional<std::string>(it->second) : std::nullopt;
}

// Aligned with openclaw: getFileExtension
inline std::optional<std::string> get_file_extension(const std::string& file_path) {
    if (file_path.empty()) return std::nullopt;
    auto pos = file_path.rfind('.');
    if (pos == std::string::npos || pos == file_path.size() - 1) return std::nullopt;
    std::string ext = file_path.substr(pos);
    for (auto& c : ext) c = std::tolower(c);
    return ext;
}

// Aligned with openclaw: detectMime (simplified, no magic bytes sniffing)
inline std::optional<std::string> detect_mime(const std::string& file_path,
                                               const std::string& header_mime = "") {
    auto ext = get_file_extension(file_path);
    std::optional<std::string> ext_mime;
    if (ext) {
        auto it = mime_by_ext().find(*ext);
        if (it != mime_by_ext().end()) ext_mime = it->second;
    }
    auto norm_header = normalize_mime_type(header_mime);
    // Prefer ext-based, then header
    if (ext_mime) return ext_mime;
    if (norm_header && *norm_header != "application/octet-stream") return norm_header;
    return norm_header;
}

// Aligned with openclaw: isAudioFileName
inline bool is_audio_file_name(const std::string& file_name) {
    auto ext = get_file_extension(file_name);
    return ext && audio_extensions().count(*ext) > 0;
}

// Aligned with openclaw: isGifMedia
inline bool is_gif_media(const std::string& content_type, const std::string& file_name = "") {
    if (normalize_mime_type(content_type) == "image/gif") return true;
    return get_file_extension(file_name) == ".gif";
}

// Aligned with openclaw: kindFromMime
inline MediaKind kind_from_mime(const std::string& mime) {
    auto norm = normalize_mime_type(mime);
    if (!norm) return MediaKind::Unknown;
    if (norm->find("image/") == 0) return MediaKind::Image;
    if (norm->find("audio/") == 0) return MediaKind::Audio;
    if (norm->find("video/") == 0) return MediaKind::Video;
    if (norm->find("application/pdf") == 0) return MediaKind::Document;
    if (norm->find("text/") == 0) return MediaKind::Document;
    return MediaKind::Unknown;
}

// ============================================================================
// Saved Media (对齐 store.ts)
// ============================================================================
struct SavedMedia {
    std::string id;
    std::string path;
    int64_t size = 0;
    std::optional<std::string> content_type;
};

// Aligned with openclaw: sanitizeFilename
inline std::string sanitize_filename(const std::string& name) {
    if (name.empty()) return "";
    std::string result;
    for (char c : name) {
        if (std::isalnum(c) || c == '.' || c == '-' || c == '_') result += c;
        else result += '_';
    }
    // Collapse underscores, trim, limit
    std::string collapsed;
    bool last_underscore = false;
    for (char c : result) {
        if (c == '_') { if (!last_underscore) collapsed += c; last_underscore = true; }
        else { collapsed += c; last_underscore = false; }
    }
    while (!collapsed.empty() && collapsed.front() == '_') collapsed.erase(0, 1);
    while (!collapsed.empty() && collapsed.back() == '_') collapsed.pop_back();
    if (collapsed.size() > 60) collapsed = collapsed.substr(0, 60);
    return collapsed;
}

// Aligned with openclaw: extractOriginalFilename
inline std::string extract_original_filename(const std::string& file_path) {
    auto basename = std::filesystem::path(file_path).filename().string();
    if (basename.empty()) return "file.bin";
    auto ext = std::filesystem::path(basename).extension().string();
    auto stem = std::filesystem::path(basename).stem().string();
    // Check for ---{uuid} pattern
    auto sep_pos = stem.rfind("---");
    if (sep_pos != std::string::npos && stem.size() - sep_pos - 3 == 36) {
        return stem.substr(0, sep_pos) + ext;
    }
    return basename;
}

// ============================================================================
// Media Store (对齐 store.ts)
// ============================================================================
class MediaStore {
public:
    explicit MediaStore(const std::string& state_dir);

    std::string get_media_dir() const;
    void ensure_media_dir();
    void clean_old_media(int64_t ttl_ms = DEFAULT_TTL_MS, bool recursive = false);
    core::Result<SavedMedia> save_media_source(const std::string& source,
                                                const std::string& subdir = "");
    core::Result<SavedMedia> save_media_buffer(const std::string& data,
                                                const std::string& content_type = "",
                                                const std::string& subdir = "inbound",
                                                const std::string& original_filename = "");
private:
    std::string state_dir_;
    std::string media_dir_;
};

// ============================================================================
// Media Parse (对齐 parse.ts)
// ============================================================================
struct MediaParseResult {
    std::string text;
    std::vector<std::string> media_urls;
    std::optional<std::string> media_url;  // legacy first item
    bool audio_as_voice = false;
};

// Aligned with openclaw: splitMediaFromOutput
MediaParseResult split_media_from_output(const std::string& raw);

// Aligned with openclaw: normalizeMediaSource
inline std::string normalize_media_source(const std::string& src) {
    if (src.find("file://") == 0) return src.substr(7);
    return src;
}

// Aligned with openclaw: isValidMedia
bool is_valid_media(const std::string& candidate, bool allow_spaces = false,
                    bool allow_bare_filename = false);

// ============================================================================
// Inbound Path Policy (对齐 inbound-path-policy.ts)
// ============================================================================

// Aligned with openclaw: isInboundPathAllowed
bool is_inbound_path_allowed(const std::string& file_path,
                              const std::vector<std::string>& roots,
                              const std::vector<std::string>& fallback_roots = {});

// Aligned with openclaw: normalizeInboundPathRoots
std::vector<std::string> normalize_inbound_path_roots(const std::vector<std::string>& roots);

// Aligned with openclaw: mergeInboundPathRoots
std::vector<std::string> merge_inbound_path_roots(
    const std::vector<std::string>& roots1,
    const std::vector<std::string>& roots2 = {});

// ============================================================================
// Local Roots (对齐 local-roots.ts)
// ============================================================================
std::vector<std::string> get_default_media_local_roots(const std::string& state_dir);
std::vector<std::string> get_agent_scoped_media_local_roots(const std::string& state_dir,
                                                             const std::string& workspace_dir = "");

// ============================================================================
// Media Fetch Error (对齐 fetch.ts)
// ============================================================================
enum class MediaFetchErrorCode { MaxBytes, HttpError, FetchFailed };

class MediaFetchError : public std::runtime_error {
public:
    MediaFetchErrorCode code;
    MediaFetchError(MediaFetchErrorCode c, const std::string& msg)
        : std::runtime_error(msg), code(c) {}
};

} // namespace media
} // namespace hunterclaw
