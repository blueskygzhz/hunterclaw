// HunterClaw 可观测性完全对齐示例
// 对齐 OpenClaw 架构: 事件驱动 + 观察者模式

#include "hunterclaw/observability/observability.h"
#include "hunterclaw/observability/diagnostic_events.h"
#include <iostream>

using namespace hunterclaw::observability;

// ============================================================================
// 1. Gateway 集成 (HTTP 网关)
// ============================================================================

void gateway_handle_request(const std::string& method, const std::string& path) {
    // 创建 Span (关键: hunterclaw.* 前缀)
    auto span = Tracer::instance().start_span("webhook.processed", SpanKind::Server);
    span->set_attribute("http.method", method);
    span->set_attribute("http.path", path);
    
    auto start = std::chrono::steady_clock::now();
    
    // 发射 Webhook Received 事件
    emit_diagnostic_event<WebhookReceivedEvent>(
        [&](WebhookReceivedEvent& evt) {
            evt.channel = "http";
            evt.method = method;
            evt.path = path;
            evt.body_size_bytes = 1024;
            evt.remote_ip = "192.168.1.100";
            evt.trace_id = span->context().trace_id;
        }
    );
    
    // 处理请求...
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    
    auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();
    
    // 发射 Webhook Processed 事件
    emit_diagnostic_event<WebhookProcessedEvent>(
        [&](WebhookProcessedEvent& evt) {
            evt.channel = "http";
            evt.duration_ms = duration_ms;
            evt.status_code = 200;
            evt.success = true;
            evt.trace_id = span->context().trace_id;
        }
    );
    
    span->set_status(true);
    span->end();
}

// ============================================================================
// 2. Session Manager 集成
// ============================================================================

void session_state_change(const std::string& session_id,
                          SessionState old_state, SessionState new_state) {
    emit_diagnostic_event<SessionStateEvent>(
        [&](SessionStateEvent& evt) {
            evt.session_id = session_id;
            evt.old_state = old_state;
            evt.new_state = new_state;
            evt.reason = "user_message";
            evt.state_duration_ms = 1500;
        }
    );
}

// ============================================================================
// 3. React Engine 集成 (LLM 调用)
// ============================================================================

void react_llm_call(const std::string& model, const std::string& provider) {
    auto span = Tracer::instance().start_span("model.usage", SpanKind::Client);
    span->set_attribute("hunterclaw.provider", provider);
    span->set_attribute("hunterclaw.model", model);
    
    auto start = std::chrono::steady_clock::now();
    
    // 模拟 LLM 调用...
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    
    auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();
    
    // 发射 Model Usage 事件 (关键指标!)
    emit_diagnostic_event<ModelUsageEvent>(
        [&](ModelUsageEvent& evt) {
            evt.provider = provider;
            evt.model = model;
            evt.prompt_tokens = 500;
            evt.completion_tokens = 300;
            evt.total_tokens = 800;
            evt.cost_usd = 0.024;
            evt.duration_ms = duration_ms;
            evt.cached = false;
            evt.trace_id = span->context().trace_id;
        }
    );
    
    span->set_status(true);
    span->end();
}

// ============================================================================
// 4. 心跳监控
// ============================================================================

void heartbeat_monitor() {
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(30));
        
        emit_diagnostic_event<HeartbeatEvent>(
            [](HeartbeatEvent& evt) {
                evt.active_sessions = 5;
                evt.queued_messages = 2;
                evt.cpu_percent = 45.5;
                evt.memory_mb = 512;
                evt.goroutines = 10;
                evt.version = "1.0.0";
            }
        );
    }
}

// ============================================================================
// Main
// ============================================================================

int main() {
    // 1. 初始化日志 (对齐 OpenClaw: 按日期滚动)
    LogConfig log_config;
    log_config.level = LogLevel::INFO;
    log_config.format = LogFormat::JSON;
    log_config.outputs = {
        {LogConfig::Output::Type::File, "/tmp/hunterclaw/hunterclaw.log", {}, {}, "daily"}
    };
    StructuredLogger::init(log_config);
    
    // 2. 初始化 Metrics
    CoreMetrics::init();
    
    // 3. 初始化 Tracing
    TracingConfig trace_config;
    trace_config.enabled = true;
    trace_config.provider = "jaeger";
    trace_config.endpoint = "http://localhost:14268/api/traces";
    trace_config.sample_rate = 0.1;
    Tracer::init(trace_config);
    
    // 4. 启动 EventBus
    DiagnosticEventBus::instance().start();
    
    // 5. 注册订阅者: EventBus → Logger + Metrics
    init_diagnostic_logging();   // 事件 → 日志
    init_diagnostic_metrics();   // 事件 → Prometheus
    
    std::cout << "🚀 HunterClaw Observability 已启动 (完全对齐 OpenClaw)\n";
    std::cout << "📊 Logs: /tmp/hunterclaw/hunterclaw-2026-03-12.log\n";
    std::cout << "📈 Metrics: http://localhost:9090/metrics\n";
    std::cout << "🔍 Traces: http://localhost:16686\n\n";
    
    // 模拟流量
    gateway_handle_request("POST", "/v1/chat/completions");
    session_state_change("sess-123", SessionState::IDLE, SessionState::PROCESSING);
    react_llm_call("gpt-4", "openai");
    
    // 导出 Prometheus Metrics
    std::string metrics = PrometheusRegistry::instance().export_all();
    std::cout << "\n=== Prometheus Metrics ===\n" << metrics << "\n";
    
    // 等待异步事件处理完成
    std::this_thread::sleep_for(std::chrono::seconds(2));
    
    DiagnosticEventBus::instance().stop();
    
    return 0;
}
