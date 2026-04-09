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

// 🎯 严格对齐 OpenClaw: image sanitizer — strip base64 data URIs from tool results
ResultProcessor create_image_sanitizer(const ImageSanitizerConfig& config) {
    return [config](const ToolResult& result) {
        ToolResult sanitized = result;
        if (sanitized.output.empty()) return sanitized;

        // Strip base64 data URIs (对齐 OpenClaw: removeBase64DataUris)
        // Pattern: data:image/...;base64,<very long base64 string>
        static const std::regex base64_uri_re(
            R"(data:image/[a-zA-Z]+;base64,[A-Za-z0-9+/=]{100,})",
            std::regex::optimize);

        std::string& text = sanitized.output;
        std::string replaced = std::regex_replace(text, base64_uri_re,
            "[base64 image data removed — " + std::to_string(config.max_image_size) + " byte limit]");

        if (replaced != text) {
            get_logger()->debug("Image sanitizer: stripped {} bytes of base64 data",
                              text.size() - replaced.size());
            text = replaced;
        }

        return sanitized;
    };
}

// 🎯 严格对齐 OpenClaw: result normalizer — normalize whitespace, trim trailing newlines
ResultProcessor create_normalizer(const ResultNormalizerConfig& config) {
    return [config](const ToolResult& result) {
        ToolResult normalized = result;
        if (normalized.output.empty()) return normalized;

        std::string& text = normalized.output;

        // Collapse excessive blank lines (>3 → 2)
        static const std::regex excessive_newlines(R"(\n{4,})");
        text = std::regex_replace(text, excessive_newlines, "\n\n\n");

        // Trim trailing whitespace
        auto end_pos = text.find_last_not_of(" \t\n\r");
        if (end_pos != std::string::npos) {
            text.resize(end_pos + 1);
        }

        // Normalize tabs if configured
        if (config.normalize_tabs) {
            std::string tab_replacement(config.tab_width, ' ');
            size_t pos = 0;
            while ((pos = text.find('\t', pos)) != std::string::npos) {
                text.replace(pos, 1, tab_replacement);
                pos += tab_replacement.size();
            }
        }

        return normalized;
    };
}

// 🎯 严格对齐 OpenClaw: sensitive filter — redact secrets/tokens from output
ResultProcessor create_sensitive_filter() {
    return [](const ToolResult& result) {
        ToolResult filtered = result;
        if (filtered.output.empty()) return filtered;

        std::string& text = filtered.output;

        // Redact common secret patterns (对齐 OpenClaw: redactSensitiveContent)
        // API keys
        static const std::regex api_key_re(
            R"((sk-|api[_-]?key[=:]\s*|token[=:]\s*|secret[=:]\s*|password[=:]\s*)['\"]?([a-zA-Z0-9_\-]{20,})['\"]?)",
            std::regex::icase | std::regex::optimize);
        text = std::regex_replace(text, api_key_re, "$1[REDACTED]");

        // AWS-style keys
        static const std::regex aws_key_re(
            R"(AKIA[A-Z0-9]{16})",
            std::regex::optimize);
        text = std::regex_replace(text, aws_key_re, "[AWS_KEY_REDACTED]");

        // Bearer tokens
        static const std::regex bearer_re(
            R"(Bearer\s+[a-zA-Z0-9_\-.]{20,})",
            std::regex::optimize);
        text = std::regex_replace(text, bearer_re, "Bearer [REDACTED]");

        return filtered;
    };
}

// 🎯 严格对齐 OpenClaw: text truncator — cap tool output at max_chars
ResultProcessor create_text_truncator(size_t max_chars) {
    return [max_chars](const ToolResult& result) {
        ToolResult truncated = result;
        if (max_chars == 0 || truncated.output.size() <= max_chars) {
            return truncated;
        }

        // Truncate at max_chars, try to break at a newline
        size_t cut = max_chars;
        size_t last_newline = truncated.output.rfind('\n', cut);
        if (last_newline != std::string::npos && last_newline > max_chars * 3 / 4) {
            cut = last_newline;
        }

        size_t original_size = truncated.output.size();
        truncated.output = truncated.output.substr(0, cut) +
            "\n\n[Output truncated: showing " + std::to_string(cut) +
            " of " + std::to_string(original_size) + " characters]";

        get_logger()->debug("Text truncator: {} → {} chars", original_size, cut);
        return truncated;
    };
}

} // namespace builtin_processors

} // namespace hunterclaw::tools
