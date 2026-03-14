#include "hunterclaw/tools/tts_tool.h"
#include "hunterclaw/core/logger.h"
#include "hunterclaw/core/base64.h"
#include <cstdlib>
#include <fstream>
#include <filesystem>

namespace hunterclaw {
namespace tools {

using namespace hunterclaw::core;
namespace fs = std::filesystem;

TtsTool::TtsTool(
    const ToolContext& context,
    std::shared_ptr<http::HttpClient> http_client
) : context_(context), http_client_(http_client) {}

std::string TtsTool::description() const {
    return R"(Text-to-Speech tool for converting text to audio.

Providers:
- openai: OpenAI TTS (tts-1, tts-1-hd models)
- elevenlabs: ElevenLabs TTS (high quality, multilingual)
- edge: Edge TTS (free, no API key required)

Output formats: MP3, OPUS, AAC, FLAC

Example:
{
  "text": "Hello world",
  "provider": "openai",
  "voice": "alloy",
  "model": "tts-1"
})";
}

Tool::Schema TtsTool::parameters() const {
    Schema schema;
    schema.type = "object";
    schema.description = "TTS tool parameters";
    
    // text (required)
    JsonValue text_prop = JsonValue::object();
    text_prop["type"] = "string";
    text_prop["description"] = "Text to convert to speech";
    schema.properties["text"] = text_prop;
    schema.required.push_back("text");
    
    // provider (optional)
    JsonValue provider_prop = JsonValue::object();
    provider_prop["type"] = "string";
    provider_prop["enum"] = JsonValue::array({"openai", "elevenlabs", "edge"});
    provider_prop["default"] = "openai";
    provider_prop["description"] = "TTS provider";
    schema.properties["provider"] = provider_prop;
    
    // voice (optional)
    JsonValue voice_prop = JsonValue::object();
    voice_prop["type"] = "string";
    voice_prop["description"] = "Voice ID (provider-specific)";
    schema.properties["voice"] = voice_prop;
    
    // model (optional)
    JsonValue model_prop = JsonValue::object();
    model_prop["type"] = "string";
    model_prop["description"] = "Model name (e.g., tts-1, tts-1-hd)";
    schema.properties["model"] = model_prop;
    
    // format (optional)
    JsonValue format_prop = JsonValue::object();
    format_prop["type"] = "string";
    format_prop["enum"] = JsonValue::array({"mp3", "opus", "aac", "flac"});
    format_prop["default"] = "mp3";
    format_prop["description"] = "Audio output format";
    schema.properties["format"] = format_prop;
    
    // speed (optional)
    JsonValue speed_prop = JsonValue::object();
    speed_prop["type"] = "number";
    speed_prop["minimum"] = 0.25;
    speed_prop["maximum"] = 4.0;
    speed_prop["default"] = 1.0;
    speed_prop["description"] = "Speech speed (0.25-4.0)";
    schema.properties["speed"] = speed_prop;
    
    return schema;
}

ToolResult TtsTool::execute(const JsonValue& params) {
    ToolParams tp(params);
    
    std::string text = tp.read_string("text", true);
    if (text.empty()) {
        return tool_result::error("Missing required parameter: text");
    }
    
    std::string provider = tp.read_string("provider", false, "openai");
    
    // 路由到对应的 provider
    if (provider == "openai") {
        return provider_openai(text, params);
    } else if (provider == "elevenlabs") {
        return provider_elevenlabs(text, params);
    } else if (provider == "edge") {
        return provider_edge(text, params);
    }
    
    return tool_result::error("Unknown provider: " + provider);
}

// ========== Provider 实现 ==========

ToolResult TtsTool::provider_openai(const std::string& text, const JsonValue& options) {
    Logger::info("[TtsTool] OpenAI TTS start");
    
    // 1. 获取 API Key
    std::string api_key = get_api_key("openai");
    if (api_key.empty()) {
        return tool_result::error("Missing OPENAI_API_KEY environment variable");
    }
    
    // 2. 读取参数
    ToolParams tp(options);
    std::string model = tp.read_string("model", false, "tts-1");
    std::string voice = tp.read_string("voice", false, "alloy");
    std::string format = tp.read_string("format", false, "mp3");
    double speed = tp.read_number("speed", false, 1.0);
    
    // 3. 构建请求
    JsonValue request_body = JsonValue::object();
    request_body["model"] = model;
    request_body["input"] = text;
    request_body["voice"] = voice;
    request_body["response_format"] = format;
    request_body["speed"] = speed;
    
    http::HttpRequest request;
    request.method = "POST";
    request.url = "https://api.openai.com/v1/audio/speech";
    request.headers["Authorization"] = "Bearer " + api_key;
    request.headers["Content-Type"] = "application/json";
    request.body = request_body.to_string();
    
    // 4. 发送请求
    if (!http_client_) {
        return tool_result::error("HTTP client not available");
    }
    
    auto response = http_client_->request(request);
    if (!response) {
        return tool_result::error("OpenAI TTS API failed: " + response.error());
    }
    
    if (response.value().status_code != 200) {
        return tool_result::error("OpenAI TTS API error: " + 
                                  std::to_string(response.value().status_code));
    }
    
    // 5. 获取音频数据 (binary)
    std::string audio_data = response.value().body;
    
    Logger::info("[TtsTool] OpenAI TTS success: " + 
                 std::to_string(audio_data.size()) + " bytes");
    
    // 6. Base64 编码
    std::string base64_audio = base64::encode(audio_data);
    
    // 7. 返回结果 - 作为文本 (包含 base64)
    JsonValue result = JsonValue::object();
    result["format"] = format;
    result["size"] = static_cast<int>(audio_data.size());
    result["audio_base64"] = base64_audio;
    
    std::string result_text = "🎵 TTS generated (" + format + ", " + 
                              std::to_string(audio_data.size()) + " bytes)\n\n" +
                              "Base64: " + base64_audio.substr(0, 100) + "...";
    
    ToolResultContent content;
    content.type = ToolResultContent::Type::Text;
    content.text = result_text;
    
    return ToolResult{
        .content = {content},
        .details = result,
        .success = true
    };
}

ToolResult TtsTool::provider_elevenlabs(const std::string& text, const JsonValue& options) {
    Logger::info("[TtsTool] ElevenLabs TTS start");
    
    // 1. 获取 API Key
    std::string api_key = get_api_key("elevenlabs");
    if (api_key.empty()) {
        return tool_result::error("Missing ELEVENLABS_API_KEY environment variable");
    }
    
    // 2. 读取参数
    ToolParams tp(options);
    std::string voice = tp.read_string("voice", false, "21m00Tcm4TlvDq8ikWAM");  // Rachel voice
    std::string model = tp.read_string("model", false, "eleven_monolingual_v1");
    
    // 3. 构建请求
    JsonValue request_body = JsonValue::object();
    request_body["text"] = text;
    request_body["model_id"] = model;
    request_body["voice_settings"] = JsonValue::object();
    request_body["voice_settings"]["stability"] = 0.5;
    request_body["voice_settings"]["similarity_boost"] = 0.5;
    
    http::HttpRequest request;
    request.method = "POST";
    request.url = "https://api.elevenlabs.io/v1/text-to-speech/" + voice;
    request.headers["xi-api-key"] = api_key;
    request.headers["Content-Type"] = "application/json";
    request.body = request_body.to_string();
    
    // 4. 发送请求
    if (!http_client_) {
        return tool_result::error("HTTP client not available");
    }
    
    auto response = http_client_->request(request);
    if (!response) {
        return tool_result::error("ElevenLabs TTS API failed: " + response.error());
    }
    
    if (response.value().status_code != 200) {
        return tool_result::error("ElevenLabs TTS API error: " + 
                                  std::to_string(response.value().status_code));
    }
    
    // 5. 获取音频数据
    std::string audio_data = response.value().body;
    std::string base64_audio = base64::encode(audio_data);
    
    Logger::info("[TtsTool] ElevenLabs TTS success: " + 
                 std::to_string(audio_data.size()) + " bytes");
    
    // 6. 返回结果
    JsonValue result = JsonValue::object();
    result["format"] = "mp3";
    result["size"] = static_cast<int>(audio_data.size());
    result["audio_base64"] = base64_audio;
    
    std::string result_text = "🎵 TTS generated (mp3, " + 
                              std::to_string(audio_data.size()) + " bytes)";
    
    ToolResultContent content;
    content.type = ToolResultContent::Type::Text;
    content.text = result_text;
    
    return ToolResult{
        .content = {content},
        .details = result,
        .success = true
    };
}

ToolResult TtsTool::provider_edge(const std::string& text, const JsonValue& options) {
    Logger::info("[TtsTool] Edge TTS start");
    
    // Edge TTS 需要调用外部命令行工具 (edge-tts)
    // 这里简化实现,返回提示信息
    
    return tool_result::error(
        "Edge TTS not yet implemented. "
        "Please use 'openai' or 'elevenlabs' provider."
    );
}

// ========== 辅助函数 ==========

std::string TtsTool::get_api_key(const std::string& provider) {
    std::string env_var;
    
    if (provider == "openai") {
        env_var = "OPENAI_API_KEY";
    } else if (provider == "elevenlabs") {
        env_var = "ELEVENLABS_API_KEY";
    } else {
        return "";
    }
    
    const char* key = std::getenv(env_var.c_str());
    return key ? std::string(key) : "";
}

std::string TtsTool::save_audio_to_temp(const std::string& audio_data, const std::string& format) {
    try {
        fs::path temp_dir = fs::temp_directory_path();
        fs::path audio_file = temp_dir / ("tts_" + std::to_string(std::time(nullptr)) + "." + format);
        
        std::ofstream ofs(audio_file, std::ios::binary);
        ofs.write(audio_data.data(), audio_data.size());
        ofs.close();
        
        return audio_file.string();
    } catch (const std::exception& e) {
        Logger::error("[TtsTool] Failed to save audio: " + std::string(e.what()));
        return "";
    }
}

} // namespace tools
} // namespace hunterclaw
