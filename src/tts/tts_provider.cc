// Aligned with openclaw:
//   src/tts/text-to-speech.ts (textToSpeech function)
//   src/tts/providers/openai.ts
//   src/tts/providers/elevenlabs.ts
//   src/tts/providers/edge.ts

#include "hunterclaw/tts/tts_provider.h"
#include "hunterclaw/infra/http_client.h"
#include "hunterclaw/core/logging.h"
#include "hunterclaw/markdown/markdown_system.h"
#include <fstream>
#include <filesystem>
#include <sstream>
#include <ctime>

namespace hunterclaw {
namespace tts {

// ============================================================================
// TtsProviderRegistry Implementation
// ============================================================================

TtsProviderRegistry& TtsProviderRegistry::instance() {
    static TtsProviderRegistry registry;
    return registry;
}

void TtsProviderRegistry::register_provider(const std::string& name, TtsProvider::Ptr provider) {
    providers_[name] = provider;
}

TtsProvider::Ptr TtsProviderRegistry::get_provider(const std::string& name) {
    auto it = providers_.find(name);
    if (it != providers_.end()) return it->second;
    return nullptr;
}

std::vector<std::string> TtsProviderRegistry::list_providers() {
    std::vector<std::string> names;
    for (const auto& [name, _] : providers_) {
        names.push_back(name);
    }
    return names;
}

// ============================================================================
// OpenAI TTS Provider
// Aligned with openclaw: src/tts/providers/openai.ts
// ============================================================================

class OpenAiTtsProvider : public TtsProvider {
public:
    OpenAiTtsProvider(const std::string& api_key, const std::string& base_url)
        : api_key_(api_key), base_url_(base_url),
          logger_(core::get_logger("tts.openai")) {}
    
    core::Result<TtsResponse> text_to_speech(const TtsRequest& request) override {
        // Aligned with openclaw: stripMarkdown before TTS
        std::string clean_text = markdown::MarkdownSystem::instance().strip_markdown(request.text);
        
        if (clean_text.empty()) {
            return core::Result<TtsResponse>::Err("Empty text after markdown stripping");
        }
        
        // OpenAI TTS API endpoint
        const std::string endpoint = base_url_ + "/audio/speech";
        
        // Build request payload
        // Aligned with openclaw: { model, input, voice, speed }
        nlohmann::json payload;
        payload["model"] = request.model.value_or("tts-1");  // default: tts-1
        payload["input"] = clean_text;
        payload["voice"] = request.voice.value_or("alloy");  // default: alloy
        
        if (request.speed.has_value()) {
            payload["speed"] = *request.speed;  // 0.25-4.0
        }
        
        // Optional: format (default: mp3)
        payload["response_format"] = "mp3";
        
        logger_->debug("OpenAI TTS request: model={}, voice={}, text_length={}",
                       payload["model"].get<std::string>(),
                       payload["voice"].get<std::string>(),
                       clean_text.size());
        
        // Send HTTP POST request
        auto http_client = infra::HttpClient::create();
        infra::HttpRequestOptions options;
        options.headers["Authorization"] = "Bearer " + api_key_;
        options.headers["Content-Type"] = "application/json";
        options.body = payload.dump();
        options.timeout_ms = 30000;  // 30s timeout for audio generation
        
        auto response = http_client->post(endpoint, options);
        if (!response.is_ok()) {
            return core::Result<TtsResponse>::Err("OpenAI TTS API error: " + response.err());
        }
        
        const auto& resp = response.get();
        if (!resp.ok) {
            logger_->error("OpenAI TTS failed: status={}, body={}", resp.status_code, resp.body);
            return core::Result<TtsResponse>::Err("OpenAI TTS failed: " + std::to_string(resp.status_code));
        }
        
        // Save audio to temp file
        // Aligned with openclaw: save to /tmp/hunterclaw-tts-*.mp3
        const std::string temp_dir = "/tmp";
        std::filesystem::create_directories(temp_dir);
        
        const auto now = std::chrono::system_clock::now();
        const auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()
        ).count();
        
        const std::string audio_path = temp_dir + "/hunterclaw-tts-" + std::to_string(timestamp) + ".mp3";
        
        std::ofstream ofs(audio_path, std::ios::binary);
        if (!ofs.is_open()) {
            return core::Result<TtsResponse>::Err("Failed to create temp audio file");
        }
        ofs.write(resp.body.data(), resp.body.size());
        ofs.close();
        
        logger_->info("OpenAI TTS generated: {} ({} bytes)", audio_path, resp.body.size());
        
        TtsResponse tts_response;
        tts_response.audio_path = audio_path;
        tts_response.format = "mp3";
        tts_response.duration_ms = 0;  // OpenAI doesn't provide duration
        
        return core::Result<TtsResponse>::Ok(tts_response);
    }
    
    core::Result<core::JsonValue> list_voices() override {
        // OpenAI TTS voices (hardcoded - API doesn't provide list endpoint)
        nlohmann::json voices = nlohmann::json::array({
            {{"id", "alloy"}, {"name", "Alloy"}, {"gender", "neutral"}},
            {{"id", "echo"}, {"name", "Echo"}, {"gender", "male"}},
            {{"id", "fable"}, {"name", "Fable"}, {"gender", "neutral"}},
            {{"id", "onyx"}, {"name", "Onyx"}, {"gender", "male"}},
            {{"id", "nova"}, {"name", "Nova"}, {"gender", "female"}},
            {{"id", "shimmer"}, {"name", "Shimmer"}, {"gender", "female"}}
        });
        
        return core::Result<core::JsonValue>::Ok(voices);
    }
    
    std::string name() const override { return "openai"; }
    
private:
    std::string api_key_;
    std::string base_url_;
    std::shared_ptr<spdlog::logger> logger_;
};

// ============================================================================
// Factory Functions
// ============================================================================

TtsProvider::Ptr create_openai_tts_provider(
    const std::string& api_key,
    const std::optional<std::string>& base_url
) {
    const std::string url = base_url.value_or("https://api.openai.com/v1");
    return std::make_shared<OpenAiTtsProvider>(api_key, url);
}

TtsProvider::Ptr create_elevenlabs_tts_provider(const std::string& api_key) {
    // TODO: Implement ElevenLabs provider
    // Aligned with openclaw: src/tts/providers/elevenlabs.ts
    return nullptr;
}

TtsProvider::Ptr create_edge_tts_provider() {
    // TODO: Implement Edge TTS provider
    // Aligned with openclaw: src/tts/providers/edge.ts
    // Uses edge-tts Python package or direct HTTP API
    return nullptr;
}

}  // namespace tts
}  // namespace hunterclaw
