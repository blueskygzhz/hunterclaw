#pragma once

// ============================================================================
// remote_a2a/remote_agent_client.h
// 单个远端 A2A agent 的 RPC 客户端。★ 这是唯一碰网络的类。
//
// 职责边界（刻意收窄）：
//   ✅ 组装 params、发 HTTP、解 JSON-RPC、维护能力探测缓存
//   ❌ 不做状态机（task_tracking 的事）
//   ❌ 不做等待/让渡（execution_engine 的事）
//   ❌ 不起线程（调用方决定在哪个线程调）
// 所以本类的每个方法都是【同步阻塞】的，且没有任何后台线程。
//
// ── ProbedCapabilities：为什么必须有 ──────────────────────────────────────
// 卡上的 capabilities 在真实世界【不可用】。Google ADK 的 AgentCardBuilder
// 默认 `capabilities or AgentCapabilities()`（agent_card_builder.py:72），
// 三张官方示例卡全是 `"capabilities": {}`。若照卡决定用不用 SSE，
// 结论永远是「不支持」—— 流式能力被白白丢掉。
//
// 解法：乐观尝试 + 结果缓存。
//   Unknown → 试一次
//   成功     → Yes（后续直接用）
//   失败且是 -32601/-32004/-32003 → No（★ 永久性答案，不再试）
//   失败但是 5xx / 传输错误       → 保持 Unknown（★ 抖动不能污染结论）
//
// ⚠ 最后这条是关键。把「网络抖动」记成「不支持 SSE」，会让这个远端在
//   整个进程生命周期里永久降级成轮询模式，而且没有任何日志能解释原因。
//
// ── force_* 逃生阀 ─────────────────────────────────────────────────────────
// 探测本身也可能误判（比如中间代理吞了 SSE）。因此每个能力都留一个
// 强制开关，运维可以绕过探测直接指定。
//
// ── 并发原语约束（项目级）────────────────────────────────────────────────
// ✅ thread / mutex / condition_variable / atomic
// ❌ future / promise / async / packaged_task
// 本文件只用 mutex（无线程）。
// ============================================================================

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "hunterclaw/core/types.h"
#include "hunterclaw/infra/http_client.h"
#include "hunterclaw/remote_a2a/card_resolver.h"
#include "hunterclaw/remote_a2a/credential_resolver.h"
#include "hunterclaw/remote_a2a/json_rpc.h"
#include "hunterclaw/remote_a2a/types.h"

namespace hunterclaw {
namespace remote_a2a {

// ── 能力探测 ────────────────────────────────────────────────────────────────

enum class CapState {
    Unknown,  // 没试过 → 乐观尝试
    Yes,      // 试成功了
    No,       // ★ 明确不支持（-32601/-32004/-32003），不再试
};

const char* cap_state_text(CapState);

/// 探测结果缓存。
/// ⚠ 只有【永久性】错误码能把 Unknown 推到 No。5xx / 超时一律保持 Unknown。
struct ProbedCapabilities {
    CapState streaming = CapState::Unknown;      // message/stream
    CapState tasks_get = CapState::Unknown;      // tasks/get
    CapState tasks_cancel = CapState::Unknown;   // tasks/cancel
    CapState push_notify = CapState::Unknown;    // pushNotificationConfig/set

    /// 卡上的声明（仅作参考，★ 不作判据 —— 见文件头）
    bool card_claims_streaming = false;
    bool card_claims_push = false;

    /// 运维逃生阀：绕过探测直接指定
    bool force_streaming = false;
    bool force_no_streaming = false;

    /// 是否应当尝试 SSE
    bool should_try_streaming() const {
        if (force_no_streaming) return false;
        if (force_streaming) return true;
        return streaming != CapState::No;  // Unknown 也试（乐观）
    }
    bool should_try_tasks_get() const { return tasks_get != CapState::No; }
    bool should_try_cancel() const { return tasks_cancel != CapState::No; }
    bool should_try_push() const { return push_notify != CapState::No; }
};

// ── 发送结果 ────────────────────────────────────────────────────────────────

/// message/send 的返回。远端可能返回 Task，也可能直接返回 Message
/// （无 task 的即时回复）。两种都要处理。
struct SendOutcome {
    /// 远端返回了 Task
    bool has_task = false;
    Task task;

    /// 远端直接返回 Message（无 task）
    bool has_message = false;
    Message message;

    /// 便捷：取文本结果
    std::string text() const;

    /// 便捷：本轮是否已终结
    bool is_terminal() const;
};

// ── SSE 回调 ────────────────────────────────────────────────────────────────

/// 流式事件回调。★ 返回 false 请求停止（会关闭连接）。
/// ⚠ 该回调在【调用 stream_message 的那个线程】上执行，不是后台线程。
///   因此实现里不要做重活，也不要再去调本客户端的其他方法（会死锁）。
using StreamEventFn = std::function<bool(const ParsedStreamEvent&)>;

// ── 配置 ────────────────────────────────────────────────────────────────────

struct RemoteAgentClientConfig {
    /// 逻辑名（日志与工具名用）
    std::string name;

    /// 单次非流式 RPC 超时
    int rpc_timeout_ms = 30000;

    /// SSE：两次事件之间的最大间隔。超过即断开转探测。
    /// ⚠ 不能设太小：远端在长思考时可能确实几十秒不发东西。
    int stream_idle_timeout_ms = 120000;

    /// SSE：单条流的总时长上限（0 = 无上限）
    int stream_total_timeout_ms = 0;

    AuthSpec auth;

    /// 强制流式开关（写入 ProbedCapabilities）
    bool force_streaming = false;
    bool force_no_streaming = false;
};

// ── 客户端 ──────────────────────────────────────────────────────────────────

class RemoteAgentClient {
public:
    static std::shared_ptr<RemoteAgentClient> create(
        const RemoteAgentClientConfig& cfg,
        std::shared_ptr<CardResolver> card_resolver,
        infra::HttpClient::Ptr http = nullptr);

    RemoteAgentClient(const RemoteAgentClientConfig& cfg,
                      std::shared_ptr<CardResolver> card_resolver,
                      infra::HttpClient::Ptr http);

    /// 确保卡已解析（首次调用会拉卡）。返回消杀后的卡。
    core::Result<AgentCard> ensure_card();

    const std::string& name() const { return cfg_.name; }

    /// 能力快照（加锁读）
    ProbedCapabilities capabilities() const;

    // ── RPC ─────────────────────────────────────────────────────────────

    /// message/send —— 非流式。
    /// ⚠ 远端可能立即返回 submitted/working 的 Task（长任务），
    ///   这不是失败，调用方需要继续用 tasks/get 或 SSE 跟进。
    core::Result<SendOutcome> send_message(const Message& msg);

    /// message/stream —— SSE。
    ///
    /// ★ 返回 Ok 不代表任务完成，只代表「流正常结束或被调用方主动停止」。
    ///   任务是否终结由 on_event 收到的事件判断。
    ///
    /// ★ 若远端不支持流式（-32601/-32004），返回 Err 且
    ///   capabilities().streaming 被置为 No，调用方应回落到 send + tasks/get。
    core::Result<void> stream_message(const Message& msg, StreamEventFn on_event);

    /// tasks/get —— 退避探测用。
    /// ★ TaskNotFound（-32001）返回 Err，failure 的 kind 为 TaskNotFound；
    ///   调用方必须把它当【终态】处理，不能继续探测。
    core::Result<Task> get_task(const std::string& task_id, int history_length = 0);

    /// tasks/cancel。
    /// ⚠ 対标 ADK executor/utils.py:31-48：远端的 cancel 往往只塞一个
    ///   canceled 事件而【不中断】实际执行 —— 远端会继续烧 token。
    ///   所以本方法返回 Ok 只意味着「取消请求已被接受」。
    core::Result<Task> cancel_task(const std::string& task_id);

    /// pushNotificationConfig/set —— 回调分支用。
    core::Result<nlohmann::json> set_push_config(const std::string& task_id,
                                                 const nlohmann::json& config);

    /// 最近一次失败详情（供调用方做分支决策）
    RpcFailure last_failure() const;

private:
    /// 组装 message/send | message/stream 的 params
    nlohmann::json build_message_params(const Message& msg) const;

    /// 一次非流式 RPC。★ 内部完成 401 一次重试（凭据可能刚轮转）。
    core::Result<nlohmann::json> call(const std::string& method,
                                      const nlohmann::json& params, int timeout_ms,
                                      RpcFailure* out);

    /// 构造请求头（Content-Type + 凭据 + additional_headers）
    core::Result<std::map<std::string, std::string>> build_headers(bool accept_sse);

    /// ★ 能力探测状态机的唯一写入点。
    ///   只有 Unsupported 才写 No；其余失败保持原状（不污染结论）。
    void note_capability(CapState* slot, const RpcFailure& f, bool success);

    void set_last_failure(const RpcFailure& f);

    RemoteAgentClientConfig cfg_;
    std::shared_ptr<CardResolver> cards_;
    infra::HttpClient::Ptr http_;
    std::shared_ptr<CredentialResolver> cred_;

    mutable std::mutex mu_;
    ProbedCapabilities caps_;
    std::string rpc_url_;
    bool card_ready_ = false;
    RpcFailure last_failure_;
};

/// result 节点 → SendOutcome。
/// 判定顺序：有 "status" 且有 "id" → Task；有 "parts" → Message。
/// ⚠ 两者都不像 → Err，【不要】兜底成空 Task：空 Task 的 state 默认
///   submitted，会让状态机以为任务在跑并永久探测下去。
core::Result<SendOutcome> parse_send_result(const nlohmann::json& result);

}  // namespace remote_a2a
}  // namespace hunterclaw
