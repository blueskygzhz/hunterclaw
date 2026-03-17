#include "hunterclaw/logging/subsystem_logger.h"
#include <algorithm>
#include <iostream>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <set>
#include <optional>

namespace hunterclaw {
namespace logging {

namespace {
    /// Subsystem Color Mappings (对齐 OpenClaw SUBSYSTEM_COLORS)
    const std::vector<std::string> SUBSYSTEM_COLORS = {
        "cyan", "green", "yellow", "blue", "magenta", "red"
    };
    
    /// Known Channel Subsystems (对齐 OpenClaw CHANNEL_SUBSYSTEM_PREFIXES)
    const std::set<std::string> CHANNEL_SUBSYSTEM_PREFIXES = {
        "telegram", "whatsapp", "discord", "irc", "googlechat",
        "slack", "signal", "imessage"
    };
    
    /// Subsystem Prefixes to Drop (对齐 OpenClaw SUBSYSTEM_PREFIXES_TO_DROP)
    const std::vector<std::string> SUBSYSTEM_PREFIXES_TO_DROP = {
        "gateway", "channels", "providers"
    };
    
    const int SUBSYSTEM_MAX_SEGMENTS = 2;
    
    /// Pick subsystem color (对齐 OpenClaw pickSubsystemColor)
    std::string pick_subsystem_color(const std::string& subsystem) {
        // Simple hash for color selection
        size_t hash = 0;
        for (char c : subsystem) {
            hash = hash * 31 + static_cast<size_t>(c);
        }
        size_t idx = hash % SUBSYSTEM_COLORS.size();
        return SUBSYSTEM_COLORS[idx];
    }
    
    /// Format subsystem for console (对齐 OpenClaw formatSubsystemForConsole)
    std::string format_subsystem_for_console(const std::string& subsystem) {
        std::vector<std::string> parts;
        std::string current;
        
        // Split by '/'
        for (char c : subsystem) {
            if (c == '/') {
                if (!current.empty()) {
                    parts.push_back(current);
                    current.clear();
                }
            } else {
                current += c;
            }
        }
        if (!current.empty()) {
            parts.push_back(current);
        }
        
        if (parts.empty()) {
            return subsystem;
        }
        
        std::string original = subsystem;
        
        // Drop prefixes (对齐 OpenClaw SUBSYSTEM_PREFIXES_TO_DROP)
        while (!parts.empty()) {
            bool found = false;
            for (const auto& prefix : SUBSYSTEM_PREFIXES_TO_DROP) {
                if (parts[0] == prefix) {
                    found = true;
                    break;
                }
            }
            if (found) {
                parts.erase(parts.begin());
            } else {
                break;
            }
        }
        
        if (parts.empty()) {
            return original;
        }
        
        // Check channel subsystem (对齐 OpenClaw CHANNEL_SUBSYSTEM_PREFIXES)
        if (CHANNEL_SUBSYSTEM_PREFIXES.count(parts[0])) {
            return parts[0];
        }
        
        // Limit segments (对齐 OpenClaw SUBSYSTEM_MAX_SEGMENTS)
        if (static_cast<int>(parts.size()) > SUBSYSTEM_MAX_SEGMENTS) {
            parts = std::vector<std::string>(
                parts.end() - SUBSYSTEM_MAX_SEGMENTS, parts.end()
            );
        }
        
        // Join
        std::string result;
        for (size_t i = 0; i < parts.size(); ++i) {
            if (i > 0) result += "/";
            result += parts[i];
        }
        return result;
    }
}

// ============================================================================
// LogLevel String Conversion
// ============================================================================

std::string to_string(LogLevel level) {
    switch (level) {
        case LogLevel::Silent: return "silent";
        case LogLevel::Fatal:  return "fatal";
        case LogLevel::Error:  return "error";
        case LogLevel::Warn:   return "warn";
        case LogLevel::Info:   return "info";
        case LogLevel::Debug:  return "debug";
        case LogLevel::Trace:  return "trace";
        default: return "unknown";
    }
}

std::optional<LogLevel> log_level_from_string(const std::string& str) {
    std::string lower = str;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    
    if (lower == "silent") return LogLevel::Silent;
    if (lower == "fatal")  return LogLevel::Fatal;
    if (lower == "error")  return LogLevel::Error;
    if (lower == "warn")   return LogLevel::Warn;
    if (lower == "info")   return LogLevel::Info;
    if (lower == "debug")  return LogLevel::Debug;
    if (lower == "trace")  return LogLevel::Trace;
    
    return std::nullopt;
}

int level_to_min_level(LogLevel level) {
    // 对齐 OpenClaw levelToMinLevel: fatal=0, error=1, warn=2, info=3, debug=4, trace=5
    switch (level) {
        case LogLevel::Fatal:  return 0;
        case LogLevel::Error:  return 1;
        case LogLevel::Warn:   return 2;
        case LogLevel::Info:   return 3;
        case LogLevel::Debug:  return 4;
        case LogLevel::Trace:  return 5;
        case LogLevel::Silent: return std::numeric_limits<int>::max();
        default: return 3; // default to info
    }
}

LogLevel normalize_log_level(const std::string& level_str, LogLevel fallback) {
    auto level = log_level_from_string(level_str);
    return level ? *level : fallback;
}

// ============================================================================
// SubsystemLoggerImpl Implementation
// ============================================================================

class SubsystemLoggerImpl : public SubsystemLogger {
public:
    explicit SubsystemLoggerImpl(std::string subsystem_name)
        : subsystem_(std::move(subsystem_name)) {
        // 获取或创建 spdlog logger
        logger_ = spdlog::get(subsystem_);
        if (!logger_) {
            logger_ = spdlog::stdout_logger_mt(subsystem_);
        }
    }
    
    std::string subsystem() const override {
        return subsystem_;
    }
    
    bool is_enabled(LogLevel level, LogTarget target) const override {
        // 简化版: 检查 spdlog level
        auto spdlog_level = to_spdlog_level(level);
        return logger_->should_log(spdlog_level);
    }
    
    void trace(const std::string& message, const std::map<std::string, std::string>& meta) override {
        log_impl(LogLevel::Trace, message, meta);
    }
    
    void debug(const std::string& message, const std::map<std::string, std::string>& meta) override {
        log_impl(LogLevel::Debug, message, meta);
    }
    
    void info(const std::string& message, const std::map<std::string, std::string>& meta) override {
        log_impl(LogLevel::Info, message, meta);
    }
    
    void warn(const std::string& message, const std::map<std::string, std::string>& meta) override {
        log_impl(LogLevel::Warn, message, meta);
    }
    
    void error(const std::string& message, const std::map<std::string, std::string>& meta) override {
        log_impl(LogLevel::Error, message, meta);
    }
    
    void fatal(const std::string& message, const std::map<std::string, std::string>& meta) override {
        log_impl(LogLevel::Fatal, message, meta);
    }
    
    void raw(const std::string& message) override {
        // 对齐 OpenClaw raw() - 直接输出到控制台
        std::cout << message << std::endl;
    }
    
    Ptr child(const std::string& name) override {
        // 对齐 OpenClaw child() - 创建子系统 "parent/child"
        std::string child_subsystem = subsystem_ + "/" + name;
        return std::make_shared<SubsystemLoggerImpl>(child_subsystem);
    }

private:
    std::string subsystem_;
    std::shared_ptr<spdlog::logger> logger_;
    
    spdlog::level::level_enum to_spdlog_level(LogLevel level) const {
        switch (level) {
            case LogLevel::Trace: return spdlog::level::trace;
            case LogLevel::Debug: return spdlog::level::debug;
            case LogLevel::Info:  return spdlog::level::info;
            case LogLevel::Warn:  return spdlog::level::warn;
            case LogLevel::Error: return spdlog::level::err;
            case LogLevel::Fatal: return spdlog::level::critical;
            default: return spdlog::level::info;
        }
    }
    
    void log_impl(
        LogLevel level,
        const std::string& message,
        const std::map<std::string, std::string>& meta
    ) {
        if (!is_enabled(level, LogTarget::Any)) {
            return;
        }
        
        // Format message with subsystem (对齐 OpenClaw formatConsoleLine)
        std::string display_subsystem = format_subsystem_for_console(subsystem_);
        std::string formatted = "[" + display_subsystem + "] " + message;
        
        // Add meta if present
        if (!meta.empty()) {
            formatted += " {";
            bool first = true;
            for (const auto& [key, value] : meta) {
                if (!first) formatted += ", ";
                formatted += key + "=" + value;
                first = false;
            }
            formatted += "}";
        }
        
        // Log to spdlog
        auto spdlog_level = to_spdlog_level(level);
        logger_->log(spdlog_level, formatted);
    }
};

// ============================================================================
// Factory Function
// ============================================================================

SubsystemLogger::Ptr create_subsystem_logger(const std::string& subsystem) {
    return std::make_shared<SubsystemLoggerImpl>(subsystem);
}

} // namespace logging
} // namespace hunterclaw
