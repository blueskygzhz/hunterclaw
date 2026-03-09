#include "hunterclaw/tools/tool_result_processor.h"
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_sinks.h>

namespace hunterclaw::tools {

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

ResultProcessor create_image_sanitizer(const ImageSanitizerConfig&) {
    return [](const ToolResult& result) {
        static auto logger = spdlog::get("tools");
        if (!logger) {
            logger = spdlog::stdout_logger_mt("tools");
        }
        logger->warn("Image sanitizer not implemented");
        return result;
    };
}

ResultProcessor create_normalizer(const ResultNormalizerConfig&) {
    return [](const ToolResult& result) {
        return result;
    };
}

ResultProcessor create_sensitive_filter() {
    return [](const ToolResult& result) {
        return result;
    };
}

ResultProcessor create_text_truncator(size_t) {
    return [](const ToolResult& result) {
        return result;
    };
}

} // namespace builtin_processors

} // namespace hunterclaw::tools
