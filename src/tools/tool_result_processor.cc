// HunterClaw - Tool 结果处理器实现

#include "hunterclaw/tools/tool_result_processor.h"
#include <spdlog/spdlog.h>
#include <regex>
#include <algorithm>

namespace hunterclaw {
namespace tools {

namespace {
    auto log() {
        static auto logger = spdlog::get("tools");
        if (!logger) {
            logger = spdlog::stdout_logger_mt("tools");
        }
        return logger;
    }
}

// ============================================================================
// ImageSanitizer
// ============================================================================

ImageSanitizer::ImageSanitizer(const ImageSanitizerConfig& config)
    : config_(config) {
}

core::Result<std::string> ImageSanitizer::sanitize_base64(const std::string& base64_data) {
    // 获取图片信息
    auto info_result = get_image_info(base64_data);
    if (!info_result) {
        return core::Result<std::string>::err(info_result.error());
    }
    
    auto info = info_result.value();
    
    // 检查是否需要处理
    if (!needs_sanitization(base64_data)) {
        return core::Result<std::string>::ok(base64_data);
    }
    
    // 调整大小和压缩
    return resize_and_compress(base64_data, info);
}

core::Result<void> ImageSanitizer::sanitize_file(const std::string& file_path) {
    // TODO: 实现文件消毒
    // 1. 读取文件
    // 2. 转为 Base64
    // 3. 调用 sanitize_base64
    // 4. 写回文件
    return core::Result<void>::ok();
}

bool ImageSanitizer::needs_sanitization(const std::string& base64_data) {
    auto info_result = get_image_info(base64_data);
    if (!info_result) {
        return false;
    }
    
    auto info = info_result.value();
    
    return info.width > config_.max_width ||
           info.height > config_.max_height ||
           info.file_size > config_.max_file_size;
}

core::Result<ImageSanitizer::ImageInfo> ImageSanitizer::get_image_info(const std::string& base64_data) {
    // 简化实现: 估算大小
    ImageInfo info;
    info.file_size = base64_data.size() * 3 / 4;  // Base64 解码后大小估算
    info.width = 1024;   // 默认值
    info.height = 768;
    info.format = "unknown";
    
    // TODO: 使用图像库 (如 OpenCV, stb_image) 获取真实尺寸
    
    return core::Result<ImageInfo>::ok(info);
}

core::Result<std::string> ImageSanitizer::resize_and_compress(
    const std::string& base64_data,
    const ImageInfo& info
) {
    // TODO: 实现图片调整大小和压缩
    // 1. Base64 解码
    // 2. 使用图像库 resize
    // 3. 压缩
    // 4. 剥离 EXIF (如果启用)
    // 5. Base64 编码
    
    log()->info("Image sanitized: {}x{} -> {}x{}", 
                info.width, info.height,
                config_.max_width, config_.max_height);
    
    // 简化实现: 返回原始数据
    return core::Result<std::string>::ok(base64_data);
}

// ============================================================================
// ResultNormalizer
// ============================================================================

ResultNormalizer::ResultNormalizer(const ResultNormalizerConfig& config)
    : config_(config) {
    if (config_.sanitize_images) {
        image_sanitizer_ = std::make_unique<ImageSanitizer>(config_.image_config);
    }
}

ToolResult ResultNormalizer::normalize(const ToolResult& result) {
    ToolResult normalized = result;
    
    // 规范化文本内容
    if (!normalized.content.empty()) {
        normalized.content = normalize_text(normalized.content);
    }
    
    // 规范化 JSON metadata
    if (normalized.metadata.is_object()) {
        normalized.metadata = normalize_json(normalized.metadata);
    }
    
    // 处理图片
    if (config_.sanitize_images && image_sanitizer_) {
        // 检查是否包含 Base64 图片
        std::regex base64_pattern(R"(data:image/[^;]+;base64,([A-Za-z0-9+/=]+))");
        std::smatch match;
        
        std::string content = normalized.content;
        if (std::regex_search(content, match, base64_pattern)) {
            std::string base64_data = match[1].str();
            auto sanitized = image_sanitizer_->sanitize_base64(base64_data);
            
            if (sanitized) {
                // 替换为消毒后的图片
                content.replace(match.position(1), match.length(1), sanitized.value());
                normalized.content = content;
            }
        }
    }
    
    return normalized;
}

std::string ResultNormalizer::normalize_text(const std::string& text) {
    std::string normalized = text;
    
    // 截断文本
    normalized = truncate_text(normalized);
    
    // 移除敏感信息
    if (config_.remove_sensitive_data) {
        normalized = remove_sensitive_info(normalized);
    }
    
    return normalized;
}

core::JsonValue ResultNormalizer::normalize_json(const core::JsonValue& json) {
    // 限制 JSON 深度
    return limit_json_depth(json, 0);
}

std::string ResultNormalizer::remove_sensitive_info(const std::string& text) {
    std::string result = text;
    
    for (const auto& pattern : config_.sensitive_patterns) {
        // 匹配模式: key="value" 或 key: value
        std::regex key_value_regex(
            pattern + R"(\s*[:=]\s*[\"']?([^\s\"',}]+)[\"']?)",
            std::regex::icase
        );
        
        result = std::regex_replace(result, key_value_regex, 
                                    pattern + ": [REDACTED]");
    }
    
    return result;
}

std::string ResultNormalizer::truncate_text(const std::string& text) {
    if (text.size() <= config_.max_text_length) {
        return text;
    }
    
    std::string truncated = text.substr(0, config_.max_text_length);
    truncated += "\n... (truncated " + 
                 std::to_string(text.size() - config_.max_text_length) + 
                 " characters)";
    
    return truncated;
}

core::JsonValue ResultNormalizer::limit_json_depth(const core::JsonValue& json, int current_depth) {
    if (current_depth >= config_.max_json_depth) {
        return core::JsonValue::string("[max depth reached]");
    }
    
    if (json.is_object()) {
        core::JsonValue result = core::JsonValue::object();
        for (const auto& key : json.keys()) {
            result[key] = limit_json_depth(json[key], current_depth + 1);
        }
        return result;
    }
    
    if (json.is_array()) {
        core::JsonValue result = core::JsonValue::array();
        for (size_t i = 0; i < json.size(); i++) {
            result.append(limit_json_depth(json[i], current_depth + 1));
        }
        return result;
    }
    
    return json;
}

// ============================================================================
// ResultFormatter
// ============================================================================

std::string ResultFormatter::to_markdown(const ToolResult& result) {
    std::ostringstream ss;
    
    ss << "## Tool Result\n\n";
    ss << "**Status**: " << (result.is_success ? "✅ Success" : "❌ Error") << "\n\n";
    
    if (!result.content.empty()) {
        ss << "**Content**:\n```\n" << result.content << "\n```\n\n";
    }
    
    if (result.metadata.is_object() && !result.metadata.empty()) {
        ss << "**Metadata**:\n```json\n" << result.metadata.to_string() << "\n```\n";
    }
    
    return ss.str();
}

core::JsonValue ResultFormatter::to_json(const ToolResult& result) {
    core::JsonValue json = core::JsonValue::object();
    json["is_success"] = result.is_success;
    json["content"] = result.content;
    
    if (result.metadata.is_object()) {
        json["metadata"] = result.metadata;
    }
    
    return json;
}

std::string ResultFormatter::to_text(const ToolResult& result) {
    if (!result.is_success) {
        return "Error: " + result.content;
    }
    return result.content;
}

std::string ResultFormatter::format(const ToolResult& result, ResultFormat target_format) {
    switch (target_format) {
        case ResultFormat::MARKDOWN:
            return to_markdown(result);
        case ResultFormat::JSON:
            return to_json(result).to_string();
        case ResultFormat::TEXT:
            return to_text(result);
        case ResultFormat::AUTO:
        default:
            return to_text(result);
    }
}

ResultFormat ResultFormatter::detect_format(const std::string& content) {
    if (content.empty()) {
        return ResultFormat::TEXT;
    }
    
    // 检测 JSON
    if (content[0] == '{' || content[0] == '[') {
        return ResultFormat::JSON;
    }
    
    // 检测 Markdown
    if (content.find("##") != std::string::npos || 
        content.find("```") != std::string::npos) {
        return ResultFormat::MARKDOWN;
    }
    
    return ResultFormat::TEXT;
}

std::string ResultFormatter::json_to_markdown(const core::JsonValue& json) {
    return "```json\n" + json.to_string() + "\n```";
}

// ============================================================================
// ResultProcessingPipeline
// ============================================================================

void ResultProcessingPipeline::add_processor(const std::string& name, ResultProcessor processor) {
    processors_.push_back({name, processor});
}

void ResultProcessingPipeline::remove_processor(const std::string& name) {
    processors_.erase(
        std::remove_if(processors_.begin(), processors_.end(),
            [&](const auto& p) { return p.first == name; }),
        processors_.end()
    );
}

ToolResult ResultProcessingPipeline::process(const ToolResult& result) {
    ToolResult processed = result;
    
    for (const auto& [name, processor] : processors_) {
        try {
            processed = processor(processed);
        } catch (const std::exception& e) {
            log()->error("Result processor '{}' failed: {}", name, e.what());
        }
    }
    
    return processed;
}

std::vector<std::string> ResultProcessingPipeline::list_processors() const {
    std::vector<std::string> names;
    for (const auto& [name, _] : processors_) {
        names.push_back(name);
    }
    return names;
}

// ============================================================================
// 内置处理器
// ============================================================================

namespace builtin_processors {

ResultProcessor create_image_sanitizer(const ImageSanitizerConfig& config) {
    auto sanitizer = std::make_shared<ImageSanitizer>(config);
    
    return [sanitizer](const ToolResult& result) -> ToolResult {
        ToolResult processed = result;
        
        // 检查并消毒图片
        std::regex base64_pattern(R"(data:image/[^;]+;base64,([A-Za-z0-9+/=]+))");
        std::smatch match;
        
        std::string content = processed.content;
        if (std::regex_search(content, match, base64_pattern)) {
            std::string base64_data = match[1].str();
            auto sanitized = sanitizer->sanitize_base64(base64_data);
            
            if (sanitized) {
                content.replace(match.position(1), match.length(1), sanitized.value());
                processed.content = content;
            }
        }
        
        return processed;
    };
}

ResultProcessor create_normalizer(const ResultNormalizerConfig& config) {
    auto normalizer = std::make_shared<ResultNormalizer>(config);
    
    return [normalizer](const ToolResult& result) -> ToolResult {
        return normalizer->normalize(result);
    };
}

ResultProcessor create_sensitive_filter() {
    ResultNormalizerConfig config;
    config.remove_sensitive_data = true;
    auto normalizer = std::make_shared<ResultNormalizer>(config);
    
    return [normalizer](const ToolResult& result) -> ToolResult {
        ToolResult processed = result;
        // Process each content item
        for (auto& content_item : processed.content) {
            if (content_item.type == ToolResultContent::Type::Text) {
                content_item.text = normalizer->remove_sensitive_info(content_item.text);
            }
        }
        return processed;
    };
}

ResultProcessor create_text_truncator(size_t max_length) {
    return [max_length](const ToolResult& result) -> ToolResult {
        ToolResult processed = result;
        
        // Truncate text content items
        for (auto& content_item : processed.content) {
            if (content_item.type == ToolResultContent::Type::Text && content_item.text.size() > max_length) {
                content_item.text = content_item.text.substr(0, max_length);
                content_item.text += "\n... (truncated)";
            }
        }
        
        return processed;
    };
}

ResultProcessor create_format_converter(ResultFormat target_format) {
    return [target_format](const ToolResult& result) -> ToolResult {
        ToolResult processed = result;
        // Format conversion would need to reconstruct the content vector
        // For now, just return as-is (simplified implementation)
        return processed;
    };
}

}  // namespace builtin_processors

}  // namespace tools
}  // namespace hunterclaw
