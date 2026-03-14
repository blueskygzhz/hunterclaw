#pragma once

#include "hunterclaw/tools/tool.h"
#include "hunterclaw/http/http_client.h"
#include <memory>

namespace hunterclaw {
namespace tools {

/**
 * Audio Transcription Tool - 音频转文本
 * 对齐 OpenClaw media understanding 的 Audio 转录能力
 * 
 * Providers:
 * - openai: OpenAI Whisper API (推荐)
 * - groq: Groq Whisper (快速)
 * - deepgram: Deepgram API (实时)
 * 
 * 输入: 音频文件路径或 base64
 * 输出: 转录文本 + 时间戳 (可选)
 */
class AudioTranscriptionTool : public Tool {
public:
    explicit AudioTranscriptionTool(
        const ToolContext& context,
        std::shared_ptr<http::HttpClient> http_client
    );
    
    ~AudioTranscriptionTool() override = default;
    
    std::string name() const override { return "audio_transcription"; }
    
    std::string description() const override;
    
    Schema parameters() const override;
    
    ToolResult execute(const core::JsonValue& params) override;
    
    bool requires_approval() const override { return false; }
    
private:
    // === Provider 实现 ===
    
    /// OpenAI Whisper
    ToolResult provider_openai(const std::string& audio_path, const core::JsonValue& options);
    
    /// Groq Whisper
    ToolResult provider_groq(const std::string& audio_path, const core::JsonValue& options);
    
    // === 辅助函数 ===
    
    /// 读取 API Key
    std::string get_api_key(const std::string& provider);
    
    /// 读取音频文件
    core::Result<std::string> read_audio_file(const std::string& path);
    
    /// 从 base64 解码音频
    core::Result<std::string> decode_audio_base64(const std::string& base64);
    
    /// 上传音频文件 (multipart/form-data)
    core::Result<http::HttpResponse> upload_audio(
        const std::string& url,
        const std::string& audio_data,
        const std::string& filename,
        const std::map<std::string, std::string>& headers,
        const std::map<std::string, std::string>& form_fields
    );
    
    ToolContext context_;
    std::shared_ptr<http::HttpClient> http_client_;
};

} // namespace tools
} // namespace hunterclaw
