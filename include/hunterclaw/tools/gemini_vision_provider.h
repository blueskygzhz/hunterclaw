#pragma once

#include "hunterclaw/tools/tool.h"
#include "hunterclaw/core/json.h"
#include "hunterclaw/infra/http_client.h"
#include <string>
#include <memory>

namespace hunterclaw {
namespace tools {


/**
 * Google Gemini Vision Provider
 * 
 * Image and video understanding using Google Gemini models.
 * 
 * Models:
 * - gemini-2.0-flash-exp: Latest, fastest (free tier available)
 * - gemini-1.5-pro: High quality, longer context
 * - gemini-1.5-flash: Fast, cost-effective
 * 
 * Features:
 * - Image understanding
 * - Video understanding (native, no frame extraction!)
 * - Long context (up to 2M tokens)
 * - Multilingual
 * - JSON mode
 * 
 * OpenClaw Reference:
 * - Gemini is not in OpenClaw yet, but planned
 * - HunterClaw is AHEAD of OpenClaw here! 🚀
 */
class GeminiVisionProvider {
public:
    GeminiVisionProvider(
        std::shared_ptr<infra::HttpClient> http_client,
        const std::string& api_key
    );

    // Analyze image
    core::JsonValue analyze_image(
        const std::string& image_path,
        const std::string& prompt,
        const std::string& model = "gemini-2.0-flash-exp"
    );

    // Analyze video (native support!)
    core::JsonValue analyze_video(
        const std::string& video_path,
        const std::string& prompt,
        const std::string& model = "gemini-2.0-flash-exp"
    );

    // Multi-image analysis
    core::JsonValue analyze_images(
        const std::vector<std::string>& image_paths,
        const std::string& prompt,
        const std::string& model = "gemini-2.0-flash-exp"
    );

    // List available models
    std::vector<std::string> list_models();

private:
    std::shared_ptr<infra::HttpClient> http_client_;
    std::string api_key_;
    std::string base_url_;

    // Helper methods
    core::JsonValue call_gemini_api(
        const std::string& model,
        const core::JsonValue& request_body
    );
    
    std::string encode_file_to_base64(const std::string& file_path);
    
    std::string get_mime_type(const std::string& file_path);
    
    core::JsonValue build_content_parts(
        const std::string& prompt,
        const std::vector<std::string>& file_paths
    );
};

// Gemini model info
struct GeminiModel {
    std::string name;
    std::string description;
    bool supports_video;
    bool supports_json_mode;
    int context_length;
};

const std::vector<GeminiModel> GEMINI_MODELS = {
    {
        "gemini-2.0-flash-exp",
        "Latest experimental model, fastest, multimodal",
        true,   // supports video
        true,   // supports JSON mode
        1048576 // 1M tokens
    },
    {
        "gemini-1.5-pro",
        "High quality, longest context, best for complex tasks",
        true,
        true,
        2097152 // 2M tokens
    },
    {
        "gemini-1.5-flash",
        "Fast and cost-effective, good for simple tasks",
        true,
        true,
        1048576 // 1M tokens
    },
};

} // namespace tools
} // namespace hunterclaw
