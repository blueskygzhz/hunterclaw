#pragma once

#include "hunterclaw/core/types.h"
#include <string>

namespace hunterclaw {
namespace core {
namespace utils {

// UUID generation
std::string generate_uuid();

// Time utilities
Timestamp now();
std::string format_timestamp(const Timestamp& ts);

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
