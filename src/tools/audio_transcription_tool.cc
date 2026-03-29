// audio_transcription_tool.cc
// Aligned with OpenClaw: media understanding audio transcription
// Providers: openai (Whisper) | groq (Whisper)

#include "hunterclaw/tools/audio_transcription_tool.h"
#include "hunterclaw/core/json.h"
#include <nlohmann/json.hpp>
#include <string>
#include <fstream>
#include <sstream>
#include <cstdlib>
#include <map>
#include <chrono>

namespace hunterclaw {
namespace tools {

using json = nlohmann::json;

// 🎯 对齐 OpenClaw: OPENAI_WHISPER_URL
static const std::string OPENAI_WHISPER_URL =
    "https://api.openai.com/v1/audio/transcriptions";
// 🎯 对齐 OpenClaw: GROQ_WHISPER_URL
static const std::string GROQ_WHISPER_URL =
    "https://api.groq.com/openai/v1/audio/transcriptions";

AudioTranscriptionTool::AudioTranscriptionTool(
    const ToolContext& context,
    std::shared_ptr<infra::HttpClient> http_client
) : context_(context), http_client_(std::move(http_client)) {}

std::string AudioTranscriptionTool::description() const {
    return "Transcribe audio files to text using Whisper (OpenAI or Groq). "
           "Supports mp3, mp4, mpeg, mpga, m4a, wav, webm formats. "
           "Provider: openai (default) or groq.";
}

Tool::Schema AudioTranscriptionTool::parameters() const {
    Tool::Schema schema;
    schema.type = "object";

    auto make_str = [](const std::string& desc) {
        core::JsonValue p = core::JsonValue::object();
        p["type"] = "string";
        p["description"] = desc;
        return p;
    };

    schema.properties["path"]     = make_str("Path to audio file");
    schema.properties["base64"]   = make_str("Base64-encoded audio data");
    schema.properties["filename"] = make_str("Filename (required when using base64)");
    {
        core::JsonValue p = core::JsonValue::object();
        p["type"] = "string";
        p["description"] = "Provider: openai (default) or groq";
        core::JsonValue e = core::JsonValue::array();
        e.push_back("openai"); e.push_back("groq");
        p["enum"] = e;
        schema.properties["provider"] = p;
    }
    schema.properties["language"] = make_str("Language code (e.g. 'en', 'zh')");
    schema.properties["prompt"]   = make_str("Optional prompt to guide transcription");
    {
        core::JsonValue p = core::JsonValue::object();
        p["type"] = "string";
        p["description"] = "Response format: json (default), text, srt, vtt, verbose_json";
        core::JsonValue e = core::JsonValue::array();
        e.push_back("json"); e.push_back("text"); e.push_back("srt");
        e.push_back("vtt"); e.push_back("verbose_json");
        p["enum"] = e;
        schema.properties["responseFormat"] = p;
    }

    schema.required = {};
    return schema;
}

ToolResult AudioTranscriptionTool::execute(const core::JsonValue& params_val) {
    try {
        json params = params_val.to_json();

        // Resolve audio path
        std::string audio_path;
        if (params.contains("path") && params["path"].is_string()) {
            audio_path = params["path"].get<std::string>();
        } else if (params.contains("base64") && params["base64"].is_string()) {
            // Decode base64 to temp file
            std::string b64 = params["base64"].get<std::string>();
            auto result = decode_audio_base64(b64);
            if (!result.ok) return tool_result::error(result.error);
            audio_path = result.get();
        } else {
            return tool_result::error("path or base64 required");
        }

        // Resolve provider
        std::string provider = "openai";
        if (params.contains("provider") && params["provider"].is_string())
            provider = params["provider"].get<std::string>();

        core::JsonValue options = core::JsonValue(params);

        if (provider == "groq") {
            return provider_groq(audio_path, options);
        }
        return provider_openai(audio_path, options);

    } catch (const std::exception& e) {
        return tool_result::error(std::string(e.what()));
    }
}

ToolResult AudioTranscriptionTool::provider_openai(
    const std::string& audio_path,
    const core::JsonValue& options
) {
    std::string api_key = get_api_key("openai");
    if (api_key.empty()) return tool_result::error("OPENAI_API_KEY not set");

    json opts = options.to_json();
    std::string language = opts.value("language", "");
    std::string prompt   = opts.value("prompt", "");
    std::string format   = opts.value("responseFormat", "json");

    std::map<std::string, std::string> headers = {
        {"Authorization", "Bearer " + api_key},
    };
    std::map<std::string, std::string> form_fields = {
        {"model", "whisper-1"},
        {"response_format", format},
    };
    if (!language.empty()) form_fields["language"] = language;
    if (!prompt.empty())   form_fields["prompt"]   = prompt;

    auto resp = upload_audio(OPENAI_WHISPER_URL, audio_path, "audio.mp3", headers, form_fields);
    if (!resp.ok) return tool_result::error(resp.error);

    // Parse response
    try {
        json body = json::parse(resp.get().body);
        std::string text = body.value("text", "");
        ToolResult result;
        result.success = true;
        ToolResultContent content;
        content.type = ToolResultContent::Type::Text;
        content.text = text;
        result.content.push_back(content);
        return result;
    } catch (...) {
        return tool_result::error("Failed to parse Whisper response");
    }
}

ToolResult AudioTranscriptionTool::provider_groq(
    const std::string& audio_path,
    const core::JsonValue& options
) {
    std::string api_key = get_api_key("groq");
    if (api_key.empty()) return tool_result::error("GROQ_API_KEY not set");

    json opts = options.to_json();
    std::string language = opts.value("language", "");
    std::string prompt   = opts.value("prompt", "");
    std::string format   = opts.value("responseFormat", "json");

    std::map<std::string, std::string> headers = {
        {"Authorization", "Bearer " + api_key},
    };
    std::map<std::string, std::string> form_fields = {
        {"model", "whisper-large-v3"},
        {"response_format", format},
    };
    if (!language.empty()) form_fields["language"] = language;
    if (!prompt.empty())   form_fields["prompt"]   = prompt;

    auto resp = upload_audio(GROQ_WHISPER_URL, audio_path, "audio.mp3", headers, form_fields);
    if (!resp.ok) return tool_result::error(resp.error);

    try {
        json body = json::parse(resp.get().body);
        std::string text = body.value("text", "");
        ToolResult result;
        result.success = true;
        ToolResultContent content;
        content.type = ToolResultContent::Type::Text;
        content.text = text;
        result.content.push_back(content);
        return result;
    } catch (...) {
        return tool_result::error("Failed to parse Groq Whisper response");
    }
}

std::string AudioTranscriptionTool::get_api_key(const std::string& provider) {
    if (provider == "openai") {
        const char* env = std::getenv("OPENAI_API_KEY");
        return env ? std::string(env) : "";
    }
    if (provider == "groq") {
        const char* env = std::getenv("GROQ_API_KEY");
        return env ? std::string(env) : "";
    }
    return "";
}

core::Result<std::string> AudioTranscriptionTool::read_audio_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open())
        return core::Result<std::string>::Err("Cannot open file: " + path);
    std::string data((std::istreambuf_iterator<char>(f)),
                      std::istreambuf_iterator<char>());
    return core::Result<std::string>::Ok(data);
}

core::Result<std::string> AudioTranscriptionTool::decode_audio_base64(
    const std::string& base64
) {
    // Decode base64 to temp file
    static const std::string b64chars =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string decoded;
    int val = 0, valb = -8;
    for (unsigned char c : base64) {
        if (c == '=') break;
        auto pos = b64chars.find(c);
        if (pos == std::string::npos) continue;
        val = (val << 6) + (int)pos;
        valb += 6;
        if (valb >= 0) {
            decoded += (char)((val >> valb) & 0xFF);
            valb -= 8;
        }
    }
    // Write to temp file
    std::string tmp_path = "/tmp/audio_transcription_" +
        std::to_string(std::chrono::system_clock::now().time_since_epoch().count()) + ".mp3";
    std::ofstream f(tmp_path, std::ios::binary);
    if (!f.is_open())
        return core::Result<std::string>::Err("Cannot write temp file");
    f.write(decoded.data(), decoded.size());
    return core::Result<std::string>::Ok(tmp_path);
}

core::Result<infra::HttpResponse> AudioTranscriptionTool::upload_audio(
    const std::string& url,
    const std::string& audio_path,
    const std::string& filename,
    const std::map<std::string, std::string>& headers,
    const std::map<std::string, std::string>& form_fields
) {
    if (!http_client_)
        return core::Result<infra::HttpResponse>::Err("No HTTP client available");

    // Build multipart/form-data body manually
    const std::string boundary = "----HunterClawBoundary7MA4YWxkTrZu0gW";
    std::string body;

    // Add form fields
    for (const auto& kv : form_fields) {
        body += "--" + boundary + "\r\n";
        body += "Content-Disposition: form-data; name=\"" + kv.first + "\"\r\n\r\n";
        body += kv.second + "\r\n";
    }

    // Add audio file
    // Read audio file content
    std::ifstream f(audio_path, std::ios::binary);
    if (!f.is_open())
        return core::Result<infra::HttpResponse>::Err("Cannot open audio file: " + audio_path);
    std::string audio_data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());

    body += "--" + boundary + "\r\n";
    body += "Content-Disposition: form-data; name=\"file\"; filename=\"" + filename + "\"\r\n";
    body += "Content-Type: application/octet-stream\r\n\r\n";
    body += audio_data + "\r\n";
    body += "--" + boundary + "--\r\n";

    infra::HttpRequestOptions opts;
    opts.headers = headers;
    opts.headers["Content-Type"] = "multipart/form-data; boundary=" + boundary;
    opts.body = body;
    opts.timeout_ms = 60000;

    auto result = http_client_->post(url, opts);
    if (!result.is_ok())
        return core::Result<infra::HttpResponse>::Err(result.error);
    return core::Result<infra::HttpResponse>::Ok(result.get());
}

ToolResult AudioTranscriptionTool::make_error(const std::string& msg) const {
    return tool_result::error(msg);
}

} // namespace tools
} // namespace hunterclaw
