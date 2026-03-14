# HunterClaw 可观测性系统 - 实战示例 🎯

## 场景: 用户通过 API 创建 Agent 并执行工具

### 1. 完整调用链

```
HTTP 请求 → 创建 Agent → 执行工具 (web_search) → 返回结果
  (5ms)      (15ms)         (50ms)                  (125ms total)
```

---

## 2. 日志输出 📝

### 2.1 结构化日志 (JSON 格式)

```json
// 1️⃣ HTTP 请求开始
{
  "timestamp": "2026-03-12T10:30:00.000Z",
  "level": "info",
  "event": "http.request.start",
  "method": "POST",
  "path": "/api/v1/agents",
  "client_ip": "192.168.1.100",
  "trace_id": "trace-10001",
  "span_id": "span-20001"
}

// 2️⃣ Agent 创建开始
{
  "timestamp": "2026-03-12T10:30:00.005Z",
  "level": "info",
  "event": "agent.create.start",
  "agent_id": "agent-12345",
  "type": "conversational",
  "trace_id": "trace-10001",
  "span_id": "span-20002",
  "parent_span_id": "span-20001"
}

// 3️⃣ Agent 创建成功
{
  "timestamp": "2026-03-12T10:30:00.015Z",
  "level": "info",
  "event": "agent.create.success",
  "agent_id": "agent-12345",
  "model": "gpt-4",
  "tools_count": 2,
  "trace_id": "trace-10001",
  "span_id": "span-20002"
}

// 4️⃣ 工具执行开始
{
  "timestamp": "2026-03-12T10:30:00.020Z",
  "level": "info",
  "event": "tool.execute.start",
  "tool": "web_search",
  "agent_id": "agent-12345",
  "query": "latest AI news",
  "trace_id": "trace-10001",
  "span_id": "span-20003",
  "parent_span_id": "span-20002"
}

// 5️⃣ 工具执行成功
{
  "timestamp": "2026-03-12T10:30:00.070Z",
  "level": "info",
  "event": "tool.execute.success",
  "tool": "web_search",
  "results_count": 5,
  "duration_ms": 50,
  "trace_id": "trace-10001",
  "span_id": "span-20003"
}

// 6️⃣ HTTP 请求完成
{
  "timestamp": "2026-03-12T10:30:00.125Z",
  "level": "info",
  "event": "http.request.end",
  "status": 200,
  "duration_ms": 125,
  "trace_id": "trace-10001",
  "span_id": "span-20001"
}
```

### 2.2 Kibana 查询示例

```bash
# 查询所有慢请求 (> 100ms)
event:"http.request.end" AND duration_ms:>100

# 查询特定 Agent 的所有操作
agent_id:"agent-12345"

# 查询工具调用失败
event:"tool.execute.failure"

# 按 Trace ID 查询完整链路
trace_id:"trace-10001"
```

---

## 3. 指标输出 📊

### 3.1 Prometheus /metrics 端点

```
# HTTP 请求总数
http_requests_total{method="POST",endpoint="/api/v1/agents",status="200"} 1

# HTTP 请求耗时分布
http_request_duration_seconds_bucket{method="POST",endpoint="/api/v1/agents",le="0.1"} 0
http_request_duration_seconds_bucket{method="POST",endpoint="/api/v1/agents",le="0.25"} 1
http_request_duration_seconds_sum{method="POST",endpoint="/api/v1/agents"} 0.125
http_request_duration_seconds_count{method="POST",endpoint="/api/v1/agents"} 1

# 工具调用总数
tool_calls_total{tool_name="web_search",status="success"} 1

# 工具调用耗时
tool_call_duration_seconds_sum{tool_name="web_search"} 0.050
tool_call_duration_seconds_count{tool_name="web_search"} 1

# 活跃 Agent 数量
agents_active{agent_id="agent-12345"} 1
```

### 3.2 Grafana Dashboard 示例

#### 面板 1: HTTP 请求 QPS

```promql
rate(http_requests_total[1m])
```

#### 面板 2: P99 延迟

```promql
histogram_quantile(0.99, 
  rate(http_request_duration_seconds_bucket[5m])
)
```

#### 面板 3: 工具调用成功率

```promql
sum(rate(tool_calls_total{status="success"}[5m])) 
/ 
sum(rate(tool_calls_total[5m])) * 100
```

#### 面板 4: 活跃 Agent 数量

```promql
sum(agents_active)
```

---

## 4. 追踪输出 🔍

### 4.1 Jaeger Trace 可视化

```
Trace ID: trace-10001 (Duration: 125ms)

┌────────────────────────────────────────────────────────────┐
│ http.request (Server)                [0ms ──────────► 125ms]│
│   └─ agent.create (Internal)         [5ms ────► 20ms]      │
│        └─ tool.execute (Client)      [20ms ──► 70ms]       │
│             └─ api.call              [25ms ─► 65ms]        │
└────────────────────────────────────────────────────────────┘

Span Details:

- Span 1: http.request
  - Duration: 125ms
  - Attributes:
    - http.method: POST
    - http.path: /api/v1/agents
    - http.status_code: 200
    - http.client_ip: 192.168.1.100

- Span 2: agent.create
  - Duration: 15ms
  - Parent: span-20001
  - Attributes:
    - agent.id: agent-12345
    - agent.model: gpt-4
    - agent.tools_count: 2
  - Events:
    - agent.initialized (10ms)

- Span 3: tool.execute
  - Duration: 50ms
  - Parent: span-20002
  - Attributes:
    - tool.name: web_search
    - tool.query: "latest AI news"
    - tool.results_count: 5
  - Events:
    - api.call (5ms)
    - tool.result (45ms)
```

### 4.2 Jaeger UI 查询

```
Service: hunterclaw
Operation: http.request
Tags: http.method=POST, http.status_code=200
Min Duration: 100ms
```

---

## 5. 审计日志 🔐

### 5.1 审计事件

```json
// API 调用审计
{
  "timestamp": "2026-03-12T10:30:00.000Z",
  "type": "NetworkRequest",
  "actor": "user@example.com",
  "action": "http.request",
  "resource": "/api/v1/agents",
  "details": {"method": "POST"},
  "ip_address": "192.168.1.100",
  "user_agent": "Mozilla/5.0",
  "success": true
}

// 工具调用审计
{
  "timestamp": "2026-03-12T10:30:00.020Z",
  "type": "ToolCall",
  "actor": "agent-12345",
  "action": "tool.execute",
  "resource": "web_search",
  "details": {
    "query": "latest AI news",
    "max_results": 5
  },
  "success": true
}
```

### 5.2 审计日志查询

```bash
# 查询所有工具调用
jq 'select(.type == "ToolCall")' audit.log

# 查询失败的操作
jq 'select(.success == false)' audit.log

# 统计各类事件数量
jq -r '.type' audit.log | sort | uniq -c
```

---

## 6. 故障排查流程 🔧

### 场景: API 响应慢 (P99 > 500ms)

#### Step 1: Grafana 发现异常

```promql
# P99 延迟突增
histogram_quantile(0.99, rate(http_request_duration_seconds_bucket[5m]))
```

**发现**: `/api/v1/agents` 端点 P99 从 100ms 跳到 500ms

#### Step 2: Prometheus 找到慢端点

```promql
# 查询最慢的端点
topk(5, 
  histogram_quantile(0.99, 
    rate(http_request_duration_seconds_bucket[5m])
  ) by (endpoint)
)
```

**结果**: `/api/v1/agents` 端点最慢

#### Step 3: Kibana 查看日志

```bash
# 过滤慢请求
event:"http.request.end" AND endpoint:"/api/v1/agents" AND duration_ms:>500
```

**发现**: 50% 的请求耗时 > 500ms

#### Step 4: Jaeger 查看 Trace

**搜索条件**:
- Service: hunterclaw
- Operation: http.request
- Tags: http.path=/api/v1/agents
- Min Duration: 500ms

**发现**:
```
http.request (520ms)
  └─ agent.create (10ms)
       └─ tool.execute (480ms)  ← 瓶颈!
            └─ api.call (google_search) (470ms)
```

**结论**: `web_search` 工具调用外部 API 超时

#### Step 5: 解决方案

1. **短期**: 增加超时配置 + 失败重试
   ```json
   {
     "tools": {
       "web_search": {
         "timeout_ms": 200,
         "max_retries": 3
       }
     }
   }
   ```

2. **中期**: 添加缓存层
   ```cpp
   // 缓存搜索结果 (5 分钟)
   auto cached_result = cache.get("web_search:" + query);
   if (cached_result) return cached_result;
   ```

3. **长期**: 切换到更快的搜索 API 或自建索引

---

## 7. 运行示例程序

### 7.1 编译

```bash
cd /data/workspace/QQMail/HunterClaw
mkdir build && cd build
cmake ..
make observability_demo
```

### 7.2 启动可观测性栈 (Docker Compose)

```bash
# 启动 Prometheus + Grafana + Jaeger
docker-compose -f docker/observability-stack.yml up -d
```

### 7.3 运行示例

```bash
./build/examples/observability_demo
```

### 7.4 查看结果

- **Grafana**: http://localhost:3000 (admin/admin)
- **Jaeger UI**: http://localhost:16686
- **Prometheus**: http://localhost:9091
- **日志**: `/tmp/hunterclaw.log`
- **审计日志**: `/tmp/hunterclaw-audit.log`

---

## 8. 核心收益 ✨

### 8.1 日志 📝
- ✅ 快速定位错误 (Kibana 全文检索)
- ✅ 完整上下文 (trace_id 自动关联)
- ✅ 安全合规 (审计日志不可变)

### 8.2 指标 📊
- ✅ 实时监控 (Grafana Dashboard)
- ✅ 自动告警 (Prometheus Alertmanager)
- ✅ 容量规划 (历史趋势分析)

### 8.3 追踪 🔍
- ✅ 精准定位瓶颈 (Jaeger 火焰图)
- ✅ 跨服务追踪 (分布式系统)
- ✅ 性能优化 (P99 延迟分析)

---

## 9. 最佳实践 🎯

1. **日志命名规范**: `模块.操作.状态` (如 `http.request.start`)
2. **指标命名规范**: `子系统_操作_单位` (如 `http_request_duration_seconds`)
3. **Trace 采样**: 生产环境 10% 采样,开发环境 100%
4. **敏感信息**: 自动脱敏 API keys / passwords
5. **日志轮转**: 每日轮转,保留 30 天
6. **告警阈值**: P99 > 500ms, 错误率 > 1%, QPS 突降 50%

---

**🎉 现在你已经掌握了 HunterClaw 完整的可观测性系统!**

**📚 更多文档**: https://hunterclaw.dev/docs/observability
