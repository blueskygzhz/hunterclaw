#pragma once

#include "hunterclaw/core/result.h"
#include "hunterclaw/core/json.h"
#include <vector>
#include <cstdint>
#include <string>
#include <memory>

namespace hunterclaw {
namespace channels {

// ============================================================================
// Discord Voice Message - Three-Step Upload Flow
// ============================================================================

// Voice attachment metadata
struct VoiceAttachmentMetadata {
    float duration_secs;
    std::string waveform_base64;  // 256 samples, 0-255 amplitude
};

// Upload URL response (Step 1)
struct DiscordUploadUrlResponse {
    std::string upload_url;          // Presigned CDN URL
    std::string upload_filename;     // Server-generated filename
    std::vector<std::string> attachments;  // Attachment IDs
};

// Discord Voice Message Handler
class DiscordVoiceMessage {
public:
    DiscordVoiceMessage(const std::string& bot_token, const std::string& base_url);
    
    // Three-step upload flow
    core::Result<bool> send_voice_message(
        const std::string& channel_id,
        const std::vector<uint8_t>& audio_buffer,
        const std::string& filename
    );
    
    // Individual steps
    core::Result<DiscordUploadUrlResponse> request_upload_url(
        const std::string& channel_id,
        const std::string& filename,
        size_t file_size
    );
    
    core::Result<bool> upload_to_cdn(
        const std::string& upload_url,
        const std::vector<uint8_t>& audio_buffer
    );
    
    core::Result<bool> send_message_with_attachment(
        const std::string& channel_id,
        const std::string& upload_filename,
        const VoiceAttachmentMetadata& metadata
    );
    
    // Audio processing
    core::Result<std::vector<uint8_t>> convert_to_ogg_opus(
        const std::vector<uint8_t>& input_buffer,
        const std::string& input_format = "auto"  // auto-detect
    );
    
    core::Result<VoiceAttachmentMetadata> extract_metadata(
        const std::vector<uint8_t>& ogg_buffer
    );
    
private:
    std::string bot_token_;
    std::string base_url_;
};

// ============================================================================
// Audio Processor (ffmpeg wrapper)
// ============================================================================

class AudioProcessor {
public:
    // Format detection
    static std::string detect_audio_format(const std::vector<uint8_t>& buffer);
    
    // Convert to OGG/Opus @ 48kHz (Discord requirement)
    static core::Result<std::vector<uint8_t>> convert_to_ogg_opus(
        const std::vector<uint8_t>& input_buffer,
        const std::string& input_format
    );
    
    // Extract duration in seconds
    static core::Result<float> get_audio_duration(
        const std::vector<uint8_t>& ogg_buffer
    );
    
    // Generate waveform (256 samples)
    static core::Result<std::vector<uint8_t>> generate_waveform(
        const std::vector<uint8_t>& ogg_buffer
    );
    
    // Fallback: sine wave placeholder
    static std::vector<uint8_t> generate_sine_waveform(float duration_secs);
    
private:
    // Execute ffmpeg/ffprobe
    static core::Result<std::vector<uint8_t>> run_ffmpeg(
        const std::vector<std::string>& args,
        const std::vector<uint8_t>* stdin_data = nullptr
    );
    
    static core::Result<std::string> run_ffprobe(
        const std::vector<std::string>& args,
        const std::vector<uint8_t>& input_buffer
    );
};

// ============================================================================
// Waveform Encoder
// ============================================================================

class WaveformEncoder {
public:
    // Encode 256-sample waveform to base64
    static std::string encode_base64(const std::vector<uint8_t>& samples);
    
    // Decode base64 to samples
    static std::vector<uint8_t> decode_base64(const std::string& base64_str);
    
    // Normalize amplitude to 0-255 range
    static std::vector<uint8_t> normalize_amplitude(
        const std::vector<int16_t>& pcm_samples,
        size_t target_count = 256
    );
};

// ============================================================================
// Magic Number Detection
// ============================================================================

namespace audio_formats {

// Magic number detection
inline bool is_ogg(const std::vector<uint8_t>& buffer) {
    return buffer.size() >= 4 &&
           buffer[0] == 0x4F && buffer[1] == 0x67 &&
           buffer[2] == 0x67 && buffer[3] == 0x53;  // "OggS"
}

inline bool is_mp3(const std::vector<uint8_t>& buffer) {
    return buffer.size() >= 2 &&
           buffer[0] == 0xFF && (buffer[1] & 0xE0) == 0xE0;  // MPEG frame sync
}

inline bool is_wav(const std::vector<uint8_t>& buffer) {
    return buffer.size() >= 8 &&
           buffer[0] == 0x52 && buffer[1] == 0x49 &&  // "RIFF"
           buffer[2] == 0x46 && buffer[3] == 0x46 &&
           buffer[8] == 0x57 && buffer[9] == 0x41;    // "WAVE"
}

inline bool is_flac(const std::vector<uint8_t>& buffer) {
    return buffer.size() >= 4 &&
           buffer[0] == 0x66 && buffer[1] == 0x4C &&
           buffer[2] == 0x61 && buffer[3] == 0x43;    // "fLaC"
}

inline bool is_opus(const std::vector<uint8_t>& buffer) {
    // Opus in OGG container, check for "OpusHead" after OggS
    return is_ogg(buffer) && buffer.size() >= 36 &&
           buffer[28] == 0x4F && buffer[29] == 0x70 &&
           buffer[30] == 0x75 && buffer[31] == 0x73;  // "Opus"
}

inline std::string detect(const std::vector<uint8_t>& buffer) {
    if (is_opus(buffer)) return "opus";
    if (is_ogg(buffer)) return "ogg";
    if (is_mp3(buffer)) return "mp3";
    if (is_wav(buffer)) return "wav";
    if (is_flac(buffer)) return "flac";
    return "unknown";
}

}  // namespace audio_formats

// ============================================================================
// Constants
// ============================================================================

namespace discord_voice_constants {
    constexpr int64_t VOICE_MESSAGE_FLAG = 8192;  // 1 << 13
    constexpr int WAVEFORM_SAMPLES = 256;
    constexpr int TARGET_SAMPLE_RATE = 48000;  // Discord requires 48kHz
    constexpr size_t MAX_VOICE_SIZE = 8 * 1024 * 1024;  // 8MB
}

}  // namespace channels
}  // namespace hunterclaw
