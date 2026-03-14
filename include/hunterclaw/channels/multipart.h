#pragma once

#include "hunterclaw/core/result.h"
#include <string>
#include <vector>
#include <map>

namespace hunterclaw {
namespace channels {

// ============================================================================
// Multipart/Form-Data Encoder
// ============================================================================

/// Multipart form field
struct MultipartField {
    std::string name;
    std::string value;
    std::string content_type;  // Optional, for files
    std::string filename;       // Optional, for files
    std::vector<uint8_t> binary_data;  // For binary files
    
    // Text field constructor
    MultipartField(const std::string& n, const std::string& v)
        : name(n), value(v) {}
    
    // File field constructor
    MultipartField(
        const std::string& n,
        const std::vector<uint8_t>& data,
        const std::string& fname,
        const std::string& ctype = "application/octet-stream"
    ) : name(n), binary_data(data), filename(fname), content_type(ctype) {}
};

/// Multipart form encoder
class MultipartFormEncoder {
public:
    MultipartFormEncoder();
    
    /// Add text field
    void add_field(const std::string& name, const std::string& value);
    
    /// Add file field from buffer
    void add_file(
        const std::string& name,
        const std::vector<uint8_t>& data,
        const std::string& filename,
        const std::string& content_type = "application/octet-stream"
    );
    
    /// Add file field from file path
    core::Result<bool> add_file_from_path(
        const std::string& name,
        const std::string& file_path,
        const std::string& content_type = ""
    );
    
    /// Get boundary string
    std::string boundary() const { return boundary_; }
    
    /// Get Content-Type header value
    std::string content_type() const {
        return "multipart/form-data; boundary=" + boundary_;
    }
    
    /// Encode to body
    std::vector<uint8_t> encode() const;
    
    /// Encode to string (for debugging, text fields only)
    std::string encode_string() const;

private:
    std::string boundary_;
    std::vector<MultipartField> fields_;
    
    static std::string generate_boundary();
};

// ============================================================================
// MIME Type Detection
// ============================================================================

/// Detect MIME type from file extension
std::string mime_from_extension(const std::string& filename);

/// Detect MIME type from file content (magic bytes)
std::string mime_from_buffer(const std::vector<uint8_t>& buffer);

/// Comprehensive MIME detection (buffer + extension)
std::string detect_mime_type(
    const std::vector<uint8_t>& buffer,
    const std::string& filename = ""
);

/// Get file extension for MIME type
std::string extension_for_mime(const std::string& mime_type);

/// Sanitize filename for upload (RFC 5987 percent-encoding)
std::string sanitize_filename(const std::string& filename);

// ============================================================================
// Media Kind Classification
// ============================================================================

enum class MediaKind {
    Image,
    Video,
    Audio,
    Document,
    Unknown
};

/// Classify media kind from MIME type
MediaKind kind_from_mime(const std::string& mime_type);

/// Check if MIME type is image
bool is_image_mime(const std::string& mime_type);

/// Check if MIME type is video
bool is_video_mime(const std::string& mime_type);

/// Check if MIME type is audio
bool is_audio_mime(const std::string& mime_type);

/// Check if GIF (animated image)
bool is_gif_mime(const std::string& mime_type, const std::string& filename = "");

} // namespace channels
} // namespace hunterclaw
