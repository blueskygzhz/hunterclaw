// ============================================================================
// remote_a2a/callback_endpoint.cc
// 远端 A2A 回调接收端实现。
// ============================================================================

#include "hunterclaw/remote_a2a/callback_endpoint.h"

#include <algorithm>
#include <cctype>

namespace hunterclaw {
namespace remote_a2a {

namespace {

int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

bool starts_with(const std::string& s, const std::string& p) {
    return s.size() >= p.size() && s.compare(0, p.size(), p) == 0;
}

std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

/// 统一的拒绝响应。
/// ★ 所有拒绝理由都回同一个 404 + 同一段文案 ——
///   区分「handle 不存在」与「token 错」就是一个 handle 枚举探针。
CallbackOutcome make_reject(const std::string& internal_note) {
    CallbackOutcome o;
    o.http_status = 404;
    o.body = nlohmann::json{{"error", "not found"}};
    o.note = internal_note;  // 只进本地日志，不进响应体
    return o;
}

}  // namespace

// ── 路径与头部解析 ──────────────────────────────────────────────────────────

std::optional<std::string> extract_callback_handle(const std::string& path) {
    if (!starts_with(path, CALLBACK_PATH_PREFIX)) return std::nullopt;

    std::string h = path.substr(std::string(CALLBACK_PATH_PREFIX).size());

    // 去掉 query / fragment
    const size_t q = h.find_first_of("?#");
    if (q != std::string::npos) h = h.substr(0, q);

    if (h.size() < 4 || h.size() > 128) return std::nullopt;

    // ★ 逐字符白名单。这同时挡住了：
    //   · 路径穿越（'/' 与 '.' 都不在白名单）
    //   · URL 编码绕过（'%' 不在白名单，故 %2F 无法通过）
    //   · 空字节截断（'\0' 不在白名单）
    for (unsigned char c : h) {
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                        (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!ok) return std::nullopt;
    }
    return h;
}

std::optional<std::string> extract_bearer_token(const std::string& auth_header) {
    const std::string s = trim(auth_header);
    if (s.empty()) return std::nullopt;

    const size_t sp = s.find(' ');
    // ★ 缺 scheme 时拒绝，不把整串当裸 token ——
    //   接受裸 token 会让 "Authorization: <handle>" 这类误配意外通过。
    if (sp == std::string::npos) return std::nullopt;

    std::string scheme = s.substr(0, sp);
    std::transform(scheme.begin(), scheme.end(), scheme.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    // scheme 比较大小写不敏感（RFC 7235 §2.1）
    if (scheme != "bearer") return std::nullopt;

    const std::string token = trim(s.substr(sp + 1));
    if (token.empty()) return std::nullopt;
    return token;
}

// ── 构造 ────────────────────────────────────────────────────────────────────

std::shared_ptr<CallbackEndpoint> CallbackEndpoint::create(
    std::shared_ptr<PendingTaskStore> store, CallbackEngineLookupFn lookup,
    const CallbackEndpointConfig& cfg) {
    return std::make_shared<CallbackEndpoint>(std::move(store), std::move(lookup), cfg);
}

CallbackEndpoint::CallbackEndpoint(std::shared_ptr<PendingTaskStore> store,
                                   CallbackEngineLookupFn lookup,
                                   const CallbackEndpointConfig& cfg)
    : store_(std::move(store)), lookup_(std::move(lookup)), cfg_(cfg) {}

// ── 失败节流 ────────────────────────────────────────────────────────────────

bool CallbackEndpoint::note_failure_and_check(const std::string& peer) {
    // peer 为空时用一个共享桶。⚠ 这会让所有匿名来源共享配额，
    //   但比"不限流"安全（gateway 一般能提供 peer，为空是异常情况）。
    const std::string key = peer.empty() ? "<unknown>" : peer;

    std::lock_guard<std::mutex> lk(mu_);
    const int64_t now = now_ms();
    auto& b = failures_[key];
    if (b.window_start_ms == 0 || now - b.window_start_ms >= cfg_.failure_window_ms) {
        b.window_start_ms = now;
        b.count = 0;
    }
    ++b.count;

    // ⚠ 桶数量上限：peer 可能是攻击者伪造的 X-Forwarded-For，
    //   不限量会让 map 无界增长。超限时清理最老的窗口。
    if (failures_.size() > 4096) {
        int64_t oldest = now;
        std::string victim;
        for (const auto& kv : failures_) {
            if (kv.first == key) continue;
            if (kv.second.window_start_ms <= oldest) {
                oldest = kv.second.window_start_ms;
                victim = kv.first;
            }
        }
        if (!victim.empty()) failures_.erase(victim);
    }

    return b.count > cfg_.max_failures_per_window;
}

int CallbackEndpoint::failure_count(const std::string& peer) const {
    const std::string key = peer.empty() ? "<unknown>" : peer;
    std::lock_guard<std::mutex> lk(mu_);
    auto it = failures_.find(key);
    if (it == failures_.end()) return 0;
    if (now_ms() - it->second.window_start_ms >= cfg_.failure_window_ms) return 0;
    return it->second.count;
}

// ── 主处理 ──────────────────────────────────────────────────────────────────

CallbackOutcome CallbackEndpoint::handle_callback(const std::string& path,
                                                 const std::string& auth_header,
                                                 const std::string& body,
                                                 const std::string& peer) {
    if (!store_) {
        rejected_.fetch_add(1);
        CallbackOutcome o;
        o.http_status = 503;
        o.body = nlohmann::json{{"error", "remote_a2a not enabled"}};
        return o;
    }

    // ── 体积检查（最先做，避免为一个超大 body 做后续工作）────────────────
    if (body.size() > cfg_.max_body_bytes) {
        rejected_.fetch_add(1);
        note_failure_and_check(peer);
        CallbackOutcome o;
        o.http_status = 413;
        o.body = nlohmann::json{{"error", "payload too large"}};
        o.note = "body " + std::to_string(body.size()) + " 字节超限";
        return o;
    }

    // ── 节流（在验证之前检查，避免被枚举者拖着做恒时比较）────────────────
    if (failure_count(peer) > cfg_.max_failures_per_window) {
        rejected_.fetch_add(1);
        CallbackOutcome o;
        o.http_status = 429;
        o.body = nlohmann::json{{"error", "too many failed attempts"}};
        o.note = "peer 已被节流";
        return o;
    }

    // ── 闸门 0：路径合法性 ──────────────────────────────────────────────
    auto handle = extract_callback_handle(path);
    if (!handle) {
        rejected_.fetch_add(1);
        note_failure_and_check(peer);
        return make_reject("路径非法或 handle 格式不合规");
    }

    // ── 闸门 2：token ───────────────────────────────────────────────────
    auto token = extract_bearer_token(auth_header);
    if (!token) {
        rejected_.fetch_add(1);
        note_failure_and_check(peer);
        // ★ 回 404 而非 401：401 会确认「这个 handle 存在，只是没给 token」
        return make_reject("缺少或格式非法的 Bearer token");
    }

    // ★ 闸门 1 + 2 合一：verify_callback_token 内部对
    //   「handle 不存在」「未绑定 token」「token 不匹配」全部返回 false，
    //   且用恒定时间比较（pending_tasks.cc:constant_time_equals）。
    if (!store_->verify_callback_token(*handle, *token)) {
        rejected_.fetch_add(1);
        const bool throttled = note_failure_and_check(peer);
        if (throttled) {
            CallbackOutcome o;
            o.http_status = 429;
            o.body = nlohmann::json{{"error", "too many failed attempts"}};
            o.note = "token 校验失败且已超限";
            return o;
        }
        return make_reject("token 校验失败 / handle 不存在 / 未绑定 token");
    }

    accepted_.fetch_add(1);

    CallbackOutcome out;
    out.handle = *handle;

    // ── payload 只用于日志，★ 其内容一概不采信 ─────────────────────────
    // 远端可控 + 公网传输 → 直接采信等于让攻击者注入「任务已完成，结果是 X」，
    // 而那个 X 会进 transcript 并进入下一轮 prompt。
    {
        std::string wire_state;
        try {
            auto j = nlohmann::json::parse(body);
            if (j.is_object()) {
                auto s = j.find("status");
                if (s != j.end() && s->is_object()) {
                    auto st = s->find("state");
                    if (st != s->end() && st->is_string()) {
                        wire_state = st->get<std::string>();
                        if (wire_state.size() > 40) wire_state = wire_state.substr(0, 40);
                    }
                }
            }
        } catch (const std::exception&) {
            // 非法 JSON 不是拒绝理由：token 已验过，说明是合法远端。
            // ★ 回调是信号不是数据，payload 解不开完全不影响后续拉取。
        }
        out.note = wire_state.empty() ? "回调已受理" : ("回调已受理，payload state=" + wire_state);
    }

    // ── 权威拉取 ────────────────────────────────────────────────────────
    // ⚠ 不回显任务状态：回调方是远端自己，它本就知道状态；
    //   回显只会给枚举者额外信息。
    nlohmann::json rb;
    rb["status"] = "accepted";
    if (cfg_.echo_handle) rb["handle"] = *handle;

    auto engine = lookup_ ? lookup_(*handle) : nullptr;
    if (!engine) {
        // ⚠ 引擎不可用（远端已移出配置）。
        //   回 200 而非 404：token 已验过，说明这是合法投递；
        //   让远端不再重试是对的（重试也没有引擎可用）。
        out.http_status = 200;
        out.body = std::move(rb);
        out.note += "；但对应引擎已不可用，未拉取";
        return out;
    }

    // ★ 唯一可信来源：tasks/get。
    out.polled = true;
    out.terminal = engine->poll_once(*handle);

    out.http_status = 200;
    out.body = std::move(rb);
    return out;
}

}  // namespace remote_a2a
}  // namespace hunterclaw
