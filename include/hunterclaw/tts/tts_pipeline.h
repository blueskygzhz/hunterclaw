#pragma once
// Aligned with openclaw: src/tts/tts.ts maybeApplyTtsToPayload (lines 845-989)
//
// TTS pipeline integration for outbound delivery:
//   - Resolves TTS auto mode (off/tagged/inbound)
//   - Parses TTS directives from payload text
//   - Cleans text, strips markdown for speech synthesis
//   - Calls textToSpeech engine
//   - Returns payload with audio attachment if successful
//
// Integration points (aligned with openclaw):
//   - dispatch-from-config.ts: onToolResult, onBlockReply, final replies
//   - dispatch-acp-delivery.ts: ACP reply delivery
//   - dispatch-acp.ts: ACP synthetic final audio

#include "hunterclaw/queue/delivery_queue.h"
#include <string>
#include <functional>
#include <optional>

namespace hunterclaw {
namespace tts {

// Aligned with openclaw: TtsAutoMode
enum class TtsAutoMode {
    Off,       // TTS disabled
    Tagged,    // Only when [[tts:text]]..[[/tts:text]] tags present
    Inbound,   // Only when inbound message contained audio
    All        // Always apply TTS
};

inline TtsAutoMode parse_tts_auto_mode(const std::string& mode) {
    if (mode == "off") return TtsAutoMode::Off;
    if (mode == "tagged") return TtsAutoMode::Tagged;
    if (mode == "inbound") return TtsAutoMode::Inbound;
    if (mode == "all") return TtsAutoMode::All;
    return TtsAutoMode::Off;
}

// Aligned with openclaw: TtsConfig
struct TtsConfig {
    TtsAutoMode auto_mode = TtsAutoMode::Off;
    std::string mode = "final";  // "final" | "block" | "all"
    int max_text_length = 4096;
    int timeout_ms = 10000;
    std::string voice;
    std::string model;
    std::string provider;  // "openai" | "elevenlabs" | "edge"
};

// Aligned with openclaw: payload kind
enum class TtsPayloadKind {
    Tool,
    Block,
    Final
};

// Aligned with openclaw: maybeApplyTtsToPayload result
struct TtsApplyResult {
    queue::MessagePayload payload;
    bool tts_applied = false;
    std::string audio_path;
    std::string error;
};

// Aligned with openclaw: TextToSpeech function signature
using TextToSpeechFn = std::function<TtsApplyResult(
    const std::string& text,
    const TtsConfig& config,
    const std::string& channel
)>;

// Aligned with openclaw: maybeApplyTtsToPayload (tts.ts:845-989)
// Returns the (potentially modified) payload with audio attachment.
inline queue::MessagePayload maybe_apply_tts_to_payload(
    const queue::MessagePayload& payload,
    const TtsConfig& config,
    TtsPayloadKind kind = TtsPayloadKind::Final,
    bool inbound_audio = false,
    const std::string& channel = "",
    TextToSpeechFn text_to_speech = nullptr
) {
    // Step 1: Check auto mode
    if (config.auto_mode == TtsAutoMode::Off) return payload;

    const std::string& text = payload.text;
    if (text.empty()) return payload;

    // Step 2: Tagged mode check
    bool has_tts_directive = text.find("[[tts:text]]") != std::string::npos;
    if (config.auto_mode == TtsAutoMode::Tagged && !has_tts_directive) return payload;

    // Step 3: Inbound mode check
    if (config.auto_mode == TtsAutoMode::Inbound && !inbound_audio) return payload;

    // Step 4: Mode/kind check (e.g., mode="final" but kind="block" → skip)
    if (config.mode == "final" && kind != TtsPayloadKind::Final) return payload;

    // Step 5: Skip if text too short or has media
    std::string trimmed = text;
    auto s = trimmed.find_first_not_of(" \t\n\r");
    auto e = trimmed.find_last_not_of(" \t\n\r");
    if (s != std::string::npos) trimmed = trimmed.substr(s, e - s + 1);
    else trimmed = "";

    if (trimmed.size() < 10) return payload;
    if (!payload.media_url.empty() || !payload.media_urls.empty()) return payload;
    if (text.find("MEDIA:") != std::string::npos) return payload;

    // Step 6: Call TTS engine
    if (!text_to_speech) return payload;  // No TTS engine configured

    auto result = text_to_speech(trimmed, config, channel);
    if (result.tts_applied && !result.audio_path.empty()) {
        queue::MessagePayload modified = payload;
        modified.media_url = result.audio_path;
        return modified;
    }

    return payload;
}

} // namespace tts
} // namespace hunterclaw
