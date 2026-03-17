#include "hunterclaw/observability/observability.h"
#include "hunterclaw/observability/diagnostic_events.h"
#include <spdlog/spdlog.h>
#include <spdlog/sinks/file_sinks.h>  // ✅ daily_file_sink 在此文件中定义
#include <spdlog/sinks/stdout_sinks.h>  // ✅ 修正头文件名
#include <nlohmann/json.hpp>
#include <regex>

using json = nlohmann::json;

namespace hunterclaw {
namespace observability {

// ============================================================================
// 完全对齐 OpenClaw - 结构化日志系统
// 参考: openclaw-analysis/src/logging/logger.ts
// ============================================================================

static std::shared_ptr<spdlog::logger> g_logger;

// ============================================================================
// LogEntry 序列化
// ============================================================================

std::string LogEntry::to_json() const {
    json j = {
        {"level", static_cast<int>(level)},
        {"event", event},
        {"timestamp", std::chrono::duration_cast<std::chrono::milliseconds>(
            timestamp.time_since_epoch()).count()}
    };
    
    // 添加自定义字段
    for (const auto& [key, value] : fields) {
        j[key] = value;
    }
    
    // 添加 trace context
    if (trace_id) j["traceId"] = *trace_id;
    if (span_id) j["spanId"] = *span_id;
    if (session_id) j["sessionId"] = *session_id;
    if (agent_id) j["agentId"] = *agent_id;
    
    return j.dump();
}

std::string LogEntry::to_console() const {
    std::string level_str;
    switch (level) {
        case LogLevel::TRACE: level_str = "TRACE"; break;
        case LogLevel::DEBUG: level_str = "DEBUG"; break;
        case LogLevel::INFO: level_str = "INFO"; break;
        case LogLevel::WARN: level_str = "WARN"; break;
        case LogLevel::ERROR: level_str = "ERROR"; break;
        case LogLevel::CRITICAL: level_str = "CRIT"; break;
    }
    
    std::ostringstream oss;
    oss << "[" << level_str << "] " << event;
    
    for (const auto& [key, value] : fields) {
        oss << " " << key << "=" << value;
    }
    
    return oss.str();
}

std::string LogEntry::to_syslog() const {
    // RFC 5424 格式
    int priority = static_cast<int>(level) + 8;  // Facility: user (1), Severity: level
    
    std::ostringstream oss;
    oss << "<" << priority << ">hunterclaw: " << event;
    
    for (const auto& [key, value] : fields) {
        oss << " " << key << "=" << value;
    }
    
    return oss.str();
}

// ============================================================================
// StructuredLogger 初始化 (完全对齐 OpenClaw)
// ============================================================================

void StructuredLogger::init(const LogConfig& config) {
    std::vector<spdlog::sink_ptr> sinks;
    
    for (const auto& output : config.outputs) {
        if (output.type == LogConfig::Output::Type::File && output.path) {
            // 关键! 按日期滚动日志 (对齐 OpenClaw)
            // 生成: /tmp/hunterclaw/hunterclaw-2026-03-12.log
            auto file_sink = std::make_shared<spdlog::sinks::daily_file_sink_mt>(
                *output.path,  // 例如: /tmp/hunterclaw/hunterclaw.log
                0,             // 午夜滚动 (小时)
                0              // 午夜滚动 (分钟)
            );
            
            // JSON Lines 格式 (每行一个 JSON 对象)
            file_sink->set_pattern("%v");  // 只输出消息本身 (不加时间戳)
            sinks.push_back(file_sink);
        } 
        else if (output.type == LogConfig::Output::Type::Console) {
            auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
            console_sink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %v");
            sinks.push_back(console_sink);
        }
    }
    
    // 创建多 sink logger
    g_logger = std::make_shared<spdlog::logger>("hunterclaw", sinks.begin(), sinks.end());
    
    // 设置日志级别
    switch (config.level) {
        case LogLevel::TRACE: g_logger->set_level(spdlog::level::trace); break;
        case LogLevel::DEBUG: g_logger->set_level(spdlog::level::debug); break;
        case LogLevel::INFO: g_logger->set_level(spdlog::level::info); break;
        case LogLevel::WARN: g_logger->set_level(spdlog::level::warn); break;
        case LogLevel::ERROR: g_logger->set_level(spdlog::level::err); break;
        case LogLevel::CRITICAL: g_logger->set_level(spdlog::level::critical); break;
    }
    
    // 异步刷新 (每1秒)
    spdlog::flush_every(std::chrono::seconds(1));
    
    // 注册为默认 logger
    spdlog::set_default_logger(g_logger);
    
    instance().config_ = config;
}

StructuredLogger& StructuredLogger::instance() {
    static StructuredLogger instance;
    return instance;
}

void StructuredLogger::log(LogLevel level, const std::string& event,
                           const std::map<std::string, core::JsonValue>& fields) {
    if (!g_logger) return;
    
    LogEntry entry;
    entry.level = level;
    entry.event = event;
    entry.fields = fields;
    entry.timestamp = std::chrono::system_clock::now();
    
    // 注入上下文
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!current_trace_id_.empty()) entry.trace_id = current_trace_id_;
        if (!current_span_id_.empty()) entry.span_id = current_span_id_;
        if (!current_session_id_.empty()) entry.session_id = current_session_id_;
        if (!current_agent_id_.empty()) entry.agent_id = current_agent_id_;
    }
    
    // 敏感信息过滤
    std::string json_str = entry.to_json();
    if (config_.redact_sensitive) {
        for (const auto& pattern : config_.redact_patterns) {
            std::regex re(pattern);
            json_str = std::regex_replace(json_str, re, "[REDACTED]");
        }
    }
    
    // 写入日志 (JSON Lines 格式)
    switch (level) {
        case LogLevel::TRACE: g_logger->trace(json_str); break;
        case LogLevel::DEBUG: g_logger->debug(json_str); break;
        case LogLevel::INFO: g_logger->info(json_str); break;
        case LogLevel::WARN: g_logger->warn(json_str); break;
        case LogLevel::ERROR: g_logger->error(json_str); break;
        case LogLevel::CRITICAL: g_logger->critical(json_str); break;
    }
}

void StructuredLogger::set_trace_context(const std::string& trace_id, const std::string& span_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    current_trace_id_ = trace_id;
    current_span_id_ = span_id;
}

void StructuredLogger::set_session_context(const std::string& session_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    current_session_id_ = session_id;
}

void StructuredLogger::set_agent_context(const std::string& agent_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    current_agent_id_ = agent_id;
}

void StructuredLogger::clear_context() {
    std::lock_guard<std::mutex> lock(mutex_);
    current_trace_id_.clear();
    current_span_id_.clear();
    current_session_id_.clear();
    current_agent_id_.clear();
}

// ============================================================================
// LogScope (RAII 自动计时)
// ============================================================================

LogScope::LogScope(const std::string& event, const std::map<std::string, core::JsonValue>& fields)
    : event_(event), fields_(fields), start_time_(std::chrono::steady_clock::now()) {
    StructuredLogger::instance().debug(event + ".start", fields_);
}

LogScope::~LogScope() {
    auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start_time_).count();
    
    fields_["durationMs"] = duration_ms;
    
    if (has_error_) {
        StructuredLogger::instance().error(event_ + ".error", fields_);
    } else {
        StructuredLogger::instance().info(event_ + ".end", fields_);
    }
}

void LogScope::add_field(const std::string& key, const core::JsonValue& value) {
    fields_[key] = value;
}

void LogScope::set_error(const std::string& error_message) {
    has_error_ = true;
    fields_["error"] = error_message;
}

// ============================================================================
// 集成 DiagnosticEventBus → StructuredLogger
// ============================================================================

void init_diagnostic_logging() {
    // 订阅所有诊断事件,自动写入结构化日志
    DiagnosticEventBus::instance().subscribe([](const std::shared_ptr<DiagnosticEvent>& event) {
        try {
            std::string json_str = event->to_json();
            
            // 写入日志 (JSON Lines 格式)
            g_logger->info(json_str);
            
        } catch (const std::exception& e) {
            // 故障隔离: 日志写入失败不影响主流程
        }
    });
}

}  // namespace observability
}  // namespace hunterclaw
