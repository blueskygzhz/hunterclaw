// ============================================================================
// question.cc — 用户提问的 gateway 方法
//
// 🎯 严格对齐 OpenClaw 的 question server methods:
//     question.request     登记一次提问（供【外部】客户端主动发起）
//     question.waitAnswer  等待答复
//     question.resolve     用答案收敛 / 取消
//     question.get         读单条
//     question.list        列出待答
//
// ★★ 注意分工: agent 内部的提问【不走这些方法】，而是直接调
//   tools::run_gateway_question() → GatewayQuestionBroker（同进程，
//   见 question_broker_impl.h 的说明）。这些方法存在的意义是:
//     ① 终端点按钮 → question.resolve（这是【唯一】的答复回流入口）
//     ② 终端重连后拉取待答列表 → question.list / question.get
//     ③ 外部编排系统主动发起提问 → question.request
//
//   ⚠ ① 是必须的: 没有它，用户只能靠"回一句纯文本"被 claim 通道认领，
//     而按钮点击（终端最主要的交互方式）完全无法送回答案。
// ============================================================================

#include "hunterclaw/gateway/gateway.h"
#include "hunterclaw/gateway/method_guard.h"
#include "hunterclaw/gateway/question_manager.h"
#include "hunterclaw/core/utils.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <string>
#include <vector>

using json = nlohmann::json;

namespace hunterclaw {
namespace gateway {
namespace methods {

namespace {

int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string read_trimmed(const json& p, const char* key) {
    if (!p.contains(key) || !p[key].is_string()) return {};
    std::string v = p[key].get<std::string>();
    while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) v.erase(0, 1);
    while (!v.empty() && (v.back() == ' ' || v.back() == '\t')) v.pop_back();
    return v;
}

/// 线上形状 → 内部形状。
///
/// ⚠ 字段名是 `questionId` 而非 `id`（对齐 QuestionInputFields）。
///   读错字段会得到一堆 id 为空的 question，然后被 validate 拒掉 ——
///   报错信息会指向"id 不合法"而不是"字段名不对"，很难查。
std::vector<UserInputQuestion> parse_questions(const json& arr, std::string& err) {
    std::vector<UserInputQuestion> out;
    if (!arr.is_array()) {
        err = "questions must be an array";
        return out;
    }
    for (const auto& q : arr) {
        if (!q.is_object()) {
            err = "each question must be an object";
            return {};
        }
        UserInputQuestion uq;
        uq.id = read_trimmed(q, "questionId");
        uq.header = read_trimmed(q, "header");
        if (q.contains("question") && q["question"].is_string()) {
            uq.question = q["question"].get<std::string>();
        }
        if (q.contains("multiSelect") && q["multiSelect"].is_boolean()) {
            uq.multi_select = q["multiSelect"].get<bool>();
        }
        if (q.contains("isOther") && q["isOther"].is_boolean()) {
            uq.is_other = q["isOther"].get<bool>();
        }
        if (q.contains("isSecret") && q["isSecret"].is_boolean()) {
            uq.is_secret = q["isSecret"].get<bool>();
        }
        if (q.contains("options") && q["options"].is_array()) {
            for (const auto& o : q["options"]) {
                if (!o.is_object()) continue;
                tools::UserInputOption op;
                op.label = read_trimmed(o, "label");
                const std::string d = read_trimmed(o, "description");
                if (!d.empty()) op.description = d;
                if (op.label.empty()) continue;  // 空 label 由 validate 兜底报错
                uq.options.push_back(std::move(op));
            }
        }
        out.push_back(std::move(uq));
    }
    return out;
}

/// 线上 answers → 内部 answers。
///
/// ⚠ 必须按 record 里声明的 question 集合做【白名单过滤】:
///   客户端可能送来一个不存在的 questionId（打错、或旧卡片残留）。
///   原样收下会让 render() 时对不上，而 agent 侧读 first_answer(id)
///   会读到空 —— 表现为"用户明明答了但 agent 说没答"。
tools::UserInputAnswers parse_answers(const json& obj,
                                      const std::vector<UserInputQuestion>& questions,
                                      std::string& err) {
    tools::UserInputAnswers out;
    if (!obj.is_object() || !obj.contains("answers") || !obj["answers"].is_object()) {
        err = "answers.answers must be an object";
        return out;
    }
    for (const auto& q : questions) {
        out.answers[q.id] = {};
    }
    for (auto it = obj["answers"].begin(); it != obj["answers"].end(); ++it) {
        auto slot = out.answers.find(it.key());
        if (slot == out.answers.end()) continue;  // 白名单过滤
        if (!it.value().is_array()) continue;
        for (const auto& v : it.value()) {
            if (!v.is_string()) continue;
            const std::string s = v.get<std::string>();
            if (s.empty()) continue;
            slot->second.push_back(s);
        }
    }
    return out;
}

std::string resolver_name(const ClientContext& ctx) {
    if (!ctx.connect.client.display_name.value_or("").empty()) {
        return ctx.connect.client.display_name.value();
    }
    if (!ctx.connect.client.id.empty()) return ctx.connect.client.id;
    return "operator";
}

void broadcast_event(const std::shared_ptr<GatewayServer>& server,
                     const std::string& event, const json& payload) {
    auto runtime = server ? server->runtime_state() : nullptr;
    if (!runtime || !runtime->broadcast) return;
    runtime->broadcast(event, core::JsonValue(payload),
                       GatewayBroadcastOpts{.drop_if_slow = true});
}

}  // namespace

// ============================================================================
// question.request
// ============================================================================
core::Result<json> method_question_request(std::shared_ptr<GatewayServer> server,
                                          std::shared_ptr<MethodGuard> method_guard,
                                          const ClientContext& client_ctx,
                                          const json& params) {
    (void)method_guard;

    auto manager = question_manager_instance();
    if (!manager) return core::Result<json>::Err("question manager not available");

    if (!params.contains("questions")) {
        return core::Result<json>::Err("questions is required");
    }
    std::string err;
    auto questions = parse_questions(params["questions"], err);
    if (!err.empty()) return core::Result<json>::Err(err);

    // ★ 在【登记之前】校验。一旦落库并广播，终端已经渲染，再报错就晚了
    //   （用户会看到一张永远收不掉的卡片）。
    const std::string invalid = tools::validate_user_input_questions(questions);
    if (!invalid.empty()) return core::Result<json>::Err("invalid questions: " + invalid);

    int64_t timeout_ms = DEFAULT_QUESTION_TIMEOUT_MS;
    if (params.contains("timeoutMs") && params["timeoutMs"].is_number_integer()) {
        const int64_t v = params["timeoutMs"].get<int64_t>();
        if (v > 0) timeout_ms = v;
    }

    const std::string explicit_id = read_trimmed(params, "id");
    if (!explicit_id.empty() && manager->get_snapshot(explicit_id).has_value()) {
        return core::Result<json>::Err("question id already pending");
    }

    std::optional<std::string> opt_id;
    if (!explicit_id.empty()) opt_id = explicit_id;
    QuestionRecord record = manager->create(questions, timeout_ms, opt_id);

    const std::string session_key = read_trimmed(params, "sessionKey");
    const std::string agent_id = read_trimmed(params, "agentId");
    const std::string run_id = read_trimmed(params, "runId");
    if (!session_key.empty()) record.session_key = session_key;
    if (!agent_id.empty()) record.agent_id = agent_id;
    if (!run_id.empty()) record.run_id = run_id;
    record.requested_by_conn_id = client_ctx.conn_id;
    record.requested_by_device_id = client_ctx.device_id;
    record.requested_by_client_id = client_ctx.connect.client.id;

    const int64_t effective = record.expires_at_ms - record.created_at_ms;
    if (!manager->register_record(record, effective)) {
        return core::Result<json>::Err("registration failed: id already settled");
    }

    broadcast_event(server, "question.requested", record.to_json());

    json result;
    result["id"] = record.id;
    result["expiresAtMs"] = record.expires_at_ms;
    return core::Result<json>::Ok(result);
}

// ============================================================================
// question.waitAnswer
// ============================================================================
core::Result<json> method_question_wait_answer(std::shared_ptr<GatewayServer> server,
                                              std::shared_ptr<MethodGuard> method_guard,
                                              const ClientContext& client_ctx,
                                              const json& params) {
    (void)server; (void)method_guard; (void)client_ctx;

    auto manager = question_manager_instance();
    if (!manager) return core::Result<json>::Err("question manager not available");

    const std::string id = read_trimmed(params, "id");
    if (id.empty()) return core::Result<json>::Err("id is required");

    auto snapshot = manager->get_snapshot(id);
    if (!snapshot.has_value()) {
        return core::Result<json>::Err("question expired or not found");
    }

    int64_t timeout_ms = 0;  // 0 = 沿用注册时的超时
    if (params.contains("timeoutMs") && params["timeoutMs"].is_number_integer()) {
        const int64_t v = params["timeoutMs"].get<int64_t>();
        if (v > 0) timeout_ms = v;
    }

    // ⚠ 这里【不传 aborted】: 本方法是外部客户端主动来等，取消由它自己
    //   断开连接或另发 question.resolve{cancel} 表达。agent 内部路径
    //   走 broker，那条路上 aborted 是必传的。
    auto res = manager->await_answer(id, timeout_ms, nullptr);

    json result;
    result["status"] = tools::question_status_to_wire(res.status);
    if (res.answered()) {
        json a = json::object();
        json m = json::object();
        for (const auto& [qid, values] : res.answers.answers) {
            m[qid] = values;
        }
        a["answers"] = std::move(m);
        result["answers"] = std::move(a);
    }
    return core::Result<json>::Ok(result);
}

// ============================================================================
// question.resolve —— 【唯一】的答复回流入口
// ============================================================================
core::Result<json> method_question_resolve(std::shared_ptr<GatewayServer> server,
                                          std::shared_ptr<MethodGuard> method_guard,
                                          const ClientContext& client_ctx,
                                          const json& params) {
    (void)method_guard;

    auto manager = question_manager_instance();
    if (!manager) return core::Result<json>::Err("question manager not available");

    const std::string raw_id = read_trimmed(params, "id");
    if (raw_id.empty()) return core::Result<json>::Err("id is required");

    // 前缀查找（与 approval 侧一致：CLI 用户不想粘贴 36 字符 uuid）
    auto lookup = manager->lookup_pending_id(raw_id);
    if (lookup.kind == QuestionIdLookupResult::Kind::None) {
        // ★ 用固定 reason 串，供调用方识别"已终态/不存在"这类【终态错误】——
        //   对齐 OpenClaw TERMINAL_QUESTION_ERROR_REASONS
        //   （QUESTION_ALREADY_TERMINAL / QUESTION_NOT_FOUND）。
        //   ⚠ 没有这个标记，认领通道无法区分"该回落 steer"与"网络抖动该重试"。
        return core::Result<json>::Err("QUESTION_NOT_FOUND: unknown or expired question id");
    }
    if (lookup.kind == QuestionIdLookupResult::Kind::Ambiguous) {
        std::string candidates;
        for (size_t i = 0; i < lookup.ids.size() && i < 3; ++i) {
            if (i > 0) candidates += ", ";
            candidates += lookup.ids[i];
        }
        const int remainder = static_cast<int>(lookup.ids.size()) - 3;
        if (remainder > 0) candidates += " (+" + std::to_string(remainder) + " more)";
        return core::Result<json>::Err("ambiguous question id prefix; matches: " +
                                       candidates + ". Use the full id.");
    }
    const std::string id = lookup.id;

    auto snapshot = manager->get_snapshot(id);
    if (!snapshot.has_value()) {
        return core::Result<json>::Err("QUESTION_NOT_FOUND: unknown or expired question id");
    }

    const std::string by = resolver_name(client_ctx);

    // ── cancel 分支 ─────────────────────────────────────────────────────
    const bool want_cancel =
        params.contains("cancel") && params["cancel"].is_boolean() &&
        params["cancel"].get<bool>();
    if (want_cancel) {
        const std::string resolved_by = read_trimmed(params, "resolvedBy");
        if (!manager->cancel(id, resolved_by.empty() ? by : resolved_by)) {
            return core::Result<json>::Err("QUESTION_ALREADY_TERMINAL: question already resolved");
        }
        json ev;
        ev["id"] = id;
        ev["status"] = "cancelled";
        broadcast_event(server, "question.resolved", ev);
        json result;
        result["status"] = "cancelled";
        return core::Result<json>::Ok(result);
    }

    // ── answers 分支 ────────────────────────────────────────────────────
    if (!params.contains("answers")) {
        return core::Result<json>::Err("answers or cancel is required");
    }
    std::string err;
    auto answers = parse_answers(params["answers"], snapshot->questions, err);
    if (!err.empty()) return core::Result<json>::Err(err);

    // ⚠ 逐题校验合法性: 终端可能送来选项外的自由文本。
    //   ★ 不校验就等于把 normalize_user_input_answer 的 ③ 号安全边界
    //     （选项固定时拒绝自由文本）绕过去了 —— 而按钮点击也走这条路。
    for (const auto& q : snapshot->questions) {
        auto it = answers.answers.find(q.id);
        if (it == answers.answers.end()) continue;
        if (it->second.size() > 1 && !q.multi_select) {
            return core::Result<json>::Err("question '" + q.id +
                                           "' is single-select but got multiple answers");
        }
        std::vector<std::string> normalized;
        for (const auto& v : it->second) {
            auto n = tools::normalize_user_input_answer(v, q);
            if (!n.has_value()) {
                return core::Result<json>::Err("answer '" + v + "' is not valid for question '" +
                                               q.id + "'");
            }
            normalized.push_back(*n);
        }
        it->second = std::move(normalized);
    }

    const std::string resolved_by = read_trimmed(params, "resolvedBy");
    if (!manager->resolve(id, answers, resolved_by.empty() ? by : resolved_by)) {
        return core::Result<json>::Err("QUESTION_ALREADY_TERMINAL: question already resolved");
    }

    json a = json::object();
    json m = json::object();
    for (const auto& [qid, values] : answers.answers) {
        m[qid] = values;
    }
    a["answers"] = std::move(m);

    json ev;
    ev["id"] = id;
    ev["status"] = "answered";
    ev["answers"] = a;
    broadcast_event(server, "question.resolved", ev);

    json result;
    result["status"] = "answered";
    result["answers"] = std::move(a);
    return core::Result<json>::Ok(result);
}

// ============================================================================
// question.get
// ============================================================================
core::Result<json> method_question_get(std::shared_ptr<GatewayServer> server,
                                      std::shared_ptr<MethodGuard> method_guard,
                                      const ClientContext& client_ctx,
                                      const json& params) {
    (void)server; (void)method_guard; (void)client_ctx;

    auto manager = question_manager_instance();
    if (!manager) return core::Result<json>::Err("question manager not available");

    const std::string id = read_trimmed(params, "id");
    if (id.empty()) return core::Result<json>::Err("id is required");

    auto snapshot = manager->get_snapshot(id);
    if (!snapshot.has_value()) {
        return core::Result<json>::Err("QUESTION_NOT_FOUND: unknown or expired question id");
    }
    json result;
    result["question"] = snapshot->to_json();
    return core::Result<json>::Ok(result);
}

// ============================================================================
// question.list
// ============================================================================
core::Result<json> method_question_list(std::shared_ptr<GatewayServer> server,
                                       std::shared_ptr<MethodGuard> method_guard,
                                       const ClientContext& client_ctx,
                                       const json& params) {
    (void)server; (void)method_guard; (void)client_ctx; (void)params;

    auto manager = question_manager_instance();
    if (!manager) return core::Result<json>::Err("question manager not available");

    json arr = json::array();
    for (const auto& r : manager->list_pending()) {
        arr.push_back(r.to_json());
    }
    json result;
    result["questions"] = std::move(arr);
    (void)now_ms();
    return core::Result<json>::Ok(result);
}

}  // namespace methods
}  // namespace gateway
}  // namespace hunterclaw
