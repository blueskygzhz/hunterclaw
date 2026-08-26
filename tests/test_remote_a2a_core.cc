// ============================================================================
// test_remote_a2a_core.cc
// Remote A2A 核心模块测试：types（M1）+ task_tracking / probe_policy（M6）
//
// 构建：
//   /usr/local/gcc-12.2.0/bin/g++ -std=c++17 -Iinclude \
//     -Iexternal/json/single_include -Ithird_party \
//     tests/test_remote_a2a_core.cc \
//     src/remote_a2a/types.cc src/remote_a2a/task_tracking.cc \
//     -o /tmp/test_remote_a2a_core && /tmp/test_remote_a2a_core
//
// ★ 标记 [不可回退] 的断言删掉就意味着引入对应缺陷，重构时不要动。
// ============================================================================

#include <iostream>
#include <string>

#include "hunterclaw/remote_a2a/probe_policy.h"
#include "hunterclaw/remote_a2a/task_tracking.h"
#include "hunterclaw/remote_a2a/types.h"

using namespace hunterclaw::remote_a2a;
using json = nlohmann::json;

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond, name)                                            \
    do {                                                             \
        if (cond) {                                                  \
            ++g_pass;                                                \
        } else {                                                     \
            ++g_fail;                                                \
            std::cout << "  FAIL: " << (name) << "  @" << __LINE__    \
                      << std::endl;                                  \
        }                                                            \
    } while (0)

#define SECTION(x) std::cout << "\n== " << x << " ==" << std::endl

// ============================================================================
// M1 types —— TaskState wire 编解码
// ============================================================================
static void test_task_state_wire() {
    SECTION("M1 TaskState wire");

    // [不可回退] 未知 state 必须显式失败。
    // 若兜底为 Working，远端 failed 会被当成还在跑，任务永久挂起。
    CHECK(!task_state_from_wire("bogus").has_value(), "未知 state 返回 nullopt");
    CHECK(!task_state_from_wire("").has_value(), "空串返回 nullopt");
    CHECK(!task_state_from_wire("   ").has_value(), "纯空白返回 nullopt");
    CHECK(!task_state_from_wire("workingg").has_value(), "近似串不匹配");

    // 三种 wire 变体
    CHECK(task_state_from_wire("input-required") == TaskState::InputRequired,
          "连字符写法");
    CHECK(task_state_from_wire("input_required") == TaskState::InputRequired,
          "下划线写法");
    CHECK(task_state_from_wire("TASK_STATE_INPUT_REQUIRED") == TaskState::InputRequired,
          "protobuf 写法");
    CHECK(task_state_from_wire("  WORKING  ") == TaskState::Working,
          "大小写+空白归一化");
    CHECK(task_state_from_wire("auth-required") == TaskState::AuthRequired,
          "auth-required");

    // to_wire 用连字符（A2A 规范）
    CHECK(std::string(task_state_to_wire(TaskState::InputRequired)) == "input-required",
          "to_wire 用连字符");
    CHECK(std::string(task_state_to_wire(TaskState::AuthRequired)) == "auth-required",
          "auth to_wire");

    // round-trip 全覆盖
    const TaskState all[] = {TaskState::Failed, TaskState::AuthRequired,
                             TaskState::InputRequired, TaskState::Canceled,
                             TaskState::Completed, TaskState::Working,
                             TaskState::Submitted};
    for (TaskState s : all) {
        CHECK(task_state_from_wire(task_state_to_wire(s)) == s, "round-trip");
    }
}

static void test_task_state_priority_order() {
    SECTION("M1 TaskState 枚举顺序");

    // [不可回退] classify() 依赖这个顺序表达优先级。改顺序会破坏优先级判定。
    CHECK(static_cast<int>(TaskState::Failed) <
              static_cast<int>(TaskState::AuthRequired),
          "Failed < AuthRequired");
    CHECK(static_cast<int>(TaskState::AuthRequired) <
              static_cast<int>(TaskState::InputRequired),
          "AuthRequired < InputRequired");
    CHECK(static_cast<int>(TaskState::InputRequired) <
              static_cast<int>(TaskState::Canceled),
          "InputRequired < Canceled");
}

static void test_is_terminal() {
    SECTION("M1 is_terminal");

    CHECK(is_terminal(TaskState::Completed), "completed 是终态");
    CHECK(is_terminal(TaskState::Failed), "failed 是终态");
    CHECK(is_terminal(TaskState::Canceled), "canceled 是终态");
    // input/auth-required 也是终态：本轮结束，等外部回填后新 message 续接
    CHECK(is_terminal(TaskState::InputRequired), "input-required 是终态（挂起）");
    CHECK(is_terminal(TaskState::AuthRequired), "auth-required 是终态（挂起）");
    CHECK(!is_terminal(TaskState::Working), "working 非终态");
    CHECK(!is_terminal(TaskState::Submitted), "submitted 非终态");
}

static void test_message_round_trip() {
    SECTION("M1 Message 序列化");

    Message m;
    m.message_id = "m-1";
    m.role = Role::User;
    m.context_id = "ctx-1";
    m.parts.push_back(Part::make_text("hello", json{{"is_user_input", true}}));
    m.parts.push_back(Part::make_data(json{{"name", "f"}, {"args", json::object()}},
                                      json{{meta_key("type"), TYPE_FUNCTION_CALL}}));

    const json j = to_json(m);
    CHECK(j["messageId"] == "m-1", "messageId 序列化");
    CHECK(j["role"] == "user", "role 序列化");
    CHECK(j["contextId"] == "ctx-1", "contextId 序列化");
    CHECK(j["parts"].size() == 2, "parts 数量");
    CHECK(j["parts"][0]["kind"] == "text", "text part kind");
    CHECK(j["parts"][1]["kind"] == "data", "data part kind");
    // taskId 为空时不应出现（避免发送空串给远端）
    CHECK(!j.contains("taskId"), "空 taskId 不序列化");

    const Message back = message_from_json(j);
    CHECK(back.message_id == "m-1", "messageId 反序列化");
    CHECK(back.role == Role::User, "role 反序列化");
    CHECK(back.parts.size() == 2, "parts 反序列化数量");
    CHECK(back.parts[0].kind == PartKind::Text, "text kind 反序列化");
    CHECK(back.parts[0].text == "hello", "text 内容");
    CHECK(back.parts[0].metadata["is_user_input"].get<bool>(), "is_user_input 保留");
    CHECK(back.parts[1].kind == PartKind::Data, "data kind 反序列化");

    // extract_function_calls
    CHECK(extract_function_calls(back.parts).size() == 1, "提取 1 个 function_call");
}

static void test_message_hostile_input() {
    SECTION("M1 恶意/畸形输入容错");

    // 远端数据不可信：所有畸形输入都不能抛异常
    CHECK(message_from_json(json::array()).parts.empty(), "数组输入不崩");
    CHECK(message_from_json(json(nullptr)).parts.empty(), "null 输入不崩");
    CHECK(message_from_json(json{{"role", 42}}).role == Role::User,
          "role 非字符串 → 默认 User");
    CHECK(message_from_json(json{{"parts", "notarray"}}).parts.empty(),
          "parts 非数组 → 空");
    CHECK(part_from_json(json{{"kind", "bogus"}}).kind == PartKind::Text,
          "未知 kind → 当 Text");
    CHECK(message_from_json(json{{"messageId", 123}}).message_id.empty(),
          "messageId 非字符串 → 空");

    // kind 缺失时按字段推断
    CHECK(part_from_json(json{{"text", "x"}}).kind == PartKind::Text,
          "无 kind 有 text → Text");
    CHECK(part_from_json(json{{"data", json::object()}}).kind == PartKind::Data,
          "无 kind 有 data → Data");
}

static void test_task_from_json() {
    SECTION("M1 Task 反序列化");

    const json j = json::parse(R"({
        "id": "t-1",
        "contextId": "ctx-1",
        "status": {
            "state": "working",
            "timestamp": "2026-08-26T03:00:00Z",
            "message": {"role":"agent","parts":[{"kind":"text","text":"半成品"}]}
        },
        "history": [{"role":"user","parts":[{"kind":"text","text":"问"}]}],
        "artifacts": [
            {"artifactId":"a1","parts":[{"kind":"text","text":"结果A"}]},
            {"artifactId":"a2","parts":[{"kind":"text","text":"结果B"}]}
        ]
    })");

    const Task t = task_from_json(j);
    CHECK(t.id == "t-1", "task id");
    CHECK(t.context_id == "ctx-1", "context id");
    CHECK(t.status.state == TaskState::Working, "status state");
    // [不可回退] timestamp 必须保留原串，ProgressFingerprint 依赖其逐字节稳定
    CHECK(t.status.timestamp == "2026-08-26T03:00:00Z", "timestamp 保留原串");
    CHECK(t.status.message.has_value(), "status.message 存在");
    CHECK(t.history.size() == 1, "history 数量");
    CHECK(t.artifacts.size() == 2, "artifacts 数量");

    // extract_result_text 拼接全部 artifact
    const std::string res = extract_result_text(t);
    CHECK(res.find("结果A") != std::string::npos, "结果A 在内");
    CHECK(res.find("结果B") != std::string::npos, "结果B 在内");

    // 空 status.message 归一化为无（対标 1.x protobuf 恒存在空 Message）
    const Task t2 = task_from_json(json::parse(
        R"({"id":"t2","status":{"state":"working","message":{"parts":[]}}})"));
    CHECK(!t2.status.message.has_value(), "空 message 归一化为 nullopt");

    // artifacts 为空时 extract_result_text 回落到 status.message
    const Task t3 = task_from_json(json::parse(
        R"({"id":"t3","status":{"state":"completed",
            "message":{"role":"agent","parts":[{"kind":"text","text":"仅状态消息"}]}}})"));
    CHECK(extract_result_text(t3) == "仅状态消息", "回落到 status.message");
}

static void test_parse_stream_event() {
    SECTION("M1 流式事件解析");

    // StatusUpdate
    const auto su = parse_stream_event(json::parse(R"({
        "taskId":"t1","contextId":"c1",
        "status":{"state":"working","timestamp":"T1",
                  "message":{"role":"agent","parts":[{"kind":"text","text":"进行中"}]}}
    })"));
    CHECK(su.kind == ParsedStreamEvent::Kind::StatusUpdate, "识别 StatusUpdate");
    CHECK(su.state == TaskState::Working, "state=working");
    CHECK(su.text == "进行中", "文本");
    CHECK(su.status_timestamp == "T1", "timestamp");
    CHECK(su.has_status_message, "has_status_message");

    // ArtifactUpdate —— [不可回退] 只取本次增量
    const auto au = parse_stream_event(json::parse(R"({
        "taskId":"t1",
        "artifact":{"artifactId":"a1","parts":[{"kind":"text","text":"片段1"}]},
        "lastChunk":false
    })"));
    CHECK(au.kind == ParsedStreamEvent::Kind::ArtifactUpdate, "识别 ArtifactUpdate");
    CHECK(au.text == "片段1", "只含本次增量文本");
    CHECK(!au.last_chunk, "lastChunk=false");
    CHECK(au.artifact_part_count == 1, "本次分片数");
    CHECK(au.state == TaskState::Working, "artifact 事件不改 state");

    // 完整 Task 快照
    const auto tk = parse_stream_event(json::parse(R"({
        "id":"t1","contextId":"c1",
        "status":{"state":"submitted","timestamp":"T0"},
        "history":[], "artifacts":[]
    })"));
    CHECK(tk.kind == ParsedStreamEvent::Kind::Task, "识别完整 Task");
    CHECK(tk.state == TaskState::Submitted, "submitted");
    CHECK(tk.task_id == "t1", "从 id 取 task_id");

    // 终态 failed
    const auto fl = parse_stream_event(json::parse(R"({
        "taskId":"t1","final":true,
        "status":{"state":"failed",
                  "message":{"role":"agent","parts":[{"kind":"text","text":"炸了"}]}}
    })"));
    CHECK(fl.state == TaskState::Failed, "failed");
    CHECK(fl.final, "final=true");
    CHECK(fl.error_text == "炸了", "error_text");

    // failed 但无消息 → 有兜底文案
    const auto fl2 = parse_stream_event(
        json::parse(R"({"taskId":"t1","status":{"state":"failed"}})"));
    CHECK(!fl2.error_text.empty(), "failed 无消息时有兜底 error_text");

    // 未知 state → 保持默认 Working，不崩
    const auto bad = parse_stream_event(
        json::parse(R"({"taskId":"t1","status":{"state":"???"}})"));
    CHECK(bad.state == TaskState::Working, "未知 state 保持默认");

    // 畸形输入
    CHECK(parse_stream_event(json::array()).kind == ParsedStreamEvent::Kind::Unknown,
          "数组 → Unknown");
}

static void test_sse_line_parsing() {
    SECTION("M1 SSE 帧解析");

    auto ok = parse_sse_data_line("data: {\"a\":1}");
    CHECK(ok.has_value() && (*ok)["a"] == 1, "标准 data 行");

    CHECK(parse_sse_data_line("data:{\"a\":2}").has_value(), "无空格 data 行");
    CHECK(parse_sse_data_line("data: {\"a\":3}\r").has_value(), "CRLF 行尾");

    // 非 data 行一律 nullopt
    CHECK(!parse_sse_data_line("").has_value(), "空行");
    CHECK(!parse_sse_data_line(": heartbeat").has_value(), "注释/心跳行");
    CHECK(!parse_sse_data_line("event: message").has_value(), "event 行");
    CHECK(!parse_sse_data_line("id: 42").has_value(), "id 行");
    CHECK(!parse_sse_data_line("retry: 1000").has_value(), "retry 行");
    CHECK(!parse_sse_data_line("data: [DONE]").has_value(), "[DONE] 标记");
    CHECK(!parse_sse_data_line("data: not-json").has_value(), "非法 JSON 静默跳过");
    CHECK(!parse_sse_data_line("data: ").has_value(), "空 payload");
}

// ============================================================================
// M6 ProgressFingerprint
// ============================================================================
static void test_fingerprint() {
    SECTION("M6 ProgressFingerprint");

    // [不可回退] 只比 state 不足以判推进 —— 卡死任务 state 永远是 working。
    // timestamp 变化必须算推进。
    ProgressFingerprint a;
    a.status_timestamp = "T1";
    a.history_count = 3;
    ProgressFingerprint b = a;
    b.status_timestamp = "T2";
    CHECK(a != b, "timestamp 变化算推进");

    ProgressFingerprint c = a;
    c.history_count = 4;
    CHECK(a != c, "history_count 变化算推进");

    ProgressFingerprint d = a;
    d.artifact_count = 1;
    CHECK(a != d, "artifact_count 变化算推进");

    ProgressFingerprint e = a;
    e.total_artifact_parts = 5;
    CHECK(a != e, "total_artifact_parts 变化算推进");

    ProgressFingerprint f = a;
    f.last_status_message_len = 99;
    CHECK(a != f, "message 长度变化算推进");

    ProgressFingerprint g = a;
    g.event_seq = 1;
    CHECK(a != g, "event_seq 变化算推进");

    CHECK(a == a, "自身相等");
    ProgressFingerprint h = a;
    CHECK(a == h, "拷贝相等");

    // from_task
    Task t;
    t.status.timestamp = "TS";
    t.history.resize(2);
    Artifact art;
    art.parts.push_back(Part::make_text("x"));
    art.parts.push_back(Part::make_text("y"));
    t.artifacts.push_back(art);
    const auto fp = ProgressFingerprint::from_task(t);
    CHECK(fp.status_timestamp == "TS", "from_task timestamp");
    CHECK(fp.history_count == 2, "from_task history");
    CHECK(fp.artifact_count == 1, "from_task artifact_count");
    CHECK(fp.total_artifact_parts == 2, "from_task parts 总数");
    CHECK(fp.event_seq == 0, "from_task 不动 event_seq");

    // bump_from_event 单调推进
    ProgressFingerprint sse;
    ParsedStreamEvent ev;
    ev.kind = ParsedStreamEvent::Kind::StatusUpdate;
    ev.status_timestamp = "T1";
    ev.has_status_message = true;
    ev.text = "abc";
    sse.bump_from_event(ev);
    CHECK(sse.event_seq == 1, "event_seq 推进");
    CHECK(sse.status_timestamp == "T1", "timestamp 同步");
    CHECK(sse.history_count == 1, "有消息则 history 增");

    // ★ 即使事件不带任何文本，也必须推进（远端活着的证据）
    ProgressFingerprint before = sse;
    ParsedStreamEvent empty_ev;
    empty_ev.kind = ParsedStreamEvent::Kind::StatusUpdate;
    sse.bump_from_event(empty_ev);
    CHECK(sse != before, "空事件也算推进");

    // artifact 增量累加
    ProgressFingerprint ar;
    ParsedStreamEvent a1;
    a1.kind = ParsedStreamEvent::Kind::ArtifactUpdate;
    a1.artifact_part_count = 1;
    a1.last_chunk = false;
    ar.bump_from_event(a1);
    ar.bump_from_event(a1);
    CHECK(ar.total_artifact_parts == 2, "artifact 分片累加");
    CHECK(ar.artifact_count == 0, "未 last_chunk 不计 artifact 数");
    a1.last_chunk = true;
    ar.bump_from_event(a1);
    CHECK(ar.artifact_count == 1, "last_chunk 计 artifact 数");
}

// ============================================================================
// M6 classify —— 优先级
// ============================================================================
static void test_classify_priority() {
    SECTION("M6 classify 优先级");

    // [不可回退] HardTimeout 压过一切。
    // 若优先级错，运维看到"远端失败"而真实原因是"我们等太久"。
    {
        TaskTracking t;
        t.remote_state = TaskState::Failed;
        t.consecutive_no_progress = 99;
        t.max_no_progress = 6;
        t.elapsed_ms = 999999;
        t.max_task_ms = 1000;
        CHECK(classify(t) == TerminalReason::HardTimeout,
              "HardTimeout 压过 Zombie 与 RemoteFailed");
    }
    // [不可回退] Zombie 压过 RemoteFailed
    {
        TaskTracking t;
        t.remote_state = TaskState::Failed;
        t.consecutive_no_progress = 6;
        t.max_no_progress = 6;
        t.elapsed_ms = 100;
        t.max_task_ms = 100000;
        CHECK(classify(t) == TerminalReason::Zombie, "Zombie 压过 RemoteFailed");
    }
    // RemoteFailed 压过 Canceled/ConnLost
    {
        TaskTracking t;
        t.remote_state = TaskState::Failed;
        t.transport_dead = true;
        t.last_err = "fatal protocol error";
        t.max_task_ms = 100000;
        CHECK(classify(t) == TerminalReason::RemoteFailed, "RemoteFailed 压过 ConnLost");
    }
    // Canceled
    {
        TaskTracking t;
        t.remote_state = TaskState::Canceled;
        t.max_task_ms = 100000;
        CHECK(classify(t) == TerminalReason::Canceled, "Canceled");
    }
    // ConnLost：不可恢复错误
    {
        TaskTracking t;
        t.remote_state = TaskState::Working;
        t.transport_dead = true;
        t.last_err = "certificate verify failed: self signed";
        t.max_task_ms = 100000;
        CHECK(classify(t) == TerminalReason::ConnLost, "不可恢复 → ConnLost");
    }
    // 可恢复的抖动不算终态
    {
        TaskTracking t;
        t.remote_state = TaskState::Working;
        t.transport_dead = true;
        t.last_err = "ECONNRESET";
        t.max_task_ms = 100000;
        CHECK(classify(t) == TerminalReason::Completed,
              "可恢复抖动不判 ConnLost（留给调用方重试）");
    }
    // 正常
    {
        TaskTracking t;
        t.remote_state = TaskState::Completed;
        t.max_task_ms = 100000;
        CHECK(classify(t) == TerminalReason::Completed, "正常完成");
    }
    // max_task_ms=0 视为不限时
    {
        TaskTracking t;
        t.elapsed_ms = 999999999;
        t.max_task_ms = 0;
        t.remote_state = TaskState::Working;
        CHECK(classify(t) != TerminalReason::HardTimeout, "max_task_ms=0 不限时");
    }
    // 边界：elapsed == max 不算超（严格大于才超）
    {
        TaskTracking t;
        t.elapsed_ms = 1000;
        t.max_task_ms = 1000;
        t.remote_state = TaskState::Working;
        CHECK(classify(t) != TerminalReason::HardTimeout, "等于不算超时");
    }
}

static void test_sticky() {
    SECTION("M6 粘性终态");

    // [不可回退] 対标 run_wait.h:105-108
    CHECK(is_sticky(TerminalReason::HardTimeout), "HardTimeout 粘性");
    CHECK(is_sticky(TerminalReason::Canceled), "Canceled 粘性");
    CHECK(!is_sticky(TerminalReason::RemoteFailed), "RemoteFailed 非粘性");
    CHECK(!is_sticky(TerminalReason::Zombie), "Zombie 非粘性");
    CHECK(!is_sticky(TerminalReason::ConnLost), "ConnLost 非粘性");
    CHECK(!is_sticky(TerminalReason::Completed), "Completed 非粘性");
}

static void test_reason_to_state() {
    SECTION("M6 TerminalReason → TaskState");

    CHECK(terminal_reason_to_task_state(TerminalReason::HardTimeout) == TaskState::Failed,
          "HardTimeout → Failed");
    CHECK(terminal_reason_to_task_state(TerminalReason::Zombie) == TaskState::Failed,
          "Zombie → Failed");
    CHECK(terminal_reason_to_task_state(TerminalReason::RemoteFailed) == TaskState::Failed,
          "RemoteFailed → Failed");
    CHECK(terminal_reason_to_task_state(TerminalReason::ConnLost) == TaskState::Failed,
          "ConnLost → Failed");
    CHECK(terminal_reason_to_task_state(TerminalReason::Canceled) == TaskState::Canceled,
          "Canceled → Canceled");
    CHECK(terminal_reason_to_task_state(TerminalReason::Completed) == TaskState::Completed,
          "Completed → Completed");
}

static void test_classify_working() {
    SECTION("M6 working 四态");

    TaskTracking t;
    t.idle_timeout_ms = 120000;
    t.max_no_progress = 6;

    t.idle_ms = 1000;
    CHECK(classify_working(t, false) == WorkingSubState::Fresh, "未超 idle → Fresh");

    t.idle_ms = 130000;
    CHECK(classify_working(t, true) == WorkingSubState::Stale,
          "超 idle 但有推进 → Stale");
    CHECK(classify_working(t, false) == WorkingSubState::Suspect,
          "超 idle 且无推进 → Suspect");

    t.consecutive_no_progress = 6;
    CHECK(classify_working(t, false) == WorkingSubState::Zombie,
          "达上限 → Zombie");
    CHECK(classify_working(t, true) == WorkingSubState::Zombie,
          "达上限即 Zombie（不因单次推进翻转）");

    // idle_timeout=0 视为不检查
    TaskTracking t2;
    t2.idle_timeout_ms = 0;
    t2.idle_ms = 999999;
    CHECK(classify_working(t2, false) == WorkingSubState::Fresh,
          "idle_timeout=0 → 恒 Fresh");
}

static void test_should_keep_waiting() {
    SECTION("M6 should_keep_waiting");

    TaskTracking t;
    t.remote_state = TaskState::Working;
    t.max_task_ms = 100000;
    CHECK(should_keep_waiting(t), "working 继续等");

    t.remote_state = TaskState::Completed;
    CHECK(!should_keep_waiting(t), "completed 不等");

    t.remote_state = TaskState::InputRequired;
    CHECK(!should_keep_waiting(t), "input-required 不等（挂起也是终态）");

    t.remote_state = TaskState::Working;
    t.elapsed_ms = 999999;
    t.max_task_ms = 1000;
    CHECK(!should_keep_waiting(t), "硬超时不等");
}

// ============================================================================
// M6 传输错误
// ============================================================================
static void test_transport_errors() {
    SECTION("M6 传输错误可恢复性");

    CHECK(is_recoverable_transport_error("ECONNRESET"), "ECONNRESET 可恢复");
    CHECK(is_recoverable_transport_error("read ECONNRESET"), "含 errno 可恢复");
    CHECK(is_recoverable_transport_error("socket hang up"), "socket hang up");
    CHECK(is_recoverable_transport_error("HTTP 503 Service Unavailable"), "503");
    // ⚠ 与 run_wait 刻意不同：HTTP 504 是上游抖动，可恢复
    CHECK(is_recoverable_transport_error("HTTP 504 Gateway Time-out"),
          "504 可恢复（与 run_wait 的 gateway timeout 语义不同）");
    CHECK(is_recoverable_transport_error("upstream connect error"), "upstream 错误");
    CHECK(is_recoverable_transport_error("429 Too Many Requests"), "429");
    CHECK(is_recoverable_transport_error("EAI_AGAIN"), "DNS 临时失败");
    CHECK(is_recoverable_transport_error("  econnrefused  "), "trim + 小写归一");

    CHECK(!is_recoverable_transport_error(""), "空串不可恢复");
    CHECK(!is_recoverable_transport_error("   "), "纯空白不可恢复");
    CHECK(!is_recoverable_transport_error("401 Unauthorized"), "401 不可恢复");
    CHECK(!is_recoverable_transport_error("403 Forbidden"), "403 不可恢复");
    CHECK(!is_recoverable_transport_error("invalid agent card"), "业务错误不可恢复");
}

// ============================================================================
// M6 ProbePolicy
// ============================================================================
static void test_probe_policy() {
    SECTION("M6 ProbePolicy 退避");

    ProbePolicy p;
    CHECK(p.interval_for(0) == 5000, "第 0 次 5s");
    CHECK(p.interval_for(1) == 10000, "第 1 次 10s");
    CHECK(p.interval_for(2) == 20000, "第 2 次 20s");
    CHECK(p.interval_for(3) == 40000, "第 3 次 40s");
    CHECK(p.interval_for(4) == 60000, "第 4 次钳到 60s");
    CHECK(p.interval_for(5) == 60000, "第 5 次仍 60s");

    // [不可回退] 大 n 不能因移位溢出变负或异常
    CHECK(p.interval_for(20) == 60000, "n=20 不溢出");
    CHECK(p.interval_for(63) == 60000, "n=63 不溢出");
    CHECK(p.interval_for(1000) == 60000, "n=1000 不溢出");
    CHECK(p.interval_for(-1) == 5000, "负数按 0 处理");

    // 总预算：5+10+20+40+60+60 = 195s
    CHECK(p.total_probe_budget_ms() == 195000, "6 次总预算 195s");
}

// ============================================================================
int main() {
    std::cout << "=== Remote A2A 核心模块测试 ===" << std::endl;

    test_task_state_wire();
    test_task_state_priority_order();
    test_is_terminal();
    test_message_round_trip();
    test_message_hostile_input();
    test_task_from_json();
    test_parse_stream_event();
    test_sse_line_parsing();

    test_fingerprint();
    test_classify_priority();
    test_sticky();
    test_reason_to_state();
    test_classify_working();
    test_should_keep_waiting();
    test_transport_errors();
    test_probe_policy();

    std::cout << "\n===============================" << std::endl;
    std::cout << "PASS: " << g_pass << "  FAIL: " << g_fail << std::endl;
    return g_fail == 0 ? 0 : 1;
}
