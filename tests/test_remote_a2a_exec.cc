// ============================================================================
// test_remote_a2a_exec.cc
// M8 pending_tasks + M7 execution_engine 测试
//
// 构建：
//   /usr/local/gcc-12.2.0/bin/g++ -std=c++17 -Iinclude \
//     -Iexternal/json/single_include -Ithird_party \
//     tests/test_remote_a2a_exec.cc \
//     src/remote_a2a/types.cc src/remote_a2a/task_tracking.cc \
//     src/remote_a2a/pending_tasks.cc src/remote_a2a/execution_engine.cc \
//     src/remote_a2a/json_rpc.cc src/remote_a2a/agent_card.cc \
//     src/remote_a2a/card_sanitizer.cc src/remote_a2a/card_validator.cc \
//     src/remote_a2a/url_safety.cc src/remote_a2a/auth.cc \
//     src/remote_a2a/credential_resolver.cc src/remote_a2a/card_resolver.cc \
//     src/remote_a2a/remote_agent_client.cc src/infra/http_client.cc \
//     -lpthread -o /tmp/test_remote_a2a_exec && /tmp/test_remote_a2a_exec
//
// ★ 全部离线：不发一个网络包。
// ============================================================================

#include <chrono>
#include <iostream>
#include <string>
#include <thread>

#include "hunterclaw/remote_a2a/execution_engine.h"
#include "hunterclaw/remote_a2a/pending_tasks.h"

using namespace hunterclaw::remote_a2a;
using json = nlohmann::json;

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond, name)                                          \
    do {                                                           \
        if (cond) {                                                \
            ++g_pass;                                              \
        } else {                                                   \
            ++g_fail;                                              \
            std::cout << "  FAIL: " << (name) << "  @" << __LINE__  \
                      << std::endl;                                \
        }                                                          \
    } while (0)

#define SECTION(x) std::cout << "\n== " << x << " ==" << std::endl

// ============================================================================
// pending_tasks —— 基本登记
// ============================================================================
static void test_register() {
    SECTION("任务登记");

    auto s = PendingTaskStore::create();
    auto h = s->register_task("agentA", "task-1", "ctx-1", "sess-1");
    CHECK(h.is_ok(), "登记成功");
    const std::string handle = h.get();
    CHECK(!handle.empty(), "句柄非空");
    CHECK(handle.rfind("ra2a-", 0) == 0, "句柄前缀");

    // ★ 句柄不能是纯序号 —— 回调端点是半公开的，纯序号可被枚举
    CHECK(handle != "ra2a-1", "句柄含随机后缀 [不可回退]");

    auto t = s->get(handle);
    CHECK(t.has_value(), "可按句柄取回");
    CHECK(t->agent_name == "agentA" && t->task_id == "task-1", "字段透传");
    CHECK(t->phase == PendingPhase::Running, "初始为 Running");
    CHECK(!t->is_terminal(), "初始非终态");
    CHECK(s->running_count() == 1, "在途计数");

    // 按 task_id 反查（★ 回调只带 task_id，没有我们的句柄）
    auto rev = s->handle_of_task("task-1");
    CHECK(rev.has_value() && *rev == handle, "task_id 反查句柄 [不可回退]");
    CHECK(!s->handle_of_task("nope").has_value(), "不存在的 task_id 反查为空");

    // ★ 空 task_id 必须拒绝（否则索引里会有个空键，后续反查全乱）
    CHECK(s->register_task("a", "", "c", "s").is_err(), "空 task_id 被拒 [不可回退]");

    // 句柄唯一
    auto h2 = s->register_task("agentA", "task-2", "", "sess-1");
    CHECK(h2.is_ok() && h2.get() != handle, "句柄唯一");

    // 会话过滤
    s->register_task("agentB", "task-3", "", "sess-2");
    CHECK(s->list_by_session("sess-1").size() == 2, "按会话过滤");
    CHECK(s->list_by_session("sess-2").size() == 1, "按会话过滤 2");
    CHECK(s->list_by_session("nope").empty(), "不存在会话为空");
}

// ============================================================================
// pending_tasks —— 容量与淘汰
// ============================================================================
static void test_capacity() {
    SECTION("容量与淘汰");

    PendingStoreConfig cfg;
    cfg.max_entries = 3;
    auto s = PendingTaskStore::create(cfg);

    auto a = s->register_task("x", "t1", "", "s");
    auto b = s->register_task("x", "t2", "", "s");
    auto c = s->register_task("x", "t3", "", "s");
    CHECK(a.is_ok() && b.is_ok() && c.is_ok(), "填满容量");

    // ★ [不可回退] 全部在途时必须【拒绝】新任务，绝不淘汰在途任务。
    //   被淘汰的在途任务会永远等不到结果，而调用方还在 wait_terminal 上等它。
    auto d = s->register_task("x", "t4", "", "s");
    CHECK(d.is_err(), "全在途时拒绝新任务 [不可回退]");
    CHECK(s->running_count() == 3, "在途任务未被淘汰 [不可回退]");

    // 终结并取走一个 → 可淘汰
    s->update_terminal(a.get(), TerminalReason::Completed, "done", "");
    s->harvest(a.get());
    auto e = s->register_task("x", "t5", "", "s");
    CHECK(e.is_ok(), "有 Harvested 可淘汰时登记成功");
    CHECK(!s->get(a.get()).has_value(), "Harvested 记录被淘汰");

    // 仅 Terminal（未超 TTL、未取走）时仍不淘汰
    {
        PendingStoreConfig c2;
        c2.max_entries = 2;
        c2.terminal_ttl_ms = 3600000;  // 很长
        auto s2 = PendingTaskStore::create(c2);
        auto x = s2->register_task("x", "t1", "", "s");
        s2->register_task("x", "t2", "", "s");
        s2->update_terminal(x.get(), TerminalReason::Completed, "", "");
        // ★ Terminal 但未超 TTL、未被取走 → 结果还没人拿，不能淘汰
        CHECK(s2->register_task("x", "t3", "", "s").is_err(),
              "未取走的 Terminal 不被淘汰 [不可回退]");
    }
}

// ============================================================================
// pending_tasks —— 进度与指纹
// ============================================================================
static void test_progress() {
    SECTION("进度与指纹");

    auto s = PendingTaskStore::create();
    auto h = s->register_task("x", "t1", "c1", "s1").get();

    ProgressFingerprint fp1;
    fp1.status_timestamp = "2026-01-01T00:00:00Z";
    fp1.history_count = 1;

    CHECK(s->note_progress(h, fp1, TaskState::Working, "step 1"), "首次推进");
    auto t = s->get(h);
    CHECK(t->state == TaskState::Working, "state 更新");
    // ★ [不可回退] 中间态只进 progress_text，不进 result_text ——
    //   否则「思考中...」会混进最终答案
    CHECK(t->progress_text == "step 1", "中间态进 progress_text");
    CHECK(t->result_text.empty(), "中间态不进 result_text [不可回退]");

    // 同指纹 → 无推进
    CHECK(!s->note_progress(h, fp1, TaskState::Working, "step 1"), "同指纹判无推进");

    // ★ [不可回退] 只有 state 相同不代表无推进 ——
    //   卡死任务的 state 永远是 working，正常输出中的任务 state 也是 working
    ProgressFingerprint fp2 = fp1;
    fp2.total_artifact_parts = 3;
    CHECK(s->note_progress(h, fp2, TaskState::Working, ""),
          "state 不变但指纹变仍判推进 [不可回退]");

    // artifact 增量
    s->append_result(h, "part A");
    s->append_result(h, "part B");
    t = s->get(h);
    CHECK(t->result_text.find("part A") != std::string::npos, "增量 A 已累积");
    CHECK(t->result_text.find("part B") != std::string::npos, "增量 B 已累积");
    // ★ 只累增量而非累积值（ADK remote_a2a_agent.py:1327-1331）
    size_t cnt = 0, pos = 0;
    while ((pos = t->result_text.find("part A", pos)) != std::string::npos) {
        ++cnt;
        pos += 6;
    }
    CHECK(cnt == 1, "增量不重复累积 [不可回退]");

    // 空增量忽略
    const std::string before = t->result_text;
    s->append_result(h, "");
    CHECK(s->get(h)->result_text == before, "空增量忽略");

    // 无推进计数
    CHECK(s->note_no_progress(h) == 1, "无推进计数 1");
    CHECK(s->note_no_progress(h) == 2, "无推进计数 2");
    // 有推进 → 计数归零
    ProgressFingerprint fp3 = fp2;
    fp3.history_count = 9;
    s->note_progress(h, fp3, TaskState::Working, "");
    CHECK(s->get(h)->consecutive_no_progress == 0, "推进后计数归零");

    // 不存在的句柄
    CHECK(!s->note_progress("nope", fp1, TaskState::Working, ""), "无效句柄返回 false");
    CHECK(s->note_no_progress("nope") == -1, "无效句柄计数 -1");
}

// ============================================================================
// pending_tasks —— 终结与粘性
// ============================================================================
static void test_terminal_sticky() {
    SECTION("终结与粘性终态");

    // 正常终结
    {
        auto s = PendingTaskStore::create();
        auto h = s->register_task("x", "t1", "", "s").get();
        CHECK(s->update_terminal(h, TerminalReason::Completed, "final", ""), "终结成功");
        auto t = s->get(h);
        CHECK(t->phase == PendingPhase::Terminal, "phase = Terminal");
        CHECK(t->state == TaskState::Completed, "state = Completed");
        CHECK(t->result_text.find("final") != std::string::npos, "结果文本");
        CHECK(t->is_terminal(), "is_terminal");
    }

    // ★ [不可回退] Canceled 是粘性 —— 用户取消后远端才把 completed 推回来，
    //   用户看到的必须是"已取消"，而不是"成功了"
    {
        auto s = PendingTaskStore::create();
        auto h = s->register_task("x", "t1", "", "s").get();
        s->update_terminal(h, TerminalReason::Canceled, "", "用户取消");
        CHECK(!s->update_terminal(h, TerminalReason::Completed, "late result", ""),
              "Canceled 后 Completed 不覆盖 [不可回退]");
        auto t = s->get(h);
        CHECK(t->reason == TerminalReason::Canceled, "reason 仍是 Canceled [不可回退]");
        CHECK(t->state == TaskState::Canceled, "state 仍是 Canceled");
    }

    // ★ HardTimeout 同样粘性
    {
        auto s = PendingTaskStore::create();
        auto h = s->register_task("x", "t1", "", "s").get();
        s->update_terminal(h, TerminalReason::HardTimeout, "", "超时");
        CHECK(!s->update_terminal(h, TerminalReason::RemoteFailed, "", "远端失败"),
              "HardTimeout 后 RemoteFailed 不覆盖 [不可回退]");
        CHECK(s->get(h)->reason == TerminalReason::HardTimeout, "reason 仍是 HardTimeout");
    }

    // 两个粘性终态互不覆盖，先到的胜
    {
        auto s = PendingTaskStore::create();
        auto h = s->register_task("x", "t1", "", "s").get();
        s->update_terminal(h, TerminalReason::Canceled, "", "");
        CHECK(!s->update_terminal(h, TerminalReason::HardTimeout, "", ""),
              "粘性之间不互相覆盖");
        CHECK(s->get(h)->reason == TerminalReason::Canceled, "先到的粘性终态胜出");
    }

    // ★★★ [不可回退] state 与 reason 不同源，reason 不得覆盖远端真实 state。
    //
    //   曾经的缺陷：update_terminal 无条件写
    //   `t->state = terminal_reason_to_task_state(reason)`。
    //   input-required 的归因是 Completed（它不是失败），于是 state 被
    //   改成 TaskState::Completed —— 远端"我还需要订单号"的事实被抹成
    //   "任务成功了"，render() 把追问当最终答案输出，续接流程彻底断裂。
    //   而且这个 bug 在 render() 的单测里【看不出来】（那里直接构造
    //   ExecutionResult 手动设 state），只有走 store 才暴露。
    {
        auto s = PendingTaskStore::create();
        auto h = s->register_task("x", "t1", "", "sess").get();
        // 归因 Completed + 远端状态 input-required
        s->update_terminal(h, TerminalReason::Completed, "", "", TaskState::InputRequired);
        auto t = s->get(h);
        CHECK(t->state == TaskState::InputRequired,
              "远端 state 被保留，未被 reason 覆盖 [不可回退]");
        CHECK(t->reason == TerminalReason::Completed, "reason 仍是 Completed");
        CHECK(t->phase == PendingPhase::Terminal, "phase 转 Terminal");
    }
    // auth-required 同理
    {
        auto s = PendingTaskStore::create();
        auto h = s->register_task("x", "t1", "", "sess").get();
        s->update_terminal(h, TerminalReason::Completed, "", "", TaskState::AuthRequired);
        CHECK(s->get(h)->state == TaskState::AuthRequired,
              "auth-required 亦被保留 [不可回退]");
    }
    // ★ 不传 remote_state 时仍由 reason 推导（HardTimeout/Zombie 是我们
    //   自己判的，远端根本没回话，此时推导才是对的）
    {
        auto s = PendingTaskStore::create();
        auto h = s->register_task("x", "t1", "", "sess").get();
        s->update_terminal(h, TerminalReason::HardTimeout, "", "超时");
        CHECK(s->get(h)->state == TaskState::Failed,
              "无 remote_state 时由 reason 推导 [不可回退]");
    }
    // ★ [不可回退] 非终态的 remote_state 必须被忽略 ——
    //   否则会写出 "phase=Terminal 但 state=working" 的矛盾态
    {
        auto s = PendingTaskStore::create();
        auto h = s->register_task("x", "t1", "", "sess").get();
        s->update_terminal(h, TerminalReason::Completed, "", "", TaskState::Working);
        auto t = s->get(h);
        CHECK(t->state == TaskState::Completed, "非终态 remote_state 被忽略 [不可回退]");
        CHECK(is_terminal(t->state), "phase 与 state 一致（都是终态）[不可回退]");
    }

    // ★ 粘性保护不受 remote_state 影响：取消后远端推 completed 仍不覆盖
    {
        auto s = PendingTaskStore::create();
        auto h = s->register_task("x", "t1", "", "sess").get();
        s->update_terminal(h, TerminalReason::Canceled, "", "用户取消");
        CHECK(!s->update_terminal(h, TerminalReason::Completed, "late", "",
                                 TaskState::Completed),
              "粘性保护优先于 remote_state [不可回退]");
        CHECK(s->get(h)->state == TaskState::Canceled, "仍是 Canceled");
    }

    // 非粘性之间可覆盖（后到的更精确）
    {
        auto s = PendingTaskStore::create();
        auto h = s->register_task("x", "t1", "", "s").get();
        s->update_terminal(h, TerminalReason::Completed, "", "");
        CHECK(s->update_terminal(h, TerminalReason::RemoteFailed, "", "err"),
              "非粘性可被覆盖");
    }

    // ★ [不可回退] 已终结的记录不再接受进度更新 ——
    //   否则「取消后远端继续推进度」会把记录改回 running 的样子
    {
        auto s = PendingTaskStore::create();
        auto h = s->register_task("x", "t1", "", "s").get();
        s->update_terminal(h, TerminalReason::Canceled, "", "");
        ProgressFingerprint fp;
        fp.history_count = 5;
        CHECK(!s->note_progress(h, fp, TaskState::Working, "still going"),
              "终结后拒绝进度更新 [不可回退]");
        s->append_result(h, "late data");
        CHECK(s->get(h)->result_text.find("late data") == std::string::npos,
              "终结后拒绝结果追加 [不可回退]");
    }
}

// ============================================================================
// pending_tasks —— harvest / wait_terminal
// ============================================================================
static void test_harvest_wait() {
    SECTION("harvest 与 wait_terminal");

    // 未终结时 harvest 返回空且不阻塞
    {
        auto s = PendingTaskStore::create();
        auto h = s->register_task("x", "t1", "", "s").get();
        CHECK(!s->harvest(h).has_value(), "未终结 harvest 为空");
        CHECK(!s->wait_terminal(h, 0).has_value(), "timeout=0 立即返回空");
    }

    // 终结后 harvest 一次并置 Harvested
    {
        auto s = PendingTaskStore::create();
        auto h = s->register_task("x", "t1", "", "s").get();
        s->update_terminal(h, TerminalReason::Completed, "ans", "");
        auto got = s->harvest(h);
        CHECK(got.has_value() && got->result_text.find("ans") != std::string::npos,
              "harvest 取到结果");
        CHECK(s->get(h)->phase == PendingPhase::Harvested, "标记为 Harvested");
        // 再 harvest 仍可读（幂等，避免调用方重试时拿不到）
        CHECK(s->harvest(h).has_value(), "重复 harvest 仍可读");
    }

    // ★ [不可回退] 超时后记录必须【仍在表里】——
    //   删除它会让后台线程/回调找不到归属，结果直接丢失
    {
        auto s = PendingTaskStore::create();
        auto h = s->register_task("x", "t1", "", "s").get();
        auto t0 = std::chrono::steady_clock::now();
        auto r = s->wait_terminal(h, 120);
        auto el = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - t0)
                      .count();
        CHECK(!r.has_value(), "超时返回空");
        CHECK(el >= 100, "确实等待了");
        CHECK(s->get(h).has_value(), "超时后记录仍在表里 [不可回退]");
    }

    // 跨线程唤醒（★ 用 cv，不用 future）
    {
        auto s = PendingTaskStore::create();
        auto h = s->register_task("x", "t1", "", "s").get();
        std::thread th([&]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(60));
            s->update_terminal(h, TerminalReason::Completed, "from thread", "");
        });
        auto t0 = std::chrono::steady_clock::now();
        auto r = s->wait_terminal(h, 5000);
        auto el = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - t0)
                      .count();
        th.join();
        CHECK(r.has_value(), "被后台线程唤醒");
        CHECK(el < 3000, "唤醒及时（非等到超时）");
        CHECK(r->result_text.find("from thread") != std::string::npos, "拿到结果");
    }

    // 不存在的句柄立即返回（而非等到超时）
    {
        auto s = PendingTaskStore::create();
        auto t0 = std::chrono::steady_clock::now();
        auto r = s->wait_terminal("nope", 3000);
        auto el = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - t0)
                      .count();
        CHECK(!r.has_value(), "不存在句柄返回空");
        CHECK(el < 500, "不等一个不存在的东西 [不可回退]");
    }
}

// ============================================================================
// pending_tasks —— 回调 token
// ============================================================================
static void test_callback_token() {
    SECTION("回调 token 校验");

    auto s = PendingTaskStore::create();
    auto h = s->register_task("x", "t1", "", "s").get();

    // ★ [不可回退] 未绑定 token 的任务必须拒绝【任何】回调 ——
    //   若返回 true，等于给未启用回调的任务开了无鉴权写入口
    CHECK(!s->verify_callback_token(h, ""), "未绑定时空 token 被拒 [不可回退]");
    CHECK(!s->verify_callback_token(h, "anything"), "未绑定时任意 token 被拒 [不可回退]");

    s->set_callback_token(h, "secret-token-abc");
    CHECK(s->verify_callback_token(h, "secret-token-abc"), "正确 token 通过");
    CHECK(!s->verify_callback_token(h, "secret-token-abd"), "错误 token 被拒");
    CHECK(!s->verify_callback_token(h, "secret-token-ab"), "短 token 被拒");
    CHECK(!s->verify_callback_token(h, "secret-token-abcd"), "长 token 被拒");
    CHECK(!s->verify_callback_token(h, ""), "空 token 被拒");
    CHECK(!s->verify_callback_token("nope", "secret-token-abc"), "无效句柄被拒");

    // 恒定时间比较正确性
    CHECK(constant_time_equals("abc", "abc"), "恒时比较：相等");
    CHECK(!constant_time_equals("abc", "abd"), "恒时比较：末位不同");
    CHECK(!constant_time_equals("abc", "xbc"), "恒时比较：首位不同");
    CHECK(!constant_time_equals("abc", "ab"), "恒时比较：长度不同");
    CHECK(!constant_time_equals("", "a"), "恒时比较：空 vs 非空");
    CHECK(constant_time_equals("", ""), "恒时比较：双空");

    // token 生成：长度足够且不重复
    const std::string t1 = generate_callback_token();
    const std::string t2 = generate_callback_token();
    CHECK(t1.size() == 64, "token 为 64 位十六进制（32 字节）[不可回退]");
    CHECK(t1 != t2, "token 不重复");
}

// ============================================================================
// pending_tasks —— sweep
// ============================================================================
static void test_sweep() {
    SECTION("过期清理");

    // 已终结 + 超 TTL → 删除
    {
        PendingStoreConfig cfg;
        cfg.terminal_ttl_ms = 30;
        auto s = PendingTaskStore::create(cfg);
        auto h = s->register_task("x", "t1", "", "s").get();
        s->update_terminal(h, TerminalReason::Completed, "", "");
        CHECK(s->sweep() == 0, "未超 TTL 不清理");
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        CHECK(s->sweep() == 1, "超 TTL 后清理");
        CHECK(!s->get(h).has_value(), "记录已删除");
        CHECK(!s->handle_of_task("t1").has_value(), "反查索引同步删除 [不可回退]");
    }

    // ★ [不可回退] 在途超上限 → 判 HardTimeout 而【非】直接删除。
    //   直接删除会让 wait_terminal 的调用方等到超时才发现，且拿不到任何原因。
    {
        PendingStoreConfig cfg;
        cfg.running_max_ms = 30;
        cfg.terminal_ttl_ms = 3600000;
        auto s = PendingTaskStore::create(cfg);
        auto h = s->register_task("x", "t1", "", "s").get();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        CHECK(s->sweep() == 1, "在途超上限被处理");
        auto t = s->get(h);
        CHECK(t.has_value(), "记录仍存在（未被删除）[不可回退]");
        CHECK(t->reason == TerminalReason::HardTimeout, "判为 HardTimeout [不可回退]");
        CHECK(t->phase == PendingPhase::Terminal, "phase 转 Terminal");
        CHECK(!t->error_text.empty(), "带原因说明");
    }

    // sweep 判死后应唤醒等待者
    {
        PendingStoreConfig cfg;
        cfg.running_max_ms = 10;
        auto s = PendingTaskStore::create(cfg);
        auto h = s->register_task("x", "t1", "", "s").get();
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        std::thread th([&]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            s->sweep();
        });
        auto r = s->wait_terminal(h, 3000);
        th.join();
        CHECK(r.has_value(), "sweep 判死后唤醒等待者 [不可回退]");
    }
}

// ============================================================================
// execution_engine —— 让渡窗口 clamp
// ============================================================================
static void test_yield_clamp() {
    SECTION("让渡窗口 clamp");

    // ★ [不可回退] 与 exec.cc:930 完全一致：max(10, min(yieldMs, 120000))
    {
        ExecutionConfig c;
        c.yield_ms = 30000;
        CHECK(c.effective_yield_ms() == 30000, "正常值透传");
    }
    {
        ExecutionConfig c;
        c.yield_ms = 1;
        CHECK(c.effective_yield_ms() == 10, "下限 10ms [不可回退]");
    }
    {
        ExecutionConfig c;
        c.yield_ms = 999999;
        CHECK(c.effective_yield_ms() == 120000, "上限 120000ms [不可回退]");
    }
    {
        ExecutionConfig c;
        c.yield_ms = -5;
        CHECK(c.effective_yield_ms() == 10, "负值钳到下限");
    }
    {
        ExecutionConfig c;
        c.background = true;
        c.yield_ms = 50000;
        CHECK(c.effective_yield_ms() == 0, "background 立即让渡（対标 exec.cc:928）");
    }
}

// ============================================================================
// execution_engine —— 结果渲染
// ============================================================================
static void test_render() {
    SECTION("结果渲染");

    // ★ [不可回退] 让渡时必须 success=true（対标 exec.cc:983 tool_result::text）。
    //   返回 error 会让模型认为工具坏了并重试 → 同一远端任务被起两遍。
    {
        ExecutionResult r;
        r.success = true;
        r.yielded = true;
        r.handle = "ra2a-1-ab";
        r.task_id = "t-9";
        const std::string s = r.render();
        CHECK(s.find("仍在执行") != std::string::npos, "让渡文案表明仍在跑");
        CHECK(s.find("ra2a-1-ab") != std::string::npos, "让渡文案含 handle");
        CHECK(s.find("remote_a2a_status") != std::string::npos,
              "让渡文案给出跟进方式 [不可回退]");
    }

    // 完成
    {
        ExecutionResult r;
        r.success = true;
        r.completed = true;
        r.state = TaskState::Completed;
        r.text = "42";
        CHECK(r.render() == "42", "完成时直接返回结果");
    }

    // 空结果有兜底文案（不能返回空串 —— 模型会以为工具没执行）
    {
        ExecutionResult r;
        r.success = true;
        r.completed = true;
        r.state = TaskState::Completed;
        CHECK(!r.render().empty(), "空结果有兜底文案 [不可回退]");
    }

    // 失败带原因
    {
        ExecutionResult r;
        r.error_text = "远端 500";
        r.reason = TerminalReason::RemoteFailed;
        const std::string s = r.render();
        CHECK(s.find("remote_failed") != std::string::npos, "失败文案含归因");
        CHECK(s.find("远端 500") != std::string::npos, "失败文案含原文");
    }

    // 失败但有部分结果 → 两者都要给出
    {
        ExecutionResult r;
        r.error_text = "中途断开";
        r.reason = TerminalReason::ConnLost;
        r.text = "已完成前半部分";
        const std::string s = r.render();
        CHECK(s.find("已完成前半部分") != std::string::npos,
              "失败时仍保留部分结果 [不可回退]");
    }

    // ── input-required / auth-required：挂起态渲染 ────────────────────────
    // ★ 这一组锁死两个曾经的真实缺陷（详见每条断言）。
    {
        ExecutionResult r;
        r.success = true;
        r.completed = true;
        r.state = TaskState::InputRequired;
        r.handle = "h1";
        r.task_id = "task-77";
        r.context_id = "ctx-9";
        r.prompt_text = "请提供订单号";
        const std::string s = r.render();

        CHECK(s.find("补充输入") != std::string::npos, "input-required 单独渲染");
        CHECK(s.find("失败") == std::string::npos, "input-required 不渲染为失败 [不可回退]");
        CHECK(r.is_suspended(), "is_suspended 为真");

        // ★★ 缺陷 1（已修）：task_id 必须出现在【文本】里。
        //   ToolResult.details 不进 prompt（react_engine.cc:1076-1088 只读
        //   content），模型只能看到 render() 的输出。缺 task_id 就无法续接，
        //   整个 input-required 流程直接断掉。
        CHECK(s.find("task-77") != std::string::npos,
              "文本含 task_id（否则模型无法续接）[不可回退]");
        CHECK(s.find("ctx-9") != std::string::npos, "文本含 context_id [不可回退]");
        // 必须给出明确的续接指令，否则模型不知道该做什么
        CHECK(s.find("再次调用") != std::string::npos, "给出续接指令 [不可回退]");
        CHECK(s.find("请提供订单号") != std::string::npos, "含远端追问");
    }

    // ★★ 缺陷 2（已修）：追问与部分结果【都要】输出，不能二选一。
    //   远端可同时给出"已完成的部分"(artifacts) 与"还缺什么"(status.message)；
    //   只输出其一会让模型看到「需要补充输入: <部分结果>」，
    //   完全不知道要补什么 —— 流程死锁。
    {
        ExecutionResult r;
        r.success = true;
        r.completed = true;
        r.state = TaskState::InputRequired;
        r.handle = "h1";
        r.task_id = "task-88";
        r.text = "已查到 3 条候选记录";   // artifacts（部分结果）
        r.prompt_text = "请指定要哪一条";  // status.message（追问）
        const std::string s = r.render();

        CHECK(s.find("请指定要哪一条") != std::string::npos,
              "有部分结果时追问不被吞掉 [不可回退]");
        CHECK(s.find("已查到 3 条候选记录") != std::string::npos,
              "部分结果同时保留 [不可回退]");
        CHECK(s.find("task-88") != std::string::npos, "仍含 task_id");
    }

    // 远端没说要什么 → 必须明说，避免模型凭空猜测后乱填
    {
        ExecutionResult r;
        r.success = true;
        r.completed = true;
        r.state = TaskState::InputRequired;
        r.handle = "h1";
        r.task_id = "t1";
        // prompt_text 与 text 都空
        const std::string s = r.render();
        CHECK(s.find("未说明") != std::string::npos,
              "远端未说明需求时明确提示 [不可回退]");
    }

    // auth-required
    {
        ExecutionResult r;
        r.success = true;
        r.completed = true;
        r.state = TaskState::AuthRequired;
        r.handle = "h1";
        r.task_id = "task-99";
        r.prompt_text = "需要 OAuth";
        const std::string s = r.render();
        CHECK(s.find("鉴权") != std::string::npos, "auth-required 单独渲染");
        CHECK(s.find("task-99") != std::string::npos, "auth-required 也含 task_id [不可回退]");
        CHECK(r.is_suspended(), "auth-required 也算挂起");
    }

    // ★ 挂起态必须在 error_text 分支【之前】判定 ——
    //   否则一个残留的 error_text 会把挂起渲染成失败，模型据此重试，
    //   远端堆积永远等不到输入的僵尸任务。
    {
        ExecutionResult r;
        r.success = true;
        r.completed = true;
        r.state = TaskState::InputRequired;
        r.handle = "h1";
        r.task_id = "t2";
        r.prompt_text = "请补充";
        r.error_text = "某条路径残留的错误文本";
        const std::string s = r.render();
        CHECK(s.find("补充输入") != std::string::npos,
              "有 error_text 时仍按挂起渲染 [不可回退]");
        CHECK(s.find("远端任务失败") == std::string::npos,
              "不落入失败分支 [不可回退]");
    }

    // 正常完成：无 artifacts 时回落到 prompt_text（status.message）
    {
        ExecutionResult r;
        r.success = true;
        r.completed = true;
        r.state = TaskState::Completed;
        r.prompt_text = "答案在这里";
        CHECK(r.render() == "答案在这里", "completed 无 artifacts 回落 status.message");
        CHECK(!r.is_suspended(), "completed 非挂起");
    }
}

// ============================================================================
// execution_engine —— push config 构造
// ============================================================================
static void test_push_config() {
    SECTION("pushNotificationConfig 构造");

    auto c = build_push_config("https://hc.example.com", "ra2a-1-ab", "tok123");
    CHECK(c["url"] == "https://hc.example.com/remote-a2a/callback/ra2a-1-ab",
          "回调 URL 拼接");
    CHECK(c["id"] == "ra2a-1-ab", "id = handle");

    // ★ [不可回退] 必须带凭据。不带 token 的回调端点等于开放写入口：
    //   任何人猜到 handle 就能伪造「任务已完成，结果是 X」。
    CHECK(c.contains("authentication"), "带 authentication 节点 [不可回退]");
    CHECK(c["authentication"]["credentials"] == "tok123", "凭据为我们生成的 token");
    CHECK(c["authentication"]["schemes"][0] == "Bearer", "scheme = Bearer");

    // 尾斜杠归一（否则会拼出双斜杠）
    auto c2 = build_push_config("https://hc.example.com/", "h", "t");
    CHECK(c2["url"] == "https://hc.example.com/remote-a2a/callback/h",
          "尾斜杠归一 [不可回退]");
}

// ============================================================================
// execution_engine —— 未初始化 / 无卡（不发包）
// ============================================================================
static void test_engine_uninitialized() {
    SECTION("引擎未就绪时的行为");

    // 无 client
    {
        auto e = ExecutionEngine::create(nullptr, PendingTaskStore::create(),
                                        ExecutionConfig{});
        auto r = e->execute(Message{}, "s1");
        CHECK(!r.success, "无 client → 失败");
        CHECK(!r.error_text.empty(), "带错误说明");
        CHECK(!r.yielded, "未让渡");
    }

    // 无 store
    {
        RemoteAgentClientConfig cc;
        auto e = ExecutionEngine::create(RemoteAgentClient::create(cc, nullptr), nullptr,
                                        ExecutionConfig{});
        auto r = e->execute(Message{}, "s1");
        CHECK(!r.success, "无 store → 失败");
    }

    // client 无卡 → 立即失败，不发包
    {
        RemoteAgentClientConfig cc;
        cc.name = "nc";
        auto e = ExecutionEngine::create(RemoteAgentClient::create(cc, nullptr),
                                        PendingTaskStore::create(), ExecutionConfig{});
        auto r = e->execute(Message{}, "s1");
        CHECK(!r.success, "无卡 → 失败");
        CHECK(r.error_text.find("AgentCard") != std::string::npos, "错误提示指向卡");
    }
}

// ============================================================================
// execution_engine —— follow_up / cancel
// ============================================================================
static void test_follow_up_cancel() {
    SECTION("follow_up 与 cancel");

    RemoteAgentClientConfig cc;
    cc.name = "fu";
    auto store = PendingTaskStore::create();
    auto engine = ExecutionEngine::create(RemoteAgentClient::create(cc, nullptr), store,
                                         ExecutionConfig{});

    // 无效句柄
    {
        auto r = engine->follow_up("nope", 0);
        CHECK(!r.success, "无效句柄 follow_up 失败");
        // ★ 一期内存存储：进程重启后句柄全失效，错误文案要说清楚
        CHECK(r.error_text.find("重启") != std::string::npos,
              "错误文案提示可能已重启 [不可回退]");
        CHECK(engine->cancel("nope").is_err(), "无效句柄 cancel 失败");
    }

    // 在途 → follow_up 返回 yielded
    {
        auto h = store->register_task("fu", "t1", "c1", "s1").get();
        auto r = engine->follow_up(h, 0);
        CHECK(r.success && r.yielded && !r.completed, "在途 → yielded");
        CHECK(r.task_id == "t1", "task_id 透传");
    }

    // 已终结 → follow_up 返回结果
    {
        auto h = store->register_task("fu", "t2", "", "s1").get();
        store->update_terminal(h, TerminalReason::Completed, "the answer", "");
        auto r = engine->follow_up(h, 0);
        CHECK(r.success && r.completed && !r.yielded, "已终结 → completed");
        CHECK(r.text.find("the answer") != std::string::npos, "拿到结果");
        CHECK(r.reason == TerminalReason::Completed, "归因正确");
    }

    // ★ [不可回退] cancel 必须【立即】本地标记 Canceled，不等远端确认。
    //   対标 ADK executor/utils.py:31-48：远端 cancel 只塞事件不中断执行。
    //   若等远端确认，用户点了取消却看到任务继续跑 = 取消无效。
    {
        auto h = store->register_task("fu", "t3", "", "s1").get();
        auto r = engine->cancel(h);
        CHECK(r.is_ok(), "cancel 返回 Ok（远端不可达也不算失败）");
        CHECK(!r.get(), "远端不可达 → 返回 false（表示未获远端确认）");
        auto t = store->get(h);
        CHECK(t->reason == TerminalReason::Canceled,
              "本地已立即标记 Canceled [不可回退]");
        CHECK(t->phase == PendingPhase::Terminal, "phase 转 Terminal");

        // 取消后远端才把 completed 推回来 → 不覆盖
        store->update_terminal(h, TerminalReason::Completed, "late", "");
        CHECK(store->get(h)->reason == TerminalReason::Canceled,
              "取消后迟到的 completed 不覆盖 [不可回退]");
    }
}

// ============================================================================
// input-required 端到端：追问与结果的字段归属
//
// ★ 这一组走 store → follow_up 的真实链路，验证「追问不进 result_text」。
//   上面的 test_render 只测渲染，这里测【数据落在哪个字段】——
//   两者都必须锁死：字段错了渲染再对也没用（harvest 拿到的会是一句提问）。
// ============================================================================
static void test_input_required_field_ownership() {
    SECTION("input-required 字段归属");

    RemoteAgentClientConfig cc;
    cc.name = "ir";
    auto store = PendingTaskStore::create();
    auto engine = ExecutionEngine::create(RemoteAgentClient::create(cc, nullptr), store,
                                         ExecutionConfig{});

    // 场景 A：只有追问，无部分结果
    {
        auto h = store->register_task("ir", "t1", "c1", "s1").get();

        // 模拟 ingest_task 对挂起态的处理：追问进 progress_text，
        // update_terminal 的 text 传空（不写 result_text）
        ProgressFingerprint fp;
        fp.last_status_message_len = 6;
        store->note_progress(h, fp, TaskState::InputRequired, "请提供订单号");
        store->update_terminal(h, TerminalReason::Completed, "", "");

        auto t = store->get(h);
        // ★★ [不可回退] 追问【不能】进 result_text ——
        //   否则 harvest 取到的"结果"是一句提问，
        //   且后续无法与真正的部分结果区分
        CHECK(t->result_text.empty(), "追问未写入 result_text [不可回退]");
        CHECK(t->progress_text == "请提供订单号", "追问在 progress_text [不可回退]");

        auto r = engine->follow_up(h, 0);
        CHECK(r.success, "follow_up 成功");
        CHECK(r.is_suspended(), "识别为挂起态");
        CHECK(r.prompt_text == "请提供订单号", "prompt_text 带出追问 [不可回退]");
        CHECK(r.text.empty(), "text（正式结果）为空");
        CHECK(r.task_id == "t1", "task_id 带出");
        CHECK(r.context_id == "c1", "context_id 带出");

        const std::string s = r.render();
        CHECK(s.find("t1") != std::string::npos, "渲染含 task_id [不可回退]");
        CHECK(s.find("请提供订单号") != std::string::npos, "渲染含追问");
    }

    // 场景 B：追问 + 部分结果同时存在（曾经会吞掉追问）
    {
        auto h = store->register_task("ir", "t2", "c2", "s1").get();

        store->append_result(h, "已查到 3 条候选");   // artifacts
        ProgressFingerprint fp;
        fp.last_status_message_len = 9;
        store->note_progress(h, fp, TaskState::InputRequired, "请指定要哪一条");
        store->update_terminal(h, TerminalReason::Completed, "", "");

        auto r = engine->follow_up(h, 0);
        CHECK(r.is_suspended(), "识别为挂起态");
        // ★★ [不可回退] 两者都要在，且分属不同字段
        CHECK(r.text.find("已查到 3 条候选") != std::string::npos,
              "部分结果在 text [不可回退]");
        CHECK(r.prompt_text == "请指定要哪一条", "追问在 prompt_text [不可回退]");

        const std::string s = r.render();
        CHECK(s.find("请指定要哪一条") != std::string::npos,
              "有部分结果时追问仍被渲染 [不可回退]");
        CHECK(s.find("已查到 3 条候选") != std::string::npos, "部分结果也渲染");
        CHECK(s.find("t2") != std::string::npos, "含 task_id");
    }

    // 场景 C：让渡时的中间态不该被当成正式结果
    {
        auto h = store->register_task("ir", "t3", "", "s1").get();
        ProgressFingerprint fp;
        fp.last_status_message_len = 5;
        store->note_progress(h, fp, TaskState::Working, "思考中...");

        auto r = engine->follow_up(h, 0);
        CHECK(r.yielded && !r.completed, "在途 → 让渡");
        CHECK(r.prompt_text == "思考中...", "中间态在 prompt_text");
        CHECK(r.text.empty(), "中间态不进 text [不可回退]");
        CHECK(r.render().find("思考中...") != std::string::npos, "让渡文案含进度");
    }

    // 场景 D：auth-required 与 input-required 同构
    {
        auto h = store->register_task("ir", "t4", "", "s1").get();
        ProgressFingerprint fp;
        fp.last_status_message_len = 8;
        store->note_progress(h, fp, TaskState::AuthRequired, "需要重新授权");
        store->update_terminal(h, TerminalReason::Completed, "", "");

        auto r = engine->follow_up(h, 0);
        CHECK(r.is_suspended(), "auth-required 也是挂起");
        CHECK(r.render().find("t4") != std::string::npos, "含 task_id [不可回退]");
        CHECK(r.render().find("需要重新授权") != std::string::npos, "含追问");
    }
}

// ============================================================================
// 分支枚举文本
// ============================================================================
static void test_texts() {
    SECTION("枚举文本");
    CHECK(std::string(exec_branch_text(ExecBranch::Stream)) == "stream", "stream");
    CHECK(std::string(exec_branch_text(ExecBranch::Probe)) == "probe", "probe");
    CHECK(std::string(exec_branch_text(ExecBranch::Callback)) == "callback", "callback");
    CHECK(std::string(pending_phase_text(PendingPhase::Running)) == "running", "running");
    CHECK(std::string(pending_phase_text(PendingPhase::Harvested)) == "harvested",
          "harvested");
}

// ============================================================================
int main() {
    std::cout << "===== remote_a2a M7/M8 测试 =====" << std::endl;

    test_register();
    test_capacity();
    test_progress();
    test_terminal_sticky();
    test_harvest_wait();
    test_callback_token();
    test_sweep();

    test_yield_clamp();
    test_render();
    test_push_config();
    test_engine_uninitialized();
    test_follow_up_cancel();
    test_input_required_field_ownership();
    test_texts();

    std::cout << "\n===== 结果: " << g_pass << " 通过, " << g_fail << " 失败 ====="
              << std::endl;
    return g_fail == 0 ? 0 : 1;
}
