#include "hunterclaw/observability/observability.h"
#include "hunterclaw/observability/diagnostic_events.h"
#include "hunterclaw/logging/redact.h"
#include <spdlog/spdlog.h>
#include <spdlog/sinks/file_sinks.h>  // ✅ file_sinks 在此文件中定义
#include <spdlog/sinks/stdout_sinks.h>  // ✅ stdout_sinks
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
        j[key] = value.to_json();
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
        oss << " " << key << "=" << value.to_string();
    }
    
    return oss.str();
}

std::string LogEntry::to_syslog() const {
    // RFC 5424 格式
    int priority = static_cast<int>(level) + 8;  // Facility: user (1), Severity: level
    
    std::ostringstream oss;
    oss << "<" << priority << ">hunterclaw: " << event;
    
    for (const auto& [key, value] : fields) {
        oss << " " << key << "=" << value.to_string();
    }
    
    return oss.str();
}

// ============================================================================
// StructuredLogger 初始化 (完全对齐 OpenClaw)
// ============================================================================

void StructuredLogger::init(const LogConfig& config) {
    // 简化实现：直接创建 logger，避免复杂的 sink 配置
    if (config.outputs.empty()) {
        g_logger = spdlog::stdout_logger_mt("hunterclaw");
    } else {
        // 使用第一个输出配置
        const auto& first_output = config.outputs[0];
        if (first_output.type == LogConfig::Output::Type::File && first_output.path) {
            g_logger = spdlog::basic_logger_mt("hunterclaw", *first_output.path);
        } else {
            g_logger = spdlog::stdout_logger_mt("hunterclaw");
        }
    }
    
    // 设置日志级别
    switch (config.level) {
        case LogLevel::TRACE: g_logger->set_level(spdlog::level::trace); break;
        case LogLevel::DEBUG: g_logger->set_level(spdlog::level::debug); break;
        case LogLevel::INFO: g_logger->set_level(spdlog::level::info); break;
        case LogLevel::WARN: g_logger->set_level(spdlog::level::warn); break;
        case LogLevel::ERROR: g_logger->set_level(spdlog::level::err); break;
        case LogLevel::CRITICAL: g_logger->set_level(spdlog::level::critical); break;
    }
    
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
    
    // 🎯 严格对齐 OpenClaw: 统一调用 logging::redact_sensitive_text
    std::string json_str = entry.to_json();
    if (config_.redact_sensitive) {
        hunterclaw::logging::RedactOptions opts;
        opts.mode = "tools";
        if (!config_.redact_patterns.empty()) {
            opts.patterns = config_.redact_patterns;
        }
        json_str = hunterclaw::logging::redact_sensitive_text(json_str, opts);
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
