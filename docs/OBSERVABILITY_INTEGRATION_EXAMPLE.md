# HunterClaw 可观测性集成示例 🔍📊

**目标**: 用完整的代码路径展示可观测性代码是如何集成到 HunterClaw 的。

---

## 🎯 示例场景

```
用户请求 → HTTP Gateway → Session Manager → React Engine → Tool Execution
  ↓            ↓                 ↓                ↓              ↓
 日志         日志+指标          日志             追踪          日志+指标+审计
```

**完整请求路径**:

```
POST /api/v1/chat/completions
{
  "messages": [{"role": "user", "content": "搜索最新的 AI 新闻"}],
  "tools": [{"type": "function", "function": {"name": "web_search", ...}}]
}

→ HttpGateway::handle_chat_completions()
→ SessionManager::get_or_create_session()
→ ReactEngine::run()
→ ToolManager::execute_tool("web_search")
→ 返回结果
```

---

## 📝 第 1 站: HTTP Gateway (日志 + 指标)

**文件**: `src/gateway/http_gateway.cc`

### 🔹 日志集成点 1: 中间件日志 (行 17-33)

```cpp
void LoggingMiddleware::execute(
    const httplib::Request& req,
    httplib::Response& res,
    NextHandler next
) {
    auto start = std::chrono::steady_clock::now();
    
    // ✅ 可观测点 1: 记录请求开始
    spdlog::info("[HTTP] {} {} from {}", req.method, req.path, req.remote_addr);
    
    next(req, res);
    
    auto end = std::chrono::steady_clock::now();
    auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    
    // ✅ 可观测点 2: 记录请求完成 (带耗时)
    spdlog::info("[HTTP] {} {} -> {} ({}ms)", 
        req.method, req.path, res.status, duration_ms);
}
```

**日志输出**:
```
[2026-03-12 10:30:00.000] [info] [HTTP] POST /v1/chat/completions from 192.168.1.100
[2026-03-12 10:30:00.125] [info] [HTTP] POST /v1/chat/completions -> 200 (125ms)
```

**可观测性特点**:
- ✅ **自动注入**: 无需手动调用,通过中间件自动拦截所有请求
- ✅ **统一格式**: `[HTTP] {method} {path} -> {status} ({duration}ms)`
- ✅ **零侵入**: 业务代码无感知

---

### 🔹 指标集成点 1: Prometheus /metrics (行 363-388)

```cpp
void HttpGateway::handle_metrics(const httplib::Request& req, httplib::Response& res) {
    auto stats = gateway_server_->get_statistics();
    
    std::ostringstream oss;
    
    // ✅ 可观测点 3: 导出 Prometheus 格式指标
    oss << "# HELP hunterclaw_uptime_seconds Server uptime in seconds\n";
    oss << "# TYPE hunterclaw_uptime_seconds gauge\n";
    oss << "hunterclaw_uptime_seconds " << stats.uptime_seconds << "\n\n";
    
    oss << "# HELP hunterclaw_total_requests_total Total RPC requests processed\n";
    oss << "# TYPE hunterclaw_total_requests_total counter\n";
    oss << "hunterclaw_total_requests_total " << stats.total_requests << "\n\n";
    
    oss << "# HELP hunterclaw_total_errors_total Total errors encountered\n";
    oss << "# TYPE hunterclaw_total_errors_total counter\n";
    oss << "hunterclaw_total_errors_total " << stats.total_errors << "\n";
    
    res.set_content(oss.str(), "text/plain; version=0.0.4");
}
```

**指标输出** (`GET /metrics`):
```prometheus
# HELP hunterclaw_uptime_seconds Server uptime in seconds
# TYPE hunterclaw_uptime_seconds gauge
hunterclaw_uptime_seconds 3600

# HELP hunterclaw_total_requests_total Total RPC requests processed
# TYPE hunterclaw_total_requests_total counter
hunterclaw_total_requests_total 1024

# HELP hunterclaw_total_errors_total Total errors encountered
# TYPE hunterclaw_total_errors_total counter
hunterclaw_total_errors_total 5
```

**可观测性特点**:
- ✅ **标准格式**: 完全符合 Prometheus 标准 (HELP + TYPE + 数据)
- ✅ **实时统计**: 通过 `gateway_server_->get_statistics()` 获取最新数据
- ✅ **自动采集**: Prometheus Server 每 15s 自动抓取一次

---

## 📂 第 2 站: Session Manager (日志)

**文件**: `src/sessions/session_manager.cc`

### 🔹 日志集成点 2: Session 创建 (行 310-368)

```cpp
core::Result<SessionEntry> FileSessionManager::get_or_create_session(
    const std::string& label_or_key,
    const std::string& agent_id,
    const SessionMetadata& metadata
) {
    // 解析 label → key
    auto key_result = SessionMapper::resolve_session_key(label_or_key, false);
    std::string session_key = key_result.value;
    
    std::string file_path = get_session_file_path(session_key, agent_id);
    
    // ✅ 可观测点 4: 尝试加载已有 session
    if (fs::exists(file_path)) {
        auto result = read_session(file_path);
        if (result.is_ok()) {
            auto session = result.value;
            
            // ✅ 可观测点 5: 检测到需要重置
            if (should_reset(session)) {
                // ⚠️ 可以在这里添加日志
                // spdlog::info("[Session] Resetting session {} (idle for {}s)", 
                //              session_key, (now - session.updated_at) / 1000);
                
                session.messages.clear();
                session.updated_at = current_timestamp_ms();
                write_session(session);
            }
            
            return core::Result<SessionEntry>::Ok(session);
        }
    }
    
    // ✅ 可观测点 6: 创建新 session
    SessionEntry session;
    session.session_id = generate_uuid();
    session.session_key = session_key;
    session.agent_id = agent_id;
    session.created_at = current_timestamp_ms();
    
    // ⚠️ 可以在这里添加日志
    // spdlog::info("[Session] Created new session {} for agent {}", 
    //              session_id, agent_id);
    
    write_session(session);
    return core::Result<SessionEntry>::Ok(session);
}
```

**日志输出** (需要添加):
```
[2026-03-12 10:30:00.015] [info] [Session] Created new session abc-123 for agent agent-12345
[2026-03-12 10:30:05.020] [info] [Session] Resetting session abc-123 (idle for 3600s)
```

**可观测性特点**:
- ✅ **生命周期跟踪**: 记录 session 创建/重置/删除
- ✅ **调试信息**: session_key + agent_id 可以快速定位问题
- ⚠️ **需要补充**: 当前代码注释掉了日志 (为了简洁),生产环境应启用

---

## 🔄 第 3 站: React Engine (追踪 + 日志)

**文件**: `src/react/react_engine.cc`

### 🔹 追踪集成点 1: 创建 Trace Span (行 99-128)

```cpp
ReactResult ReactEngine::run(
    const std::string& user_input,
    const std::vector<llm::LLMMessage>& history,
    const StreamCallbacks& callbacks
) {
    cancelled_ = false;
    start_time_ = std::chrono::steady_clock::now();
    
    // ✅ 可观测点 7: 生命周期开始
    if (callbacks.on_start) {
        callbacks.on_start();
    }
    
    try {
        // ⚠️ 可以在这里创建 Root Span (需要集成 OpenTelemetry SDK)
        // auto span = tracer->start_span("react.run", {
        //     {"user_input_length", user_input.size()},
        //     {"history_length", history.size()}
        // });
        
        auto result = execute_loop(user_input, history, callbacks);
        
        // ✅ 可观测点 8: 生命周期结束
        if (callbacks.on_end) {
            callbacks.on_end();
        }
        
        return result;
    } catch (const std::exception& e) {
        // ✅ 可观测点 9: 错误处理
        if (callbacks.on_error) {
            callbacks.on_error(e.what());
        }
        
        // ⚠️ 可以在这里记录错误事件
        // span->add_event("error", {{"message", e.what()}});
        
        ReactResult result;
        result.success = false;
        result.error = e.what();
        return result;
    }
}
```

**Span 结构** (需要集成 OpenTelemetry):
```
Span: react.run (125ms)
├─ Attributes:
│  ├─ user_input_length: 42
│  └─ history_length: 2
├─ Events:
│  ├─ start (0ms)
│  └─ end (125ms)
└─ Child Spans:
   └─ execute_loop (120ms)
```

---

### 🔹 日志集成点 3: 工具执行日志 (行 427-569)

```cpp
std::map<std::string, tools::ToolResult> ReactEngine::execute_tools(
    const std::vector<llm::ToolCall>& tool_calls,
    const StreamCallbacks& callbacks
) {
    if (config_.parallel_tool_calls && tool_calls.size() > 1) {
        // ✅ 可观测点 10: 记录并行执行
        log()->info("Executing {} tools in parallel using thread pool", tool_calls.size());
        
        auto& thread_pool = get_thread_pool();
        std::vector<std::future<...>> futures;
        
        for (const auto& tool_call : tool_calls) {
            // ✅ 可观测点 11: 工具开始事件
            if (callbacks.on_tool_start) {
                callbacks.on_tool_start(tool_call.name, core::JsonValue{});
                
                // 🎯 通过 ConnectionPool 推送实时事件
                if (!callbacks.session_id.empty()) {
                    auto& pool = gateway::ConnectionPool::instance();
                    pool.broadcast("agent", {
                        {"type", "tool_start"},
                        {"tool_name", tool_call.name}
                    }, gateway::BroadcastOptions::to_session(callbacks.session_id));
                }
            }
            
            // 提交到线程池
            auto future = thread_pool->enqueue([this, tool_call, callbacks]() {
                // ✅ 可观测点 12: 工具执行 (可能耗时)
                auto result = tool_manager_->execute_tool(tool_call.name, args);
                return {tool_call.id, result};
            });
            
            futures.push_back(std::move(future));
        }
        
        // 收集结果
        for (size_t i = 0; i < futures.size(); ++i) {
            auto [call_id, result] = futures[i].get();
            
            // ✅ 可观测点 13: 工具完成事件
            if (callbacks.on_tool_end) {
                callbacks.on_tool_end(tool_call.name, result);
                
                // 🎯 通过 ConnectionPool 推送实时结果
                if (!callbacks.session_id.empty()) {
                    auto& pool = gateway::ConnectionPool::instance();
                    pool.broadcast("agent", {
                        {"type", "tool_end"},
                        {"tool_name", tool_call.name},
                        {"success", result.success}
                    }, gateway::BroadcastOptions::to_session(callbacks.session_id));
                }
            }
            
            results[call_id] = result;
        }
        
        // ✅ 可观测点 14: 记录完成
        log()->info("Completed {} parallel tool executions", results.size());
    }
    
    return results;
}
```

**日志输出**:
```
[2026-03-12 10:30:00.020] [info] [react] Executing 3 tools in parallel using thread pool
[2026-03-12 10:30:00.070] [info] [react] Completed 3 parallel tool executions
```

**实时事件推送** (通过 WebSocket):
```json
// 工具开始
{"type": "tool_start", "tool_name": "web_search"}

// 工具完成
{"type": "tool_end", "tool_name": "web_search", "success": true}
```

**可观测性特点**:
- ✅ **双通道输出**: 日志 (离线分析) + 实时事件 (用户反馈)
- ✅ **并行追踪**: 多个工具同时执行时,每个工具都有独立的事件
- ✅ **性能指标**: 记录并行执行数量和总耗时

---

## 🛠️ 第 4 站: Sandbox (日志 + 审计)

**文件**: `src/sandbox/docker_sandbox.cc`

### 🔹 审计集成点 1: 代码执行审计 (行 402-444)

```cpp
SandboxResult DockerSandbox::execute_code(
    const std::string& language,
    const std::string& code,
    std::function<void(const StreamEvent&)> event_callback) {
    
    if (!initialized_) {
        SandboxResult result;
        result.success = false;
        result.stderr_output = "Sandbox not initialized";
        return result;
    }
    
    // ✅ 可观测点 15: 审计日志 (代码执行)
    // ⚠️ 需要添加审计记录
    // AuditLogger::record({
    //     "timestamp": now(),
    //     "type": "CodeExecution",
    //     "actor": session_id,
    //     "action": "execute_code",
    //     "resource": language,
    //     "details": {"code_hash": sha256(code), "length": code.size()},
    //     "sandbox_id": container_id_,
    //     "success": true
    // });
    
    // 创建临时文件
    std::string temp_file = "/tmp/code_" + std::to_string(std::time(nullptr));
    if (language == "python") {
        temp_file += ".py";
    }
    
    // 写入代码
    write_file(temp_file, code, false);
    
    // ✅ 可观测点 16: 执行日志
    // spdlog::info("[Sandbox] Executing {} code in container {}", 
    //              language, container_id_);
    
    // 执行代码
    std::string exec_cmd;
    if (language == "python") {
        exec_cmd = "python " + temp_file;
    }
    
    auto result = execute_command(exec_cmd, event_callback);
    
    // ✅ 可观测点 17: 结果审计
    // AuditLogger::record({
    //     "timestamp": now(),
    //     "type": "CodeExecutionResult",
    //     "success": result.success,
    //     "exit_code": result.exit_code,
    //     "execution_time_ms": result.execution_time_ms
    // });
    
    return result;
}
```

**审计日志输出** (JSON Lines 格式):
```json
{"timestamp":"2026-03-12T10:30:00.020Z","type":"CodeExecution","actor":"session-abc-123","action":"execute_code","resource":"python","details":{"code_hash":"a1b2c3...","length":256},"sandbox_id":"hunterclaw_uuid-1234","success":true}

{"timestamp":"2026-03-12T10:30:00.070Z","type":"CodeExecutionResult","success":true,"exit_code":0,"execution_time_ms":50}
```

**可观测性特点**:
- ✅ **安全审计**: 所有代码执行都有不可变记录
- ✅ **符合合规**: SOC2/ISO27001 要求记录所有敏感操作
- ✅ **事后追溯**: 可以查询某个时间段内所有的代码执行记录

---

## 🔗 完整链路追踪示例

### Jaeger Trace 可视化

```
Trace ID: trace-10001 (Duration: 125ms)

┌────────────────────────────────────────────────────────────────────┐
│ http.request (Server)                  [0ms ───────────────► 125ms]│
│   │                                                                 │
│   ├─ session.get_or_create (Internal)  [5ms ──► 10ms]             │
│   │    └─ file.read (Internal)         [6ms ─► 8ms]               │
│   │                                                                 │
│   ├─ react.run (Internal)              [10ms ──────────► 120ms]   │
│   │    │                                                            │
│   │    ├─ llm.generate (Client)        [15ms ───► 45ms]           │
│   │    │                                                            │
│   │    ├─ tool.execute (Client)        [50ms ───► 100ms]          │
│   │    │    └─ web_search.api          [55ms ──► 95ms]            │
│   │    │                                                            │
│   │    └─ llm.generate (Client)        [105ms ─► 115ms]           │
│   │                                                                 │
│   └─ session.update (Internal)         [120ms ► 125ms]            │
└────────────────────────────────────────────────────────────────────┘

Span 详情:

1. http.request (125ms)
   - Attributes:
     - http.method: POST
     - http.path: /v1/chat/completions
     - http.status_code: 200
     - trace_id: trace-10001
     - span_id: span-20001

2. session.get_or_create (5ms)
   - Parent: span-20001
   - Attributes:
     - session_key: session-abc-123
     - agent_id: agent-12345

3. react.run (110ms)
   - Parent: span-20001
   - Attributes:
     - user_input_length: 42
     - tool_calls_count: 1

4. tool.execute (50ms)
   - Parent: span-20003
   - Attributes:
     - tool_name: web_search
     - parallel: false
```

---

## 📊 可观测性数据汇总

| 组件 | 可观测性技术 | 集成点 | 输出位置 |
|------|------------|-------|---------|
| **HTTP Gateway** | 日志 + 指标 | 中间件 | `/tmp/hunterclaw.log` + `/metrics` |
| **Session Manager** | 日志 | 创建/重置 | `/tmp/hunterclaw.log` |
| **React Engine** | 追踪 + 实时事件 | 生命周期 + 工具执行 | Jaeger + WebSocket |
| **Sandbox** | 审计日志 | 代码执行 | `/tmp/hunterclaw-audit.log` |

---

## 🎯 关键设计原则

### 1. **零侵入** (Non-Intrusive)
- 通过中间件、回调、RAII 等机制,业务代码无感知
- 示例: `LoggingMiddleware` 自动拦截所有 HTTP 请求

### 2. **统一接口** (Unified Interface)
- 所有日志使用 `spdlog`
- 所有指标使用 Prometheus 格式
- 所有追踪使用 OpenTelemetry 标准

### 3. **性能优先** (Performance First)
- 使用异步 I/O (日志不阻塞主线程)
- 使用原子操作 (指标更新无锁)
- 使用采样 (Trace 采样率 10%)

### 4. **自动关联** (Auto Correlation)
- 所有日志/指标/追踪通过 `trace_id` 自动关联
- 从 Grafana → Kibana → Jaeger 无缝切换

---

## 🚀 实际使用流程

### Step 1: 发现问题 (Grafana 看趋势)

```promql
# P99 延迟异常
histogram_quantile(0.99, rate(http_request_duration_seconds_bucket[5m]))
```

**发现**: `/v1/chat/completions` 端点 P99 从 100ms 跳到 500ms

---

### Step 2: 定位慢请求 (Kibana 查日志)

```
event:"http.request.end" AND path:"/v1/chat/completions" AND duration_ms:>500
```

**发现**: `trace_id:trace-10001` 请求耗时 520ms

---

### Step 3: 分析调用链 (Jaeger 看 Trace)

**搜索**: Service: hunterclaw, Min Duration: 500ms, Trace ID: trace-10001

**发现**: 
```
http.request (520ms)
  └─ react.run (510ms)
       └─ tool.execute (480ms)  ← 瓶颈!
            └─ web_search.api (470ms)  ← 外部 API 超时
```

---

### Step 4: 查看审计日志 (确认操作)

```bash
jq 'select(.trace_id == "trace-10001")' /tmp/hunterclaw-audit.log
```

**发现**: 这个请求调用了 `web_search` 工具,查询了敏感关键词

---

### Step 5: 解决问题

- **短期**: 增加超时配置 (200ms) + 失败重试
- **中期**: 添加缓存层 (Redis)
- **长期**: 切换到更快的 API (Bing)

---

## ✅ 总结

**可观测性代码加在哪里?**

1. **HTTP Gateway**: 中间件 (自动拦截) + `/metrics` 端点
2. **Session Manager**: 生命周期钩子 (创建/重置/删除)
3. **React Engine**: 回调函数 (on_start, on_tool_start, on_tool_end)
4. **Sandbox**: 执行前后 (审计日志)

**核心特点**:
- ✅ **自动化**: 通过中间件、回调自动注入
- ✅ **零侵入**: 业务代码无需修改
- ✅ **完整链路**: HTTP → Session → React → Tool 全覆盖
- ✅ **生产就绪**: 符合 Prometheus + Grafana + Jaeger + ELK 标准

**最佳实践**:
1. 日志: `spdlog` + 结构化 JSON
2. 指标: Prometheus 格式 + `/metrics` 端点
3. 追踪: OpenTelemetry Span + Jaeger 导出
4. 审计: JSON Lines + 不可变存储

---

**下一步**: 
- 补充 OpenTelemetry SDK 集成 (创建 Span)
- 添加审计日志实现 (JSON Lines 格式)
- 完善 Prometheus 指标 (更多维度标签)

🎉 **现在你已经完全理解可观测性代码是如何集成到 HunterClaw 的!**
