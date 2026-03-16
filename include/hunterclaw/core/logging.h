#pragma once

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_sinks.h>
#include <memory>

namespace hunterclaw {
namespace core {

// Logger wrapper
inline std::shared_ptr<spdlog::logger> get_logger(const std::string& name = "hunterclaw") {
    auto logger = spdlog::get(name);
    if (!logger) {
        // Create a basic stdout logger if not exists
        auto sink = std::make_shared<spdlog::sinks::stdout_sink_mt>();
        logger = std::make_shared<spdlog::logger>(name, sink);
        spdlog::register_logger(logger);
    }
    return logger;
}

} // namespace core
} // namespace hunterclaw
