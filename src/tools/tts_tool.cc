// tts_tool.cc
// Strictly aligned with OpenClaw: src/agents/tools/tts-tool.ts
//
// Tool: tts
// Description: Convert text to speech. Audio is delivered automatically from
//              the tool result — reply with SILENT_REPLY after a successful
//              call to avoid duplicate messages.
// Providers: openai | elevenlabs | edge (aligned with HunterClaw tts_tool.h)

#include "hunterclaw/tools/tts_tool.h"
#include "hunterclaw/infra/http_client.h"
#include "hunterclaw/core/types.h"
#include <nlohmann/json.hpp>
#include <string>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <random>
#include <cstdio>

namespace hunterclaw {
namespace tools {

using json = nlohmann::json;

// ============================================================================
// 🎯 对齐 OpenClaw: SILENT_REPLY_TOKEN
// ============================================================================
static const std::string SILENT_REPLY_TOKEN = "SILENT_REPLY";

// ============================================================================
// Helpers
// ============================================================================

static std::string trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

static std::string generate_temp_path(const std::string& ext) {
    static std::random_device rd;
    static std::mt19937 gen(rd());
    static std::uniform_int_distribution<uint32_t> dis;
    char buf[64];
    snprintf(buf, sizeof(buf), "/tmp/tts_%08x.%s", dis(gen), ext.c_str());
    return buf;
}

// ============================================================================
// TtsTool implementation
// ============================================================================

TtsTool::TtsTool(const ToolContext& context,
                 std::shared_ptr<infra::HttpClient> http_client)
    : context_(context), http_client_(std::move(http_client)) {}

std::string TtsTool::description() const {
    return "Convert text to speech. Audio is delivered automatically from the tool result — "
           "reply with " + SILENT_REPLY_TOKEN + " after a successful call to avoid duplicate messages.";
}

Tool::Schema TtsTool::parameters() const {
    Tool::Schema schema;
    schema.type = "object";

    auto make_str = [](const std::string& desc) {
        core::JsonValue p = core::JsonValue::object();
        p["type"] = "string";
        p["description"] = desc;
        return p;
    };

    // 🎯 对齐 OpenClaw: TtsToolSchema
    schema.properties["text"]    = make_str("Text to convert to speech.");
    schema.properties["channel"] = make_str("Optional channel id to pick output format (e.g. telegram).");

    // HunterClaw extensions (not in OpenClaw but needed for provider selection)
    {
        core::JsonValue p = core::JsonValue::object();
        p["type"] = "string";
        p["description"] = "TTS provider: openai, elevenlabs, edge (default: auto-detect)";
        core::JsonValue e = core::JsonValue::array();
        e.push_back("openai"); e.push_back("elevenlabs"); e.push_back("edge");
        p["enum"] = e;
        schema.properties["provider"] = p;
    }
    schema.properties["voice"]   = make_str("Voice name/id (provider-specific).");
    schema.properties["speed"]   = []{
        core::JsonValue p = core::JsonValue::object();
        p["type"] = "number";
        p["description"] = "Speech speed (0.25-4.0, default 1.0)";
        return p;
    }();

    schema.required = {"text"};
    return schema;
}

ToolResult TtsTool::execute(const core::JsonValue& params_val) {
    try {
        json params = params_val.to_json();

        // 🎯 对齐 OpenClaw: readStringParam(params, "text", { required: true })
        if (!params.contains("text") || !params["text"].is_string())
            return make_error("text is required");
        std::string text = trim(params["text"].get<std::string>());
        if (text.empty())
            return make_error("text must not be empty");

        std::string channel = params.contains("channel") && params["channel"].is_string()
            ? trim(params["channel"].get<std::string>()) : "";

        // Resolve provider
        std::string provider = params.contains("provider") && params["provider"].is_string()
            ? trim(params["provider"].get<std::string>()) : "";

        // Auto-detect provider from environment
        if (provider.empty()) {
            if (!get_api_key("openai").empty())       provider = "openai";
            else if (!get_api_key("elevenlabs").empty()) provider = "elevenlabs";
            else                                         provider = "edge";
        }

        ToolResult result;
        if (provider == "openai") {
            result = provider_openai(text, params_val);
        } else if (provider == "elevenlabs") {
            result = provider_elevenlabs(text, params_val);
        } else {
            result = provider_edge(text, params_val);
        }

        if (!result.success) return result;

        // 🎯 对齐 OpenClaw: voice_compatible → [[audio_as_voice]] tag for Telegram
        bool voice_compatible = (channel == "telegram");
        if (!result.content.empty() && result.content[0].type == ToolResultContent::Type::Text) {
            std::string audio_path = result.content[0].text;
            // Strip "MEDIA:" prefix if present
            if (audio_path.substr(0, 6) == "MEDIA:") audio_path = audio_path.substr(6);

            std::string output;
            if (voice_compatible) {
                output = "[[audio_as_voice]]\n";
            }
            output += "MEDIA:" + audio_path;

            result.content[0].text = output;
        }

        return result;

    } catch (const std::exception& e) {
        return make_error(std::string(e.what()));
    }
}

// ============================================================================
// Provider: OpenAI TTS
// ============================================================================

ToolResult TtsTool::provider_openai(const std::string& text, const core::JsonValue& options_val) {
    json options = options_val.to_json();
    std::string api_key = get_api_key("openai");
    if (api_key.empty())
        return make_error("OpenAI API key not found (set OPENAI_API_KEY)");

    // 🎯 对齐 OpenClaw: textToSpeech({ text, cfg, channel })
    std::string voice = "alloy";
    if (options.contains("voice") && options["voice"].is_string())
        voice = trim(options["voice"].get<std::string>());

    double speed = 1.0;
    if (options.contains("speed") && options["speed"].is_number())
        speed = options["speed"].get<double>();

    // Build request
    json body = {
        {"model",  "tts-1"},
        {"input",  text},
        {"voice",  voice},
        {"speed",  speed},
    };

    if (!http_client_) return make_error("HTTP client not available");

    infra::HttpRequestOptions req_opts;
    req_opts.headers["Authorization"] = "Bearer " + api_key;
    req_opts.headers["Content-Type"]  = "application/json";
    req_opts.body = body.dump();
    req_opts.timeout_ms = 30000;

    auto response_result = http_client_->post(
        "https://api.openai.com/v1/audio/speech",
        req_opts
    );

    if (!response_result.is_ok())
        return make_error("OpenAI TTS failed: " + response_result.error);
    auto& response = response_result.get();
    if (!response.ok)
        return make_error("OpenAI TTS failed: HTTP " + std::to_string(response.status_code));

    // Save audio to temp file
    std::string audio_path = save_audio_to_temp(response.body, "mp3");
    if (audio_path.empty())
        return make_error("Failed to save audio file");

    ToolResult result;
    result.success = true;
    ToolResultContent content;
    content.type = ToolResultContent::Type::Text;
    content.text = "MEDIA:" + audio_path;
    result.content.push_back(content);
    result.details = json{{"audioPath", audio_path}, {"provider", "openai"}};
    return result;
}

// ============================================================================
// Provider: ElevenLabs TTS
// ============================================================================

ToolResult TtsTool::provider_elevenlabs(const std::string& text, const core::JsonValue& options_val) {
    json options = options_val.to_json();
    std::string api_key = get_api_key("elevenlabs");
    if (api_key.empty())
        return make_error("ElevenLabs API key not found (set ELEVENLABS_API_KEY)");

    // Default voice: Rachel
    std::string voice_id = "21m00Tcm4TlvDq8ikWAM";
    if (options.contains("voice") && options["voice"].is_string()) {
        std::string v = trim(options["voice"].get<std::string>());
        if (!v.empty()) voice_id = v;
    }

    json body = {
        {"text",           text},
        {"model_id",       "eleven_monolingual_v1"},
        {"voice_settings", {{"stability", 0.5}, {"similarity_boost", 0.75}}},
    };

    if (!http_client_) return make_error("HTTP client not available");

    infra::HttpRequestOptions req_opts;
    req_opts.headers["xi-api-key"]   = api_key;
    req_opts.headers["Content-Type"] = "application/json";
    req_opts.headers["Accept"]       = "audio/mpeg";
    req_opts.body = body.dump();
    req_opts.timeout_ms = 30000;

    auto response_result = http_client_->post(
        "https://api.elevenlabs.io/v1/text-to-speech/" + voice_id,
        req_opts
    );

    if (!response_result.is_ok())
        return make_error("ElevenLabs TTS failed: " + response_result.error);
    auto& response = response_result.get();
    if (!response.ok)
        return make_error("ElevenLabs TTS failed: HTTP " + std::to_string(response.status_code));

    std::string audio_path = save_audio_to_temp(response.body, "mp3");
    if (audio_path.empty())
        return make_error("Failed to save audio file");

    ToolResult result;
    result.success = true;
    ToolResultContent content;
    content.type = ToolResultContent::Type::Text;
    content.text = "MEDIA:" + audio_path;
    result.content.push_back(content);
    result.details = json{{"audioPath", audio_path}, {"provider", "elevenlabs"}};
    return result;
}

// ============================================================================
// Provider: Edge TTS (free, no API key)
// ============================================================================

ToolResult TtsTool::provider_edge(const std::string& text, const core::JsonValue& options_val) {
    json options = options_val.to_json();

    // 🎯 对齐 OpenClaw: edge-tts provider (edge_tts_provider.cc)
    std::string voice = "en-US-AriaNeural";
    if (options.contains("voice") && options["voice"].is_string()) {
        std::string v = trim(options["voice"].get<std::string>());
        if (!v.empty()) voice = v;
    }

    std::string audio_path = generate_temp_path("mp3");

    // Call edge-tts CLI tool
    std::string cmd = "edge-tts --voice \"" + voice + "\" --text \"" +
                      text + "\" --write-media \"" + audio_path + "\" 2>/dev/null";
    int ret = std::system(cmd.c_str());
    if (ret != 0 || !std::filesystem::exists(audio_path)) {
        // Fallback: try espeak
        cmd = "espeak -w \"" + audio_path + "\" \"" + text + "\" 2>/dev/null";
        ret = std::system(cmd.c_str());
        if (ret != 0 || !std::filesystem::exists(audio_path)) {
            return make_error("Edge TTS failed: edge-tts and espeak not available");
        }
    }

    ToolResult result;
    result.success = true;
    ToolResultContent content;
    content.type = ToolResultContent::Type::Text;
    content.text = "MEDIA:" + audio_path;
    result.content.push_back(content);
    result.details = json{{"audioPath", audio_path}, {"provider", "edge"}};
    return result;
}

// ============================================================================
// Helpers
// ============================================================================

std::string TtsTool::get_api_key(const std::string& provider) {
    std::string env_var;
    if (provider == "openai")       env_var = "OPENAI_API_KEY";
    else if (provider == "elevenlabs") env_var = "ELEVENLABS_API_KEY";
    else return "";

    const char* val = std::getenv(env_var.c_str());
    if (!val) return "";
    std::string s = trim(std::string(val));
    return s;
}

std::string TtsTool::save_audio_to_temp(const std::string& audio_data, const std::string& format) {
    std::string path = generate_temp_path(format);
    std::ofstream f(path, std::ios::binary);
    if (!f.is_open()) return "";
    f.write(audio_data.data(), (std::streamsize)audio_data.size());
    f.close();
    return path;
}

ToolResult TtsTool::make_error(const std::string& msg) const {
    ToolResult result;
    result.success = false;
    result.error = msg;
    ToolResultContent content;
    content.type = ToolResultContent::Type::Text;
    content.text = msg;
    result.content.push_back(content);
    result.details = json{{"error", msg}};
    return result;
}

} // namespace tools
} // namespace hunterclaw
