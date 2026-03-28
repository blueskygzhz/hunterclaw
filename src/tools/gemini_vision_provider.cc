// gemini_vision_provider.cc
// Google Gemini Vision Provider - image and video understanding
// HunterClaw extension beyond OpenClaw (Gemini native video support)

#include "hunterclaw/tools/gemini_vision_provider.h"
#include "hunterclaw/core/json.h"
#include <nlohmann/json.hpp>
#include <string>
#include <fstream>
#include <filesystem>
#include <algorithm>
#include <map>

namespace hunterclaw {
namespace tools {

using json = nlohmann::json;
namespace fs = std::filesystem;

// 🎯 Gemini API base URL
static const std::string GEMINI_API_BASE =
    "https://generativelanguage.googleapis.com/v1beta/models";

GeminiVisionProvider::GeminiVisionProvider(
    std::shared_ptr<infra::HttpClient> http_client,
    const std::string& api_key
) : http_client_(std::move(http_client)),
    api_key_(api_key),
    base_url_(GEMINI_API_BASE) {}

core::JsonValue GeminiVisionProvider::analyze_image(
    const std::string& image_path,
    const std::string& prompt,
    const std::string& model
) {
    auto parts = build_content_parts(prompt, {image_path});
    json request_body = {
        {"contents", json::array({
            {{"parts", parts.to_json()}}
        })}
    };
    return call_gemini_api(model, core::JsonValue(request_body));
}

core::JsonValue GeminiVisionProvider::analyze_video(
    const std::string& video_path,
    const std::string& prompt,
    const std::string& model
) {
    // 🎯 Gemini native video support (no frame extraction needed!)
    auto parts = build_content_parts(prompt, {video_path});
    json request_body = {
        {"contents", json::array({
            {{"parts", parts.to_json()}}
        })}
    };
    return call_gemini_api(model, core::JsonValue(request_body));
}

core::JsonValue GeminiVisionProvider::analyze_images(
    const std::vector<std::string>& image_paths,
    const std::string& prompt,
    const std::string& model
) {
    auto parts = build_content_parts(prompt, image_paths);
    json request_body = {
        {"contents", json::array({
            {{"parts", parts.to_json()}}
        })}
    };
    return call_gemini_api(model, core::JsonValue(request_body));
}

std::vector<std::string> GeminiVisionProvider::list_models()    std::vector<std::string> models;
    for (const auto& m : GEMINI_MODELS) {
        models.push_back(m.name);
    }
    return models;
}

core::JsonValue GeminiVisionProvider::call_gemini_api(
    const std::string& model,
    const core::JsonValue& request_body
) {
    if (!http_client_) {
    return core::JsonValue(json{{"error", "No HTTP client available"}, {"stub", true}});
    }

    std::string url = base_url_ + "/" + model + ":generateContent?key=" + api_key_;
    auto response = http_client_->post_json(url, request_body);
    if (!response.ok) {
        return core::JsonValue(json{{"error", response.error}});
    }

    try {
        json body = json::parse(response.get().body);
        // Extract text from Gemini response
        if (body.contains("candidates") && body["candidates"].is_array() &&
            !body["candidates"].empty()) {
            const auto& candidate = body["candidates"][0];
            if (candidate.contains("content") && candidate["content"].contains("parts") &&
                candidate["content"]["parts"].is_array() &&
                !candidate["content"]["parts"].empty()) {
                const auto& part = candidate["content"]["parts"][0];
                if (part.contains("text") && part["text"].is_string()) {
        return core::JsonValue(json{
                {"text",  part["text"].get<std::string>()},
                {"model", model},
            });
                }
            }
        }
        return core::JsonValue(body);
    } catch (const std::exception& e) {
        return core::JsonValue(json{{"error", std::string(e.what())}});
    }
}

std::string GeminiVisionProvider::encode_file_to_base64(const std::string& file_path) {
    std::ifstream f(file_path, std::ios::binary);
    if (!f.is_open()) return "";
    std::string data((std::istreambuf_iterator<char>(f)),
                      std::istreambuf_iterator<char>());
    // Base64 encode
    static const char* b64 =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string encoded;
    for (size_t i = 0; i < data.size(); i += 3) {
        unsigned char a = (unsigned char)data[i];
        unsigned char b = (i+1 < data.size()) ? (unsigned char)data[i+1] : 0;
        unsigned char c = (i+2 < data.size()) ? (unsigned char)data[i+2] : 0;
        encoded += b64[a >> 2];
        encoded += b64[((a & 3) << 4) | (b >> 4)];
        encoded += (i+1 < data.size()) ? b64[((b & 15) << 2) | (c >> 6)] : '=';
        encoded += (i+2 < data.size()) ? b64[c & 63] : '=';
    }
    return encoded;
}

std::string GeminiVisionProvider::get_mime_type(const std::string& file_path) {
    std::string ext = fs::path(file_path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    static const std::map<std::string, std::string> mime_map = {
        {".jpg",  "image/jpeg"}, {".jpeg", "image/jpeg"},
        {".png",  "image/png"},  {".gif",  "image/gif"},
        {".webp", "image/webp"}, {".bmp",  "image/bmp"},
        {".mp4",  "video/mp4"},  {".mov",  "video/quicktime"},
        {".avi",  "video/x-msvideo"}, {".mkv", "video/x-matroska"},
        {".webm", "video/webm"}, {".mp3",  "audio/mpeg"},
        {".wav",  "audio/wav"},  {".m4a",  "audio/mp4"},
        {".pdf",  "application/pdf"},
    };
    auto it = mime_map.find(ext);
    return (it != mime_map.end()) ? it->second : "application/octet-stream";
}

core::JsonValue GeminiVisionProvider::build_content_parts(
    const std::string& prompt,
    const std::vector<std::string>& file_paths
) {
    json parts = json::array();

    // Add text prompt first
    if (!prompt.empty()) {
        parts.push_back({{"text", prompt}});
    }

    // Add file parts
    for (const auto& path : file_paths) {
        std::string b64 = encode_file_to_base64(path);
        if (b64.empty()) continue;
        std::string mime = get_mime_type(path);
        parts.push_back({
            {"inlineData", {
                {"mimeType", mime},
                {"data",     b64},
            }}
        });
    }

    return core::JsonValue(parts);
}

} // namespace tools
} // namespace hunterclaw
