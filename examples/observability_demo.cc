// HunterClaw 可观测性系统 - 完整示例
// 场景: 用户通过 HTTP API 创建一个 Agent,Agent 执行 Web 搜索工具,然后返回结果

#include "hunterclaw/core/structured_logger.h"
#include "hunterclaw/observability/observability.h"
#include <chrono>
#include <thread>
#include <iostream>

using namespace hunterclaw;
using namespace hunterclaw::core;
using namespace hunterclaw::observability;
using json = nlohmann::json;

// ============================================================================
// 模拟场景: HTTP 请求 → Agent 创建 → 工具调用 → 返回结果
// ============================================================================

// 1️⃣ HTTP Gateway 处理请求
void handle_http_request(const std::string& trace_id) {
    // 启动 HTTP 请求的 Span (服务端类型)
    auto span = Tracer::instance().start_span("http.request", SpanKind::Server);
    span->set_attribute("http.method", "POST");
    span->set_attribute("http.path", "/api/v1/agents");
    span->set_attribute("http.client_ip", "192.168.1.100");
    
    // 设置日志追踪上下文
    StructuredLogger::instance().set_trace_context(trace_id, span->context().span_id);
    
    // 📊 记录指标: HTTP 请求计数
    CoreMetrics::instance().http_requests_total->inc(1.0, {
        {"method", "POST"},
        {"endpoint", "/api/v1/agents"},
        {"status", "200"}
    });
    
    // 📝 记录日志: 收到请求
    StructuredLogger::instance().info("http.request.start", {
        {"method", "POST"},
        {"path", "/api/v1/agents"},
        {"client_ip", "192.168.1.100"},
        {"user_agent", "Mozilla/5.0"}
    });
    
    // 🔐 审计日志: 记录 API 调用
    AuditEvent audit_event{
        .type = AuditEventType::NetworkRequest,
        .actor = "user@example.com",
        .action = "http.request",
        .resource = "/api/v1/agents",
        .details = {{"method", "POST"}},
        .timestamp = std::chrono::system_clock::now(),
        .ip_address = "192.168.1.100",
        .user_agent = "Mozilla/5.0",
        .success = true,
        .error_message = std::nullopt
    };
    AuditLogger::instance().log_event(audit_event);
    
    // 模拟请求处理时间
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    
    // 调用下游: 创建 Agent
    create_agent(trace_id, span->context().span_id);
    
    // 📊 记录指标: HTTP 请求耗时
    CoreMetrics::instance().http_request_duration_seconds->observe(0.125, {
        {"method", "POST"},
        {"endpoint", "/api/v1/agents"}
    });
    
    // 📝 记录日志: 请求完成
    StructuredLogger::instance().info("http.request.end", {
        {"status", 200},
        {"duration_ms", 125}
    });
    
    span->set_status(true, "Request processed successfully");
    span->end();
}

// 2️⃣ Agent Manager 创建 Agent
void create_agent(const std::string& trace_id, const std::string& parent_span_id) {
    // 启动子 Span (内部操作)
    SpanContext parent_ctx{trace_id, parent_span_id, std::nullopt, {}};
    auto span = Tracer::instance().start_span_with_parent("agent.create", parent_ctx, SpanKind::Internal);
    
    // 更新日志上下文
    StructuredLogger::instance().set_trace_context(trace_id, span->context().span_id);
    
    // 📝 记录日志: 开始创建 Agent
    StructuredLogger::instance().info("agent.create.start", {
        {"agent_id", "agent-12345"},
        {"type", "conversational"}
    });
    
    // 模拟 Agent 初始化
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    
    // 📊 记录指标: 活跃 Agent 数量 +1
    CoreMetrics::instance().agents_active->inc(1.0, {{"agent_id", "agent-12345"}});
    
    // 🔍 添加 Span 事件
    span->add_event("agent.initialized", {
        {"agent_id", "agent-12345"},
        {"model", "gpt-4"},
        {"tools", json::array({"web_search", "calculator"})}
    });
    
    // 📝 记录日志: Agent 创建成功
    StructuredLogger::instance().info("agent.create.success", {
        {"agent_id", "agent-12345"},
        {"model", "gpt-4"},
        {"tools_count", 2}
    });
    
    // 调用下游: 执行工具
    execute_tool(trace_id, span->context().span_id, "agent-12345", "web_search");
    
    span->set_status(true, "Agent created successfully");
    span->end();
}

// 3️⃣ Tool Executor 执行工具
void execute_tool(const std::string& trace_id, const std::string& parent_span_id,
                 const std::string& agent_id, const std::string& tool_name) {
    // 启动子 Span (客户端类型,因为要调用外部 API)
    SpanContext parent_ctx{trace_id, parent_span_id, std::nullopt, {}};
    auto span = Tracer::instance().start_span_with_parent("tool.execute", parent_ctx, SpanKind::Client);
    
    span->set_attribute("tool.name", tool_name);
    span->set_attribute("tool.agent_id", agent_id);
    
    // 更新日志上下文
    StructuredLogger::instance().set_trace_context(trace_id, span->context().span_id);
    StructuredLogger::instance().set_agent_context(agent_id);
    
    // 📝 记录日志: 开始执行工具
    StructuredLogger::instance().info("tool.execute.start", {
        {"tool", tool_name},
        {"agent_id", agent_id},
        {"query", "latest AI news"}
    });
    
    // 🔐 审计日志: 记录工具调用
    AuditEvent audit_event{
        .type = AuditEventType::ToolCall,
        .actor = agent_id,
        .action = "tool.execute",
        .resource = tool_name,
        .details = {
            {"query", "latest AI news"},
            {"max_results", 5}
        },
        .timestamp = std::chrono::system_clock::now(),
        .ip_address = std::nullopt,
        .user_agent = std::nullopt,
        .success = true,
        .error_message = std::nullopt
    };
    AuditLogger::instance().log_event(audit_event);
    
    // 📊 记录指标: 工具调用开始
    auto tool_start = std::chrono::steady_clock::now();
    
    // 模拟工具执行 (例如: 调用搜索 API)
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    
    // 🔍 添加 Span 事件: API 调用
    span->add_event("api.call", {
        {"provider", "google_search"},
        {"query", "latest AI news"}
    });
    
    // 模拟随机失败 (10% 概率)
    bool success = (rand() % 10 != 0);
    
    if (success) {
        // ✅ 成功场景
        span->add_event("tool.result", {
            {"results_count", 5},
            {"first_title", "GPT-5 Released"}
        });
        
        // 📝 记录日志: 工具执行成功
        StructuredLogger::instance().info("tool.execute.success", {
            {"tool", tool_name},
            {"results_count", 5},
            {"duration_ms", 50}
        });
        
        // 📊 记录指标: 工具调用成功
        CoreMetrics::instance().tool_calls_total->inc(1.0, {
            {"tool_name", tool_name},
            {"status", "success"}
        });
        
        span->set_status(true, "Tool executed successfully");
        
    } else {
        // ❌ 失败场景
        std::string error_msg = "Search API rate limit exceeded";
        
        span->add_event("tool.error", {
            {"error", error_msg}
        });
        
        // 📝 记录日志: 工具执行失败
        StructuredLogger::instance().error("tool.execute.failure", {
            {"tool", tool_name},
            {"error", error_msg}
        });
        
        // 📊 记录指标: 工具调用失败
        CoreMetrics::instance().tool_calls_total->inc(1.0, {
            {"tool_name", tool_name},
            {"status", "failure"}
        });
        
        span->set_status(false, error_msg);
    }
    
    // 📊 记录指标: 工具调用耗时
    auto tool_end = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(tool_end - tool_start);
    CoreMetrics::instance().tool_call_duration_seconds->observe(duration.count() / 1000.0, {
        {"tool_name", tool_name}
    });
    
    span->end();
}

// 4️⃣ LLM API 调用示例 (带 Token 统计)
void call_llm_api(const std::string& trace_id, const std::string& parent_span_id) {
    SpanContext parent_ctx{trace_id, parent_span_id, std::nullopt, {}};
    auto span = Tracer::instance().start_span_with_parent("llm.request", parent_ctx, SpanKind::Client);
    
    span->set_attribute("llm.model", "gpt-4");
    span->set_attribute("llm.provider", "openai");
    
    StructuredLogger::instance().set_trace_context(trace_id, span->context().span_id);
    
    // 📝 记录日志: LLM 请求
    StructuredLogger::instance().info("llm.request.start", {
        {"model", "gpt-4"},
        {"provider", "openai"},
        {"prompt_length", 150}
    });
    
    // 模拟 LLM API 调用
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    
    // 📊 记录指标: LLM Token 使用
    CoreMetrics::instance().llm_tokens_total->inc(150, {
        {"model", "gpt-4"},
        {"type", "prompt"}
    });
    CoreMetrics::instance().llm_tokens_total->inc(80, {
        {"model", "gpt-4"},
        {"type", "completion"}
    });
    
    // 📊 记录指标: LLM 请求成功
    CoreMetrics::instance().llm_requests_total->inc(1.0, {
        {"model", "gpt-4"},
        {"provider", "openai"},
        {"status", "success"}
    });
    
    // 📊 记录指标: LLM 请求耗时
    CoreMetrics::instance().llm_request_duration_seconds->observe(0.200, {
        {"model", "gpt-4"},
        {"provider", "openai"}
    });
    
    // 📝 记录日志: LLM 响应
    StructuredLogger::instance().info("llm.request.success", {
        {"prompt_tokens", 150},
        {"completion_tokens", 80},
        {"duration_ms", 200}
    });
    
    span->set_status(true);
    span->end();
}

// ============================================================================
// 主程序: 初始化可观测性系统 + 执行示例场景
// ============================================================================

int main() {
    std::cout << "🚀 HunterClaw 可观测性系统 - 完整示例\n" << std::endl;
    std::cout << "场景: HTTP 请求 → 创建 Agent → 执行工具 → 返回结果\n" << std::endl;
    
    // ========================================================================
    // Step 1: 初始化日志系统
    // ========================================================================
    std::cout << "📝 Step 1: 初始化结构化日志系统\n" << std::endl;
    
    LogConfig log_config;
    log_config.level = LogLevel::INFO;
    log_config.format = LogFormat::JSON;  // 生产环境使用 JSON 格式
    log_config.outputs = {
        {LogConfig::Output::Type::Console, std::nullopt, std::nullopt, std::nullopt, "daily"},
        {LogConfig::Output::Type::File, "/tmp/hunterclaw.log", 100, 10, "size"}
    };
    log_config.redact_sensitive = true;  // 自动过滤敏感信息
    log_config.audit.enabled = true;
    log_config.audit.path = "/tmp/hunterclaw-audit.log";
    
    StructuredLogger::init(log_config);
    std::cout << "✅ 日志系统已启动 (JSON 格式, 输出到控制台和文件)" << std::endl;
    std::cout << "   - 日志文件: /tmp/hunterclaw.log" << std::endl;
    std::cout << "   - 审计日志: /tmp/hunterclaw-audit.log\n" << std::endl;
    
    // ========================================================================
    // Step 2: 初始化指标系统
    // ========================================================================
    std::cout << "📊 Step 2: 初始化 Prometheus 指标系统\n" << std::endl;
    
    CoreMetrics::init();
    std::cout << "✅ 指标系统已启动 (预定义 15+ 核心指标)" << std::endl;
    std::cout << "   - HTTP 请求: http_requests_total, http_request_duration_seconds" << std::endl;
    std::cout << "   - 工具调用: tool_calls_total, tool_call_duration_seconds" << std::endl;
    std::cout << "   - LLM 调用: llm_requests_total, llm_tokens_total\n" << std::endl;
    
    // ========================================================================
    // Step 3: 初始化追踪系统
    // ========================================================================
    std::cout << "🔍 Step 3: 初始化 OpenTelemetry 追踪系统\n" << std::endl;
    
    TracingConfig tracing_config;
    tracing_config.enabled = true;
    tracing_config.provider = "jaeger";
    tracing_config.endpoint = "http://localhost:14268/api/traces";
    tracing_config.sample_rate = 1.0;  // 100% 采样 (演示用)
    tracing_config.service_name = "hunterclaw";
    tracing_config.resource_attributes = {
        {"service.version", "1.0.0"},
        {"deployment.environment", "production"}
    };
    
    Tracer::init(tracing_config);
    std::cout << "✅ 追踪系统已启动 (Jaeger, 100% 采样)" << std::endl;
    std::cout << "   - Jaeger 端点: http://localhost:14268/api/traces" << std::endl;
    std::cout << "   - 查看 UI: http://localhost:16686\n" << std::endl;
    
    // ========================================================================
    // Step 4: 执行示例场景 (3 次请求)
    // ========================================================================
    std::cout << "🎬 Step 4: 执行示例场景\n" << std::endl;
    
    for (int i = 1; i <= 3; i++) {
        std::string trace_id = "trace-" + std::to_string(10000 + i);
        
        std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━" << std::endl;
        std::cout << "🔹 请求 #" << i << " (Trace ID: " << trace_id << ")" << std::endl;
        std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n" << std::endl;
        
        handle_http_request(trace_id);
        
        std::cout << "\n✅ 请求 #" << i << " 处理完成\n" << std::endl;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    
    // ========================================================================
    // Step 5: 导出 Prometheus 指标
    // ========================================================================
    std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━" << std::endl;
    std::cout << "📊 Step 5: 导出 Prometheus 指标 (模拟 /metrics 端点)\n" << std::endl;
    
    std::string metrics = PrometheusRegistry::instance().export_all();
    std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━" << std::endl;
    std::cout << "Prometheus Metrics (/metrics):\n" << std::endl;
    std::cout << metrics << std::endl;
    std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n" << std::endl;
    
    // ========================================================================
    // Step 6: 查看日志和审计日志
    // ========================================================================
    std::cout << "📝 Step 6: 查看生成的日志\n" << std::endl;
    std::cout << "运行以下命令查看日志:\n" << std::endl;
    std::cout << "  # 结构化日志 (JSON 格式)\n";
    std::cout << "  $ tail -f /tmp/hunterclaw.log | jq .\n" << std::endl;
    std::cout << "  # 审计日志 (安全事件)\n";
    std::cout << "  $ tail -f /tmp/hunterclaw-audit.log | jq .\n" << std::endl;
    
    // ========================================================================
    // Step 7: 可观测性数据流向总结
    // ========================================================================
    std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━" << std::endl;
    std::cout << "🎯 Step 7: 可观测性数据流向总结\n" << std::endl;
    
    std::cout << R"(
┌─────────────────────────────────────────────────────────────┐
│                  HunterClaw 可观测性架构                     │
└─────────────────────────────────────────────────────────────┘

1️⃣  结构化日志 (Structured Logging)
   ┌──────────────┐
   │ Application  │
   │   Code       │  StructuredLogger::info("event", {...})
   └──────┬───────┘
          │
          ▼
   ┌──────────────────┐
   │ StructuredLogger │  - 自动添加 trace_id/span_id
   │   (JSON)         │  - 过滤敏感信息 (API keys)
   └──────┬───────────┘
          │
          ├─────────────────┐
          │                 │
          ▼                 ▼
   ┌─────────────┐   ┌─────────────┐
   │  File Log   │   │   Console   │
   │  (轮转)     │   │   (实时)    │
   └─────┬───────┘   └─────────────┘
         │
         ▼
   ┌─────────────┐
   │  Filebeat   │  → Elasticsearch → Kibana 查询分析
   └─────────────┘

2️⃣  指标系统 (Prometheus Metrics)
   ┌──────────────┐
   │ Application  │
   │   Code       │  CoreMetrics::http_requests_total->inc()
   └──────┬───────┘
          │
          ▼
   ┌──────────────────┐
   │ PrometheusReg.   │  - Counter: 累加计数
   │  (in-memory)     │  - Gauge: 瞬时值
   │                  │  - Histogram: 分布统计
   └──────┬───────────┘
          │
          ▼
   ┌─────────────┐
   │ /metrics    │  ← Prometheus 定期抓取 (每 15 秒)
   │  Endpoint   │
   └─────┬───────┘
         │
         ▼
   ┌─────────────┐
   │ Prometheus  │  → Grafana 可视化 + 告警
   └─────────────┘

3️⃣  分布式追踪 (OpenTelemetry Tracing)
   ┌──────────────┐
   │ Application  │
   │   Code       │  auto span = Tracer::start_span("operation")
   └──────┬───────┘
          │
          ▼
   ┌──────────────────┐
   │     Tracer       │  - 自动生成 trace_id/span_id
   │  (OpenTelemetry) │  - 记录时间/属性/事件
   └──────┬───────────┘
          │
          ▼
   ┌─────────────┐
   │   Jaeger    │  HTTP POST /api/traces
   │  Collector  │
   └─────┬───────┘
         │
         ▼
   ┌─────────────┐
   │   Jaeger    │  → Jaeger UI 查看调用链
   │     UI      │     (http://localhost:16686)
   └─────────────┘

4️⃣  审计日志 (Audit Log)
   ┌──────────────┐
   │ Application  │
   │   Code       │  AuditLogger::log_event({...})
   └──────┬───────┘
          │
          ▼
   ┌──────────────────┐
   │   AuditLogger    │  - 记录安全事件
   │   (JSON Lines)   │  - 不可删除/不可修改
   └──────┬───────────┘
          │
          ▼
   ┌─────────────┐
   │  Audit Log  │  → SIEM 系统 (安全分析)
   │   File      │     或 S3 长期存储
   └─────────────┘

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

🎯 三个核心问题 (Three Pillars of Observability)

1. What happened? (发生了什么?)
   → 📝 日志: 查看事件流, 搜索错误信息
   
2. How fast? (性能如何?)
   → 📊 指标: 查看 P50/P95/P99 延迟, 吞吐量, 错误率
   
3. Where did it happen? (在哪个环节?)
   → 🔍 追踪: 查看调用链, 定位慢查询, 跨服务追踪

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
)" << std::endl;
    
    // ========================================================================
    // Step 8: 实际生产环境配置示例
    // ========================================================================
    std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━" << std::endl;
    std::cout << "🛠️  Step 8: 生产环境配置示例\n" << std::endl;
    
    std::cout << R"(
# hunterclaw.json (生产环境配置)
{
  "observability": {
    // 结构化日志
    "logging": {
      "level": "info",
      "format": "json",
      "outputs": [
        {
          "type": "file",
          "path": "/var/log/hunterclaw/app.log",
          "max_size_mb": 100,
          "max_files": 30,
          "rotation": "daily"
        }
      ],
      "redact_sensitive": true,
      "audit": {
        "enabled": true,
        "path": "/var/log/hunterclaw/audit.log",
        "log_requests": true,
        "log_tool_calls": true
      }
    },
    
    // 指标系统
    "metrics": {
      "enabled": true,
      "port": 9090,              // Prometheus 抓取端点
      "path": "/metrics"
    },
    
    // 分布式追踪
    "tracing": {
      "enabled": true,
      "provider": "jaeger",
      "endpoint": "http://jaeger-collector:14268/api/traces",
      "sample_rate": 0.1,        // 10% 采样 (生产环境)
      "service_name": "hunterclaw",
      "resource_attributes": {
        "service.version": "1.0.0",
        "deployment.environment": "production"
      }
    }
  }
}

# docker-compose.yml (可观测性栈)
version: '3.8'

services:
  # HunterClaw 主服务
  hunterclaw:
    image: hunterclaw:latest
    ports:
      - "8080:8080"      # HTTP Gateway
      - "9090:9090"      # Prometheus Metrics
    volumes:
      - ./logs:/var/log/hunterclaw
    environment:
      - HUNTERCLAW_CONFIG=/app/hunterclaw.json

  # Prometheus (指标采集)
  prometheus:
    image: prom/prometheus:latest
    ports:
      - "9091:9090"
    volumes:
      - ./prometheus.yml:/etc/prometheus/prometheus.yml
    command:
      - '--config.file=/etc/prometheus/prometheus.yml'

  # Grafana (指标可视化)
  grafana:
    image: grafana/grafana:latest
    ports:
      - "3000:3000"
    environment:
      - GF_SECURITY_ADMIN_PASSWORD=admin
    volumes:
      - ./grafana-dashboards:/etc/grafana/provisioning/dashboards

  # Jaeger (分布式追踪)
  jaeger:
    image: jaegertracing/all-in-one:latest
    ports:
      - "16686:16686"    # Jaeger UI
      - "14268:14268"    # HTTP Collector
    environment:
      - COLLECTOR_ZIPKIN_HOST_PORT=:9411

  # Elasticsearch (日志存储)
  elasticsearch:
    image: docker.elastic.co/elasticsearch/elasticsearch:8.9.0
    environment:
      - discovery.type=single-node
      - xpack.security.enabled=false
    ports:
      - "9200:9200"

  # Kibana (日志查询分析)
  kibana:
    image: docker.elastic.co/kibana/kibana:8.9.0
    ports:
      - "5601:5601"
    environment:
      - ELASTICSEARCH_HOSTS=http://elasticsearch:9200

  # Filebeat (日志采集)
  filebeat:
    image: docker.elastic.co/beats/filebeat:8.9.0
    volumes:
      - ./logs:/var/log/hunterclaw:ro
      - ./filebeat.yml:/usr/share/filebeat/filebeat.yml
    command: filebeat -e -strict.perms=false

# prometheus.yml (Prometheus 配置)
global:
  scrape_interval: 15s

scrape_configs:
  - job_name: 'hunterclaw'
    static_configs:
      - targets: ['hunterclaw:9090']
        labels:
          service: 'hunterclaw'
          environment: 'production'

# filebeat.yml (Filebeat 配置)
filebeat.inputs:
  - type: log
    enabled: true
    paths:
      - /var/log/hunterclaw/app.log
    json.keys_under_root: true
    json.add_error_key: true

output.elasticsearch:
  hosts: ["elasticsearch:9200"]
  index: "hunterclaw-logs-%{+yyyy.MM.dd}"

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
)" << std::endl;
    
    // ========================================================================
    // Step 9: 常见故障排查场景
    // ========================================================================
    std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━" << std::endl;
    std::cout << "🔧 Step 9: 常见故障排查场景\n" << std::endl;
    
    std::cout << R"(
🚨 场景 1: API 响应慢 (P99 延迟突增)

1. Grafana 查看 http_request_duration_seconds P99 曲线
   → 发现某个端点 /api/v1/agents 从 100ms 跳到 500ms

2. Prometheus 查询慢端点:
   histogram_quantile(0.99, rate(http_request_duration_seconds_bucket{endpoint="/api/v1/agents"}[5m]))

3. Kibana 查看日志 (过滤慢请求):
   event:"http.request.end" AND duration_ms:>500

4. Jaeger 查看 Trace:
   → 发现 tool.execute 耗时 450ms (占比 90%)
   → 进一步发现是 web_search 工具调用外部 API 超时

5. 解决方案:
   - 增加工具超时配置
   - 添加缓存层
   - 启用并行工具调用

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

🚨 场景 2: LLM Token 消耗异常

1. Grafana 查看 llm_tokens_total 趋势
   → 发现某个 Agent 消耗了 100K tokens

2. Kibana 查询日志:
   event:"llm.request.start" AND agent_id:"agent-12345"
   → 发现该 Agent 的 Context Window 没有正确清理

3. Prometheus 告警触发:
   rate(llm_tokens_total{type="prompt"}[5m]) > 10000

4. 审计日志查看:
   $ jq '.actor == "agent-12345" and .event == "llm.request"' /tmp/hunterclaw-audit.log
   → 发现该 Agent 在循环调用 LLM

5. 解决方案:
   - 限制单个 Agent 的 Token 配额
   - 添加 Context Window 清理逻辑
   - 启用 Token 使用率告警

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

🚨 场景 3: 安全审计 (未授权工具调用)

1. 审计日志查询 (工具调用失败):
   $ jq '.type == "ToolCall" and .success == false' /tmp/hunterclaw-audit.log

2. 发现未授权的 system.run 调用:
   {
     "type": "ToolCall",
     "actor": "agent-99999",
     "action": "tool.call",
     "resource": "system.run",
     "details": {"command": "rm -rf /"},
     "success": false,
     "error_message": "Permission denied"
   }

3. Kibana 查看相关日志:
   agent_id:"agent-99999" AND event:"tool.execute.start"

4. Jaeger 查看完整调用链:
   → 追踪该 Agent 的创建来源 (HTTP 请求)

5. 解决方案:
   - 封禁恶意 Agent
   - 加强工具权限检查
   - 添加 RBAC 策略

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
)" << std::endl;
    
    std::cout << "✅ 可观测性系统演示完成!\n" << std::endl;
    std::cout << "📚 更多文档: https://hunterclaw.dev/docs/observability\n" << std::endl;
    
    return 0;
}
