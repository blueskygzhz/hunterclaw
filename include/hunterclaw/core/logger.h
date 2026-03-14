#pragma once

#include <spdlog/spdlog.h>
#include <string>

namespace hunterclaw {
namespace core {

// Logger macros for consistent logging
#define HUNTERCLAW_LOG_DEBUG(...) spdlog::debug(__VA_ARGS__)
#define HUNTERCLAW_LOG_INFO(...)  spdlog::info(__VA_ARGS__)
#define HUNTERCLAW_LOG_WARN(...)  spdlog::warn(__VA_ARGS__)
#define HUNTERCLAW_LOG_ERROR(...) spdlog::error(__VA_ARGS__)

// Initialize logging system
void init_logger(const std::string& level = "info");

// Get or create named logger
std::shared_ptr<spdlog::logger> get_logger(const std::string& name = "hunterclaw");

} // namespace core
} // namespace hunterclaw
