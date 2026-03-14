#include "hunterclaw/tools/audio_transcription_tool.h"
#include "hunterclaw/core/logger.h"
#include "hunterclaw/core/base64.h"
#include <fstream>
#include <sstream>
#include <cstdlib>

namespace hunterclaw {
namespace tools {

using namespace hunterclaw::core;

AudioTranscriptionTool::AudioTranscriptionTool(
    const ToolContext& context,
    std::shared_ptr<http::HttpClient> http_client
) : context_(context), http_client_(http_client) {}

std::string AudioTranscriptionTool::description() const {
    return R"(Audio transcription tool using Whisper models.

Providers:
- openai: OpenAI Whisper API (whisper-1 model)
- groq: Groq Whisper (fast, free tier available)

Supported formats: MP3, MP4, MPEG, MPGA, M4A, WAV, WEBM

Example:
{
  "audio_path": "/path/to/audio.mp3",
  "provider": "openai",
  "language": "en"
})";
}

Tool::Schema AudioTranscriptionTool::parameters() const {
    Schema schema;
    schema.type = "object";
    schema.description = "Audio transcription parameters";
    
    // audio_path or audio_base64 (required, mutually exclusive)
    JsonValue path_prop = JsonValue::object();
    path_prop["type"] = "string";
    path_prop["description"] = "Path to audio file";
    schema.properties["audio_path"] = path_prop;
    
    JsonValue base64_prop = JsonValue::object();
    base64_prop["type"] = "string";
    base64_prop["description"] = "Base64-encoded audio data";
    schema.properties["audio_base64"] = base64_prop;
    
    // provider (optional)
    JsonValue provider_prop = JsonValue::object();
    provider_prop["type"] = "string";
    provider_prop["enum"] = JsonValue::array({"openai", "groq"});
    provider_prop["default"] = "openai";
    provider_prop["description"] = "Transcription provider";
    schema.properties["provider"] = provider_prop;
    
    // language (optional)
    JsonValue lang_prop = JsonValue::object();
    lang_prop["type"] = "string";
    lang_prop["description"] = "Language code (e.g., en, zh, es)";
    schema.properties["language"] = lang_prop;
    
    // response_format (optional)
    JsonValue format_prop = JsonValue::object();
    format_prop["type"] = "string";
    format_prop["enum"] = JsonValue::array({"text", "json", "verbose_json"});
    format_prop["default"] = "text";
    format_prop["description"] = "Response format";
    schema.properties["response_format"] = format_prop;
    
    // temperature (optional)
    JsonValue temp_prop = JsonValue::object();
    temp_prop["type"] = "number";
    temp_prop["minimum"] = 0.0;
    temp_prop["maximum"] = 1.0;
    temp_prop["default"] = 0.0;
    temp_prop["description"] = "Sampling temperature (0-1)";
    schema.properties["temperature"] = temp_prop;
    
    return schema;
}

ToolResult AudioTranscriptionTool::execute(const JsonValue& params) {
    ToolParams tp(params);
    
    // 1. 读取音频源
    std::string audio_path = tp.read_string("audio_path");
    std::string audio_base64 = tp.read_string("audio_base64");
    
    if (audio_path.empty() && audio_base64.empty()) {
        return tool_result::error("Missing required parameter: audio_path or audio_base64");
    }
    
    // 2. 读取 provider
    std::string provider = tp.read_string("provider", false, "openai");
    
    // 3. 路由到对应的 provider
    if (provider == "openai") {
        return provider_openai(audio_path.empty() ? audio_base64 : audio_path, params);
    } else if (provider == "groq") {
        return provider_groq(audio_path.empty() ? audio_base64 : audio_path, params);
    }
    
    return tool_result::error("Unknown provider: " + provider);
}

// ========== Provider 实现 ==========

ToolResult AudioTranscriptionTool::provider_openai(
    const std::string& audio_source,
    const JsonValue& options
) {
    Logger::info("[AudioTranscription] OpenAI Whisper start");
    
    // 1. 获取 API Key
    std::string api_key = get_api_key("openai");
    if (api_key.empty()) {
        return tool_result::error("Missing OPENAI_API_KEY environment variable");
    }
    
    // 2. 读取音频数据
    Result<std::string> audio_result;
    if (audio_source.find("/") != std::string::npos || audio_source.find("\\") != std::string::npos) {
        // 文件路径
        audio_result = read_audio_file(audio_source);
    } else {
        // Base64
        audio_result = decode_audio_base64(audio_source);
    }
    
    if (!audio_result) {
        return tool_result::error("Failed to read audio: " + audio_result.error());
    }
    
    std::string audio_data = audio_result.value();
    
    // 3. 读取参数
    ToolParams tp(options);
    std::string language = tp.read_string("language");
    std::string response_format = tp.read_string("response_format", false, "text");
    double temperature = tp.read_number("temperature", false, 0.0);
    
    // 4. 构建 multipart/form-data 请求
    std::map<std::string, std::string> headers;
    headers["Authorization"] = "Bearer " + api_key;
    
    std::map<std::string, std::string> form_fields;
    form_fields["model"] = "whisper-1";
    if (!language.empty()) {
        form_fields["language"] = language;
    }
    form_fields["response_format"] = response_format;
    form_fields["temperature"] = std::to_string(temperature);
    
    // 5. 上传音频
    auto response = upload_audio(
        "https://api.openai.com/v1/audio/transcriptions",
        audio_data,
        "audio.mp3",
        headers,
        form_fields
    );
    
    if (!response) {
        return tool_result::error("OpenAI Whisper API failed: " + response.error());
    }
    
    if (response.value().status_code != 200) {
        return tool_result::error("OpenAI Whisper API error: " + 
                                  std::to_string(response.value().status_code) + 
                                  " - " + response.value().body);
    }
    
    // 6. 解析结果
    std::string transcription;
    if (response_format == "text") {
        transcription = response.value().body;
    } else {
        // JSON 格式
        auto json_result = JsonValue::parse(response.value().body);
        if (json_result && json_result.value().has("text")) {
            transcription = json_result.value()["text"].as_string();
        } else {
            transcription = response.value().body;
        }
    }
    
    Logger::info("[AudioTranscription] OpenAI Whisper success: " + 
                 std::to_string(transcription.size()) + " chars");
    
    // 7. 返回结果
    ToolResultContent content;
    content.type = ToolResultContent::Type::Text;
    content.text = "🎤 Transcription:\n\n" + transcription;
    
    JsonValue details = JsonValue::object();
    details["provider"] = "openai";
    details["text"] = transcription;
    details["length"] = static_cast<int>(transcription.size());
    
    return ToolResult{
        .content = {content},
        .details = details,
        .success = true
    };
}

ToolResult AudioTranscriptionTool::provider_groq(
    const std::string& audio_source,
    const JsonValue& options
) {
    Logger::info("[AudioTranscription] Groq Whisper start");
    
    // 1. 获取 API Key
    std::string api_key = get_api_key("groq");
    if (api_key.empty()) {
        return tool_result::error("Missing GROQ_API_KEY environment variable");
    }
    
    // 2. 读取音频数据
    Result<std::string> audio_result;
    if (audio_source.find("/") != std::string::npos) {
        audio_result = read_audio_file(audio_source);
    } else {
        audio_result = decode_audio_base64(audio_source);
    }
    
    if (!audio_result) {
        return tool_result::error("Failed to read audio: " + audio_result.error());
    }
    
    std::string audio_data = audio_result.value();
    
    // 3. 读取参数
    ToolParams tp(options);
    std::string language = tp.read_string("language");
    std::string response_format = tp.read_string("response_format", false, "text");
    
    // 4. 构建请求
    std::map<std::string, std::string> headers;
    headers["Authorization"] = "Bearer " + api_key;
    
    std::map<std::string, std::string> form_fields;
    form_fields["model"] = "whisper-large-v3";
    if (!language.empty()) {
        form_fields["language"] = language;
    }
    form_fields["response_format"] = response_format;
    
    // 5. 上传音频
    auto response = upload_audio(
        "https://api.groq.com/openai/v1/audio/transcriptions",
        audio_data,
        "audio.mp3",
        headers,
        form_fields
    );
    
    if (!response) {
        return tool_result::error("Groq Whisper API failed: " + response.error());
    }
    
    if (response.value().status_code != 200) {
        return tool_result::error("Groq Whisper API error: " + 
                                  std::to_string(response.value().status_code));
    }
    
    // 6. 解析结果
    std::string transcription;
    if (response_format == "text") {
        transcription = response.value().body;
    } else {
        auto json_result = JsonValue::parse(response.value().body);
        if (json_result && json_result.value().has("text")) {
            transcription = json_result.value()["text"].as_string();
        }
    }
    
    Logger::info("[AudioTranscription] Groq Whisper success");
    
    // 7. 返回结果
    ToolResultContent content;
    content.type = ToolResultContent::Type::Text;
    content.text = "🎤 Transcription:\n\n" + transcription;
    
    JsonValue details = JsonValue::object();
    details["provider"] = "groq";
    details["text"] = transcription;
    
    return ToolResult{
        .content = {content},
        .details = details,
        .success = true
    };
}

// ========== 辅助函数 ==========

std::string AudioTranscriptionTool::get_api_key(const std::string& provider) {
    std::string env_var;
    
    if (provider == "openai") {
        env_var = "OPENAI_API_KEY";
    } else if (provider == "groq") {
        env_var = "GROQ_API_KEY";
    } else {
        return "";
    }
    
    const char* key = std::getenv(env_var.c_str());
    return key ? std::string(key) : "";
}

Result<std::string> AudioTranscriptionTool::read_audio_file(const std::string& path) {
    try {
        std::ifstream ifs(path, std::ios::binary);
        if (!ifs) {
            return Result<std::string>::error("Cannot open file: " + path);
        }
        
        std::ostringstream oss;
        oss << ifs.rdbuf();
        
        return Result<std::string>::ok(oss.str());
    } catch (const std::exception& e) {
        return Result<std::string>::error("Failed to read file: " + std::string(e.what()));
    }
}

Result<std::string> AudioTranscriptionTool::decode_audio_base64(const std::string& base64) {
    try {
        std::string decoded = base64::decode(base64);
        return Result<std::string>::ok(decoded);
    } catch (const std::exception& e) {
        return Result<std::string>::error("Failed to decode base64: " + std::string(e.what()));
    }
}

Result<http::HttpResponse> AudioTranscriptionTool::upload_audio(
    const std::string& url,
    const std::string& audio_data,
    const std::string& filename,
    const std::map<std::string, std::string>& headers,
    const std::map<std::string, std::string>& form_fields
) {
    if (!http_client_) {
        return Result<http::HttpResponse>::error("HTTP client not available");
    }
    
    // 构建 multipart/form-data
    std::string boundary = "----HunterClawBoundary" + std::to_string(std::time(nullptr));
    std::ostringstream body;
    
    // 添加表单字段
    for (const auto& [key, value] : form_fields) {
        body << "--" << boundary << "\r\n";
        body << "Content-Disposition: form-data; name=\"" << key << "\"\r\n\r\n";
        body << value << "\r\n";
    }
    
    // 添加文件
    body << "--" << boundary << "\r\n";
    body << "Content-Disposition: form-data; name=\"file\"; filename=\"" << filename << "\"\r\n";
    body << "Content-Type: audio/mpeg\r\n\r\n";
    body << audio_data << "\r\n";
    body << "--" << boundary << "--\r\n";
    
    // 发送请求
    http::HttpRequest request;
    request.method = "POST";
    request.url = url;
    request.headers = headers;
    request.headers["Content-Type"] = "multipart/form-data; boundary=" + boundary;
    request.body = body.str();
    
    return http_client_->request(request);
}

} // namespace tools
} // namespace hunterclaw
