#pragma once

#include "hunterclaw/core/result.h"
#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace hunterclaw {
namespace observability {

// ============================================================================
// 1. 结构化日志系统 (Structured Logging)
// ============================================================================

enum class LogLevel {
    TRACE = 0,
    DEBUG = 1,
    INFO = 2,
    WARN = 3,
    ERROR = 4,
    CRITICAL = 5
};

enum class LogFormat {
    JSON,      // {"level":"info","event":"daemon.start","port":18789}
    CONSOLE,   // [INFO] daemon.start: port=18789
    SYSLOG     // <6>hunterclaw: daemon.start port=18789
};

struct LogEntry {
    LogLevel level;
    std::string event;                                  // 事件名称 (例如: "gateway.request")
    std::map<std::string, core::JsonValue> fields;      // 结构化字段
    std::chrono::system_clock::time_point timestamp;
    std::optional<std::string> trace_id;                // 分布式追踪 ID
    std::optional<std::string> span_id;
    std::optional<std::string> session_id;
    std::optional<std::string> agent_id;
    
    std::string to_json() const;
    std::string to_console() const;
    std::string to_syslog() const;
};

struct LogConfig {
    LogLevel level = LogLevel::INFO;
    LogFormat format = LogFormat::JSON;
    
    // 输出配置
    struct Output {
        enum class Type { File, Console, Syslog };
        Type type;
        std::optional<std::string> path;                // 文件路径
        std::optional<size_t> max_size_mb;              // 最大文件大小
        std::optional<size_t> max_files;                // 最大文件数
        std::string rotation = "daily";                 // daily, hourly, size
    };
    std::vector<Output> outputs;
    
    // 敏感信息过滤
    bool redact_sensitive = true;
    std::vector<std::string> redact_patterns = {
        R"(sk-[a-zA-Z0-9]+)",           // OpenAI API keys
        R"(Bearer [a-zA-Z0-9]+)",       // JWT tokens
        R"(password=\S+)"                // Passwords
    };
    
    // 审计日志
    struct Audit {
        bool enabled = true;
        std::string path = "/var/log/hunterclaw/audit.log";
        bool log_requests = true;
        bool log_tool_calls = true;
        bool log_config_changes = true;
    };
    Audit audit;
    
    core::JsonValue to_json() const;
    static core::Result<LogConfig> from_json(const core::JsonValue& json);
};

class StructuredLogger {
public:
    static void init(const LogConfig& config);
    static StructuredLogger& instance();
    
    // 日志接口
    void log(LogLevel level, const std::string& event, 
             const std::map<std::string, core::JsonValue>& fields = {});
    
    void trace(const std::string& event, const std::map<std::string, core::JsonValue>& fields = {}) {
        log(LogLevel::TRACE, event, fields);
    }
    void debug(const std::string& event, const std::map<std::string, core::JsonValue>& fields = {}) {
        log(LogLevel::DEBUG, event, fields);
    }
    void info(const std::string& event, const std::map<std::string, core::JsonValue>& fields = {}) {
        log(LogLevel::INFO, event, fields);
    }
    void warn(const std::string& event, const std::map<std::string, core::JsonValue>& fields = {}) {
        log(LogLevel::WARN, event, fields);
    }
    void error(const std::string& event, const std::map<std::string, core::JsonValue>& fields = {}) {
        log(LogLevel::ERROR, event, fields);
    }
    void critical(const std::string& event, const std::map<std::string, core::JsonValue>& fields = {}) {
        log(LogLevel::CRITICAL, event, fields);
    }
    
    // 上下文注入
    void set_trace_context(const std::string& trace_id, const std::string& span_id);
    void set_session_context(const std::string& session_id);
    void set_agent_context(const std::string& agent_id);
    void clear_context();
    
private:
    StructuredLogger() = default;
    
    LogConfig config_;
    std::string current_trace_id_;
    std::string current_span_id_;
    std::string current_session_id_;
    std::string current_agent_id_;
    std::mutex mutex_;
};

// RAII 作用域日志
class LogScope {
public:
    LogScope(const std::string& event, const std::map<std::string, core::JsonValue>& fields = {});
    ~LogScope();
    
    void add_field(const std::string& key, const core::JsonValue& value);
    void set_error(const std::string& error_message);
    
private:
    std::string event_;
    std::map<std::string, core::JsonValue> fields_;
    std::chrono::steady_clock::time_point start_time_;
    bool has_error_ = false;
};

// ============================================================================
// 2. Prometheus 指标系统 (Metrics)
// ============================================================================

class MetricCollector {
public:
    virtual ~MetricCollector() = default;
    
    // 导出 Prometheus 格式
    virtual std::string export_prometheus() const = 0;
};

class Counter : public MetricCollector {
public:
    Counter(const std::string& name, const std::string& help, 
            const std::vector<std::string>& label_names = {});
    
    void inc(double value = 1.0, const std::map<std::string, std::string>& labels = {});
    
    std::string export_prometheus() const override;
    
private:
    struct LabelHash {
        size_t operator()(const std::map<std::string, std::string>& labels) const;
    };
    
    std::string name_;
    std::string help_;
    std::vector<std::string> label_names_;
    std::map<std::map<std::string, std::string>, std::atomic<double>> values_;
    mutable std::mutex mutex_;
};

class Gauge : public MetricCollector {
public:
    Gauge(const std::string& name, const std::string& help,
          const std::vector<std::string>& label_names = {});
    
    void set(double value, const std::map<std::string, std::string>& labels = {});
    void inc(double value = 1.0, const std::map<std::string, std::string>& labels = {});
    void dec(double value = 1.0, const std::map<std::string, std::string>& labels = {});
    
    std::string export_prometheus() const override;
    
private:
    std::string name_;
    std::string help_;
    std::vector<std::string> label_names_;
    std::map<std::map<std::string, std::string>, std::atomic<double>> values_;
    mutable std::mutex mutex_;
};

class Histogram : public MetricCollector {
public:
    Histogram(const std::string& name, const std::string& help,
              const std::vector<double>& buckets = {0.001, 0.005, 0.01, 0.02, 0.05, 0.1, 0.25, 0.5, 1, 2.5, 5, 10},
              const std::vector<std::string>& label_names = {});
    
    void observe(double value, const std::map<std::string, std::string>& labels = {});
    
    std::string export_prometheus() const override;
    
private:
    struct Bucket {
        double upper_bound;
        std::atomic<uint64_t> count{0};
    };
    
    std::string name_;
    std::string help_;
    std::vector<double> buckets_;
    std::vector<std::string> label_names_;
    std::map<std::map<std::string, std::string>, std::vector<Bucket>> buckets_by_label_;
    std::map<std::map<std::string, std::string>, std::atomic<double>> sum_;
    std::map<std::map<std::string, std::string>, std::atomic<uint64_t>> count_;
    mutable std::mutex mutex_;
};

class PrometheusRegistry {
public:
    static PrometheusRegistry& instance();
    
    std::shared_ptr<Counter> register_counter(
        const std::string& name, const std::string& help,
        const std::vector<std::string>& label_names = {});
    
    std::shared_ptr<Gauge> register_gauge(
        const std::string& name, const std::string& help,
        const std::vector<std::string>& label_names = {});
    
    std::shared_ptr<Histogram> register_histogram(
        const std::string& name, const std::string& help,
        const std::vector<double>& buckets = {},
        const std::vector<std::string>& label_names = {});
    
    std::string export_all() const;
    
private:
    PrometheusRegistry() = default;
    
    std::vector<std::shared_ptr<MetricCollector>> collectors_;
    mutable std::mutex mutex_;
};

// 预定义指标
struct CoreMetrics {
    // HTTP 请求
    std::shared_ptr<Counter> http_requests_total;      // method, endpoint, status
    std::shared_ptr<Histogram> http_request_duration_seconds;
    
    // 消息处理
    std::shared_ptr<Counter> messages_total;           // channel, direction (in/out)
    std::shared_ptr<Histogram> message_processing_duration_seconds;
    
    // 工具调用
    std::shared_ptr<Counter> tool_calls_total;         // tool_name, status (success/failure)
    std::shared_ptr<Histogram> tool_call_duration_seconds;
    
    // LLM 调用
    std::shared_ptr<Counter> llm_requests_total;       // model, provider, status
    std::shared_ptr<Histogram> llm_request_duration_seconds;
    std::shared_ptr<Counter> llm_tokens_total;         // model, type (prompt/completion)
    
    // Agent 状态
    std::shared_ptr<Gauge> agents_active;              // agent_id
    std::shared_ptr<Gauge> sessions_active;            // session_id
    
    // 系统资源
    std::shared_ptr<Gauge> process_cpu_percent;
    std::shared_ptr<Gauge> process_memory_bytes;
    std::shared_ptr<Gauge> process_threads;
    std::shared_ptr<Gauge> process_fds;
    
    static CoreMetrics& instance();
    static void init();
    
private:
    CoreMetrics() = default;
};

// ============================================================================
// 3. OpenTelemetry 追踪系统 (Tracing)
// ============================================================================

struct SpanContext {
    std::string trace_id;
    std::string span_id;
    std::optional<std::string> parent_span_id;
    std::map<std::string, std::string> baggage;
};

enum class SpanKind {
    Internal,    // 内部操作
    Server,      // 服务端 (处理请求)
    Client,      // 客户端 (发起请求)
    Producer,    // 消息生产者
    Consumer     // 消息消费者
};

class Span {
public:
    Span(const std::string& name, SpanKind kind = SpanKind::Internal);
    ~Span();
    
    void set_attribute(const std::string& key, const core::JsonValue& value);
    void add_event(const std::string& name, const std::map<std::string, core::JsonValue>& attributes = {});
    void set_status(bool success, const std::string& description = "");
    void end();
    
    const SpanContext& context() const { return context_; }
    
private:
    std::string name_;
    SpanKind kind_;
    SpanContext context_;
    std::chrono::system_clock::time_point start_time_;
    std::optional<std::chrono::system_clock::time_point> end_time_;
    std::map<std::string, core::JsonValue> attributes_;
    std::vector<std::pair<std::string, std::map<std::string, core::JsonValue>>> events_;
    bool success_ = true;
    std::string status_description_;
};

struct TracingConfig {
    bool enabled = false;
    std::string provider = "jaeger";  // jaeger, zipkin, otlp
    std::string endpoint = "http://localhost:14268/api/traces";
    double sample_rate = 0.1;         // 10% 采样
    std::string service_name = "hunterclaw";
    std::map<std::string, std::string> resource_attributes;
};

class TracingExporter {
public:
    virtual ~TracingExporter() = default;
    virtual void export_span(const Span& span) = 0;
    virtual void flush() = 0;
};

class JaegerExporter : public TracingExporter {
public:
    explicit JaegerExporter(const std::string& endpoint);
    void export_span(const Span& span) override;
    void flush() override;
    
private:
    std::string endpoint_;
    std::vector<Span> buffer_;
    std::mutex mutex_;
};

class Tracer {
public:
    static void init(const TracingConfig& config);
    static Tracer& instance();
    
    std::unique_ptr<Span> start_span(const std::string& name, SpanKind kind = SpanKind::Internal);
    std::unique_ptr<Span> start_span_with_parent(
        const std::string& name, const SpanContext& parent, SpanKind kind = SpanKind::Internal);
    
    void export_span(const Span& span);
    void flush();
    
private:
    Tracer() = default;
    
    TracingConfig config_;
    std::unique_ptr<TracingExporter> exporter_;
    std::mutex mutex_;
};

// ============================================================================
// 4. 审计日志与事件流 (Audit Log & Event Streaming)
// ============================================================================

enum class AuditEventType {
    ConfigChange,      // 配置变更
    UserLogin,         // 用户登录
    ToolCall,          // 工具调用
    FileAccess,        // 文件访问
    NetworkRequest,    // 网络请求
    PermissionDenied,  // 权限拒绝
    DataExport         // 数据导出
};

struct AuditEvent {
    AuditEventType type;
    std::string actor;                                  // 操作者 (user_id, agent_id)
    std::string action;                                 // 操作动作
    std::string resource;                               // 资源标识
    std::map<std::string, core::JsonValue> details;     // 详细信息
    std::chrono::system_clock::time_point timestamp;
    std::optional<std::string> ip_address;
    std::optional<std::string> user_agent;
    bool success;
    std::optional<std::string> error_message;
    
    std::string to_json() const;
};

class AuditLogger {
public:
    static AuditLogger& instance();
    
    void log_event(const AuditEvent& event);
    void flush();
    
private:
    AuditLogger() = default;
    
    std::string log_path_;
    std::mutex mutex_;
};

// 便捷宏
#define AUDIT_CONFIG_CHANGE(actor, key, old_val, new_val) \
    AuditLogger::instance().log_event({ \
        AuditEventType::ConfigChange, actor, "config.set", key, \
        {{"old_value", old_val}, {"new_value", new_val}}, \
        std::chrono::system_clock::now(), {}, {}, true, {} \
    })

#define AUDIT_TOOL_CALL(actor, tool_name, args, result, success) \
    AuditLogger::instance().log_event({ \
        AuditEventType::ToolCall, actor, "tool.call", tool_name, \
        {{"args", args}, {"result", result}}, \
        std::chrono::system_clock::now(), {}, {}, success, {} \
    })

}  // namespace observability
}  // namespace hunterclaw
