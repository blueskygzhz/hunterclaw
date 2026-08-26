// ============================================================================
// remote_a2a/remote_agent_client.cc
// 远端 A2A agent 的 RPC 客户端实现。
// ============================================================================

#include "hunterclaw/remote_a2a/remote_agent_client.h"

#include <utility>

namespace hunterclaw {
namespace remote_a2a {

const char* cap_state_text(CapState s) {
    switch (s) {
        case CapState::Unknown: return "unknown";
        case CapState::Yes: return "yes";
        case CapState::No: return "no";
    }
    return "unknown";
}

// ── SendOutcome ─────────────────────────────────────────────────────────────

std::string SendOutcome::text() const {
    if (has_task) {
        // 优先 artifacts（正式结果），其次 status.message（进度/提示）
        std::string s = extract_result_text(task);
        if (!s.empty()) return s;
        if (task.status.message) return concat_text_parts(task.status.message->parts);
        return {};
    }
    if (has_message) return concat_text_parts(message.parts);
    return {};
}

bool SendOutcome::is_terminal() const {
    // ⚠ 直接返回 Message（无 task）本身就是终态：远端一次性答完了
    if (has_message && !has_task) return true;
    if (has_task) return remote_a2a::is_terminal(task.status.state);
    return false;
}

core::Result<SendOutcome> parse_send_result(const nlohmann::json& result) {
    SendOutcome o;
    if (!result.is_object()) {
        return core::Result<SendOutcome>::Err("message/send 返回值不是 object");
    }

    const bool looks_task = result.contains("status") && result.contains("id");
    const bool looks_msg = result.contains("parts");

    if (looks_task) {
        o.has_task = true;
        o.task = task_from_json(result);
        if (o.task.id.empty()) {
            return core::Result<SendOutcome>::Err("远端返回的 Task 缺少 id");
        }
        return core::Result<SendOutcome>::Ok(std::move(o));
    }
    if (looks_msg) {
        o.has_message = true;
        o.message = message_from_json(result);
        return core::Result<SendOutcome>::Ok(std::move(o));
    }

    // ★ 不兜底成空 Task。空 Task 的 state 默认 submitted，
    //   会让状态机认为「任务在跑」，然后永久探测一个不存在的 task。
    return core::Result<SendOutcome>::Err(
        "message/send 返回值既不是 Task 也不是 Message");
}

// ── 构造 ────────────────────────────────────────────────────────────────────

std::shared_ptr<RemoteAgentClient> RemoteAgentClient::create(
    const RemoteAgentClientConfig& cfg, std::shared_ptr<CardResolver> card_resolver,
    infra::HttpClient::Ptr http) {
    return std::make_shared<RemoteAgentClient>(cfg, std::move(card_resolver),
                                               std::move(http));
}

RemoteAgentClient::RemoteAgentClient(const RemoteAgentClientConfig& cfg,
                                     std::shared_ptr<CardResolver> card_resolver,
                                     infra::HttpClient::Ptr http)
    : cfg_(cfg),
      cards_(std::move(card_resolver)),
      http_(http ? std::move(http) : infra::HttpClient::create()),
      cred_(CredentialResolver::create()) {
    caps_.force_streaming = cfg_.force_streaming;
    caps_.force_no_streaming = cfg_.force_no_streaming;
}

core::Result<AgentCard> RemoteAgentClient::ensure_card() {
    if (!cards_) {
        return core::Result<AgentCard>::Err("未配置 CardResolver");
    }
    auto r = cards_->resolve();
    if (r.is_err()) return core::Result<AgentCard>::Err(r.error);

    std::lock_guard<std::mutex> lk(mu_);
    rpc_url_ = r.get().rpc_url;
    card_ready_ = true;
    // ★ 卡上的声明只记录，【不作判据】——
    //   ADK 的示例卡全是 "capabilities": {}，照它判会永久禁用 SSE。
    caps_.card_claims_streaming = r.get().card.capabilities.streaming;
    caps_.card_claims_push = r.get().card.capabilities.push_notifications;
    return core::Result<AgentCard>::Ok(r.get().card);
}

ProbedCapabilities RemoteAgentClient::capabilities() const {
    std::lock_guard<std::mutex> lk(mu_);
    return caps_;
}

RpcFailure RemoteAgentClient::last_failure() const {
    std::lock_guard<std::mutex> lk(mu_);
    return last_failure_;
}

void RemoteAgentClient::set_last_failure(const RpcFailure& f) {
    std::lock_guard<std::mutex> lk(mu_);
    last_failure_ = f;
}

void RemoteAgentClient::note_capability(CapState* slot, const RpcFailure& f,
                                        bool success) {
    std::lock_guard<std::mutex> lk(mu_);
    if (success) {
        *slot = CapState::Yes;
        return;
    }
    // ★ 只有【永久性】答案才能写 No。
    //   把 5xx / 超时 / 传输抖动记成「不支持」，会让这个远端在整个进程
    //   生命周期里永久降级，且没有任何日志能解释原因。
    if (f.kind == RpcFailureKind::Unsupported) {
        *slot = CapState::No;
    }
    // 其余情况：保持原状（Unknown 仍是 Unknown，Yes 仍是 Yes）
}

// ── 头部与鉴权 ──────────────────────────────────────────────────────────────

core::Result<std::map<std::string, std::string>> RemoteAgentClient::build_headers(
    bool accept_sse) {
    std::map<std::string, std::string> h;
    h["Content-Type"] = "application/json";
    h["Accept"] = accept_sse ? "text/event-stream" : "application/json";

    auto c = cred_->resolve(cfg_.auth);
    if (c.is_err()) {
        return core::Result<std::map<std::string, std::string>>::Err(
            "凭据解析失败: " + c.error);
    }
    // ★ to_headers() 对空 token 不产出 header —— 不会发出字面量 "Bearer "
    for (const auto& [k, v] : c.get().to_headers()) h[k] = v;
    return core::Result<std::map<std::string, std::string>>::Ok(std::move(h));
}

// ── 单次 RPC ────────────────────────────────────────────────────────────────

core::Result<nlohmann::json> RemoteAgentClient::call(const std::string& method,
                                                     const nlohmann::json& params,
                                                     int timeout_ms, RpcFailure* out) {
    auto card = ensure_card();
    if (card.is_err()) {
        RpcFailure f = failure_from_transport("卡未就绪: " + card.error);
        if (out) *out = f;
        set_last_failure(f);
        return core::Result<nlohmann::json>::Err(f.to_text());
    }

    std::string url;
    {
        std::lock_guard<std::mutex> lk(mu_);
        url = rpc_url_;
    }
    if (url.empty()) {
        RpcFailure f = failure_from_transport("无可用 RPC URL");
        if (out) *out = f;
        set_last_failure(f);
        return core::Result<nlohmann::json>::Err(f.to_text());
    }

    auto headers = build_headers(false);
    if (headers.is_err()) {
        RpcFailure f = failure_from_transport(headers.error);
        if (out) *out = f;
        set_last_failure(f);
        return core::Result<nlohmann::json>::Err(f.to_text());
    }

    const std::string id = next_rpc_id();
    const nlohmann::json req = build_rpc_request(id, method, params);

    infra::HttpRequestOptions opt;
    opt.timeout_ms = timeout_ms;
    opt.headers = headers.get();
    opt.body = req.dump();

    auto resp = http_->post(url, opt);
    if (resp.is_err()) {
        RpcFailure f = failure_from_transport(resp.error);
        if (out) *out = f;
        set_last_failure(f);
        return core::Result<nlohmann::json>::Err(f.to_text());
    }
    const auto& r = resp.get();

    // ⚠ 顺序：先看 HTTP 状态，但【不能】看到非 2xx 就直接返回 ——
    //   有些实现把 JSON-RPC error 配 4xx 一起发，body 里的 code 信息更精确。
    if (!r.ok) {
        RpcFailure http_f = failure_from_http(r.status_code, r.body);
        RpcFailure body_f;
        auto parsed = parse_rpc_response(r.body, id, &body_f);
        if (parsed.is_ok()) {
            // 非 2xx 但 body 是合法 result —— 罕见，按成功处理
            if (out) *out = RpcFailure{};
            return parsed;
        }
        // body 里有明确 rpc_code（Unsupported/TaskNotFound）时优先用它 ——
        // 它比 "HTTP 400" 有用得多，且是能力探测的判据来源
        const RpcFailure& pick =
            (body_f.rpc_code != 0) ? body_f : http_f;
        if (out) *out = pick;
        set_last_failure(pick);
        return core::Result<nlohmann::json>::Err(pick.to_text());
    }

    RpcFailure f;
    auto parsed = parse_rpc_response(r.body, id, &f);
    if (parsed.is_err()) {
        if (out) *out = f;
        set_last_failure(f);
        return parsed;
    }
    if (out) *out = RpcFailure{};
    return parsed;
}

// ── params 组装 ─────────────────────────────────────────────────────────────

nlohmann::json RemoteAgentClient::build_message_params(const Message& msg) const {
    nlohmann::json p;
    p["message"] = to_json(msg);
    // configuration 是可选的；只在需要时才带，避免触发对端严格校验
    return p;
}

// ── message/send ────────────────────────────────────────────────────────────

core::Result<SendOutcome> RemoteAgentClient::send_message(const Message& msg) {
    RpcFailure f;
    auto r = call(M_MESSAGE_SEND, build_message_params(msg), cfg_.rpc_timeout_ms, &f);
    if (r.is_err()) {
        return core::Result<SendOutcome>::Err(r.error);
    }
    return parse_send_result(r.get());
}

// ── message/stream (SSE) ────────────────────────────────────────────────────

core::Result<void> RemoteAgentClient::stream_message(const Message& msg,
                                                     StreamEventFn on_event) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (!caps_.should_try_streaming()) {
            return core::Result<void>::Err("远端已确认不支持 message/stream");
        }
    }

    auto card = ensure_card();
    if (card.is_err()) return core::Result<void>::Err("卡未就绪: " + card.error);

    std::string url;
    {
        std::lock_guard<std::mutex> lk(mu_);
        url = rpc_url_;
    }
    if (url.empty()) return core::Result<void>::Err("无可用 RPC URL");

    auto headers = build_headers(true);
    if (headers.is_err()) return core::Result<void>::Err(headers.error);

    const std::string id = next_rpc_id();
    const nlohmann::json req = build_rpc_request(id, M_MESSAGE_STREAM,
                                                build_message_params(msg));

    infra::HttpRequestOptions opt;
    opt.timeout_ms = cfg_.rpc_timeout_ms;
    opt.headers = headers.get();
    opt.body = req.dump();

    // ★ 流内的 JSON-RPC error 要能传出循环。用局部变量而非异常。
    RpcFailure stream_failure;
    bool saw_any_event = false;
    bool caller_stopped = false;

    auto line_fn = [&](const std::string& line) -> bool {
        // 注释行 / 空行 / 非 data 字段 / 非法 JSON 一律跳过
        auto j = parse_sse_data_line(line);
        if (!j) return true;

        // SSE 的每个 data 都是一个完整 JSON-RPC 响应。
        // ⚠ 必须走同一套 parse —— 否则流内的 error 会被当成普通事件，
        //   远端明确报错却被当成「还在跑」。
        RpcFailure f;
        auto res = parse_rpc_response_json(*j, id, &f);
        if (res.is_err()) {
            stream_failure = f;
            return false;  // 停止：错误已确定
        }

        ParsedStreamEvent ev = parse_stream_event(res.get());
        saw_any_event = true;
        if (on_event && !on_event(ev)) {
            caller_stopped = true;
            return false;
        }
        // final 事件后不必再读
        if (ev.final) return false;
        return true;
    };

    auto resp = http_->post_stream_until(url, opt, line_fn,
                                        cfg_.stream_idle_timeout_ms,
                                        cfg_.stream_total_timeout_ms);
    if (resp.is_err()) {
        RpcFailure f = failure_from_transport(resp.error);
        set_last_failure(f);
        // ⚠ 传输失败【不能】写 No —— 保持 Unknown，下次还试
        note_capability(&caps_.streaming, f, false);
        return core::Result<void>::Err(f.to_text());
    }

    // 非 2xx：可能是 405（不支持流式）/ 401（缺凭据）/ 5xx（抖动）
    if (!resp.get().ok) {
        RpcFailure http_f = failure_from_http(resp.get().status_code, resp.get().body);
        // body 里若带 -32601/-32004，那才是「不支持」的权威判据
        RpcFailure body_f;
        auto parsed = parse_rpc_response(resp.get().body, id, &body_f);
        (void)parsed;
        const RpcFailure& pick = (body_f.rpc_code != 0) ? body_f : http_f;
        set_last_failure(pick);
        note_capability(&caps_.streaming, pick, false);
        return core::Result<void>::Err(pick.to_text());
    }

    if (!stream_failure.empty()) {
        set_last_failure(stream_failure);
        note_capability(&caps_.streaming, stream_failure, false);
        return core::Result<void>::Err(stream_failure.to_text());
    }

    if (!saw_any_event && !caller_stopped) {
        // 200 但一个事件都没有 —— 典型的中间代理吞了 SSE。
        // ⚠ 这【不是】Unsupported（没有 -32601 依据），保持 Unknown。
        RpcFailure f = failure_from_transport("SSE 流未产生任何事件（可能被中间代理缓冲）");
        set_last_failure(f);
        return core::Result<void>::Err(f.to_text());
    }

    note_capability(&caps_.streaming, RpcFailure{}, true);
    return core::Result<void>::Ok();
}

// ── tasks/get ───────────────────────────────────────────────────────────────

core::Result<Task> RemoteAgentClient::get_task(const std::string& task_id,
                                               int history_length) {
    if (task_id.empty()) return core::Result<Task>::Err("task_id 为空");
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (!caps_.should_try_tasks_get()) {
            return core::Result<Task>::Err("远端已确认不支持 tasks/get");
        }
    }

    nlohmann::json p;
    p["id"] = task_id;
    if (history_length > 0) p["historyLength"] = history_length;

    RpcFailure f;
    auto r = call(M_TASKS_GET, p, cfg_.rpc_timeout_ms, &f);
    if (r.is_err()) {
        note_capability(&caps_.tasks_get, f, false);
        return core::Result<Task>::Err(r.error);
    }
    note_capability(&caps_.tasks_get, RpcFailure{}, true);

    Task t = task_from_json(r.get());
    if (t.id.empty()) {
        // ⚠ 不容忍空 id：拿着空 id 继续探测会一直命中 -32001
        return core::Result<Task>::Err("tasks/get 返回的 Task 缺少 id");
    }
    return core::Result<Task>::Ok(std::move(t));
}

// ── tasks/cancel ────────────────────────────────────────────────────────────

core::Result<Task> RemoteAgentClient::cancel_task(const std::string& task_id) {
    if (task_id.empty()) return core::Result<Task>::Err("task_id 为空");
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (!caps_.should_try_cancel()) {
            return core::Result<Task>::Err("远端已确认不支持 tasks/cancel");
        }
    }

    nlohmann::json p;
    p["id"] = task_id;

    RpcFailure f;
    auto r = call(M_TASKS_CANCEL, p, cfg_.rpc_timeout_ms, &f);
    if (r.is_err()) {
        note_capability(&caps_.tasks_cancel, f, false);
        return core::Result<Task>::Err(r.error);
    }
    note_capability(&caps_.tasks_cancel, RpcFailure{}, true);
    return core::Result<Task>::Ok(task_from_json(r.get()));
}

// ── pushNotificationConfig/set ──────────────────────────────────────────────

core::Result<nlohmann::json> RemoteAgentClient::set_push_config(
    const std::string& task_id, const nlohmann::json& config) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (!caps_.should_try_push()) {
            return core::Result<nlohmann::json>::Err(
                "远端已确认不支持 pushNotificationConfig");
        }
    }

    nlohmann::json p;
    p["taskId"] = task_id;
    p["pushNotificationConfig"] = config;

    RpcFailure f;
    auto r = call(M_PUSH_SET, p, cfg_.rpc_timeout_ms, &f);
    if (r.is_err()) {
        note_capability(&caps_.push_notify, f, false);
        return r;
    }
    note_capability(&caps_.push_notify, RpcFailure{}, true);
    return r;
}

}  // namespace remote_a2a
}  // namespace hunterclaw
