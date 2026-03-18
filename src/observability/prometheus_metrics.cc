#include "hunterclaw/observability/observability.h"
#include "hunterclaw/observability/diagnostic_events.h"
#include <sstream>
#include <iomanip>
#include <algorithm>

namespace hunterclaw {
namespace observability {

// ============================================================================
// 完全对齐 OpenClaw - Prometheus 指标系统
// 参考: openclaw-analysis/docs/logging.md 行 268-307
// ============================================================================

// ============================================================================
// Counter 实现
// ============================================================================

Counter::Counter(const std::string& name, const std::string& help,
                 const std::vector<std::string>& label_names)
    : name_(name), help_(help), label_names_(label_names) {}

void Counter::inc(double value, const std::map<std::string, std::string>& labels) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto& atomic_val = values_[labels];
    atomic_val.store(atomic_val.load() + value);
}

std::string Counter::export_prometheus() const {
    std::ostringstream oss;
    
    // HELP 行
    oss << "# HELP " << name_ << " " << help_ << "\n";
    // TYPE 行
    oss << "# TYPE " << name_ << " counter\n";
    
    // 数据行
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& [labels, value] : values_) {
        oss << name_;
        
        // 标签
        if (!labels.empty()) {
            oss << "{";
            bool first = true;
            for (const auto& [key, val] : labels) {
                if (!first) oss << ",";
                oss << key << "=\"" << val << "\"";
                first = false;
            }
            oss << "}";
        }
        
        oss << " " << value.load() << "\n";
    }
    
    return oss.str();
}

// ============================================================================
// Gauge 实现
// ============================================================================

Gauge::Gauge(const std::string& name, const std::string& help,
             const std::vector<std::string>& label_names)
    : name_(name), help_(help), label_names_(label_names) {}

void Gauge::set(double value, const std::map<std::string, std::string>& labels) {
    std::lock_guard<std::mutex> lock(mutex_);
    values_[labels] = value;
}

void Gauge::inc(double value, const std::map<std::string, std::string>& labels) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto& atomic_val = values_[labels];
    atomic_val.store(atomic_val.load() + value);
}

void Gauge::dec(double value, const std::map<std::string, std::string>& labels) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto& atomic_val = values_[labels];
    atomic_val.store(atomic_val.load() - value);
}

std::string Gauge::export_prometheus() const {
    std::ostringstream oss;
    
    oss << "# HELP " << name_ << " " << help_ << "\n";
    oss << "# TYPE " << name_ << " gauge\n";
    
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& [labels, value] : values_) {
        oss << name_;
        
        if (!labels.empty()) {
            oss << "{";
            bool first = true;
            for (const auto& [key, val] : labels) {
                if (!first) oss << ",";
                oss << key << "=\"" << val << "\"";
                first = false;
            }
            oss << "}";
        }
        
        oss << " " << value.load() << "\n";
    }
    
    return oss.str();
}

// ============================================================================
// Histogram 实现
// ============================================================================

Histogram::Histogram(const std::string& name, const std::string& help,
                     const std::vector<double>& buckets,
                     const std::vector<std::string>& label_names)
    : name_(name), help_(help), buckets_(buckets), label_names_(label_names) {}

void Histogram::observe(double value, const std::map<std::string, std::string>& labels) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    // 初始化 bucket (如果不存在)
    if (buckets_by_label_.find(labels) == buckets_by_label_.end()) {
        std::vector<Bucket> bs;
        bs.reserve(buckets_.size());
        for (double bound : buckets_) {
            Bucket b;
            b.upper_bound = bound;
            b.count.store(0);
            bs.push_back(std::move(b));
        }
        buckets_by_label_[labels] = std::move(bs);
    }
    
    // 更新 bucket 计数
    for (auto& bucket : buckets_by_label_[labels]) {
        if (value <= bucket.upper_bound) {
            bucket.count++;
        }
    }
    
    // 更新 sum 和 count
    auto& sum_val = sum_[labels];
    sum_val.store(sum_val.load() + value);
    count_[labels]++;
}

std::string Histogram::export_prometheus() const {
    std::ostringstream oss;
    
    oss << "# HELP " << name_ << " " << help_ << "\n";
    oss << "# TYPE " << name_ << " histogram\n";
    
    std::lock_guard<std::mutex> lock(mutex_);
    
    for (const auto& [labels, buckets] : buckets_by_label_) {
        // 导出 bucket
        for (const auto& bucket : buckets) {
            oss << name_ << "_bucket{";
            
            bool first = true;
            for (const auto& [key, val] : labels) {
                if (!first) oss << ",";
                oss << key << "=\"" << val << "\"";
                first = false;
            }
            
            if (!labels.empty()) oss << ",";
            oss << "le=\"" << bucket.upper_bound << "\"} " << bucket.count.load() << "\n";
        }
        
        // +Inf bucket
        oss << name_ << "_bucket{";
        bool first = true;
        for (const auto& [key, val] : labels) {
            if (!first) oss << ",";
            oss << key << "=\"" << val << "\"";
            first = false;
        }
        if (!labels.empty()) oss << ",";
        oss << "le=\"+Inf\"} " << count_.at(labels).load() << "\n";
        
        // sum
        oss << name_ << "_sum{";
        first = true;
        for (const auto& [key, val] : labels) {
            if (!first) oss << ",";
            oss << key << "=\"" << val << "\"";
            first = false;
        }
        oss << "} " << sum_.at(labels).load() << "\n";
        
        // count
        oss << name_ << "_count{";
        first = true;
        for (const auto& [key, val] : labels) {
            if (!first) oss << ",";
            oss << key << "=\"" << val << "\"";
            first = false;
        }
        oss << "} " << count_.at(labels).load() << "\n";
    }
    
    return oss.str();
}

// ============================================================================
// PrometheusRegistry
// ============================================================================

PrometheusRegistry& PrometheusRegistry::instance() {
    static PrometheusRegistry instance;
    return instance;
}

std::shared_ptr<Counter> PrometheusRegistry::register_counter(
    const std::string& name, const std::string& help,
    const std::vector<std::string>& label_names) {
    
    std::lock_guard<std::mutex> lock(mutex_);
    auto counter = std::make_shared<Counter>(name, help, label_names);
    collectors_.push_back(counter);
    return counter;
}

std::shared_ptr<Gauge> PrometheusRegistry::register_gauge(
    const std::string& name, const std::string& help,
    const std::vector<std::string>& label_names) {
    
    std::lock_guard<std::mutex> lock(mutex_);
    auto gauge = std::make_shared<Gauge>(name, help, label_names);
    collectors_.push_back(gauge);
    return gauge;
}

std::shared_ptr<Histogram> PrometheusRegistry::register_histogram(
    const std::string& name, const std::string& help,
    const std::vector<double>& buckets,
    const std::vector<std::string>& label_names) {
    
    std::lock_guard<std::mutex> lock(mutex_);
    
    std::vector<double> final_buckets = buckets.empty() 
        ? std::vector<double>{0.001, 0.005, 0.01, 0.02, 0.05, 0.1, 0.25, 0.5, 1, 2.5, 5, 10}
        : buckets;
    
    auto histogram = std::make_shared<Histogram>(name, help, final_buckets, label_names);
    collectors_.push_back(histogram);
    return histogram;
}

std::string PrometheusRegistry::export_all() const {
    std::ostringstream oss;
    
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& collector : collectors_) {
        oss << collector->export_prometheus() << "\n";
    }
    
    return oss.str();
}

// ============================================================================
// CoreMetrics - 对齐 OpenClaw 11 个指标
// ============================================================================

CoreMetrics& CoreMetrics::instance() {
    static CoreMetrics instance;
    return instance;
}

void CoreMetrics::init() {
    auto& metrics = instance();
    auto& registry = PrometheusRegistry::instance();
    
    // 1. HTTP 请求 (对齐: openclaw.webhook.*)
    metrics.http_requests_total = registry.register_counter(
        "hunterclaw_http_requests_total",
        "Total HTTP requests received",
        {"method", "endpoint", "status"}
    );
    
    metrics.http_request_duration_seconds = registry.register_histogram(
        "hunterclaw_http_request_duration_seconds",
        "HTTP request duration in seconds",
        {0.001, 0.005, 0.01, 0.02, 0.05, 0.1, 0.25, 0.5, 1, 2, 5},
        {"method", "endpoint"}
    );
    
    // 2. 消息处理 (对齐: openclaw.message.*)
    metrics.messages_total = registry.register_counter(
        "hunterclaw_messages_total",
        "Total messages processed",
        {"channel", "direction"}  // direction: in/out
    );
    
    metrics.message_processing_duration_seconds = registry.register_histogram(
        "hunterclaw_message_processing_duration_seconds",
        "Message processing duration in seconds",
        {},
        {"channel"}
    );
    
    // 3. 工具调用
    metrics.tool_calls_total = registry.register_counter(
        "hunterclaw_tool_calls_total",
        "Total tool calls",
        {"tool_name", "status"}  // status: success/failure
    );
    
    metrics.tool_call_duration_seconds = registry.register_histogram(
        "hunterclaw_tool_call_duration_seconds",
        "Tool call duration in seconds",
        {},
        {"tool_name"}
    );
    
    // 4. LLM 调用 (对齐: openclaw.tokens, openclaw.cost.usd)
    metrics.llm_requests_total = registry.register_counter(
        "hunterclaw_llm_requests_total",
        "Total LLM requests",
        {"model", "provider", "status"}
    );
    
    metrics.llm_request_duration_seconds = registry.register_histogram(
        "hunterclaw_llm_request_duration_seconds",
        "LLM request duration in seconds",
        {},
        {"model", "provider"}
    );
    
    metrics.llm_tokens_total = registry.register_counter(
        "hunterclaw_llm_tokens_total",
        "Total LLM tokens used",
        {"model", "type"}  // type: prompt/completion
    );
    
    // 5. Agent 状态 (对齐: openclaw.session.state)
    metrics.agents_active = registry.register_gauge(
        "hunterclaw_agents_active",
        "Number of active agents",
        {"agent_id"}
    );
    
    metrics.sessions_active = registry.register_gauge(
        "hunterclaw_sessions_active",
        "Number of active sessions",
        {"state"}  // idle/processing/waiting
    );
    
    // 6. 系统资源
    metrics.process_cpu_percent = registry.register_gauge(
        "hunterclaw_process_cpu_percent",
        "Process CPU usage percentage",
        {}
    );
    
    metrics.process_memory_bytes = registry.register_gauge(
        "hunterclaw_process_memory_bytes",
        "Process memory usage in bytes",
        {}
    );
    
    metrics.process_threads = registry.register_gauge(
        "hunterclaw_process_threads",
        "Number of process threads",
        {}
    );
    
    metrics.process_fds = registry.register_gauge(
        "hunterclaw_process_fds",
        "Number of open file descriptors",
        {}
    );
}

// ============================================================================
// 集成 DiagnosticEventBus → Prometheus Metrics
// ============================================================================

void init_diagnostic_metrics() {
    auto& metrics = CoreMetrics::instance();
    auto& bus = DiagnosticEventBus::instance();
    
    // 1. Model Usage → LLM Metrics
    bus.subscribe(DiagnosticEventType::MODEL_USAGE, 
        [&metrics](const std::shared_ptr<DiagnosticEvent>& event) {
            auto evt = std::static_pointer_cast<ModelUsageEvent>(event);
            
            metrics.llm_requests_total->inc(1, {
                {"model", evt->model},
                {"provider", evt->provider},
                {"status", "success"}
            });
            
            metrics.llm_request_duration_seconds->observe(
                evt->duration_ms / 1000.0,
                {{"model", evt->model}, {"provider", evt->provider}}
            );
            
            metrics.llm_tokens_total->inc(evt->prompt_tokens, {
                {"model", evt->model},
                {"type", "prompt"}
            });
            
            metrics.llm_tokens_total->inc(evt->completion_tokens, {
                {"model", evt->model},
                {"type", "completion"}
            });
        });
    
    // 2. Webhook → HTTP Metrics
    bus.subscribe(DiagnosticEventType::WEBHOOK_PROCESSED,
        [&metrics](const std::shared_ptr<DiagnosticEvent>& event) {
            auto evt = std::static_pointer_cast<WebhookProcessedEvent>(event);
            
            metrics.http_requests_total->inc(1, {
                {"method", "POST"},
                {"endpoint", evt->channel},
                {"status", std::to_string(evt->status_code)}
            });
            
            metrics.http_request_duration_seconds->observe(
                evt->duration_ms / 1000.0,
                {{"method", "POST"}, {"endpoint", evt->channel}}
            );
        });
    
    // 3. Message → Message Metrics
    bus.subscribe(DiagnosticEventType::MESSAGE_PROCESSED,
        [&metrics](const std::shared_ptr<DiagnosticEvent>& event) {
            auto evt = std::static_pointer_cast<MessageProcessedEvent>(event);
            
            metrics.messages_total->inc(1, {
                {"channel", evt->channel},
                {"direction", "in"}
            });
            
            metrics.message_processing_duration_seconds->observe(
                evt->duration_ms / 1000.0,
                {{"channel", evt->channel}}
            );
        });
}

}  // namespace observability
}  // namespace hunterclaw
