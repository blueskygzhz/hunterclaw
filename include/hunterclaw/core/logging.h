#pragma once

#include <spdlog/spdlog.h>
#include <memory>

namespace hunterclaw {
namespace core {

// Logger wrapper
inline std::shared_ptr<spdlog::logger> get_logger(const std::string& name = "hunterclaw") {
    auto logger = spdlog::get(name);
    return logger ? logger : spdlog::default_logger();
}

} // namespace core
} // namespace hunterclaw
