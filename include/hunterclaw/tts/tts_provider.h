#pragma once

#include "hunterclaw/core/result.h"
#include "hunterclaw/core/json.h"
#include <string>
#include <optional>
#include <memory>

namespace hunterclaw {
namespace tts {

/**
 * @brief TTS 响应
 * 
 * Aligned with OpenClaw: textToSpeech() response
 */
struct TtsResponse {
    std::string audio_path;           // 音频文件路径 (临时文件 or URL)
    std::string format;               // 音频格式: mp3/ogg/opus/wav
    int duration_ms = 0;              // 音频时长 (毫秒)
    std::optional<std::string> url;   // 可选: 音频 URL (如果是云端生成)
};

/**
 * @brief TTS 请求参数
 * 
 * Aligned with OpenClaw: textToSpeech({ text, provider, voice, ... })
 */
struct TtsRequest {
    std::string text;                      // 待转换文本
    std::string provider;                  // "openai" | "elevenlabs" | "edge" | "azure"
    std::optional<std::string> voice;      // 语音名称 (provider-specific)
    std::optional<std::string> model;      // 模型名称 (如 "tts-1" for OpenAI)
    std::optional<std::string> channel;    // 目标频道 (用于选择 voice-bubble 模式)
    std::optional<float> speed;            // 语速 (0.25-4.0, default 1.0)
    std::optional<std::string> language;   // 语言代码 (如 "en-US", "zh-CN")
};

/**
 * @brief TTS Provider 接口
 * 
 * Aligned with OpenClaw: src/tts/providers/* 
 * 
 * **支持的 Provider**:
 * - **OpenAI**: tts-1, tts-1-hd (alloy/echo/fable/onyx/nova/shimmer)
 * - **ElevenLabs**: multilingual-v2 (高质量多语言)
 * - **Azure**: neural voices (200+ voices)
 * - **Edge TTS**: Microsoft Edge TTS (免费,内置)
 */
class TtsProvider {
public:
    using Ptr = std::shared_ptr<TtsProvider>;
    virtual ~TtsProvider() = default;
    
    /**
     * @brief 文本转语音
     * 
     * @param request TTS 请求参数
     * @return TtsResponse 或错误
     */
    virtual core::Result<TtsResponse> text_to_speech(const TtsRequest& request) = 0;
    
    /**
     * @brief 获取支持的语音列表
     * 
     * @return JSON array of voice objects
     */
    virtual core::Result<core::JsonValue> list_voices() = 0;
    
    /**
     * @brief Provider 名称
     */
    virtual std::string name() const = 0;
};

/**
 * @brief TTS Provider Registry
 * 
 * 单例注册表,管理所有 TTS provider 实例
 */
class TtsProviderRegistry {
public:
    static TtsProviderRegistry& instance();
    
    /**
     * @brief 注册 provider
     */
    void register_provider(const std::string& name, TtsProvider::Ptr provider);
    
    /**
     * @brief 获取 provider
     */
    TtsProvider::Ptr get_provider(const std::string& name);
    
    /**
     * @brief 列出所有 provider
     */
    std::vector<std::string> list_providers();
    
private:
    TtsProviderRegistry() = default;
    std::map<std::string, TtsProvider::Ptr> providers_;
};

/**
 * @brief 创建 OpenAI TTS provider
 * 
 * @param api_key OpenAI API key
 * @param base_url Optional base URL (default: https://api.openai.com/v1)
 * @return TtsProvider 实例
 */
TtsProvider::Ptr create_openai_tts_provider(
    const std::string& api_key,
    const std::optional<std::string>& base_url = std::nullopt
);

/**
 * @brief 创建 ElevenLabs TTS provider
 * 
 * @param api_key ElevenLabs API key
 * @return TtsProvider 实例
 */
TtsProvider::Ptr create_elevenlabs_tts_provider(const std::string& api_key);

/**
 * @brief 创建 Edge TTS provider (免费,无需 API key)
 * 
 * @return TtsProvider 实例
 */
TtsProvider::Ptr create_edge_tts_provider();

}  // namespace tts
}  // namespace hunterclaw
