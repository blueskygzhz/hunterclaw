#pragma once

#include "tool.h"
#include "../core/json_value.h"
#include "../http/http_client.h"
#include "../llm/llm_client.h"
#include <string>
#include <memory>

namespace hunterclaw {

/**
 * Video Understanding Tool
 * 
 * Analyzes videos using AI vision models.
 * Supports frame extraction, video description, action recognition, and Q&A.
 * 
 * Providers:
 * - OpenAI GPT-4o: Video analysis via frame sampling
 * - Anthropic Claude: Frame-by-frame analysis
 * - Google Gemini: Native video understanding (TODO)
 * 
 * Features:
 * - Video description
 * - Action/event detection
 * - Object tracking
 * - Video Q&A
 * - Frame extraction
 * - Temporal understanding
 * 
 * OpenClaw Reference:
 * - src/agents/tools/video-understanding-tool.ts
 */
class VideoUnderstandingTool : public Tool {
public:
    VideoUnderstandingTool(
        std::shared_ptr<HttpClient> http_client,
        std::shared_ptr<LLMClient> llm_client
    );

    std::string name() const override { return "video_understanding"; }
    std::string description() const override;
    JsonValue schema() const override;
    JsonValue execute(const JsonValue& params) override;

private:
    std::shared_ptr<HttpClient> http_client_;
    std::shared_ptr<LLMClient> llm_client_;

    // Provider implementations
    JsonValue analyze_with_openai(
        const std::string& video_path,
        const std::string& prompt,
        int frame_count
    );
    
    JsonValue analyze_with_claude(
        const std::string& video_path,
        const std::string& prompt,
        int frame_count
    );
    
    JsonValue analyze_with_gemini(
        const std::string& video_path,
        const std::string& prompt
    );

    // Helper methods
    std::vector<std::string> extract_frames(
        const std::string& video_path,
        int frame_count
    );
    
    std::string encode_frame_to_base64(const std::string& frame_path);
    
    JsonValue get_video_info(const std::string& video_path);
    
    bool is_ffmpeg_installed();
    
    std::string get_provider_description(const std::string& provider);
};

} // namespace hunterclaw
