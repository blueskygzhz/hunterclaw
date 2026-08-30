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
    // 🎯 approval 中继（session/request_permission 反向权限链路，对齐 OC）。
    ApprovalDetailsFn approval_details; // 可选：exec.approval.get
    ApprovalResolveFn approval_resolve; // 可选：exec.approval.resolve

    // 🎯 sessions.patch（対齐 OC translator.ts:587-590 / 617-620）。
    // ⚠ 不注入时 setSessionMode / setSessionConfigOption 会【报错】而不是
    //   静默返回成功 —— 后者会让客户端以为 thinking level 已切换，
    //   而 Gateway 侧毫无变化（修复前的行为）。
    SessionPatchFn session_patch;       // 可选：sessions.patch

    // 🎯 gateway 事件回流出参：serve_acp_stdio 在内部创建 agent 后，
    //   会把一个"事件转发器"写入该 shared_ptr，供调用方在 gateway on_event
    //   回调中调用，从而把 chat 流 / approval 事件回流到 translator，
    //   驱动 session/update 与 session/request_permission。
 // 调用方应在 serve 之前把该指针指向一个可写的 std::function 容器。
    std::shared_ptr<std::function<void(const nlohmann::json&)>> event_forwarder;
};

// 阻塞式运行；返回时表示连接已关闭（stdin EOF 或错误）。
void serve_acp_stdio(const AcpServerOptions& opts, const AcpStdioServerDeps& deps);

/// 🎯 对齐: defaultAcpSessionStore
std::unique_ptr<AcpSessionStore> create_default_session_store();
std::unique_ptr<AcpSessionStore> create_session_store(int max_sessions, int64_t idle_ttl_ms);

} // namespace acp
} // namespace hunterclaw
