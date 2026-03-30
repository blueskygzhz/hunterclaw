// 🎯 严格对齐 OpenClaw image-tool.ts
// Reference: openclaw-analysis/src/agents/tools/image-tool.ts
//            openclaw-analysis/src/agents/tools/image-tool.helpers.ts
//            openclaw-analysis/src/agents/tools/media-tool-shared.ts
//            openclaw-analysis/src/agents/model-fallback.ts
#include "hunterclaw/tools/image_analysis_tool.h"
#include "hunterclaw/core/logger.h"
#include "hunterclaw/core/base64.h"
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <fstream>
#include <sstream>
#include <set>
#include <regex>
#include <algorithm>
#include <cstdlib>
#include <chrono>

namespace hunterclaw {
namespace tools {

using namespace hunterclaw::core;

// ============================================================================
// 常量 (对齐 OpenClaw image-tool.ts)
// ============================================================================

// 🎯 对齐 OpenClaw: const DEFAULT_PROMPT = "Describe the image."
static constexpr const char* DEFAULT_PROMPT = "Describe the image.";

// 🎯 对齐 OpenClaw: const ANTHROPIC_IMAGE_PRIMARY = "anthropic/claude-opus-4-6"
static constexpr const char* ANTHROPIC_IMAGE_PRIMARY = "anthropic/claude-opus-4-6";

// 🎯 对齐 OpenClaw: const ANTHROPIC_IMAGE_FALLBACK = "anthropic/claude-opus-4-5"
static constexpr const char* ANTHROPIC_IMAGE_FALLBACK = "anthropic/claude-opus-4-5";

// 🎯 对齐 OpenClaw: const DEFAULT_MAX_IMAGES = 20
static constexpr int DEFAULT_MAX_IMAGES = 20;

// ============================================================================
// ImageModelConfig (对齐 OpenClaw image-tool.helpers.ts)
// ============================================================================

// 🎯 对齐 OpenClaw: export type ImageModelConfig = { primary?: string; fallbacks?: string[] }
struct ImageModelConfig {
    std::string primary;                  // optional
    std::vector<std::string> fallbacks;   // optional
};

// ============================================================================
// 辅助函数
// ============================================================================

// 🎯 对齐 OpenClaw: resolveImageToolMaxTokens(modelMaxTokens, requestedMaxTokens = 4096)
// function resolveImageToolMaxTokens(modelMaxTokens: number | undefined, requestedMaxTokens = 4096)
//   if (!Number.isFinite(modelMaxTokens) || modelMaxTokens <= 0) return requestedMaxTokens
//   return Math.min(requestedMaxTokens, modelMaxTokens)
static int resolve_image_tool_max_tokens(int model_max_tokens = 0, int requested_max_tokens = 4096) {
    if (model_max_tokens <= 0) {
        return requested_max_tokens;
    }
    return std::min(requested_max_tokens, model_max_tokens);
}

// 🎯 对齐 OpenClaw: resolvePromptAndModelOverride(args, defaultPrompt)
// Returns { prompt, modelOverride? }
struct PromptAndModelOverride {
    std::string prompt;
    std::string model_override;  // empty = undefined
};

static PromptAndModelOverride resolve_prompt_and_model_override(
    const JsonValue& args,
    const std::string& default_prompt
) {
    PromptAndModelOverride result;
    // 🎯 对齐 OpenClaw: prompt = typeof args.prompt === "string" && args.prompt.trim() ? args.prompt.trim() : defaultPrompt
    if (args.has("prompt") && args["prompt"].is_string()) {
        std::string p = args["prompt"].as_string();
        auto s = p.find_first_not_of(" \t\r\n");
        if (s != std::string::npos) {
            auto e = p.find_last_not_of(" \t\r\n");
            p = p.substr(s, e - s + 1);
        } else {
            p = "";
        }
        result.prompt = p.empty() ? default_prompt : p;
    } else {
        result.prompt = default_prompt;
    }
    // 🎯 对齐 OpenClaw: modelOverride = typeof args.model === "string" && args.model.trim() ? args.model.trim() : undefined
    if (args.has("model") && args["model"].is_string()) {
        std::string m = args["model"].as_string();
        auto s = m.find_first_not_of(" \t");
        if (s != std::string::npos) {
            auto e = m.find_last_not_of(" \t");
            m = m.substr(s, e - s + 1);
        } else {
            m = "";
        }
        result.model_override = m;  // empty = undefined
    }
    return result;
}

// 🎯 对齐 OpenClaw: pickMaxBytes(cfg?, maxBytesMb?)
// function pickMaxBytes(cfg?, maxBytesMb?): number | undefined
//   if (typeof maxBytesMb === "number" && isFinite && > 0) return Math.floor(maxBytesMb * 1024 * 1024)
//   const configured = cfg?.agents?.defaults?.mediaMaxMb
//   if (typeof configured === "number" && isFinite && > 0) return Math.floor(configured * 1024 * 1024)
//   return undefined
static size_t pick_max_bytes(double max_bytes_mb, double cfg_media_max_mb = 0.0) {
    if (max_bytes_mb > 0 && std::isfinite(max_bytes_mb)) {
        return static_cast<size_t>(std::floor(max_bytes_mb * 1024.0 * 1024.0));
    }
    if (cfg_media_max_mb > 0 && std::isfinite(cfg_media_max_mb)) {
        return static_cast<size_t>(std::floor(cfg_media_max_mb * 1024.0 * 1024.0));
    }
    return 0;  // 0 = undefined (no limit)
}

// ============================================================================
// MIME type 检测
// ============================================================================

static std::string detect_mime_type_from_bytes(const std::string& data) {
    if (data.size() < 4) return "image/jpeg";
    if ((unsigned char)data[0] == 0x89 && data[1] == 'P' && data[2] == 'N' && data[3] == 'G')
        return "image/png";
    if ((unsigned char)data[0] == 0xFF && (unsigned char)data[1] == 0xD8 && (unsigned char)data[2] == 0xFF)
        return "image/jpeg";
    if (data[0] == 'G' && data[1] == 'I' && data[2] == 'F' && data[3] == '8')
        return "image/gif";
    if (data.size() >= 12 &&
        data[0] == 'R' && data[1] == 'I' && data[2] == 'F' && data[3] == 'F' &&
        data[8] == 'W' && data[9] == 'E' && data[10] == 'B' && data[11] == 'P')
        return "image/webp";
    return "image/jpeg";
}

static std::string detect_mime_type_from_ext(const std::string& path) {
    auto dot = path.rfind('.');
    if (dot == std::string::npos) return "image/jpeg";
    std::string ext = path.substr(dot + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    if (ext == "png")  return "image/png";
    if (ext == "gif")  return "image/gif";
    if (ext == "webp") return "image/webp";
    if (ext == "bmp")  return "image/bmp";
    if (ext == "tiff" || ext == "tif") return "image/tiff";
    return "image/jpeg";
}

// ============================================================================
// decodeDataUrl (对齐 OpenClaw image-tool.helpers.ts)
// ============================================================================

// 🎯 对齐 OpenClaw: export function decodeDataUrl(dataUrl: string): { buffer, mimeType, kind: "image" }
struct DecodedMedia {
    std::string base64;
    std::string mime_type;
    std::string kind;  // "image"
};

static Result<DecodedMedia> decode_data_url(const std::string& data_url) {
    // 🎯 对齐 OpenClaw: /^data:([^;,]+);base64,([a-z0-9+/=\r\n]+)$/i
    const std::string trimmed = [&]() {
        std::string t = data_url;
        auto s = t.find_first_not_of(" \t\r\n");
        if (s == std::string::npos) return std::string{};
        auto e = t.find_last_not_of(" \t\r\n");
        return t.substr(s, e - s + 1);
    }();

    static const std::regex data_url_re(
        R"(^data:([^;,]+);base64,([a-zA-Z0-9+/=\r\n]+)$)",
        std::regex::icase
    );
    std::smatch m;
    if (!std::regex_match(trimmed, m, data_url_re)) {
        return Result<DecodedMedia>::Err("Invalid data URL (expected base64 data: URL).");
    }
    std::string mime_type = m[1].str();
    // 🎯 对齐 OpenClaw: mimeType = (match[1] ?? "").trim().toLowerCase()
    std::transform(mime_type.begin(), mime_type.end(), mime_type.begin(), ::tolower);
    auto s = mime_type.find_first_not_of(" \t");
    if (s != std::string::npos) mime_type = mime_type.substr(s);
    auto e = mime_type.find_last_not_of(" \t");
    if (e != std::string::npos) mime_type = mime_type.substr(0, e + 1);

    // 🎯 对齐 OpenClaw: if (!mimeType.startsWith("image/")) throw new Error(...)
    if (mime_type.find("image/") != 0) {
        return Result<DecodedMedia>::Err(
            "Unsupported data URL type: " + (mime_type.empty() ? "unknown" : mime_type)
        );
    }
    // 🎯 对齐 OpenClaw: const b64 = (match[2] ?? "").trim()
    std::string b64 = m[2].str();
    {
        auto bs = b64.find_first_not_of(" \t\r\n");
        if (bs != std::string::npos) {
            auto be = b64.find_last_not_of(" \t\r\n");
            b64 = b64.substr(bs, be - bs + 1);
        } else {
            b64 = "";
        }
    }
    // 🎯 对齐 OpenClaw: if (buffer.length === 0) throw new Error("Invalid data URL: empty payload.")
    if (b64.empty()) {
        return Result<DecodedMedia>::Err("Invalid data URL: empty payload.");
    }
    return Result<DecodedMedia>::Ok({b64, mime_type, "image"});
}

// ============================================================================
// loadWebMedia (对齐 OpenClaw web/media.ts)
// ============================================================================

// 🎯 对齐 OpenClaw: loadWebMedia(path, { maxBytes, localRoots }) → { kind, buffer, mimeType/contentType }
// Returns DecodedMedia with kind="image"

static size_t curl_write_cb(char* ptr, size_t size, size_t nmemb, void* ud) {
    auto* s = static_cast<std::string*>(ud);
    s->append(ptr, size * nmemb);
    return size * nmemb;
}

static Result<DecodedMedia> load_web_media(
    const std::string& path,
    size_t max_bytes = 0
) {
    // Determine if it's an HTTP URL or a file path
    static const std::regex http_re(R"(^https?://)", std::regex::icase);
    bool is_http = std::regex_search(path, http_re);

    std::string raw_data;
    std::string mime_type;

    if (is_http) {
        // Download via curl
        CURL* curl = curl_easy_init();
        if (!curl) return Result<DecodedMedia>::Err("Failed to init curl");

        curl_easy_setopt(curl, CURLOPT_URL, path.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &raw_data);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);

        CURLcode res = curl_easy_perform(curl);
        long status = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);

        // Get Content-Type header
        char* content_type_raw = nullptr;
        curl_easy_getinfo(curl, CURLINFO_CONTENT_TYPE, &content_type_raw);
        if (content_type_raw) {
            mime_type = content_type_raw;
            // Strip parameters (e.g. "image/jpeg; charset=utf-8")
            auto semi = mime_type.find(';');
            if (semi != std::string::npos) mime_type = mime_type.substr(0, semi);
            auto ms = mime_type.find_first_not_of(" \t");
            if (ms != std::string::npos) mime_type = mime_type.substr(ms);
            auto me = mime_type.find_last_not_of(" \t");
            if (me != std::string::npos) mime_type = mime_type.substr(0, me + 1);
            std::transform(mime_type.begin(), mime_type.end(), mime_type.begin(), ::tolower);
        }
        curl_easy_cleanup(curl);

        if (res != CURLE_OK) {
            return Result<DecodedMedia>::Err(
                std::string("Failed to download image: ") + curl_easy_strerror(res)
            );
        }
        if (status >= 400) {
            return Result<DecodedMedia>::Err(
                "HTTP " + std::to_string(status) + " downloading image: " + path
            );
        }
        if (raw_data.empty()) {
            return Result<DecodedMedia>::Err("Downloaded image is empty: " + path);
        }
    } else {
        // File path
        std::ifstream ifs(path, std::ios::binary);
        if (!ifs) {
            return Result<DecodedMedia>::Err("Cannot open file: " + path);
        }
        std::ostringstream oss;
        oss << ifs.rdbuf();
        raw_data = oss.str();
        if (raw_data.empty()) {
            return Result<DecodedMedia>::Err("File is empty: " + path);
        }
    }

    // maxBytes check
    if (max_bytes > 0 && raw_data.size() > max_bytes) {
        return Result<DecodedMedia>::Err(
            "Image too large: " + std::to_string(raw_data.size()) +
            " bytes (max " + std::to_string(max_bytes) + " bytes)"
        );
    }

    // Detect MIME type from bytes (fallback to extension)
    if (mime_type.empty() || mime_type.find("image/") != 0) {
        mime_type = detect_mime_type_from_bytes(raw_data);
        if (mime_type == "image/jpeg") {
            std::string ext_mime = detect_mime_type_from_ext(path);
            if (ext_mime != "image/jpeg") mime_type = ext_mime;
        }
    }

    std::string b64 = base64::encode(raw_data);
    return Result<DecodedMedia>::Ok({b64, mime_type, "image"});
}

// ============================================================================
// 图像加载结构体 (对齐 OpenClaw loadedImages array)
// ============================================================================

struct LoadedImage {
    std::string base64;
    std::string mime_type;
    std::string resolved_image;    // 🎯 对齐 OpenClaw: resolvedImage (original input, ~ expanded)
    std::string rewritten_from;    // 🎯 对齐 OpenClaw: rewrittenFrom (optional)
};

// ============================================================================
// buildImageContext (对齐 OpenClaw image-tool.ts)
// ============================================================================

// 🎯 对齐 OpenClaw: function buildImageContext(prompt, images): Context
// content = [{type:"text", text:prompt}, {type:"image", data, mimeType}...]
// Note: pi-ai Context format, not OpenAI format
static nlohmann::json build_image_context(
    const std::string& prompt,
    const std::vector<LoadedImage>& images
) {
    nlohmann::json content = nlohmann::json::array();
    content.push_back({{"type", "text"}, {"text", prompt}});
    for (const auto& img : images) {
        content.push_back({
            {"type", "image"},
            {"data", img.base64},
            {"mimeType", img.mime_type}
        });
    }
    // 🎯 对齐 OpenClaw: timestamp: Date.now()
    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
    return nlohmann::json{
        {"messages", nlohmann::json::array({
            {
                {"role", "user"},
                {"content", content},
                {"timestamp", now_ms}
            }
        })}
    };
}

// ============================================================================
// coerceImageAssistantText (对齐 OpenClaw image-tool.helpers.ts)
// ============================================================================

// 🎯 对齐 OpenClaw: export function coerceImageAssistantText({ message, provider, model }): string
// Checks stopReason == "error" | "aborted", errorMessage, then extractAssistantText
static Result<std::string> coerce_image_assistant_text(
    const nlohmann::json& response_json,
    const std::string& provider,
    const std::string& model
) {
    // 🎯 对齐 OpenClaw: const stop = params.message.stopReason
    std::string stop_reason = response_json.value("stop_reason", "");
    // Normalize: OpenAI uses "stop", Anthropic uses "end_turn"/"error"
    // 🎯 对齐 OpenClaw: params.message.errorMessage?.trim() — camelCase field
    std::string error_message = response_json.value("errorMessage", "");
    if (error_message.empty()) {
        // Also check snake_case for compatibility
        error_message = response_json.value("error_message", "");
    }
    // Trim errorMessage
    {
        auto s = error_message.find_first_not_of(" \t\r\n");
        if (s == std::string::npos) error_message = "";
        else {
            auto e = error_message.find_last_not_of(" \t\r\n");
            error_message = error_message.substr(s, e - s + 1);
        }
    }

    // 🎯 对齐 OpenClaw: if (stop === "error" || stop === "aborted") throw new Error(...)
    if (stop_reason == "error" || stop_reason == "aborted") {
        std::string msg = "Image model failed (" + provider + "/" + model + ")";
        if (!error_message.empty()) msg += ": " + error_message;
        return Result<std::string>::Err(msg);
    }
    // 🎯 对齐 OpenClaw: if (errorMessage) throw new Error(...)
    if (!error_message.empty()) {
        return Result<std::string>::Err(
            "Image model failed (" + provider + "/" + model + "): " + error_message
        );
    }

    // 🎯 对齐 OpenClaw: const text = extractAssistantText(params.message)
    // Extract text from response based on provider format
    std::string text;

    // OpenAI format: choices[0].message.content
    if (response_json.contains("choices") && response_json["choices"].is_array() &&
        !response_json["choices"].empty()) {
        const auto& choice = response_json["choices"][0];
        if (choice.contains("message") && choice["message"].contains("content")) {
            text = choice["message"]["content"].get<std::string>();
        }
    }
    // Anthropic format: content[].text
    else if (response_json.contains("content") && response_json["content"].is_array()) {
        for (const auto& block : response_json["content"]) {
            if (block.value("type", "") == "text") {
                text += block.value("text", "");
            }
        }
    }
    // Gemini format: candidates[0].content.parts[].text
    else if (response_json.contains("candidates") && response_json["candidates"].is_array() &&
             !response_json["candidates"].empty()) {
        const auto& cand = response_json["candidates"][0];
        if (cand.contains("content") && cand["content"].contains("parts")) {
            for (const auto& part : cand["content"]["parts"]) {
                if (part.contains("text")) {
                    text += part["text"].get<std::string>();
                }
            }
        }
    }

    // 🎯 对齐 OpenClaw: if (text.trim()) return text.trim()
    auto s = text.find_first_not_of(" \t\r\n");
    if (s != std::string::npos) {
        auto e = text.find_last_not_of(" \t\r\n");
        text = text.substr(s, e - s + 1);
    } else {
        text = "";
    }
    if (!text.empty()) {
        return Result<std::string>::Ok(text);
    }

    // 🎯 对齐 OpenClaw: throw new Error(`Image model returned no text (${provider}/${model}).`)
    return Result<std::string>::Err(
        "Image model returned no text (" + provider + "/" + model + ")."
    );
}

// ============================================================================
// HTTP POST helper
// ============================================================================

struct HttpResponse {
    std::string body;
    long status_code = 0;
};

static Result<HttpResponse> http_post_json(
    const std::string& url,
    const nlohmann::json& body,
    const std::map<std::string, std::string>& headers,
    int timeout_sec = 120
) {
    CURL* curl = curl_easy_init();
    if (!curl) return Result<HttpResponse>::Err("Failed to init curl");

    std::string body_str = body.dump();
    HttpResponse resp;

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body_str.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)body_str.size());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp.body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, (long)timeout_sec);

    struct curl_slist* curl_headers = nullptr;
    for (const auto& [k, v] : headers) {
        curl_headers = curl_slist_append(curl_headers, (k + ": " + v).c_str());
    }
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, curl_headers);

    CURLcode res = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &resp.status_code);
    curl_slist_free_all(curl_headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        return Result<HttpResponse>::Err(
            std::string("curl error: ") + curl_easy_strerror(res)
        );
    }
    if (resp.status_code >= 400) {
        return Result<HttpResponse>::Err(
            "HTTP " + std::to_string(resp.status_code) + ": " + resp.body
        );
    }
    return Result<HttpResponse>::Ok(resp);
}

// ============================================================================
// Provider API calls (complete() equivalent)
// ============================================================================

// 🎯 对齐 OpenClaw: complete(model, context, { apiKey, maxTokens })
// Calls the appropriate provider API with the image context
static Result<nlohmann::json> call_vision_provider(
    const std::string& provider,
    const std::string& model,
    const std::string& api_key,
    const std::string& prompt,
    const std::vector<LoadedImage>& images,
    int max_tokens
) {
    if (provider == "openai") {
        // OpenAI Vision: content = [{type:"text",...}, {type:"image_url", image_url:{url:"data:..."}}]
        nlohmann::json content = nlohmann::json::array();
        content.push_back({{"type", "text"}, {"text", prompt}});
        for (const auto& img : images) {
            content.push_back({
                {"type", "image_url"},
                {"image_url", {{"url", "data:" + img.mime_type + ";base64," + img.base64}}}
            });
        }
        nlohmann::json body = {
            {"model", model},
            {"max_tokens", max_tokens},
            {"messages", nlohmann::json::array({
                {{"role", "user"}, {"content", content}}
            })}
        };
        auto resp = http_post_json(
            "https://api.openai.com/v1/chat/completions",
            body,
            {{"Content-Type", "application/json"}, {"Authorization", "Bearer " + api_key}}
        );
        if (!resp.ok) return Result<nlohmann::json>::Err(resp.error);
        try {
            return Result<nlohmann::json>::Ok(nlohmann::json::parse(resp.get().body));
        } catch (const std::exception& ex) {
            return Result<nlohmann::json>::Err(
                std::string("Failed to parse OpenAI response: ") + ex.what()
            );
        }
    } else if (provider == "anthropic") {
        // Anthropic Vision: {type:"image", source:{type:"base64", media_type, data}}
        nlohmann::json content = nlohmann::json::array();
        for (const auto& img : images) {
            content.push_back({
                {"type", "image"},
                {"source", {
                    {"type", "base64"},
                    {"media_type", img.mime_type},
                    {"data", img.base64}
                }}
            });
        }
        content.push_back({{"type", "text"}, {"text", prompt}});
        nlohmann::json body = {
            {"model", model},
            {"max_tokens", max_tokens},
            {"messages", nlohmann::json::array({
                {{"role", "user"}, {"content", content}}
            })}
        };
        auto resp = http_post_json(
            "https://api.anthropic.com/v1/messages",
            body,
            {
                {"Content-Type", "application/json"},
                {"x-api-key", api_key},
                {"anthropic-version", "2023-06-01"}
            }
        );
        if (!resp.ok) return Result<nlohmann::json>::Err(resp.error);
        try {
            return Result<nlohmann::json>::Ok(nlohmann::json::parse(resp.get().body));
        } catch (const std::exception& ex) {
            return Result<nlohmann::json>::Err(
                std::string("Failed to parse Anthropic response: ") + ex.what()
            );
        }
    } else if (provider == "google") {
        // Gemini Vision: parts = [{inlineData:{mimeType,data}}..., {text:prompt}]
        nlohmann::json parts = nlohmann::json::array();
        for (const auto& img : images) {
            parts.push_back({
                {"inlineData", {{"mimeType", img.mime_type}, {"data", img.base64}}}
            });
        }
        parts.push_back({{"text", prompt}});
        nlohmann::json body = {
            {"contents", nlohmann::json::array({{{"parts", parts}}})},
            {"generationConfig", {{"maxOutputTokens", max_tokens}}}
        };
        std::string url = "https://generativelanguage.googleapis.com/v1beta/models/" +
                          model + ":generateContent?key=" + api_key;
        auto resp = http_post_json(url, body, {{"Content-Type", "application/json"}});
        if (!resp.ok) return Result<nlohmann::json>::Err(resp.error);
        try {
            return Result<nlohmann::json>::Ok(nlohmann::json::parse(resp.get().body));
        } catch (const std::exception& ex) {
            return Result<nlohmann::json>::Err(
                std::string("Failed to parse Gemini response: ") + ex.what()
            );
        }
    }
    return Result<nlohmann::json>::Err("Unknown provider: " + provider);
}

// ============================================================================
// resolveImageModelConfigForTool (对齐 OpenClaw image-tool.ts)
// ============================================================================

// 🎯 对齐 OpenClaw: resolveImageModelConfigForTool({ cfg, agentDir }): ImageModelConfig | null
// Key logic:
//   1. Explicit config (imageModel.primary / imageModel.fallbacks) takes priority
//   2. providerVisionFromConfig: check context metadata for provider-specific vision model
//   3. Primary provider pairing: same-provider vision model preferred
//   4. Cross-provider fallback: openai/gpt-5-mini > anthropic > google
static ImageModelConfig resolve_image_model_config_for_tool(
    const ToolContext& context
) {
    ImageModelConfig config;

    // 🎯 对齐 OpenClaw: const explicit = coerceImageModelConfig(params.cfg)
    // Check context metadata for explicit imageModel config
    auto it = context.metadata.find("imageModel.primary");
    if (it != context.metadata.end() && !it->second.empty()) {
        config.primary = it->second;
    }
    auto it2 = context.metadata.find("imageModel.fallbacks");
    if (it2 != context.metadata.end() && !it2->second.empty()) {
        std::istringstream ss(it2->second);
        std::string tok;
        while (std::getline(ss, tok, ',')) {
            auto s = tok.find_first_not_of(" \t");
            if (s != std::string::npos) {
                auto e = tok.find_last_not_of(" \t");
                tok = tok.substr(s, e - s + 1);
            }
            if (!tok.empty()) config.fallbacks.push_back(tok);
        }
    }
    // 🎯 对齐 OpenClaw: if (explicit.primary?.trim() || explicit.fallbacks?.length > 0) return explicit
    if (!config.primary.empty() || !config.fallbacks.empty()) {
        return config;
    }

    // 🎯 对齐 OpenClaw: resolve primary provider and check auth
    bool openai_ok = (std::getenv("OPENAI_API_KEY") != nullptr);
    bool anthropic_ok = (std::getenv("ANTHROPIC_API_KEY") != nullptr);
    bool google_ok = (std::getenv("GOOGLE_API_KEY") != nullptr ||
                      std::getenv("GEMINI_API_KEY") != nullptr);

    // 🎯 对齐 OpenClaw: providerVisionFromConfig — check context metadata for provider-specific vision model
    // e.g. context.metadata["providerVisionModel"] = "openai/gpt-4o"
    std::string provider_vision_from_config;
    {
        auto pv = context.metadata.find("providerVisionModel");
        if (pv != context.metadata.end() && !pv->second.empty()) {
            provider_vision_from_config = pv->second;
        }
    }

    // 🎯 对齐 OpenClaw: resolve primary provider from context
    std::string primary_provider;
    {
        auto pp = context.metadata.find("primaryProvider");
        if (pp != context.metadata.end() && !pp->second.empty()) {
            primary_provider = pp->second;
        }
    }
    bool provider_ok = false;
    if (primary_provider == "openai") provider_ok = openai_ok;
    else if (primary_provider == "anthropic") provider_ok = anthropic_ok;
    else if (primary_provider == "google") provider_ok = google_ok;

    // 🎯 对齐 OpenClaw: preferred model selection
    std::string preferred;
    auto add_fallback = [&](const std::string& ref) {
        if (ref.empty()) return;
        for (const auto& f : config.fallbacks) {
            if (f == ref) return;  // dedupe
        }
        config.fallbacks.push_back(ref);
    };

    if (provider_ok && !provider_vision_from_config.empty()) {
        // 🎯 对齐 OpenClaw: providerOk && providerVisionFromConfig
        preferred = provider_vision_from_config;
    } else if (primary_provider == "openai" && openai_ok) {
        // 🎯 对齐 OpenClaw: primary.provider === "openai" && openaiOk => "openai/gpt-5-mini"
        preferred = "openai/gpt-5-mini";
    } else if (primary_provider == "anthropic" && anthropic_ok) {
        // 🎯 对齐 OpenClaw: primary.provider === "anthropic" && anthropicOk => ANTHROPIC_IMAGE_PRIMARY
        preferred = ANTHROPIC_IMAGE_PRIMARY;
    }

    if (!preferred.empty()) {
        // 🎯 对齐 OpenClaw: add openai/anthropic fallbacks, then prune primary from fallbacks
        if (openai_ok) add_fallback("openai/gpt-5-mini");
        if (anthropic_ok) add_fallback(ANTHROPIC_IMAGE_FALLBACK);
        // Don't duplicate primary in fallbacks
        config.fallbacks.erase(
            std::remove(config.fallbacks.begin(), config.fallbacks.end(), preferred),
            config.fallbacks.end()
        );
        config.primary = preferred;
        return config;
    }

    // 🎯 对齐 OpenClaw: cross-provider fallback
    if (openai_ok) {
        if (anthropic_ok) {
            add_fallback(ANTHROPIC_IMAGE_FALLBACK);
        }
        config.primary = "openai/gpt-5-mini";
        return config;
    }
    if (anthropic_ok) {
        config.primary = ANTHROPIC_IMAGE_PRIMARY;
        config.fallbacks = {ANTHROPIC_IMAGE_FALLBACK};
        return config;
    }
    if (google_ok) {
        config.primary = "google/gemini-2.0-flash";
        return config;
    }

    return config;  // empty = null
}

// ============================================================================
// runWithImageModelFallback (对齐 OpenClaw model-fallback.ts)
// ============================================================================

struct ImageRunResult {
    std::string text;
    std::string provider;
    std::string model;
    struct Attempt {
        std::string provider;
        std::string model;
        std::string error;
        std::string reason;  // 🎯 对齐 OpenClaw: FallbackAttempt.reason (optional)
    };
    std::vector<Attempt> attempts;
};

// 🎯 对齐 OpenClaw: export async function runWithImageModelFallback({ cfg, modelOverride, run })
// Iterates candidates (primary + fallbacks), records attempts, throws on all failure
static ImageRunResult run_with_image_model_fallback(
    const ImageModelConfig& image_model_config,
    const std::string& model_override,
    const std::string& prompt,
    const std::vector<LoadedImage>& images,
    int max_tokens,
    // API key resolver: provider → api_key
    std::function<std::string(const std::string&)> resolve_api_key
) {
    // 🎯 对齐 OpenClaw: resolveImageFallbackCandidates
    // Build candidates: [modelOverride || primary, ...fallbacks]
    struct Candidate { std::string provider; std::string model; };
    std::vector<Candidate> candidates;

    auto parse_model_ref = [](const std::string& ref) -> Candidate {
        auto slash = ref.find('/');
        if (slash != std::string::npos) {
            return {ref.substr(0, slash), ref.substr(slash + 1)};
        }
        return {"openai", ref};  // default provider
    };

    // 🎯 对齐 OpenClaw: if (params.modelOverride?.trim()) addRaw(params.modelOverride)
    if (!model_override.empty()) {
        candidates.push_back(parse_model_ref(model_override));
    } else {
        // 🎯 对齐 OpenClaw: primary = resolveAgentModelPrimaryValue(cfg?.agents?.defaults?.imageModel)
        if (!image_model_config.primary.empty()) {
            candidates.push_back(parse_model_ref(image_model_config.primary));
        }
    }
    // 🎯 对齐 OpenClaw: for (const raw of imageFallbacks) addRaw(raw)
    for (const auto& fallback : image_model_config.fallbacks) {
        if (!fallback.empty()) {
            auto c = parse_model_ref(fallback);
            // Dedupe
            bool dup = false;
            for (const auto& existing : candidates) {
                if (existing.provider == c.provider && existing.model == c.model) { dup = true; break; }
            }
            if (!dup) candidates.push_back(c);
        }
    }

    // 🎯 对齐 OpenClaw: if (candidates.length === 0) throw new Error("No image model configured...")
    if (candidates.empty()) {
        throw std::runtime_error(
            "No image model configured. Set agents.defaults.imageModel.primary or "
            "agents.defaults.imageModel.fallbacks."
        );
    }

    ImageRunResult result;
    std::string last_error;

    // 🎯 对齐 OpenClaw: for (let i = 0; i < candidates.length; i++) { ... runFallbackAttempt ... }
    for (size_t i = 0; i < candidates.size(); ++i) {
        const auto& candidate = candidates[i];

        // 🎯 对齐 OpenClaw: run(provider, modelId) — check model supports images
        // In HunterClaw, we assume all configured models support images

        // Resolve API key
        std::string api_key = resolve_api_key(candidate.provider);
        if (api_key.empty()) {
            std::string err = "No API key for provider '" + candidate.provider + "'";
            result.attempts.push_back({candidate.provider, candidate.model, err, "auth"});
            last_error = err;
            continue;
        }

        // 🎯 对齐 OpenClaw: const context = buildImageContext(params.prompt, params.images)
        // const message = await complete(model, context, { apiKey, maxTokens })
        auto resp = call_vision_provider(
            candidate.provider, candidate.model, api_key,
            prompt, images, max_tokens
        );

        if (!resp.ok) {
            result.attempts.push_back({candidate.provider, candidate.model, resp.error, ""});
            last_error = resp.error;
            continue;
        }

        // 🎯 对齐 OpenClaw: const text = coerceImageAssistantText({ message, provider, model })
        auto text_result = coerce_image_assistant_text(
            resp.get(), candidate.provider, candidate.model
        );

        if (!text_result.ok) {
            result.attempts.push_back({candidate.provider, candidate.model, text_result.error, ""});
            last_error = text_result.error;
            continue;
        }

        // 🎯 对齐 OpenClaw: return { text, provider, model }
        result.text = text_result.get();
        result.provider = candidate.provider;
        result.model = candidate.model;
        return result;
    }

    // 🎯 对齐 OpenClaw: throwFallbackFailureSummary({ attempts, candidates, lastError, label: "image models" })
    // if (attempts.length <= 1 && lastError) throw lastError  (single failure: rethrow original)
    if (result.attempts.size() <= 1 && !last_error.empty()) {
        throw std::runtime_error(last_error);
    }
    // "All image models failed (N): provider/model: error | ..."
    std::string summary;
    for (const auto& att : result.attempts) {
        if (!summary.empty()) summary += " | ";
        summary += att.provider + "/" + att.model + ": " + att.error;
    }
    throw std::runtime_error(
        "All image models failed (" + std::to_string(result.attempts.size()) + "): " + summary
    );
}

// ============================================================================
// buildTextToolResult (对齐 OpenClaw media-tool-shared.ts)
// ============================================================================

// 🎯 对齐 OpenClaw: export function buildTextToolResult(result, extraDetails)
// Returns: { content:[{type:"text",text}], details:{model, ...extraDetails, attempts} }
static ToolResult build_text_tool_result(
    const ImageRunResult& result,
    const nlohmann::json& extra_details
) {
    // 🎯 对齐 OpenClaw: details = { model: `${result.provider}/${result.model}`, ...extraDetails, attempts }
    nlohmann::json details;
    details["model"] = result.provider + "/" + result.model;
    // Merge extraDetails
    for (auto it = extra_details.begin(); it != extra_details.end(); ++it) {
        details[it.key()] = it.value();
    }
    // 🎯 对齐 OpenClaw: attempts: result.attempts (array of {provider, model, error, reason?})
    nlohmann::json attempts_arr = nlohmann::json::array();
    for (const auto& att : result.attempts) {
        nlohmann::json attempt_obj = {
            {"provider", att.provider},
            {"model", att.model},
            {"error", att.error}
        };
        // 🎯 对齐 OpenClaw: reason is optional — only include when non-empty
        if (!att.reason.empty()) {
            attempt_obj["reason"] = att.reason;
        }
        attempts_arr.push_back(attempt_obj);
    }
    details["attempts"] = attempts_arr;

    ToolResultContent content;
    content.type = ToolResultContent::Type::Text;
    content.text = result.text;

    return ToolResult{
        .content = {content},
        .details = JsonValue(details),
        .success = true
    };
}

// ============================================================================
// ImageAnalysisTool Implementation
// ============================================================================

ImageAnalysisTool::ImageAnalysisTool(
    const ToolContext& context,
    std::shared_ptr<llm::LLMClient> llm_client
) : context_(context), llm_client_(llm_client) {}

std::string ImageAnalysisTool::description() const {
    // 🎯 对齐 OpenClaw: createImageTool description (modelHasVision variant)
    // Check context metadata for modelHasVision flag
    auto it = context_.metadata.find("modelHasVision");
    bool model_has_vision = (it != context_.metadata.end() && it->second == "true");

    if (model_has_vision) {
        // 🎯 对齐 OpenClaw: modelHasVision description
        return "Analyze one or more images with a vision model. "
               "Use image for a single path/URL, or images for multiple (up to 20). "
               "Only use this tool when images were NOT already provided in the user's message. "
               "Images mentioned in the prompt are automatically visible to you.";
    }
    // 🎯 对齐 OpenClaw: default description
    return "Analyze one or more images with the configured image model "
           "(agents.defaults.imageModel). "
           "Use image for a single path/URL, or images for multiple (up to 20). "
           "Provide a prompt describing what to analyze.";
}

Tool::Schema ImageAnalysisTool::parameters() const {
    // 🎯 对齐 OpenClaw: Type.Object({ prompt, image, images, model, maxBytesMb, maxImages })
    // Note: NO provider or api_key — these are internal config, not tool parameters
    Schema schema;
    schema.type = "object";

    JsonValue prompt_prop = JsonValue::object();
    prompt_prop["type"] = "string";
    schema.properties["prompt"] = prompt_prop;

    JsonValue image_prop = JsonValue::object();
    image_prop["type"] = "string";
    image_prop["description"] = "Single image path or URL.";
    schema.properties["image"] = image_prop;

    JsonValue images_prop = JsonValue::object();
    images_prop["type"] = "array";
    images_prop["description"] = "Multiple image paths or URLs (up to maxImages, default 20).";
    schema.properties["images"] = images_prop;

    JsonValue model_prop = JsonValue::object();
    model_prop["type"] = "string";
    schema.properties["model"] = model_prop;

    JsonValue max_bytes_prop = JsonValue::object();
    max_bytes_prop["type"] = "number";
    schema.properties["maxBytesMb"] = max_bytes_prop;

    JsonValue max_images_prop = JsonValue::object();
    max_images_prop["type"] = "number";
    schema.properties["maxImages"] = max_images_prop;

    return schema;
}

ToolResult ImageAnalysisTool::execute(const JsonValue& params) {
    // =========================================================================
    // MARK: - Normalize image + images input and dedupe while preserving order
    // 🎯 对齐 OpenClaw: imageCandidates, seenImages, imageInputs
    // =========================================================================
    std::vector<std::string> image_candidates;

    // 🎯 对齐 OpenClaw: if (typeof record.image === "string") imageCandidates.push(record.image)
    if (params.has("image") && params["image"].is_string()) {
        image_candidates.push_back(params["image"].as_string());
    }

    // 🎯 对齐 OpenClaw: if (Array.isArray(record.images)) imageCandidates.push(...record.images.filter(v => typeof v === "string"))
    if (params.has("images") && params["images"].is_array()) {
        size_t n = params["images"].size();
        for (size_t i = 0; i < n; ++i) {
            const auto& item = params["images"][i];
            if (item.is_string()) {
                image_candidates.push_back(item.as_string());
            }
        }
    }

    // 🎯 对齐 OpenClaw: dedupe while preserving order
    // const seenImages = new Set<string>()
    // const imageInputs: string[] = []
    // for (const candidate of imageCandidates) {
    //   const trimmedCandidate = candidate.trim()
    //   const normalizedForDedupe = trimmedCandidate.startsWith("@") ? trimmedCandidate.slice(1).trim() : trimmedCandidate
    //   if (!normalizedForDedupe || seenImages.has(normalizedForDedupe)) continue
    //   seenImages.add(normalizedForDedupe)
    //   imageInputs.push(trimmedCandidate)
    // }
    std::set<std::string> seen_images;
    std::vector<std::string> image_inputs;
    for (const auto& candidate : image_candidates) {
        std::string trimmed = candidate;
        {
            auto s = trimmed.find_first_not_of(" \t");
            if (s != std::string::npos) trimmed = trimmed.substr(s);
            else trimmed = "";
            if (!trimmed.empty()) {
                auto e = trimmed.find_last_not_of(" \t");
                if (e != std::string::npos) trimmed = trimmed.substr(0, e + 1);
            }
        }
        std::string normalized = trimmed;
        if (!normalized.empty() && normalized[0] == '@') {
            normalized = normalized.substr(1);
            auto ns = normalized.find_first_not_of(" \t");
            if (ns != std::string::npos) normalized = normalized.substr(ns);
            else normalized = "";
        }
        if (normalized.empty() || seen_images.count(normalized)) continue;
        seen_images.insert(normalized);
        image_inputs.push_back(trimmed);
    }

    // 🎯 对齐 OpenClaw: if (imageInputs.length === 0) throw new Error("image required")
    if (image_inputs.empty()) {
        return tool_result::error("image required");
    }

    // =========================================================================
    // MARK: - Enforce max images cap
    // 🎯 对齐 OpenClaw: maxImages cap check
    // =========================================================================
    // 🎯 对齐 OpenClaw: const maxImagesRaw = typeof record.maxImages === "number" ? record.maxImages : undefined
    // const maxImages = typeof maxImagesRaw === "number" && isFinite && > 0 ? Math.floor(maxImagesRaw) : DEFAULT_MAX_IMAGES
    int max_images = DEFAULT_MAX_IMAGES;
    if (params.has("maxImages") && params["maxImages"].is_number()) {
        double raw = params["maxImages"].as_number();
        if (std::isfinite(raw) && raw > 0) {
            max_images = static_cast<int>(std::floor(raw));
        }
    }

    // 🎯 对齐 OpenClaw: if (imageInputs.length > maxImages) return { content, details:{error,count,max} }
    if ((int)image_inputs.size() > max_images) {
        nlohmann::json details = {
            {"error", "too_many_images"},
            {"count", (int)image_inputs.size()},
            {"max", max_images}
        };
        ToolResultContent content;
        content.type = ToolResultContent::Type::Text;
        content.text = "Too many images: " + std::to_string(image_inputs.size()) +
                       " provided, maximum is " + std::to_string(max_images) +
                       ". Please reduce the number of images.";
        return ToolResult{
            .content = {content},
            .details = JsonValue(details),
            .success = true
        };
    }

    // =========================================================================
    // MARK: - Resolve prompt and model override
    // 🎯 对齐 OpenClaw: resolvePromptAndModelOverride(record, DEFAULT_PROMPT)
    // =========================================================================
    auto [prompt, model_override] = resolve_prompt_and_model_override(params, DEFAULT_PROMPT);

    // 🎯 对齐 OpenClaw: pickMaxBytes(options?.config, maxBytesMb)
    double max_bytes_mb = 0.0;
    if (params.has("maxBytesMb") && params["maxBytesMb"].is_number()) {
        max_bytes_mb = params["maxBytesMb"].as_number();
    }
    // cfg_media_max_mb from context metadata
    double cfg_media_max_mb = 0.0;
    {
        auto it = context_.metadata.find("mediaMaxMb");
        if (it != context_.metadata.end()) {
            try { cfg_media_max_mb = std::stod(it->second); } catch (...) {}
        }
    }
    size_t max_bytes = pick_max_bytes(max_bytes_mb, cfg_media_max_mb);

    // =========================================================================
    // MARK: - Load and resolve each image
    // 🎯 对齐 OpenClaw: for (const imageRawInput of imageInputs) { ... loadWebMedia ... }
    // =========================================================================
    std::vector<LoadedImage> loaded_images;

    for (const auto& image_raw_input : image_inputs) {
        // 🎯 对齐 OpenClaw: const trimmed = imageRawInput.trim()
        // const imageRaw = trimmed.startsWith("@") ? trimmed.slice(1).trim() : trimmed
        std::string trimmed = image_raw_input;
        std::string image_raw = trimmed;
        if (!image_raw.empty() && image_raw[0] == '@') {
            image_raw = image_raw.substr(1);
            auto ns = image_raw.find_first_not_of(" \t");
            if (ns != std::string::npos) image_raw = image_raw.substr(ns);
            else image_raw = "";
        }
        // 🎯 对齐 OpenClaw: if (!imageRaw) throw new Error("image required (empty string in array)")
        if (image_raw.empty()) {
            return tool_result::error("image required (empty string in array)");
        }

        // 🎯 对齐 OpenClaw: scheme detection
        // const looksLikeWindowsDrivePath = /^[a-zA-Z]:[\\/]/.test(imageRaw)
        // const hasScheme = /^[a-z][a-z0-9+.-]*:/i.test(imageRaw)
        // const isFileUrl = /^file:/i.test(imageRaw)
        // const isHttpUrl = /^https?:\/\//i.test(imageRaw)
        // const isDataUrl = /^data:/i.test(imageRaw)
        static const std::regex windows_drive_re(R"(^[a-zA-Z]:[\\/])");
        static const std::regex has_scheme_re(R"(^[a-z][a-z0-9+.\-]*:)", std::regex::icase);
        static const std::regex file_url_re(R"(^file:)", std::regex::icase);
        static const std::regex http_url_re(R"(^https?://)", std::regex::icase);
        static const std::regex data_url_re(R"(^data:)", std::regex::icase);

        bool looks_like_windows_drive = std::regex_search(image_raw, windows_drive_re);
        bool has_scheme = std::regex_search(image_raw, has_scheme_re);
        bool is_file_url = std::regex_search(image_raw, file_url_re);
        bool is_http_url = std::regex_search(image_raw, http_url_re);
        bool is_data_url = std::regex_search(image_raw, data_url_re);

        // 🎯 对齐 OpenClaw: unsupported scheme check
        // if (hasScheme && !looksLikeWindowsDrivePath && !isFileUrl && !isHttpUrl && !isDataUrl)
        if (has_scheme && !looks_like_windows_drive && !is_file_url && !is_http_url && !is_data_url) {
            nlohmann::json details = {
                {"error", "unsupported_image_reference"},
                {"image", image_raw_input}
            };
            ToolResultContent content;
            content.type = ToolResultContent::Type::Text;
            content.text = "Unsupported image reference: " + image_raw_input +
                           ". Use a file path, a file:// URL, a data: URL, or an http(s) URL.";
            return ToolResult{
                .content = {content},
                .details = JsonValue(details),
                .success = true
            };
        }

        // 🎯 对齐 OpenClaw: resolvedImage = imageRaw.startsWith("~") ? resolveUserPath(imageRaw) : imageRaw
        // Note: resolvedImage is the original input (before file:// stripping), used in details
        std::string resolved_image = image_raw;
        if (!resolved_image.empty() && resolved_image[0] == '~') {
            const char* home = std::getenv("HOME");
            if (home) resolved_image = std::string(home) + resolved_image.substr(1);
        }

        // 🎯 对齐 OpenClaw: resolvedPath = isDataUrl ? null : (file:// strip or resolvedImage)
        std::string resolved_path;
        if (!is_data_url) {
            if (is_file_url) {
                // 🎯 对齐 OpenClaw: resolvedImage.startsWith("file://") ? resolvedImage.slice("file://".length) : resolvedImage
                resolved_path = resolved_image.substr(7);  // strip "file://"
            } else {
                resolved_path = resolved_image;
            }
        }

        // 🎯 对齐 OpenClaw: const media = isDataUrl ? decodeDataUrl(resolvedImage) : loadWebMedia(resolvedPath, { maxBytes })
        LoadedImage loaded;
        loaded.resolved_image = resolved_image;  // 🎯 对齐 OpenClaw: resolvedImage (original, ~ expanded)

        if (is_data_url) {
            // 🎯 对齐 OpenClaw: decodeDataUrl(resolvedImage)
            auto decoded = decode_data_url(resolved_image);
            if (!decoded.ok) {
                return tool_result::error(decoded.error);
            }
            loaded.base64 = decoded.get().base64;
            loaded.mime_type = decoded.get().mime_type;
        } else {
            // 🎯 对齐 OpenClaw: loadWebMedia(resolvedPath, { maxBytes, localRoots })
            auto media = load_web_media(resolved_path, max_bytes);
            if (!media.ok) {
                return tool_result::error(media.error);
            }
            // 🎯 对齐 OpenClaw: if (media.kind !== "image") throw new Error(...)
            if (media.get().kind != "image") {
                return tool_result::error("Unsupported media type: " + media.get().kind);
            }
            // 🎯 对齐 OpenClaw: const mimeType = ("contentType" in media && media.contentType) || ("mimeType" in media && media.mimeType) || "image/png"
            loaded.mime_type = media.get().mime_type.empty() ? "image/png" : media.get().mime_type;
            loaded.base64 = media.get().base64;
        }

        loaded_images.push_back(std::move(loaded));
    }

    // =========================================================================
    // MARK: - Resolve image model config
    // 🎯 对齐 OpenClaw: imageModelConfig = resolveImageModelConfigForTool({ cfg, agentDir })
    // =========================================================================
    ImageModelConfig image_model_config = resolve_image_model_config_for_tool(context_);

    if (image_model_config.primary.empty() && image_model_config.fallbacks.empty()) {
        return tool_result::error(
            "No image model configured. Set agents.defaults.imageModel.primary or "
            "agents.defaults.imageModel.fallbacks."
        );
    }

    // =========================================================================
    // MARK: - Run image prompt with all loaded images
    // 🎯 对齐 OpenClaw: runImagePrompt({ cfg, agentDir, imageModelConfig, modelOverride, prompt, images })
    // =========================================================================
    get_logger()->info("[ImageAnalysis] Running vision: {} images={}", 
                 image_model_config.primary, loaded_images.size());

    // 🎯 对齐 OpenClaw: resolveImageToolMaxTokens(model.maxTokens)
    int max_tokens = resolve_image_tool_max_tokens(0, 4096);

    // API key resolver
    auto resolve_api_key = [](const std::string& provider) -> std::string {
        if (provider == "openai") {
            const char* k = std::getenv("OPENAI_API_KEY");
            return k ? k : "";
        } else if (provider == "anthropic") {
            const char* k = std::getenv("ANTHROPIC_API_KEY");
            return k ? k : "";
        } else if (provider == "google") {
            const char* k = std::getenv("GOOGLE_API_KEY");
            if (!k) k = std::getenv("GEMINI_API_KEY");
            return k ? k : "";
        }
        return "";
    };

    ImageRunResult run_result;
    try {
        run_result = run_with_image_model_fallback(
            image_model_config,
            model_override,
            prompt,
            loaded_images,
            max_tokens,
            resolve_api_key
        );
    } catch (const std::exception& ex) {
        return tool_result::error(ex.what());
    }

    // =========================================================================
    // MARK: - Build result
    // 🎯 对齐 OpenClaw: buildTextToolResult(result, imageDetails)
    // imageDetails = loadedImages.length === 1
    //   ? { image: loadedImages[0].resolvedImage, rewrittenFrom? }
    //   : { images: loadedImages.map(img => ({ image, rewrittenFrom? })) }
    // =========================================================================
    nlohmann::json image_details;
    if (loaded_images.size() == 1) {
        // 🎯 对齐 OpenClaw: { image: loadedImages[0].resolvedImage, ...(rewrittenFrom ? { rewrittenFrom } : {}) }
        image_details["image"] = loaded_images[0].resolved_image;
        if (!loaded_images[0].rewritten_from.empty()) {
            image_details["rewrittenFrom"] = loaded_images[0].rewritten_from;
        }
    } else {
        // 🎯 对齐 OpenClaw: { images: loadedImages.map(img => ({ image, rewrittenFrom? })) }
        nlohmann::json images_arr = nlohmann::json::array();
        for (const auto& img : loaded_images) {
            nlohmann::json img_detail = {{"image", img.resolved_image}};
            if (!img.rewritten_from.empty()) {
                img_detail["rewrittenFrom"] = img.rewritten_from;
            }
            images_arr.push_back(img_detail);
        }
        image_details["images"] = images_arr;
    }

    return build_text_tool_result(run_result, image_details);
}

// ============================================================================
// Legacy provider_* methods (stub — actual logic in execute())
// ============================================================================

ToolResult ImageAnalysisTool::provider_openai(
    const std::string& /*image_source*/,
    const std::string& /*prompt*/,
    const JsonValue& /*options*/
) {
    return tool_result::error("Use execute() directly");
}

ToolResult ImageAnalysisTool::provider_anthropic(
    const std::string& /*image_source*/,
    const std::string& /*prompt*/,
    const JsonValue& /*options*/
) {
    return tool_result::error("Use execute() directly");
}

ToolResult ImageAnalysisTool::provider_google(
    const std::string& /*image_source*/,
    const std::string& /*prompt*/,
    const JsonValue& /*options*/
) {
    return tool_result::error("Use execute() directly");
}

core::Result<std::string> ImageAnalysisTool::load_image_base64(const std::string& source) {
    auto r = load_web_media(source, 0);
    if (!r.ok) return core::Result<std::string>::Err(r.error);
    return core::Result<std::string>::Ok(r.get().base64);
}

std::string ImageAnalysisTool::detect_mime_type(const std::string& image_data) {
    return detect_mime_type_from_bytes(image_data);
}

} // namespace tools
} // namespace hunterclaw
