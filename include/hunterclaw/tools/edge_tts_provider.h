#pragma once

#include "tool.h"
#include "../core/json_value.h"
#include "../http/http_client.h"
#include <string>
#include <memory>
#include <vector>

namespace hunterclaw {

/**
 * Edge TTS Provider
 * 
 * FREE text-to-speech using Microsoft Edge's TTS API.
 * No API key required!
 * 
 * Features:
 * - 100% free, no rate limits
 * - 300+ voices in 130+ languages
 * - Neural voices (high quality)
 * - SSML support
 * - Multiple output formats
 * 
 * OpenClaw Reference:
 * - src/agents/tools/tts-tool.ts (edge provider)
 * - Uses edge-tts Python library internally
 */
class EdgeTTSProvider {
public:
    EdgeTTSProvider(std::shared_ptr<HttpClient> http_client);

    // Generate speech from text
    JsonValue synthesize(
        const std::string& text,
        const std::string& voice = "en-US-AriaNeural",
        const std::string& format = "mp3",
        double rate = 1.0,
        double pitch = 0.0,
        double volume = 1.0
    );

    // List available voices
    std::vector<std::string> list_voices(const std::string& language = "");

    // Get voice info
    JsonValue get_voice_info(const std::string& voice_name);

private:
    std::shared_ptr<HttpClient> http_client_;

    // Helper methods
    std::string build_ssml(
        const std::string& text,
        const std::string& voice,
        double rate,
        double pitch,
        double volume
    );
    std::string call_edge_tts_cli(const std::vector<std::string>& args);
    bool is_edge_tts_installed();
    std::string install_edge_tts();
};

// Popular Edge TTS voices
struct EdgeVoice {
    std::string name;
    std::string language;
    std::string gender;
    std::string description;
};

// Recommended voices
const std::vector<EdgeVoice> POPULAR_EDGE_VOICES = {
    // English
    {"en-US-AriaNeural", "en-US", "Female", "Natural, friendly American English"},
    {"en-US-GuyNeural", "en-US", "Male", "Natural American English male"},
    {"en-GB-SoniaNeural", "en-GB", "Female", "British English female"},
    {"en-GB-RyanNeural", "en-GB", "Male", "British English male"},
    {"en-AU-NatashaNeural", "en-AU", "Female", "Australian English"},
    
    // Chinese
    {"zh-CN-XiaoxiaoNeural", "zh-CN", "Female", "Mandarin Chinese female"},
    {"zh-CN-YunxiNeural", "zh-CN", "Male", "Mandarin Chinese male"},
    {"zh-TW-HsiaoChenNeural", "zh-TW", "Female", "Taiwan Chinese"},
    {"zh-HK-HiuMaanNeural", "zh-HK", "Female", "Hong Kong Cantonese"},
    
    // Japanese
    {"ja-JP-NanamiNeural", "ja-JP", "Female", "Japanese female"},
    {"ja-JP-KeitaNeural", "ja-JP", "Male", "Japanese male"},
    
    // Korean
    {"ko-KR-SunHiNeural", "ko-KR", "Female", "Korean female"},
    {"ko-KR-InJoonNeural", "ko-KR", "Male", "Korean male"},
    
    // Spanish
    {"es-ES-ElviraNeural", "es-ES", "Female", "Spanish female"},
    {"es-MX-DaliaNeural", "es-MX", "Female", "Mexican Spanish"},
    
    // French
    {"fr-FR-DeniseNeural", "fr-FR", "Female", "French female"},
    {"fr-FR-HenriNeural", "fr-FR", "Male", "French male"},
    
    // German
    {"de-DE-KatjaNeural", "de-DE", "Female", "German female"},
    {"de-DE-ConradNeural", "de-DE", "Male", "German male"},
};

} // namespace hunterclaw
