#include "hunterclaw/core/types.h"
#include <random>
#include <sstream>
#include <iomanip>
#include <fstream>
#include <sys/stat.h>
#include <ctime>

namespace hunterclaw {
namespace core {
namespace utils {

// Generate UUID v4
std::string generate_uuid() {
    static std::random_device rd;
    static std::mt19937 gen(rd());
    static std::uniform_int_distribution<> dis(0, 15);
    static std::uniform_int_distribution<> dis2(8, 11);

    std::stringstream ss;
    ss << std::hex;
    for (int i = 0; i < 8; i++) ss << dis(gen);
    ss << "-";
    for (int i = 0; i < 4; i++) ss << dis(gen);
    ss << "-4";
    for (int i = 0; i < 3; i++) ss << dis(gen);
    ss << "-";
    ss << dis2(gen);
    for (int i = 0; i < 3; i++) ss << dis(gen);
    ss << "-";
    for (int i = 0; i < 12; i++) ss << dis(gen);
    return ss.str();
}

// Generate random string of given length
std::string generate_random_string(size_t length) {
    static const char alphanum[] =
        "0123456789"
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "abcdefghijklmnopqrstuvwxyz";
    static std::random_device rd;
    static std::mt19937 gen(rd());
    static std::uniform_int_distribution<> dis(0, sizeof(alphanum) - 2);
    
    std::string result;
    result.reserve(length);
    for (size_t i = 0; i < length; i++) {
        result += alphanum[dis(gen)];
    }
    return result;
}

// Get current timestamp
Timestamp now() {
    return std::chrono::system_clock::now();
}

// Format timestamp to ISO 8601
std::string format_timestamp(const Timestamp& ts) {
    auto time = std::chrono::system_clock::to_time_t(ts);
    std::tm tm = *std::gmtime(&time);
    
    std::stringstream ss;
    ss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%S");
    
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        ts.time_since_epoch()) % 1000;
    ss << '.' << std::setfill('0') << std::setw(3) << ms.count() << 'Z';
    
    return ss.str();
}

// Get current timestamp in milliseconds
int64_t current_timestamp_ms() {
    auto now_time = std::chrono::system_clock::now();
    auto duration = now_time.time_since_epoch();
    return std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
}

// Check if file exists
bool file_exists(const std::string& path) {
    struct stat buffer;
    return (stat(path.c_str(), &buffer) == 0);
}

// Read file content
Result<std::string> read_file(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return Result<std::string>::Err("Failed to open file: " + path);
    }
    
    std::stringstream buffer;
    buffer << file.rdbuf();
    return Result<std::string>::Ok(buffer.str());
}

// Write file content
Result<bool> write_file(const std::string& path, const std::string& content) {
    std::ofstream file(path, std::ios::binary);
    if (!file) {
        return Result<bool>::Err("Failed to open file for writing: " + path);
    }
    
    file << content;
    if (!file) {
        return Result<bool>::Err("Failed to write to file: " + path);
    }
    
    return Result<bool>::Ok(true);
}

// Create directory if not exists
bool mkdir_p(const std::string& path) {
    // Simple implementation - could use std::filesystem in C++17+
    std::string cmd = "mkdir -p " + path;
    return system(cmd.c_str()) == 0;
}

// Get home directory
std::string get_home_dir() {
    const char* home = getenv("HOME");
    if (home) return std::string(home);
    return "/tmp";
}

// Expand ~ to home directory
std::string expand_path(const std::string& path) {
    if (path.empty() || path[0] != '~') {
        return path;
    }
    return get_home_dir() + path.substr(1);
}

} // namespace utils
} // namespace core
} // namespace hunterclaw
