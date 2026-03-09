// HunterClaw - High-Performance AI Agent Platform (C++)
// Copyright (c) 2025 HunterClaw Team
// 
// Tool 结果处理器 (100% 对齐 OpenClaw)
// 提供结果规范化、图片消毒、内容过滤等功能

#pragma once

#include <string>
#include <vector>
#include <memory>
#include "hunterclaw/tools/tool.h"

namespace hunterclaw {
namespace tools {

// ============================================================================
// 图片消毒配置
// ============================================================================

struct ImageSanitizerConfig {
    // 最大图片尺寸 (像素)
    int max_width = 1024;
    int max_height = 1024;
    
    // 最大文件大小 (字节)
    size_t max_file_size = 5 * 1024 * 1024;  // 5MB
    
    // 压缩质量 (1-100)
    int compression_quality = 85;
    
    // 是否启用降采样
    bool enable_downsampling = true;
    
    // 是否剥离 EXIF 元数据
    bool strip_metadata = true;
};

/// 图片消毒器
class ImageSanitizer {
public:
    explicit ImageSanitizer(const ImageSanitizerConfig& config = ImageSanitizerConfig{});
    
    /// 消毒图片 (Base64 编码)
    core::Result<std::string> sanitize_base64(const std::string& base64_data);
    
    /// 消毒图片文件
    core::Result<void> sanitize_file(const std::string& file_path);
    
    /// 检查图片是否需要消毒
    bool needs_sanitization(const std::string& base64_data);
    
    const ImageSanitizerConfig& get_config() const { return config_; }

private:
    ImageSanitizerConfig config_;
    
    struct ImageInfo {
        int width;
        int height;
        size_t file_size;
        std::string format;
    };
    
    core::Result<ImageInfo> get_image_info(const std::string& base64_data);
    core::Result<std::string> resize_and_compress(
        const std::string& base64_data,
        const ImageInfo& info
    );
};

// ============================================================================
// 结果规范化配置
// ============================================================================

struct ResultNormalizerConfig {
    // 最大文本长度
    size_t max_text_length = 100000;
    
    // 最大 JSON 深度
    int max_json_depth = 10;
    
    // 是否启用图片消毒
    bool sanitize_images = true;
    
    // 图片消毒配置
    ImageSanitizerConfig image_config;
    
    // 是否移除敏感信息 (API keys, passwords)
    bool remove_sensitive_data = true;
    
    // 敏感信息模式
    std::vector<std::string> sensitive_patterns = {
        "api_key", "apikey", "api-key",
        "password", "passwd", "pwd",
        "secret", "token", "bearer",
        "authorization", "auth"
    };
};

/// 结果规范化器 (100% 对齐 OpenClaw)
class ResultNormalizer {
public:
    explicit ResultNormalizer(const ResultNormalizerConfig& config = ResultNormalizerConfig{});
    
    /// 规范化工具结果
    ToolResult normalize(const ToolResult& result);
    
    /// 规范化文本内容
    std::string normalize_text(const std::string& text);
    
    /// 规范化 JSON 内容
    core::JsonValue normalize_json(const core::JsonValue& json);
    
    /// 移除敏感信息
    std::string remove_sensitive_info(const std::string& text);
    
    const ResultNormalizerConfig& get_config() const { return config_; }

private:
    ResultNormalizerConfig config_;
    std::unique_ptr<ImageSanitizer> image_sanitizer_;
    
    std::string truncate_text(const std::string& text);
    core::JsonValue limit_json_depth(const core::JsonValue& json, int current_depth);
};

// ============================================================================
// 结果格式化器
// ============================================================================

/// 结果格式选项
enum class ResultFormat {
    AUTO,       // 自动检测
    TEXT,       // 纯文本
    JSON,       // JSON
    MARKDOWN,   // Markdown
    HTML        // HTML
};

/// 结果格式化器
class ResultFormatter {
public:
    /// 格式化为 Markdown
    static std::string to_markdown(const ToolResult& result);
    
    /// 格式化为 JSON
    static core::JsonValue to_json(const ToolResult& result);
    
    /// 格式化为纯文本
    static std::string to_text(const ToolResult& result);
    
    /// 自动检测格式并转换
    static std::string format(const ToolResult& result, ResultFormat target_format);

private:
    static ResultFormat detect_format(const std::string& content);
    static std::string json_to_markdown(const core::JsonValue& json);
};

// ============================================================================
// 结果处理管道
// ============================================================================

/// 结果处理步骤
using ResultProcessor = std::function<ToolResult(const ToolResult&)>;

/// 结果处理管道
class ResultProcessingPipeline {
public:
    /// 添加处理步骤
    void add_processor(const std::string& name, ResultProcessor processor);
    
    /// 移除处理步骤
    void remove_processor(const std::string& name);
    
    /// 处理结果
    ToolResult process(const ToolResult& result);
    
    /// 列出所有处理器
    std::vector<std::string> list_processors() const;

private:
    std::vector<std::pair<std::string, ResultProcessor>> processors_;
};

// ============================================================================
// 内置结果处理器
// ============================================================================

namespace builtin_processors {

/// 创建图片消毒处理器
ResultProcessor create_image_sanitizer(const ImageSanitizerConfig& config);

/// 创建结果规范化处理器
ResultProcessor create_normalizer(const ResultNormalizerConfig& config);

/// 创建敏感信息移除处理器
ResultProcessor create_sensitive_filter();

/// 创建文本截断处理器
ResultProcessor create_text_truncator(size_t max_length);

/// 创建格式转换处理器
ResultProcessor create_format_converter(ResultFormat target_format);

}  // namespace builtin_processors

}  // namespace tools
}  // namespace hunterclaw
