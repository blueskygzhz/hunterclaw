#include "hunterclaw/core/structured_logger.h"
#include <iostream>
#include <iomanip>
#include <ctime>
#include <sstream>
#include <fstream>
#include <sys/stat.h>

namespace hunterclaw {
namespace core {

// ==================== Impl 类 ====================

class StructuredLogger::Impl {
public:
    StructuredLogConfig config;
    std::ofstream file_stream;
    std::mutex mutex;
    
    Impl() = default;
    
    void OpenFile() {
        if (!config.output_file.empty()) {
            file_stream.open(config.output_file, std::ios::app);
        }
    }
    
    void CloseFile() {
        if (file_stream.is_open()) {
            file_stream.close();
        }
    }
    
    void CheckRotation() {
        if (config.output_file.empty() || !file_stream.is_open()) return;
        
        struct stat st;
        if (stat(config.output_file.c_str(), &st) == 0) {
            if (static_cast<uint64_t>(st.st_size) >= config.rotate_size) {
                RotateLogs();
            }
        }
    }
    
    void RotateLogs() {
        CloseFile();
        
        // Rotate: log.N -> log.N+1
        for (int i = config.rotate_count - 1; i > 0; i--) {
            std::string old_path = config.output_file + "." + std::to_string(i);
            std::string new_path = config.output_file + "." + std::to_string(i + 1);
            rename(old_path.c_str(), new_path.c_str());
        }
        
        // log -> log.1
        std::string backup_path = config.output_file + ".1";
        rename(config.output_file.c_str(), backup_path.c_str());
        
        OpenFile();
    }
};

std::unique_ptr<StructuredLogger::Impl> StructuredLogger::impl_ = std::make_unique<Impl>();

// ==================== 公共接口 ====================

void StructuredLogger::Configure(const StructuredLogConfig& config) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->config = config;
    impl_->OpenFile();
}

StructuredLogConfig StructuredLogger::GetConfig() {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->config;
}

void StructuredLogger::Debug(const std::string& event, const nlohmann::json& data) {
    Write(LogLevel::DEBUG, event, data);
}

void StructuredLogger::Info(const std::string& event, const nlohmann::json& data) {
    Write(LogLevel::INFO, event, data);
}

void StructuredLogger::Warn(const std::string& event, const nlohmann::json& data) {
    Write(LogLevel::WARN, event, data);
}

void StructuredLogger::Error(const std::string& event, const nlohmann::json& data) {
    Write(LogLevel::ERROR, event, data);
}

void StructuredLogger::DebugText(const std::string& message) {
    Debug("log.message", {{"message", message}});
}

void StructuredLogger::InfoText(const std::string& message) {
    Info("log.message", {{"message", message}});
}

void StructuredLogger::WarnText(const std::string& message) {
    Warn("log.message", {{"message", message}});
}

void StructuredLogger::ErrorText(const std::string& message) {
    Error("log.message", {{"message", message}});
}

void StructuredLogger::InfoIf(bool condition, const std::string& event, const nlohmann::json& data) {
    if (condition) Info(event, data);
}

void StructuredLogger::WarnIf(bool condition, const std::string& event, const nlohmann::json& data) {
    if (condition) Warn(event, data);
}

void StructuredLogger::Flush() {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->file_stream.is_open()) {
        impl_->file_stream.flush();
    }
    std::cout.flush();
    std::cerr.flush();
}

// ==================== 内部实现 ====================

void StructuredLogger::Write(LogLevel level, const std::string& event, const nlohmann::json& data) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    
    // 检查级别过滤
    if (level < impl_->config.min_level) return;
    
    // 格式化输出
    std::string output;
    switch (impl_->config.format) {
        case LogFormat::JSON:
            output = FormatJson(level, event, data);
            break;
        case LogFormat::CONSOLE:
            output = FormatConsole(level, event, data, impl_->config.colorize);
            break;
        case LogFormat::SYSLOG:
            output = FormatSyslog(level, event, data);
            break;
    }
    
    // 输出到目标
    if (impl_->config.output_file.empty()) {
        if (level >= LogLevel::WARN) {
            std::cerr << output << std::endl;
        } else {
            std::cout << output << std::endl;
        }
    } else {
        if (impl_->file_stream.is_open()) {
            impl_->file_stream << output << std::endl;
            impl_->CheckRotation();
        }
    }
}

std::string StructuredLogger::FormatJson(LogLevel level, const std::string& event, const nlohmann::json& data) {
    nlohmann::json log_entry = data;
    log_entry["level"] = LevelToString(level);
    log_entry["event"] = event;
    
    // 添加时间戳
    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
    auto time_t_now = std::chrono::system_clock::to_time_t(now);
    std::tm tm_now;
    localtime_r(&time_t_now, &tm_now);
    
    char timestamp[32];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%S", &tm_now);
    log_entry["timestamp"] = std::string(timestamp) + "." + std::to_string(ms.count()) + "Z";
    
    return log_entry.dump();
}

std::string StructuredLogger::FormatConsole(LogLevel level, const std::string& event, 
                                           const nlohmann::json& data, bool colorize) {
    std::ostringstream oss;
    
    // 时间戳
    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);
    std::tm tm_now;
    localtime_r(&time_t_now, &tm_now);
    oss << std::put_time(&tm_now, "%H:%M:%S");
    
    // 级别
    if (colorize) oss << " " << ColorFor(level);
    oss << " [" << LevelToString(level) << "]";
    if (colorize) oss << ColorReset();
    
    // 事件
    oss << " " << event;
    
    // 数据
    if (!data.empty()) {
        oss << ": ";
        bool first = true;
        for (auto& [key, value] : data.items()) {
            if (!first) oss << ", ";
            oss << key << "=";
            if (value.is_string()) oss << value.get<std::string>();
            else oss << value.dump();
            first = false;
        }
    }
    
    return oss.str();
}

std::string StructuredLogger::FormatSyslog(LogLevel level, const std::string& event, const nlohmann::json& data) {
    // RFC 5424 syslog format
    int priority = 8 + static_cast<int>(level);  // facility=user(8)
    
    std::ostringstream oss;
    oss << "<" << priority << ">hunterclaw: " << event;
    
    if (!data.empty()) {
        for (auto& [key, value] : data.items()) {
            oss << " " << key << "=" << (value.is_string() ? value.get<std::string>() : value.dump());
        }
    }
    
    return oss.str();
}

std::string StructuredLogger::LevelToString(LogLevel level) {
    switch (level) {
        case LogLevel::DEBUG: return "DEBUG";
        case LogLevel::INFO:  return "INFO";
        case LogLevel::WARN:  return "WARN";
        case LogLevel::ERROR: return "ERROR";
        default: return "UNKNOWN";
    }
}

std::string StructuredLogger::ColorFor(LogLevel level) {
    switch (level) {
        case LogLevel::DEBUG: return "\033[36m";  // Cyan
        case LogLevel::INFO:  return "\033[32m";  // Green
        case LogLevel::WARN:  return "\033[33m";  // Yellow
        case LogLevel::ERROR: return "\033[31m";  // Red
        default: return "";
    }
}

std::string StructuredLogger::ColorReset() {
    return "\033[0m";
}

// ==================== LogScope ====================

LogScope::LogScope(const std::string& scope_name, const nlohmann::json& data)
    : scope_name_(scope_name), data_(data), start_(std::chrono::steady_clock::now()) {
    StructuredLogger::Info(scope_name_ + ".start", data_);
}

LogScope::~LogScope() {
    auto end = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start_);
    
    data_["duration_ms"] = duration.count();
    StructuredLogger::Info(scope_name_ + ".end", data_);
}

void LogScope::AddData(const std::string& key, const nlohmann::json& value) {
    data_[key] = value;
}

}  // namespace core
}  // namespace hunterclaw
