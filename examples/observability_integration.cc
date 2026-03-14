/**
 * HunterClaw 可观测性集成示例
 * 
 * 目标: 展示如何在实际代码中集成日志/指标/追踪/审计
 * 场景: 完整的 HTTP 请求处理流程
 */

#include <iostream>
#include <chrono>
#include <map>
#include <string>
#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

// ============================================================================
// 第 1 层: HTTP Gateway (日志 + 指标)
// ============================================================================

namespace gateway {

// ✅ 可观测点 1: 中间件自动记录所有请求
class LoggingMiddleware {
public:
    void execute(const std::string& method, const std::string& path, 
                 const std::string& client_ip,
                 std::function<int()> next) {
        auto start = std::chrono::steady_clock::now();
        
        // 📝 日志 1: 请求开始
        spdlog::info("[HTTP] {} {} from {}", method, path, client_ip);
        
        // 调用下一个处理器
        int status = next();
        
        auto end = std::chrono::steady_clock::now();
        auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            end - start
        ).count();
        
        // 📝 日志 2: 请求完成 (带耗时和状态码)
        spdlog::info("[HTTP] {} {} -> {} ({}ms)", method, path, status, duration_ms);
        
        // 📊 指标 1: 更新 Prometheus 指标 (伪代码)
        // CoreMetrics::http_requests_total
        //     ->labels({{"method", method}, {"path", path}, {"status", status}})
        //     ->inc();
        // 
        // CoreMetrics::http_request_duration_seconds
        //     ->labels({{"method", method}, {"path", path}})
        //     ->observe(duration_ms / 1000.0);
    }
};

// ✅ 可观测点 2: Prometheus /metrics 端点
class MetricsHandler {
public:
    std::string handle_metrics() {
        // 📊 指标 2: 导出 Prometheus 格式
        std::ostringstream oss;
        
        oss << "# HELP hunterclaw_http_requests_total Total HTTP requests\n";
        oss << "# TYPE hunterclaw_http_requests_total counter\n";
        oss << "hunterclaw_http_requests_total{method=\"POST\",path=\"/v1/chat/completions\",status=\"200\"} 1024\n\n";
        
        oss << "# HELP hunterclaw_http_request_duration_seconds HTTP request duration\n";
        oss << "# TYPE hunterclaw_http_request_duration_seconds histogram\n";
        oss << "hunterclaw_http_request_duration_seconds_bucket{method=\"POST\",le=\"0.1\"} 500\n";
        oss << "hunterclaw_http_request_duration_seconds_bucket{method=\"POST\",le=\"0.25\"} 800\n";
        oss << "hunterclaw_http_request_duration_seconds_bucket{method=\"POST\",le=\"0.5\"} 950\n";
        oss << "hunterclaw_http_request_duration_seconds_bucket{method=\"POST\",le=\"+Inf\"} 1024\n";
        oss << "hunterclaw_http_request_duration_seconds_sum{method=\"POST\"} 102.4\n";
        oss << "hunterclaw_http_request_duration_seconds_count{method=\"POST\"} 1024\n";
        
        return oss.str();
    }
};

} // namespace gateway

// ============================================================================
// 第 2 层: Session Manager (日志)
// ============================================================================

namespace sessions {

struct SessionEntry {
    std::string session_id;
    std::string session_key;
    std::string agent_id;
    uint64_t created_at;
    uint64_t updated_at;
};

class SessionManager {
public:
    SessionEntry get_or_create_session(const std::string& session_key, 
                                       const std::string& agent_id) {
        // 尝试加载已有 session
        auto session = load_session(session_key);
        
        if (session) {
            // 📝 日志 3: 加载已有 session
            spdlog::info("[Session] Loaded existing session {} for agent {}", 
                        session_key, agent_id);
            
            // 检查是否需要重置
            if (should_reset(*session)) {
                // 📝 日志 4: 重置 session
                spdlog::info("[Session] Resetting session {} (idle for {}s)", 
                            session_key, 
                            (current_timestamp_ms() - session->updated_at) / 1000);
                
                session->updated_at = current_timestamp_ms();
                save_session(*session);
            }
            
            return *session;
        }
        
        // 创建新 session
        SessionEntry new_session;
        new_session.session_id = generate_uuid();
        new_session.session_key = session_key;
        new_session.agent_id = agent_id;
        new_session.created_at = current_timestamp_ms();
        new_session.updated_at = new_session.created_at;
        
        // 📝 日志 5: 创建新 session
        spdlog::info("[Session] Created new session {} for agent {}", 
                    new_session.session_id, agent_id);
        
        save_session(new_session);
        
        // 📊 指标 3: 更新 session 计数器 (伪代码)
        // CoreMetrics::sessions_total->inc();
        // CoreMetrics::sessions_active->labels({{"agent_id", agent_id}})->set(1);
        
        return new_session;
    }

private:
    std::optional<SessionEntry> load_session(const std::string& key) { 
        return std::nullopt; 
    }
    
    void save_session(const SessionEntry& session) {}
    
    bool should_reset(const SessionEntry& session) { 
        return false; 
    }
    
    std::string generate_uuid() { 
        return "session-uuid-12345"; 
    }
    
    uint64_t current_timestamp_ms() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count();
    }
};

} // namespace sessions

// ============================================================================
// 第 3 层: React Engine (追踪 + 日志)
// ============================================================================

namespace react {

struct ToolCall {
    std::string id;
    std::string name;
    std::map<std::string, std::string> args;
};

struct ToolResult {
    bool success;
    std::string output;
    std::string error;
    int64_t execution_time_ms;
};

// ✅ 可观测点 3: Trace Span 管理
class TraceSpan {
public:
    TraceSpan(const std::string& name, const std::string& trace_id) 
        : name_(name), trace_id_(trace_id), start_(std::chrono::steady_clock::now()) {
        
        // 🔍 追踪 1: 创建 Span
        spdlog::debug("[Trace] Start span '{}' (trace_id: {})", name, trace_id);
    }
    
    ~TraceSpan() {
        auto end = std::chrono::steady_clock::now();
        auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            end - start_
        ).count();
        
        // 🔍 追踪 2: 完成 Span
        spdlog::debug("[Trace] End span '{}' ({}ms)", name_, duration_ms);
        
        // 📊 指标 4: 记录 Span 耗时 (伪代码)
        // CoreMetrics::span_duration_seconds
        //     ->labels({{"span_name", name_}})
        //     ->observe(duration_ms / 1000.0);
    }
    
    void add_event(const std::string& event, const json& attributes = {}) {
        // 🔍 追踪 3: 记录事件
        spdlog::debug("[Trace] Span '{}' event: {} {}", name_, event, attributes.dump());
    }

private:
    std::string name_;
    std::string trace_id_;
    std::chrono::steady_clock::time_point start_;
};

class ReactEngine {
public:
    void run(const std::string& user_input, const std::string& trace_id) {
        // 🔍 追踪 4: 创建 Root Span (RAII 自动管理生命周期)
        TraceSpan span("react.run", trace_id);
        span.add_event("start", {{"user_input_length", user_input.size()}});
        
        // 📝 日志 6: 开始执行
        spdlog::info("[React] Starting execution (trace_id: {})", trace_id);
        
        try {
            // 步骤 1: LLM 推理
            auto llm_response = call_llm(user_input, trace_id);
            
            // 步骤 2: 执行工具
            if (!llm_response.tool_calls.empty()) {
                auto tool_results = execute_tools(llm_response.tool_calls, trace_id);
                
                // 📝 日志 7: 工具执行完成
                spdlog::info("[React] Executed {} tools (trace_id: {})", 
                            tool_results.size(), trace_id);
            }
            
            span.add_event("end", {{"success", true}});
            
        } catch (const std::exception& e) {
            // 📝 日志 8: 错误处理
            spdlog::error("[React] Execution failed: {} (trace_id: {})", 
                         e.what(), trace_id);
            
            span.add_event("error", {{"message", e.what()}});
            
            // 📊 指标 5: 错误计数 (伪代码)
            // CoreMetrics::react_errors_total->inc();
        }
    }

private:
    struct LLMResponse {
        std::string text;
        std::vector<ToolCall> tool_calls;
    };
    
    LLMResponse call_llm(const std::string& input, const std::string& trace_id) {
        // 🔍 追踪 5: 子 Span
        TraceSpan span("llm.generate", trace_id);
        
        // 模拟 LLM 调用
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        
        LLMResponse response;
        response.text = "Let me search for that...";
        response.tool_calls = {{"call-1", "web_search", {{"query", "AI news"}}}};
        
        return response;
    }
    
    std::map<std::string, ToolResult> execute_tools(
        const std::vector<ToolCall>& tool_calls,
        const std::string& trace_id) {
        
        std::map<std::string, ToolResult> results;
        
        // 📝 日志 9: 开始执行工具
        spdlog::info("[React] Executing {} tools in parallel (trace_id: {})", 
                    tool_calls.size(), trace_id);
        
        for (const auto& tool_call : tool_calls) {
            // 🔍 追踪 6: 工具执行 Span
            TraceSpan span("tool.execute", trace_id);
            span.add_event("tool_start", {{"tool_name", tool_call.name}});
            
            // 📝 日志 10: 工具开始
            spdlog::info("[React] Tool '{}' started (call_id: {}, trace_id: {})", 
                        tool_call.name, tool_call.id, trace_id);
            
            auto start = std::chrono::steady_clock::now();
            
            // 模拟工具执行
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            
            auto end = std::chrono::steady_clock::now();
            auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                end - start
            ).count();
            
            ToolResult result;
            result.success = true;
            result.output = "Search results: ...";
            result.execution_time_ms = duration_ms;
            
            // 📝 日志 11: 工具完成
            spdlog::info("[React] Tool '{}' completed in {}ms (call_id: {}, trace_id: {})", 
                        tool_call.name, duration_ms, tool_call.id, trace_id);
            
            span.add_event("tool_end", {
                {"success", result.success},
                {"duration_ms", duration_ms}
            });
            
            // 📊 指标 6: 工具调用计数和耗时 (伪代码)
            // CoreMetrics::tool_calls_total
            //     ->labels({{"tool_name", tool_call.name}, {"status", "success"}})
            //     ->inc();
            // 
            // CoreMetrics::tool_call_duration_seconds
            //     ->labels({{"tool_name", tool_call.name}})
            //     ->observe(duration_ms / 1000.0);
            
            results[tool_call.id] = result;
        }
        
        return results;
    }
};

} // namespace react

// ============================================================================
// 第 4 层: Sandbox (审计日志)
// ============================================================================

namespace sandbox {

struct AuditEvent {
    std::string timestamp;
    std::string type;
    std::string actor;
    std::string action;
    std::string resource;
    json details;
    bool success;
    std::string trace_id;
};

// ✅ 可观测点 4: 审计日志记录器
class AuditLogger {
public:
    static void record(const AuditEvent& event) {
        // 🔐 审计 1: 记录审计事件 (JSON Lines 格式)
        json j;
        j["timestamp"] = event.timestamp;
        j["type"] = event.type;
        j["actor"] = event.actor;
        j["action"] = event.action;
        j["resource"] = event.resource;
        j["details"] = event.details;
        j["success"] = event.success;
        j["trace_id"] = event.trace_id;
        
        // 写入审计日志文件 (追加模式, 不可变)
        // std::ofstream audit_log("/tmp/hunterclaw-audit.log", std::ios::app);
        // audit_log << j.dump() << "\n";
        
        spdlog::info("[Audit] {}", j.dump());
    }
};

class Sandbox {
public:
    void execute_code(const std::string& language, 
                     const std::string& code,
                     const std::string& session_id,
                     const std::string& trace_id) {
        
        // 🔐 审计 2: 代码执行审计 (事前)
        AuditEvent pre_event;
        pre_event.timestamp = current_iso_timestamp();
        pre_event.type = "CodeExecution";
        pre_event.actor = session_id;
        pre_event.action = "execute_code";
        pre_event.resource = language;
        pre_event.details = {
            {"code_hash", sha256(code)},
            {"code_length", code.size()},
            {"sandbox_id", "sandbox-uuid-1234"}
        };
        pre_event.success = true;
        pre_event.trace_id = trace_id;
        
        AuditLogger::record(pre_event);
        
        // 📝 日志 12: 代码执行开始
        spdlog::info("[Sandbox] Executing {} code (session: {}, trace_id: {})", 
                    language, session_id, trace_id);
        
        auto start = std::chrono::steady_clock::now();
        
        // 模拟代码执行
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        bool success = true;
        int exit_code = 0;
        
        auto end = std::chrono::steady_clock::now();
        auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            end - start
        ).count();
        
        // 📝 日志 13: 代码执行完成
        spdlog::info("[Sandbox] Code execution completed in {}ms (exit_code: {}, trace_id: {})", 
                    duration_ms, exit_code, trace_id);
        
        // 🔐 审计 3: 代码执行审计 (事后)
        AuditEvent post_event;
        post_event.timestamp = current_iso_timestamp();
        post_event.type = "CodeExecutionResult";
        post_event.actor = session_id;
        post_event.action = "code_result";
        post_event.resource = language;
        post_event.details = {
            {"success", success},
            {"exit_code", exit_code},
            {"execution_time_ms", duration_ms}
        };
        post_event.success = success;
        post_event.trace_id = trace_id;
        
        AuditLogger::record(post_event);
        
        // 📊 指标 7: 代码执行计数和耗时 (伪代码)
        // CoreMetrics::code_executions_total
        //     ->labels({{"language", language}, {"status", success ? "success" : "failure"}})
        //     ->inc();
        // 
        // CoreMetrics::code_execution_duration_seconds
        //     ->labels({{"language", language}})
        //     ->observe(duration_ms / 1000.0);
    }

private:
    std::string sha256(const std::string& data) {
        // 简化实现
        return "a1b2c3d4e5f6...";
    }
    
    std::string current_iso_timestamp() {
        auto now = std::chrono::system_clock::now();
        auto time_t = std::chrono::system_clock::to_time_t(now);
        std::stringstream ss;
        ss << std::put_time(std::gmtime(&time_t), "%Y-%m-%dT%H:%M:%S.000Z");
        return ss.str();
    }
};

} // namespace sandbox

// ============================================================================
// 主函数: 完整调用链路演示
// ============================================================================

int main() {
    // 配置日志输出
    spdlog::set_level(spdlog::level::info);
    spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %v");
    
    std::cout << "=== HunterClaw 可观测性集成示例 ===" << std::endl;
    std::cout << std::endl;
    
    // 生成 trace_id (全链路追踪)
    std::string trace_id = "trace-10001";
    std::string session_id = "session-abc-123";
    
    std::cout << "📍 Trace ID: " << trace_id << std::endl;
    std::cout << "📍 Session ID: " << session_id << std::endl;
    std::cout << std::endl;
    
    // ========== 第 1 层: HTTP Gateway ==========
    std::cout << "┌─────────────────────────────────────┐" << std::endl;
    std::cout << "│ 第 1 层: HTTP Gateway (日志 + 指标) │" << std::endl;
    std::cout << "└─────────────────────────────────────┘" << std::endl;
    
    gateway::LoggingMiddleware middleware;
    middleware.execute("POST", "/v1/chat/completions", "192.168.1.100", [&]() {
        
        // ========== 第 2 层: Session Manager ==========
        std::cout << std::endl;
        std::cout << "┌────────────────────────────────┐" << std::endl;
        std::cout << "│ 第 2 层: Session Manager (日志) │" << std::endl;
        std::cout << "└────────────────────────────────┘" << std::endl;
        
        sessions::SessionManager session_mgr;
        auto session = session_mgr.get_or_create_session("session-key-123", "agent-12345");
        
        // ========== 第 3 层: React Engine ==========
        std::cout << std::endl;
        std::cout << "┌──────────────────────────────────────┐" << std::endl;
        std::cout << "│ 第 3 层: React Engine (追踪 + 日志) │" << std::endl;
        std::cout << "└──────────────────────────────────────┘" << std::endl;
        
        react::ReactEngine engine;
        engine.run("搜索最新的 AI 新闻", trace_id);
        
        // ========== 第 4 层: Sandbox ==========
        std::cout << std::endl;
        std::cout << "┌──────────────────────────────────┐" << std::endl;
        std::cout << "│ 第 4 层: Sandbox (审计日志)      │" << std::endl;
        std::cout << "└──────────────────────────────────┘" << std::endl;
        
        sandbox::Sandbox sandbox;
        sandbox.execute_code("python", "print('Hello, World!')", session_id, trace_id);
        
        return 200;  // HTTP status code
    });
    
    // ========== Prometheus Metrics ==========
    std::cout << std::endl;
    std::cout << "┌──────────────────────────────────┐" << std::endl;
    std::cout << "│ Prometheus Metrics (GET /metrics) │" << std::endl;
    std::cout << "└──────────────────────────────────┘" << std::endl;
    
    gateway::MetricsHandler metrics_handler;
    std::string metrics = metrics_handler.handle_metrics();
    std::cout << metrics.substr(0, 500) << "..." << std::endl;  // 只显示前 500 字符
    
    std::cout << std::endl;
    std::cout << "✅ 完成! 所有可观测性数据已生成" << std::endl;
    std::cout << std::endl;
    std::cout << "📂 输出位置:" << std::endl;
    std::cout << "  - 日志: /tmp/hunterclaw.log (或控制台)" << std::endl;
    std::cout << "  - 审计: /tmp/hunterclaw-audit.log" << std::endl;
    std::cout << "  - 指标: GET /metrics (Prometheus 格式)" << std::endl;
    std::cout << "  - 追踪: Jaeger UI (http://localhost:16686)" << std::endl;
    std::cout << std::endl;
    std::cout << "🔍 查询示例:" << std::endl;
    std::cout << "  - Kibana: trace_id:\"" << trace_id << "\"" << std::endl;
    std::cout << "  - Jaeger: Trace ID = " << trace_id << std::endl;
    std::cout << "  - Audit: jq 'select(.trace_id == \"" << trace_id << "\")' /tmp/hunterclaw-audit.log" << std::endl;
    
    return 0;
}
