#include "hunterclaw/tools/tool_result_processor.h"
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_sinks.h>
#include <regex>
#include <algorithm>

namespace hunterclaw::tools {

namespace {

// 获取 Tools logger
auto get_logger() {
    static auto logger = []() {
        auto log = spdlog::get("tools");
        if (!log) {
            log = spdlog::stdout_logger_mt("tools");
        }
        return log;
    }();
    return logger;
}

} // namespace

void ResultProcessingPipeline::add_processor(const std::string& name, ResultProcessor processor) {
    processors_.push_back({name, processor});
}

void ResultProcessingPipeline::remove_processor(const std::string& name) {
    processors_.erase(
        std::remove_if(processors_.begin(), processors_.end(),
            [&name](const auto& p) { return p.first == name; }),
        processors_.end());
}

ToolResult ResultProcessingPipeline::process(const ToolResult& result) {
    ToolResult output = result;
    for (const auto& [name, processor] : processors_) {
        output = processor(output);
    }
    return output;
}

std::vector<std::string> ResultProcessingPipeline::list_processors() const {
    std::vector<std::string> names;
    for (const auto& [name, _] : processors_) {
        names.push_back(name);
    }
    return names;
}

namespace builtin_processors {

// ✅ 对齐 OC: tool result 后处理 — image sanitizer + normalizer + sensitive filter + truncator

ResultProcessor create_image_sanitizer(const ImageSanitizerConfig& config) {
    return [config](const ToolResult& result) {
        ToolResult output = result;
        // Strip base64 image data from text content to avoid bloating context
        for (auto& content : output.content) {
            if (content.type == ToolResultContent::Type::Text) {
                std::regex base64_re(R"(data:image/[^;]+;base64,[A-Za-z0-9+/=]{100,})");
                content.text = std::regex_replace(content.text, base64_re,
                    "[image data removed — " + std::to_string(config.max_file_size) + "B limit]");
            }
        }
        return output;
    };
}

ResultProcessor create_normalizer(const ResultNormalizerConfig&) {
    return [](const ToolResult& result) {
        ToolResult output = result;
        for (auto& content : output.content) {
            if (content.type == ToolResultContent::Type::Text) {
                // Collapse excessive whitespace
                content.text = std::regex_replace(content.text, std::regex(R"(\s{3,})"), "\n\n");
                // Strip ANSI escape codes
                content.text = std::regex_replace(content.text,
                    std::regex(R"(\x1B\[[0-9;]*[A-Za-z])"), "");
            }
        }
        return output;
    };
}

ResultProcessor create_sensitive_filter() {
    return [](const ToolResult& result) {
        ToolResult output = result;
        for (auto& content : output.content) {
            if (content.type == ToolResultContent::Type::Text) {
                // Mask API keys and tokens: sk-..., token-..., ghp_..., etc.
                content.text = std::regex_replace(content.text,
                    std::regex(R"((sk-|token-|ghp_|gho_|Bearer\s+)[A-Za-z0-9_\-]{20,})"),
                    "$1[REDACTED]");
                // Mask email addresses
                content.text = std::regex_replace(content.text,
                    std::regex(R"([a-zA-Z0-9._%+-]+@[a-zA-Z0-9.-]+\.[a-zA-Z]{2,})"),
                    "[email]");
            }
        }
        return output;
    };
}

ResultProcessor create_text_truncator(size_t max_chars) {
    return [max_chars](const ToolResult& result) {
        if (max_chars == 0) return result;
        ToolResult output = result;
        for (auto& content : output.content) {
            if (content.type == ToolResultContent::Type::Text &&
                content.text.size() > max_chars) {
                auto truncated_size = content.text.size();
                content.text = content.text.substr(0, max_chars) +
                    "\n\n... [truncated " + std::to_string(truncated_size - max_chars) + " chars]";
            }
        }
        return output;
    };
}

} // namespace builtin_processors

} // namespace hunterclaw::tools
