#pragma once

#include "hunterclaw/tools/tool.h"
#include "hunterclaw/http/http_client.h"
#include "hunterclaw/llm/llm_client.h"
#include <memory>

namespace hunterclaw {
namespace tools {

/**
 * Image Analysis Tool - AI 图像理解
 * 对齐 OpenClaw image-tool 和 media understanding 能力
 * 
 * Providers:
 * - openai: OpenAI Vision (gpt-4o, gpt-4o-mini)
 * - anthropic: Claude Vision (claude-3.5-sonnet, claude-opus-4)
 * - google: Gemini Vision (gemini-2.0-flash)
 * 
 * 输入: 图像文件路径或 base64 或 URL
 * 输出: AI 分析结果
 */
class ImageAnalysisTool : public Tool {
public:
    explicit ImageAnalysisTool(
        const ToolContext& context,
        std::shared_ptr<llm::LlmClient> llm_client
    );
    
    ~ImageAnalysisTool() override = default;
    
    std::string name() const override { return "image_analysis"; }
    
    std::string description() const override;
    
    Schema parameters() const override;
    
    ToolResult execute(const core::JsonValue& params) override;
    
    bool requires_approval() const override { return false; }
    
private:
    // === Provider 实现 ===
    
    /// OpenAI Vision
    ToolResult provider_openai(
        const std::string& image_source,
        const std::string& prompt,
        const core::JsonValue& options
    );
    
    /// Anthropic Vision
    ToolResult provider_anthropic(
        const std::string& image_source,
        const std::string& prompt,
        const core::JsonValue& options
    );
    
    /// Google Gemini
    ToolResult provider_google(
        const std::string& image_source,
        const std::string& prompt,
        const core::JsonValue& options
    );
    
    // === 辅助函数 ===
    
    /// 读取图像为 base64
    core::Result<std::string> load_image_base64(const std::string& source);
    
    /// 检测 MIME type
    std::string detect_mime_type(const std::string& image_data);
    
    ToolContext context_;
    std::shared_ptr<llm::LlmClient> llm_client_;
};

} // namespace tools
} // namespace hunterclaw
