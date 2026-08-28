// ============================================================================
// remote_a2a/agent_tool.cc
// 远端 A2A agent → HunterClaw Tool 适配。
// ============================================================================

#include "hunterclaw/remote_a2a/agent_tool.h"

#include <algorithm>
#include <cctype>
#include <sstream>

#include "hunterclaw/infra/system_events.h"

namespace hunterclaw {
namespace remote_a2a {

namespace {

std::string to_lower_copy(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

/// 対标 mcp/bundle_mcp_names.cc:23-44 的 sanitize_tool_fragment
std::string sanitize_fragment(const std::string& raw, const std::string& fallback) {
    std::string s = raw;
    while (!s.empty() && s.front() == ' ') s.erase(s.begin());
    while (!s.empty() && s.back() == ' ') s.pop_back();

    std::string cleaned;
    cleaned.reserve(s.size());
    for (unsigned char c : s) {
        const bool safe = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                          (c >= '0' && c <= '9') || c == '_' || c == '-';
        cleaned.push_back(safe ? static_cast<char>(c) : '-');
    }
    // 折叠连续 '-' 并去掉首尾 '-'（避免 "remote_a2a__---" 这种名字）
    std::string folded;
    for (char c : cleaned) {
        if (c == '-' && !folded.empty() && folded.back() == '-') continue;
        folded.push_back(c);
    }
    while (!folded.empty() && folded.front() == '-') folded.erase(folded.begin());
    while (!folded.empty() && folded.back() == '-') folded.pop_back();

    return folded.empty() ? fallback : folded;
}

}  // namespace

// ── 工具名 ──────────────────────────────────────────────────────────────────

std::string safe_remote_a2a_tool_name(const std::string& agent_name,
                                     std::vector<std::string>& reserved) {
    const std::string prefix = TOOL_PREFIX;
    const size_t budget =
        (TOOL_NAME_MAX_TOTAL > prefix.size()) ? (TOOL_NAME_MAX_TOTAL - prefix.size()) : 1;

    std::string base = sanitize_fragment(agent_name, "agent");
    if (base.size() > budget) base = base.substr(0, budget);

    std::string candidate = prefix + base;
    int n = 2;
    // ⚠ 去重后缀基于传入顺序而非哈希/时间：同一份配置每次启动必须得到
    //   同样的工具名，否则 transcript 里历史 tool_call 的名字会对不上。
    auto taken = [&](const std::string& c) {
        const std::string lc = to_lower_copy(c);
        for (const auto& r : reserved) {
            if (to_lower_copy(r) == lc) return true;
        }
        return false;
    };
    while (taken(candidate)) {
        const std::string suffix = "-" + std::to_string(n);
        size_t keep = (budget > suffix.size()) ? (budget - suffix.size()) : 1;
        candidate = prefix + base.substr(0, std::min(keep, base.size())) + suffix;
        ++n;
        if (n > 200) break;  // 防病态配置死循环
    }
    reserved.push_back(candidate);
    return candidate;
}

// ── Message 构造 ────────────────────────────────────────────────────────────

Message build_user_message(const std::string& text, const std::string& task_id,
                          const std::string& context_id) {
    Message m;
    m.role = Role::User;
    // ★ messageId 被远端用于去重，必须【全局唯一】。
    //   ⚠ 不能用对象地址（栈地址会复用，两条不同消息可能撞 id，
    //     远端会把第二条当重复投递直接丢弃）。用进程内单调计数。
    static std::atomic<uint64_t> seq{1};
    m.message_id = "hc-msg-" + std::to_string(seq.fetch_add(1));
    nlohmann::json meta;
    meta["is_user_input"] = true;
    m.parts.push_back(Part::make_text(text, meta));
    m.task_id = task_id;
    m.context_id = context_id;
    return m;
}

// ── 归属校验 ────────────────────────────────────────────────────────────────

std::string check_handle_ownership(const PendingTaskStore& store,
                                  const std::string& handle,
                                  const std::string& session_key) {
    // ⚠ const 方法里不能直接用非 const 的 get()，这里用 list_by_session 反查。
    //   代价是 O(n)，但 n 是在途任务数（上限 256），可接受。
    if (handle.empty()) return "handle 为空";
    const auto mine = store.list_by_session(session_key);
    for (const auto& t : mine) {
        if (t.handle == handle) return {};
    }
    // ★ 不区分「不存在」与「属于别人」——
    //   区分开会变成一个「探测其他会话是否有某 handle」的探针。
    return "未找到该 handle 对应的远端任务（可能已过期、进程已重启，或不属于当前会话）";
}

// ── 进度投递 ────────────────────────────────────────────────────────────────

void deliver_progress(const std::string& session_key, const std::string& agent_name,
                     const std::string& text, bool is_heartbeat) {
    if (session_key.empty() || text.empty()) return;

    // 截断：远端可控文本，不能整段塞进 prompt
    std::string body = text;
    if (body.size() > 300) body = body.substr(0, 300) + "...";

    const std::string line = "[remote-a2a:" + agent_name + "] " + body;

    // ★ context_key 让同一来源的连续心跳被视为同一上下文。
    //   system_events.cc:86-88 会跳过连续重复文本；不同文本仍会入队，
    //   而队列上限是 20（system_events.cc:18）——
    //   心跳全部入队会把真正的事件（任务完成、失败）挤掉。
    const std::string ctx =
        is_heartbeat ? ("remote-a2a-heartbeat:" + agent_name) : ("remote-a2a:" + agent_name);
    try {
        infra::enqueue_system_event(line, session_key, ctx);
    } catch (const std::exception&) {
        // enqueue_system_event 在 session_key 全空白时抛异常
        // （system_events.cc:31-38）。进度投递失败绝不能影响任务本身。
    }
}

// ── 发送工具 ────────────────────────────────────────────────────────────────

RemoteA2aAgentTool::RemoteA2aAgentTool(std::string tool_name, std::string agent_name,
                                       std::shared_ptr<RemoteAgentClient> client,
                                       std::shared_ptr<ExecutionEngine> engine,
                                       const AgentToolPolicy& policy,
                                       const tools::ToolContext& ctx)
    : tool_name_(std::move(tool_name)),
      agent_name_(std::move(agent_name)),
      client_(std::move(client)),
      engine_(std::move(engine)),
      policy_(policy),
      // ★ 只从 ToolContext 取。params 由模型构造，允许它指定 session_key
      //   等于让模型把任务挂到别人的会话上。
      session_key_(ctx.session_key) {}

void RemoteA2aAgentTool::warm_up() {
    if (!client_) return;
    auto card = client_->ensure_card();
    if (card.is_err()) return;  // 失败不阻断注册，调用时再报错
    std::lock_guard<std::mutex> lk(desc_mu_);
    cached_description_ = build_description(card.get());
}

std::string RemoteA2aAgentTool::build_description(const AgentCard& card) const {
    std::ostringstream ss;
    ss << "调用远端 A2A agent \"" << agent_name_ << "\"。";
    // ★ card.description 已由 card_resolver 的 sanitize 阶段处理过
    //   （剥控制字符、过滤注入模式、UTF-8 安全截断）。此处只控总长。
    if (!card.description.empty()) ss << " " << card.description;

    if (policy_.include_skills_in_description && !card.skills.empty()) {
        ss << "\n可用能力:";
        // skills 条数已由 sanitize_agent_card 裁到 max_skills（默认 8）
        for (const auto& s : card.skills) {
            ss << "\n- " << (s.name.empty() ? s.id : s.name);
            if (!s.description.empty()) ss << ": " << s.description;
        }
    }
    ss << "\n长任务会在等待窗口结束后返回 handle，用 " << TOOL_STATUS << " 跟进。";

    std::string out = ss.str();
    if (out.size() > policy_.max_description_chars) {
        out = out.substr(0, policy_.max_description_chars) + "...";
    }
    return out;
}

std::string RemoteA2aAgentTool::description() const {
    {
        std::lock_guard<std::mutex> lk(desc_mu_);
        if (!cached_description_.empty()) return cached_description_;
    }
    // ⚠ description() 会在组装 prompt 时被频繁调用，是热路径。
    //   这里【不】触发拉卡（会把网络 IO 放进 prompt 组装），只给兜底文案。
    //   卡由 warm_up() 或首次 execute() 负责加载。
    return "调用远端 A2A agent \"" + agent_name_ + "\"（能力清单尚未加载）。";
}

tools::Tool::Schema RemoteA2aAgentTool::parameters() const {
    Schema s;
    s.type = "object";
    s.description = "向远端 A2A agent 发送一条消息";

    auto prop = [](const char* type, const char* desc) {
        nlohmann::json j;
        j["type"] = type;
        j["description"] = desc;
        return core::JsonValue(j);
    };

    s.properties["message"] = prop("string", "要发送给远端 agent 的内容");
    s.properties["task_id"] =
        prop("string",
             "续接已有任务时传入（远端返回 input-required 后补充输入的场景）");
    s.properties["context_id"] = prop("string", "续接同一会话上下文时传入");
    s.properties["yield_ms"] =
        prop("number", "等待窗口毫秒数（10-120000）。超时后返回 handle 稍后跟进");
    s.properties["background"] =
        prop("boolean", "立即返回 handle，不等待结果");
    s.required.push_back("message");
    // ★ 刻意【不】提供 session_key 参数 —— 见文件头说明
    return s;
}

tools::ToolResult RemoteA2aAgentTool::execute(const core::JsonValue& params) {
    if (!client_ || !engine_) {
        return tools::tool_result::error("remote_a2a 工具未正确初始化");
    }

    tools::ToolParams p(params);
    const std::string message = p.read_string("message", false, "");
    if (message.empty()) {
        return tools::tool_result::error("参数 message 不能为空");
    }
    const std::string task_id = p.read_string("task_id", false, "");
    const std::string context_id = p.read_string("context_id", false, "");

    // 首次调用时补拉卡（description() 不做网络 IO，故这里必须做）
    {
        bool need_card = false;
        {
            std::lock_guard<std::mutex> lk(desc_mu_);
            need_card = cached_description_.empty();
        }
        // ⚠ 必须在锁外做网络 IO：ensure_card() 会发 HTTP，
        //   持锁期间 description() 会被 prompt 组装线程阻塞数秒。
        if (need_card) {
            auto card = client_->ensure_card();
            if (card.is_err()) {
                return tools::tool_result::error("远端 AgentCard 不可用: " + card.error);
            }
            const std::string desc = build_description(card.get());
            std::lock_guard<std::mutex> lk(desc_mu_);
            if (cached_description_.empty()) cached_description_ = desc;
        }
    }

    Message msg = build_user_message(message, task_id, context_id);

    const std::string agent = agent_name_;
    const std::string sess = session_key_;
    ProgressFn on_progress = [agent, sess](const std::string& text, bool hb) {
        deliver_progress(sess, agent, text, hb);
    };

    // yield_ms / background 由模型按需给出；clamp 在引擎内完成
    const bool background = p.read_bool("background", false, false);
    const bool has_yield = p.has("yield_ms");
    const int64_t yield_ms =
        has_yield ? static_cast<int64_t>(p.read_number("yield_ms", false, 0)) : 0;

    ExecutionResult r =
        (background || has_yield)
            ? engine_->execute_with_yield(msg, session_key_, yield_ms, background,
                                         on_progress)
            : engine_->execute(msg, session_key_, on_progress);

    nlohmann::json details;
    details["remoteA2aAgent"] = agent_name_;
    details["branch"] = exec_branch_text(r.branch);
    details["taskId"] = r.task_id;
    details["contextId"] = r.context_id;
    details["handle"] = r.handle;
    details["state"] = task_state_to_wire(r.state);
    details["yielded"] = r.yielded;
    details["completed"] = r.completed;
    // ★ 挂起态（input-required / auth-required）单独标出，供程序化消费方
    //   （二期的「二次确认」终端流程）判断是否需要拉起补输入的 UI。
    //   ⚠ details 不进 prompt（react_engine.cc:1076-1088 只读 content），
    //     所以模型看不到它 —— 模型所需的信息全在 r.render() 的文本里。
    details["suspended"] = r.is_suspended();
    if (r.reason) details["terminalReason"] = terminal_reason_text(*r.reason);

    if (!r.success) {
        auto res = tools::tool_result::error(
            r.error_text.empty() ? "远端 A2A 调用失败" : r.error_text);
        res.details = core::JsonValue(details);
        return res;
    }

    // ★ 让渡与完成【都是】 success=true（対标 exec.cc:983 tool_result::text）。
    //   让渡返回 error 会让模型认为工具坏了并重试 → 同一远端任务被起两遍。
    auto res = tools::tool_result::text(r.render());
    res.details = core::JsonValue(details);
    // 远端明确失败时才置 success=false（让渡不算失败）。
    //
    // ★★ 挂起态【绝不】置 success=false —— 即使 error_text 因某条路径
    //   被意外填了值。input-required 不是失败，而是一次正常的往返中断；
    //   置 false 会让模型认为工具坏了并重试，而重试用的是同样的输入，
    //   于是远端上堆积一串永远等不到输入的僵尸任务（每个都占着配额）。
    if (r.completed && !r.error_text.empty() && !r.is_suspended()) {
        res.success = false;
        res.error = r.error_text;
    }
    return res;
}

// ── status 工具 ─────────────────────────────────────────────────────────────

RemoteA2aStatusTool::RemoteA2aStatusTool(std::shared_ptr<PendingTaskStore> store,
                                         EngineLookupFn lookup,
                                         const tools::ToolContext& ctx)
    : store_(std::move(store)), lookup_(std::move(lookup)), session_key_(ctx.session_key) {}

std::string RemoteA2aStatusTool::description() const {
    return "查询/跟进已让渡的远端 A2A 任务。不带 handle 时列出当前会话的全部在途任务。";
}

tools::Tool::Schema RemoteA2aStatusTool::parameters() const {
    Schema s;
    s.type = "object";
    s.description = "按 handle 跟进远端 A2A 任务";

    auto prop = [](const char* type, const char* desc) {
        nlohmann::json j;
        j["type"] = type;
        j["description"] = desc;
        return core::JsonValue(j);
    };
    s.properties["handle"] =
        prop("string", "任务句柄（发送工具返回）。省略则列出本会话全部在途任务");
    s.properties["wait_ms"] =
        prop("number", "最多再等待多少毫秒（0 = 只看当前状态，上限 120000）");
    return s;
}

tools::ToolResult RemoteA2aStatusTool::execute(const core::JsonValue& params) {
    if (!store_) return tools::tool_result::error("remote_a2a 未启用");

    tools::ToolParams p(params);
    const std::string handle = p.read_string("handle", false, "");

    // 不带 handle → 列出本会话在途任务
    if (handle.empty()) {
        const auto list = store_->list_by_session(session_key_);
        if (list.empty()) return tools::tool_result::text("当前会话没有在途的远端 A2A 任务。");
        std::ostringstream ss;
        ss << "当前会话的远端 A2A 任务（" << list.size() << " 项）:";
        for (const auto& t : list) {
            ss << "\n- " << t.handle << "  agent=" << t.agent_name
               << "  state=" << task_state_to_wire(t.state)
               << "  phase=" << pending_phase_text(t.phase);
            if (t.reason) ss << "  reason=" << terminal_reason_text(*t.reason);
        }
        return tools::tool_result::text(ss.str());
    }

    // ★ 归属校验：handle 出现在工具返回文本里，模型可能把它带到别的会话。
    //   不校验则 A 会话能读走 B 会话的任务结果。
    const std::string deny = check_handle_ownership(*store_, handle, session_key_);
    if (!deny.empty()) return tools::tool_result::error(deny);

    int64_t wait_ms = static_cast<int64_t>(p.read_number("wait_ms", false, 0));
    // ⚠ 必须钳制：模型可能传 3600000，那会把整个 agent 线程挂住一小时。
    wait_ms = std::max<int64_t>(0, std::min<int64_t>(wait_ms, 120000));

    auto engine = lookup_ ? lookup_(handle) : nullptr;
    if (!engine) {
        // 引擎不可用（远端已被移除）→ 退化为只读当前状态，不阻塞
        auto t = store_->get(handle);
        if (!t) return tools::tool_result::error("任务记录已不存在");
        std::ostringstream ss;
        ss << "任务 " << handle << " state=" << task_state_to_wire(t->state)
           << " phase=" << pending_phase_text(t->phase)
           << "（对应远端已不在配置中，无法继续跟进）";
        if (!t->result_text.empty()) ss << "\n已获得的结果: " << t->result_text;
        return tools::tool_result::text(ss.str());
    }

    ExecutionResult r = engine->follow_up(handle, wait_ms);

    nlohmann::json details;
    details["handle"] = handle;
    details["taskId"] = r.task_id;
    details["contextId"] = r.context_id;
    details["state"] = task_state_to_wire(r.state);
    details["completed"] = r.completed;
    details["yielded"] = r.yielded;
    details["suspended"] = r.is_suspended();
    if (r.reason) details["terminalReason"] = terminal_reason_text(*r.reason);

    if (!r.success) {
        auto res = tools::tool_result::error(r.error_text);
        res.details = core::JsonValue(details);
        return res;
    }
    auto res = tools::tool_result::text(r.render());
    res.details = core::JsonValue(details);
    // ★★ 与发送工具一致：挂起态绝不置 success=false。
    //   follow_up 恰恰是 input-required 最常见的入口（让渡后模型来取结果），
    //   在这里误判失败的后果与发送路径相同 —— 模型重试 → 远端堆僵尸任务。
    if (r.completed && !r.error_text.empty() && !r.is_suspended()) {
        res.success = false;
        res.error = r.error_text;
    }
    return res;
}

// ── cancel 工具 ─────────────────────────────────────────────────────────────

RemoteA2aCancelTool::RemoteA2aCancelTool(std::shared_ptr<PendingTaskStore> store,
                                         EngineLookupFn lookup,
                                         const tools::ToolContext& ctx,
                                         bool requires_approval)
    : store_(std::move(store)),
      lookup_(std::move(lookup)),
      session_key_(ctx.session_key),
      requires_approval_(requires_approval) {}

std::string RemoteA2aCancelTool::description() const {
    return "取消一个在途的远端 A2A 任务。注意：远端可能不会真正中断执行，"
           "本地会立即标记为已取消。";
}

tools::Tool::Schema RemoteA2aCancelTool::parameters() const {
    Schema s;
    s.type = "object";
    s.description = "按 handle 取消远端 A2A 任务";
    nlohmann::json j;
    j["type"] = "string";
    j["description"] = "任务句柄";
    s.properties["handle"] = core::JsonValue(j);
    s.required.push_back("handle");
    return s;
}

tools::ToolResult RemoteA2aCancelTool::execute(const core::JsonValue& params) {
    if (!store_) return tools::tool_result::error("remote_a2a 未启用");

    tools::ToolParams p(params);
    const std::string handle = p.read_string("handle", false, "");
    if (handle.empty()) return tools::tool_result::error("参数 handle 不能为空");

    const std::string deny = check_handle_ownership(*store_, handle, session_key_);
    if (!deny.empty()) return tools::tool_result::error(deny);

    auto engine = lookup_ ? lookup_(handle) : nullptr;
    if (!engine) {
        // ★ 引擎不可用时仍要本地标记取消 —— 否则任务会一直挂在在途列表里，
        //   且占着容量（register_task 会因此拒绝新任务）。
        store_->update_terminal(handle, TerminalReason::Canceled, "",
                               "已由本地取消（远端已不在配置中）");
        return tools::tool_result::text("任务 " + handle +
                                       " 已在本地标记为取消（无法通知远端）。");
    }

    auto r = engine->cancel(handle);
    if (r.is_err()) return tools::tool_result::error(r.error);

    nlohmann::json details;
    details["handle"] = handle;
    details["remoteAcked"] = r.get();
    auto res = tools::tool_result::text(
        r.get() ? ("任务 " + handle + " 已取消（远端已确认）。")
                : ("任务 " + handle +
                   " 已在本地标记为取消；远端未确认，可能仍在继续执行。"));
    res.details = core::JsonValue(details);
    return res;
}

}  // namespace remote_a2a
}  // namespace hunterclaw
