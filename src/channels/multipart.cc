#include "hunterclaw/channels/multipart.h"
#include <fstream>
#include <sstream>
#include <random>
#include <algorithm>
#include <cstring>

namespace hunterclaw {
namespace channels {

// ============================================================================
// Multipart Form Encoder Implementation
// ============================================================================

std::string MultipartFormEncoder::generate_boundary() {
    // Generate random boundary string
    static const char charset[] = 
        "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, sizeof(charset) - 2);
    
    std::string boundary = "----HunterClawBoundary";
    for (int i = 0; i < 16; ++i) {
        boundary += charset[dis(gen)];
    }
    return boundary;
}

MultipartFormEncoder::MultipartFormEncoder()
    : boundary_(generate_boundary()) {}

void MultipartFormEncoder::add_field(
    const std::string& name,
    const std::string& value
) {
    fields_.emplace_back(name, value);
}

void MultipartFormEncoder::add_file(
    const std::string& name,
    const std::vector<uint8_t>& data,
    const std::string& filename,
    const std::string& content_type
) {
    fields_.emplace_back(name, data, filename, content_type);
}

core::Result<bool> MultipartFormEncoder::add_file_from_path(
    const std::string& name,
    const std::string& file_path,
    const std::string& content_type
) {
    // Read file
    std::ifstream file(file_path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        return core::Result<bool>::error("Failed to open file: " + file_path);
    }
    
    auto size = file.tellg();
    file.seekg(0, std::ios::beg);
    
    std::vector<uint8_t> buffer(size);
    if (!file.read(reinterpret_cast<char*>(buffer.data()), size)) {
        return core::Result<bool>::error("Failed to read file: " + file_path);
    }
    
    // Extract filename
    size_t slash_pos = file_path.find_last_of("/\\");
    std::string filename = (slash_pos != std::string::npos)
        ? file_path.substr(slash_pos + 1)
        : file_path;
    
    // Detect MIME type if not provided
    std::string mime = content_type.empty()
        ? detect_mime_type(buffer, filename)
        : content_type;
    
    add_file(name, buffer, filename, mime);
    return core::Result<bool>::ok(true);
}

std::vector<uint8_t> MultipartFormEncoder::encode() const {
    std::vector<uint8_t> result;
    
    for (const auto& field : fields_) {
        // Boundary line
        std::string boundary_line = "--" + boundary_ + "\r\n";
        result.insert(result.end(), boundary_line.begin(), boundary_line.end());
        
        // Content-Disposition header
        std::string disposition = "Content-Disposition: form-data; name=\"" + 
                                  field.name + "\"";
        
        if (!field.filename.empty()) {
            // File field - add filename
            std::string safe_filename = sanitize_filename(field.filename);
            disposition += "; filename=\"" + safe_filename + "\"";
        }
        disposition += "\r\n";
        result.insert(result.end(), disposition.begin(), disposition.end());
        
        // Content-Type header (for files)
        if (!field.content_type.empty()) {
            std::string ctype = "Content-Type: " + field.content_type + "\r\n";
            result.insert(result.end(), ctype.begin(), ctype.end());
        }
        
        // Empty line
        result.insert(result.end(), {'\r', '\n'});
        
        // Field data
        if (!field.binary_data.empty()) {
            // Binary data (file)
            result.insert(result.end(), field.binary_data.begin(), field.binary_data.end());
        } else {
            // Text data
            result.insert(result.end(), field.value.begin(), field.value.end());
        }
        
        // Line ending
        result.insert(result.end(), {'\r', '\n'});
    }
    
    // Final boundary
    std::string final_boundary = "--" + boundary_ + "--\r\n";
    result.insert(result.end(), final_boundary.begin(), final_boundary.end());
    
    return result;
}

std::string MultipartFormEncoder::encode_string() const {
    auto bytes = encode();
    return std::string(bytes.begin(), bytes.end());
}

// ============================================================================
// MIME Type Detection
// ============================================================================

static const std::map<std::string, std::string> MIME_BY_EXT = {
    // Images
    {".jpg", "image/jpeg"}, {".jpeg", "image/jpeg"},
    {".png", "image/png"}, {".gif", "image/gif"},
    {".webp", "image/webp"}, {".bmp", "image/bmp"},
    {".svg", "image/svg+xml"}, {".ico", "image/x-icon"},
    
    // Videos
    {".mp4", "video/mp4"}, {".mov", "video/quicktime"},
    {".avi", "video/x-msvideo"}, {".webm", "video/webm"},
    {".mkv", "video/x-matroska"}, {".flv", "video/x-flv"},
    
    // Audio
    {".mp3", "audio/mpeg"}, {".ogg", "audio/ogg"},
    {".wav", "audio/wav"}, {".m4a", "audio/mp4"},
    {".opus", "audio/opus"}, {".flac", "audio/flac"},
    
    // Documents
    {".pdf", "application/pdf"},
    {".doc", "application/msword"},
    {".docx", "application/vnd.openxmlformats-officedocument.wordprocessingml.document"},
    {".xls", "application/vnd.ms-excel"},
    {".xlsx", "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet"},
    {".ppt", "application/vnd.ms-powerpoint"},
    {".pptx", "application/vnd.openxmlformats-officedocument.presentationml.presentation"},
    {".txt", "text/plain"},
    {".json", "application/json"},
    {".xml", "application/xml"},
    {".zip", "application/zip"},
    {".tar", "application/x-tar"},
    {".gz", "application/gzip"},
};

static const std::map<std::string, std::string> EXT_BY_MIME = {
    {"image/jpeg", ".jpg"}, {"image/png", ".png"}, {"image/gif", ".gif"},
    {"image/webp", ".webp"}, {"image/bmp", ".bmp"},
    {"video/mp4", ".mp4"}, {"video/quicktime", ".mov"},
    {"audio/mpeg", ".mp3"}, {"audio/ogg", ".ogg"},
    {"application/pdf", ".pdf"}, {"text/plain", ".txt"},
    {"application/json", ".json"}, {"application/zip", ".zip"},
};

std::string mime_from_extension(const std::string& filename) {
    // Extract extension
    size_t dot_pos = filename.find_last_of('.');
    if (dot_pos == std::string::npos) {
        return "application/octet-stream";
    }
    
    std::string ext = filename.substr(dot_pos);
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    
    auto it = MIME_BY_EXT.find(ext);
    if (it != MIME_BY_EXT.end()) {
        return it->second;
    }
    
    return "application/octet-stream";
}

std::string mime_from_buffer(const std::vector<uint8_t>& buffer) {
    if (buffer.size() < 16) {
        return "";
    }
    
    // Magic bytes detection
    const uint8_t* data = buffer.data();
    
    // PNG: 89 50 4E 47 0D 0A 1A 0A
    if (buffer.size() >= 8 && data[0] == 0x89 && data[1] == 0x50 &&
        data[2] == 0x4E && data[3] == 0x47) {
        return "image/png";
    }
    
    // JPEG: FF D8 FF
    if (buffer.size() >= 3 && data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF) {
        return "image/jpeg";
    }
    
    // GIF: 47 49 46 38
    if (buffer.size() >= 4 && data[0] == 0x47 && data[1] == 0x49 &&
        data[2] == 0x46 && data[3] == 0x38) {
        return "image/gif";
    }
    
    // WebP: RIFF ... WEBP
    if (buffer.size() >= 12 && data[0] == 0x52 && data[1] == 0x49 &&
        data[2] == 0x46 && data[3] == 0x46 && data[8] == 0x57 &&
        data[9] == 0x45 && data[10] == 0x42 && data[11] == 0x50) {
        return "image/webp";
    }
    
    // PDF: 25 50 44 46 (%PDF)
    if (buffer.size() >= 4 && data[0] == 0x25 && data[1] == 0x50 &&
        data[2] == 0x44 && data[3] == 0x46) {
        return "application/pdf";
    }
    
    // ZIP: 50 4B 03 04
    if (buffer.size() >= 4 && data[0] == 0x50 && data[1] == 0x4B &&
        data[2] == 0x03 && data[3] == 0x04) {
        return "application/zip";
    }
    
    // MP4/M4A: ... ftyp
    if (buffer.size() >= 12) {
        if (memcmp(data + 4, "ftyp", 4) == 0) {
            // Check for audio-specific brands
            if (memcmp(data + 8, "M4A ", 4) == 0) {
                return "audio/mp4";
            }
            return "video/mp4";
        }
    }
    
    // MP3: FF FB or FF F3 (MPEG audio)
    if (buffer.size() >= 2 && data[0] == 0xFF && 
        (data[1] == 0xFB || data[1] == 0xF3)) {
        return "audio/mpeg";
    }
    
    // OGG: 4F 67 67 53 (OggS)
    if (buffer.size() >= 4 && data[0] == 0x4F && data[1] == 0x67 &&
        data[2] == 0x67 && data[3] == 0x53) {
        return "audio/ogg";
    }
    
    return "";
}

std::string detect_mime_type(
    const std::vector<uint8_t>& buffer,
    const std::string& filename
) {
    // Try magic bytes first (most reliable)
    std::string mime_from_magic = mime_from_buffer(buffer);
    if (!mime_from_magic.empty() && mime_from_magic != "application/octet-stream") {
        return mime_from_magic;
    }
    
    // Fall back to extension
    if (!filename.empty()) {
        std::string mime_from_ext = mime_from_extension(filename);
        if (mime_from_ext != "application/octet-stream") {
            return mime_from_ext;
        }
    }
    
    // Default
    return mime_from_magic.empty() ? "application/octet-stream" : mime_from_magic;
}

std::string extension_for_mime(const std::string& mime_type) {
    auto it = EXT_BY_MIME.find(mime_type);
    if (it != EXT_BY_MIME.end()) {
        return it->second;
    }
    return ".bin";
}

std::string sanitize_filename(const std::string& filename) {
    // Check if filename is pure ASCII
    bool is_ascii = true;
    for (char c : filename) {
        if (static_cast<unsigned char>(c) > 127) {
            is_ascii = false;
            break;
        }
    }
    
    if (is_ascii) {
        return filename;
    }
    
    // RFC 5987 percent-encoding for non-ASCII
    std::ostringstream escaped;
    for (unsigned char c : filename) {
        if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || 
            (c >= 'a' && c <= 'z') || c == '.' || c == '_' || c == '-') {
            escaped << c;
        } else {
            // Percent-encode
            escaped << '%' << std::hex << std::uppercase 
                    << static_cast<int>(c);
        }
    }
    
    return escaped.str();
}

// ============================================================================
// Media Kind Classification
// ============================================================================

MediaKind kind_from_mime(const std::string& mime_type) {
    if (mime_type.empty()) {
        return MediaKind::Unknown;
    }
    
    // Extract primary type
    size_t slash_pos = mime_type.find('/');
    if (slash_pos == std::string::npos) {
        return MediaKind::Unknown;
    }
    
    std::string primary = mime_type.substr(0, slash_pos);
    std::transform(primary.begin(), primary.end(), primary.begin(), ::tolower);
    
    if (primary == "image") return MediaKind::Image;
    if (primary == "video") return MediaKind::Video;
    if (primary == "audio") return MediaKind::Audio;
    if (primary == "text" || mime_type.find("pdf") != std::string::npos) {
        return MediaKind::Document;
    }
    
    return MediaKind::Unknown;
}

bool is_image_mime(const std::string& mime_type) {
    return kind_from_mime(mime_type) == MediaKind::Image;
}

bool is_video_mime(const std::string& mime_type) {
    return kind_from_mime(mime_type) == MediaKind::Video;
}

bool is_audio_mime(const std::string& mime_type) {
    return kind_from_mime(mime_type) == MediaKind::Audio;
}

bool is_gif_mime(const std::string& mime_type, const std::string& filename) {
    if (mime_type == "image/gif") {
        return true;
    }
    if (!filename.empty()) {
        std::string lower_name = filename;
        std::transform(lower_name.begin(), lower_name.end(), 
                       lower_name.begin(), ::tolower);
        return lower_name.find(".gif") != std::string::npos;
    }
    return false;
}

} // namespace channels
} // namespace hunterclaw
