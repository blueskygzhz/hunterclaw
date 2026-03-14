#pragma once

#include "hunterclaw/core/result.h"
#include <nlohmann/json.hpp>
#include <string>
#include <memory>
#include <mutex>
#include <fstream>

namespace hunterclaw {
namespace core {

// 日志级别
enum class LogLevel {
    DEBUG = 0,
    INFO = 1,
    WARN = 2,
    ERROR = 3,
};

// 日志输出格式
enum class LogFormat {
    JSON,      // {"level":"info","event":"daemon.start",...}
    CONSOLE,   // [INFO] daemon.start: port=18789
    SYSLOG,    // <6>hunterclaw: daemon.start port=18789
};

// 结构化日志器配置
struct StructuredLogConfig {
    LogLevel min_level = LogLevel::INFO;
    LogFormat format = LogFormat::CONSOLE;
    std::string output_file;  // 空 = stdout
    bool colorize = true;      // 彩色输出 (仅 CONSOLE 格式)
    
    // 日志轮转
    uint64_t rotate_size = 10 * 1024 * 1024;  // 10MB
    int rotate_count = 5;
};

class StructuredLogger {
public:
    // 配置日志器
    static void Configure(const StructuredLogConfig& config);
    
    // 结构化日志 (带 data)
    static void Debug(const std::string& event, const nlohmann::json& data = {});
    static void Info(const std::string& event, const nlohmann::json& data = {});
    static void Warn(const std::string& event, const nlohmann::json& data = {});
    static void Error(const std::string& event, const nlohmann::json& data = {});
    
    // 简单日志 (纯文本)
    static void DebugText(const std::string& message);
    static void InfoText(const std::string& message);
    static void WarnText(const std::string& message);
    static void ErrorText(const std::string& message);
    
    // 条件日志
    static void InfoIf(bool condition, const std::string& event, const nlohmann::json& data = {});
    static void WarnIf(bool condition, const std::string& event, const nlohmann::json& data = {});
    
    // 获取当前配置
    static StructuredLogConfig GetConfig();
    
    // 刷新缓冲区
    static void Flush();
    
private:
    class Impl;
    static std::unique_ptr<Impl> impl_;
    
    // 写入日志
    static void Write(LogLevel level, const std::string& event, const nlohmann::json& data);
    
    // 格式化输出
    static std::string FormatJson(LogLevel level, const std::string& event, const nlohmann::json& data);
    static std::string FormatConsole(LogLevel level, const std::string& event, const nlohmann::json& data, bool colorize);
    static std::string FormatSyslog(LogLevel level, const std::string& event, const nlohmann::json& data);
    
    // 级别转字符串
    static std::string LevelToString(LogLevel level);
    
    // 彩色终端
    static std::string ColorFor(LogLevel level);
    static std::string ColorReset();
};

// RAII 日志作用域 (自动记录开始/结束)
class LogScope {
public:
    LogScope(const std::string& scope_name, const nlohmann::json& data = {});
    ~LogScope();
    
    // 添加额外数据
    void AddData(const std::string& key, const nlohmann::json& value);
    
private:
    std::string scope_name_;
    nlohmann::json data_;
    std::chrono::steady_clock::time_point start_;
};

// 便捷宏 (自动添加文件名和行号)
#define HC_LOG_DEBUG(event, data) \
    StructuredLogger::Debug((event), nlohmann::json(data).merge_patch({{"file", __FILE__}, {"line", __LINE__}}))

#define HC_LOG_INFO(event, data) \
    StructuredLogger::Info((event), nlohmann::json(data).merge_patch({{"file", __FILE__}, {"line", __LINE__}}))

#define HC_LOG_WARN(event, data) \
    StructuredLogger::Warn((event), nlohmann::json(data).merge_patch({{"file", __FILE__}, {"line", __LINE__}}))

#define HC_LOG_ERROR(event, data) \
    StructuredLogger::Error((event), nlohmann::json(data).merge_patch({{"file", __FILE__}, {"line", __LINE__}}))

}  // namespace core
}  // namespace hunterclaw
