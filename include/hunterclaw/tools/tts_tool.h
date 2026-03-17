#pragma once

#include "hunterclaw/tools/tool.h"
#include "hunterclaw/infra/http_client.h"
#include <memory>

namespace hunterclaw {
namespace tools {

/**
 * TTS Tool - 文本转语音
 * 对齐 OpenClaw tts-tool 能力
 * 
 * Providers:
 * - openai: OpenAI TTS API (推荐)
 * - elevenlabs: ElevenLabs TTS API (高质量)
 * - edge: Edge TTS (免费,无需 API key)
 * 
 * Output: OPUS/MP3 音频 (base64 或文件路径)
 */
class TtsTool : public Tool {
public:
    explicit TtsTool(
        const ToolContext& context,
        std::shared_ptr<infra::HttpClient> http_client
    );
    
    ~TtsTool() override = default;
    
    std::string name() const override { return "tts"; }
    
    std::string description() const override;
    
    Schema parameters() const override;
    
    ToolResult execute(const core::JsonValue& params) override;
    
    bool requires_approval() const override { return false; }
    
private:
    // === Provider 实现 ===
    
    /// OpenAI TTS
    ToolResult provider_openai(const std::string& text, const core::JsonValue& options);
    
    /// ElevenLabs TTS
    ToolResult provider_elevenlabs(const std::string& text, const core::JsonValue& options);
    
    /// Edge TTS (免费)
    ToolResult provider_edge(const std::string& text, const core::JsonValue& options);
    
    // === 辅助函数 ===
    
    /// 读取 API Key (从环境变量或配置)
    std::string get_api_key(const std::string& provider);
    
    /// 保存音频到临时文件
    std::string save_audio_to_temp(const std::string& audio_data, const std::string& format);
    
    ToolContext context_;
    std::shared_ptr<infra::HttpClient> http_client_;
};

} // namespace tools
} // namespace hunterclaw
