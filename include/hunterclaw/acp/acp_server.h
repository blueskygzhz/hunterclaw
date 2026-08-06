#pragma once

// ============================================================================
// acp_server.h
// 🎯 对齐 OpenClaw: src/acp/server.ts + src/acp/session.ts
// ============================================================================

#include "hunterclaw/acp/acp_translator.h"
#include <functional>
#include <memory>

namespace hunterclaw {
namespace acp {

class AcpStdioConnection;

/// 🎯 对齐: serveAcpGateway(opts)（旧入口，逻辑已移至 gateway 层）
void serve_acp_gateway(const AcpServerOptions& opts);

// 🎯 严格对齐 OC server.ts serveAcpGateway: 真正的 stdio JSON-RPC 主循环。
// 在当前进程的 stdin/stdout 上以 ndjson JSON-RPC 提供 ACP Agent 服务，
// 注册 ACP 规范方法（initialize / session/new / session/prompt / session/cancel
// / session/load / session/list / ...），阻塞直到 stdin EOF。
//
// prompt/cancel 的真正派发由调用方通过注入的回调提供（接 gateway），
// 以避免 acp 库反向依赖 gateway 库（gateway 已依赖 acp）。
struct AcpStdioServerDeps {
    PromptDispatchFn prompt_dispatch;   // 必填：把 prompt 派发到 gateway chat
    CancelDispatchFn cancel_dispatch;   // 可选：abort run
};

// 阻塞式运行；返回时表示连接已关闭（stdin EOF 或错误）。
void serve_acp_stdio(const AcpServerOptions& opts, const AcpStdioServerDeps& deps);

/// 🎯 对齐: defaultAcpSessionStore
std::unique_ptr<AcpSessionStore> create_default_session_store();
std::unique_ptr<AcpSessionStore> create_session_store(int max_sessions, int64_t idle_ttl_ms);

} // namespace acp
} // namespace hunterclaw
