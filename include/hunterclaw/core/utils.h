#pragma once

#include "hunterclaw/core/types.h"
#include <string>

namespace hunterclaw {
namespace core {
namespace utils {

// UUID generation
std::string generate_uuid();

// Random string generation
std::string generate_random_string(size_t length);

// Time utilities
Timestamp now();
std::string format_timestamp(const Timestamp& ts);
int64_t current_timestamp_ms();  // 获取当前时间戳(毫秒)

// File utilities
bool file_exists(const std::string& path);
Result<std::string> read_file(const std::string& path);
Result<bool> write_file(const std::string& path, const std::string& content);
bool mkdir_p(const std::string& path);

// Path utilities
std::string get_home_dir();
std::string expand_path(const std::string& path);

} // namespace utils
} // namespace core
} // namespace hunterclaw
