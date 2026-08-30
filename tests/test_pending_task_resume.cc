// ============================================================================
// test_pending_task_resume.cc
// 续接语义测试：同一 task_id 的挂起记录必须复用同一个 handle
//
// 🎯 A2A 的续接语义是「在同一个 task_id 上再发一次 message/send 或
//   message/stream」。本地必须复用同一条记录。
//
// 覆盖的不变量 —— 每条都对应一个"错了不报错、只静默走偏"的缺陷:
//   1. 挂起态 + 同 task_id 同 agent → 复用 handle（回调 URL 才不会失联）
//   2. 复用时 phase 从 Harvested/Terminal 拉回 Running（否则本轮一次远端
//      调用都不会发生，直接空转结束）
//   3. 复用时 state 离开挂起态（否则 is_terminal() 立刻为真，同上）
//   4. 复用时清 progress_text（否则把已答过的追问再输出一遍）
//   5. 复用时清 pending_token_ids（否则要用户确认刚确认过的东西）
//   6. 复用时重置 requires_user_action（否则某轮要过人工确认，之后每轮都要）
//   7. 复用时重置 question_raised（否则第二轮确认被去重闸吃掉）
//   8. 复用时【保留】result_text（否则前几轮产出丢失）
//   9. 复用时【保留】callback_token（回调凭据不能换）
//  10. 非挂起态（Completed/Failed）不复用 —— 那是新交互
//  11. 不同 agent 同 task_id 不复用 —— task_id 只在 agent 内唯一
// ============================================================================

#include <chrono>
#include <iostream>
#include <thread>
#include <string>

#include "hunterclaw/remote_a2a/pending_tasks.h"

using namespace hunterclaw::remote_a2a;

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

/// 把一条记录推到"挂起 + 已收割"状态 —— 这正是 execute() 返回后的真实形态。
static void drive_to_suspended(PendingTaskStore& store, const std::string& handle,
                               TaskState suspended_state = TaskState::InputRequired) {
    store.note_confirm_signal(handle, true, {"tok-A", "tok-B"});
    store.note_progress(handle, ProgressFingerprint{}, suspended_state, "还需要订单号");
    // ★ 归因是 Completed（挂起不是失败），但真实 state 要原样传进去 ——
    //   不传会让 input-required 被抹成 Completed，这是历史上的真实 bug。
    store.update_terminal(handle, TerminalReason::Completed, "部分结果", "",
                          suspended_state);
    (void)store.harvest(handle);  // execute() 末尾会做
}

// ── 1~9: 挂起态复用 ─────────────────────────────────────────────────────────
static void test_reuse_on_suspended() {
    SECTION("挂起态复用同一 handle");

    PendingTaskStore store(PendingStoreConfig{});
    auto h1 = store.register_task("agentX", "task-1", "ctx-1", "sess-1").get();
    CHECK(!h1.empty(), "首轮登记成功");

    store.set_callback_token(h1, "tok-callback-abc");
    drive_to_suspended(store, h1);

    {
        auto snap = store.get(h1);
        CHECK(snap.has_value(), "挂起后记录仍在");
        CHECK(snap->state == TaskState::InputRequired,
              "挂起态未被 Completed 抹掉（update_terminal 自保生效）");
        CHECK(snap->phase == PendingPhase::Harvested, "已被收割");
        CHECK(snap->requires_user_action, "requires_user_action 已置位");
        CHECK(snap->pending_token_ids.size() == 2, "待确认凭据已记录");
    }

    // ── 续接：同 agent + 同 task_id ──────────────────────────────────────
    auto h2 = store.register_task("agentX", "task-1", "ctx-1", "sess-1").get();
    // ★★ 1: 必须是同一个 handle —— 回调 URL 里带的就是它
    CHECK(h2 == h1, "同 agent + 同 task_id 的挂起记录复用同一 handle");

    auto snap = store.get(h2);
    CHECK(snap.has_value(), "复用后记录可取");
    // ★★ 2 / 3: 必须真的回到可运行状态
    CHECK(snap->phase == PendingPhase::Running,
          "phase 从 Harvested 拉回 Running（否则本轮一次远端调用都不会发生）");
    CHECK(!snap->reason.has_value(), "terminal reason 已清除");
    CHECK(snap->state == TaskState::Submitted,
          "state 离开挂起态（否则 is_terminal 立刻为真，本轮空转）");
    // ★★ 4 / 5: 上一轮的挂起痕迹必须清掉
    CHECK(snap->progress_text.empty(),
          "progress_text 已清（否则把已答过的追问再输出一遍）");
    CHECK(snap->pending_token_ids.empty(),
          "pending_token_ids 已清（否则要用户确认刚确认过的东西）");
    CHECK(snap->error_text.empty(), "error_text 已清");
    // ★★ 6 / 7
    CHECK(!snap->requires_user_action,
          "requires_user_action 已重置（否则某轮要过确认后每轮都要）");
    CHECK(!snap->question_raised,
          "question_raised 已重置（否则第二轮确认被去重闸吃掉）");
    CHECK(snap->consecutive_no_progress == 0, "无推进计数已重置");
    // ★★ 8 / 9: 必须保留的
    CHECK(snap->result_text == "部分结果",
          "result_text 保留（前几轮产出要能一起交付）");
    CHECK(snap->callback_token == "tok-callback-abc",
          "callback_token 保留（回调凭据不能换）");
    CHECK(snap->task_id == "task-1" && snap->context_id == "ctx-1",
          "task_id / context_id 不变");

    // 复用不应新增条目
    CHECK(store.size() == 1, "复用不占用新的容量额度");

    // ── 多轮确认：再挂起一次仍能复用并再问 ──────────────────────────────
    drive_to_suspended(store, h2, TaskState::AuthRequired);
    auto h3 = store.register_task("agentX", "task-1", "", "").get();
    CHECK(h3 == h1, "第二轮挂起仍复用同一 handle（AuthRequired 同样是挂起态）");
    auto s3 = store.get(h3);
    CHECK(s3.has_value() && !s3->question_raised,
          "多轮确认时去重闸每轮重置（第二轮还能问用户）");
    CHECK(store.size() == 1, "多轮确认不产生多条记录");
}

// ── 10: 非挂起态不复用 ──────────────────────────────────────────────────────
static void test_no_reuse_on_final() {
    SECTION("非挂起终态不复用");

    for (const auto st : {TaskState::Completed, TaskState::Failed,
                          TaskState::Canceled}) {
        PendingTaskStore store(PendingStoreConfig{});
        auto h1 = store.register_task("agentX", "task-9", "", "sess").get();
        const auto reason = (st == TaskState::Completed) ? TerminalReason::Completed
                            : (st == TaskState::Failed)  ? TerminalReason::RemoteFailed
                                                         : TerminalReason::Canceled;
        store.update_terminal(h1, reason, "done", "", st);
        (void)store.harvest(h1);

        auto h2 = store.register_task("agentX", "task-9", "", "sess").get();
        // ★ A2A 里终结的 task 不接受新 message；此时是"复用了同一 task_id
        //   起新交互"，语义上确实该是另一条记录。
        CHECK(h2 != h1, std::string("终态 ") + task_state_to_wire(st) +
                            " 不复用 handle（是新交互）");
    }
}

// ── 11: 跨 agent 不复用 ─────────────────────────────────────────────────────
static void test_no_reuse_cross_agent() {
    SECTION("跨 agent 不复用");

    PendingTaskStore store(PendingStoreConfig{});
    auto h1 = store.register_task("agentA", "same-task-id", "", "sess").get();
    drive_to_suspended(store, h1);

    auto h2 = store.register_task("agentB", "same-task-id", "", "sess").get();
    // ⚠ task_id 只在单个远端 agent 内唯一。跨 agent 复用会把 B 的回调
    //   推到 A 的记录上，结果串台。
    CHECK(h2 != h1, "不同 agent 的同名 task_id 不复用（task_id 仅 agent 内唯一）");
}

// ── 续接时补齐 context_id ────────────────────────────────────────────────────
static void test_context_id_fill() {
    SECTION("续接补齐 context_id");

    PendingTaskStore store(PendingStoreConfig{});
    // 首轮远端还没给 context_id
    auto h1 = store.register_task("agentX", "task-c", "", "sess").get();
    drive_to_suspended(store, h1);

    // 续接时远端给了
    auto h2 = store.register_task("agentX", "task-c", "ctx-late", "sess").get();
    CHECK(h2 == h1, "复用 handle");
    auto s = store.get(h2);
    CHECK(s.has_value() && s->context_id == "ctx-late",
          "非空 context_id 会被补上（远端可能在续接时才给）");

    // 空值不应把已有的抹掉
    drive_to_suspended(store, h2);
    auto h3 = store.register_task("agentX", "task-c", "", "").get();
    auto s3 = store.get(h3);
    CHECK(s3.has_value() && s3->context_id == "ctx-late",
          "空 context_id 不覆盖已有值");
    CHECK(s3.has_value() && s3->session_key == "sess",
          "空 session_key 不覆盖已有值（否则认领路由失去归属会话）");
}

// ── epoch: 僵尸线程复活防护 ──────────────────────────────────────────────────
static void test_epoch_guard() {
    SECTION("epoch 代号（僵尸线程复活防护）");

    PendingTaskStore store(PendingStoreConfig{});
    auto h = store.register_task("agentX", "task-e", "", "sess").get();

    const uint64_t e1 = store.current_epoch(h);
    CHECK(e1 == 1, "首轮 epoch 为 1（0 保留给'记录不存在'）");
    CHECK(store.current_epoch("no-such-handle") == 0,
          "不存在的 handle → epoch 0（调用方据此退出）");

    // 首轮线程记下 e1，然后挂起
    drive_to_suspended(store, h);
    CHECK(store.current_epoch(h) == e1,
          "挂起本身不改变 epoch（同一代交互结束）");

    // ★★ 续接 → epoch 必须 +1
    //   这是防"老 background 线程睡醒后看到 phase==Running 继续探测"的唯一手段。
    //   ⚠ 双线程同时探测会双倍推进 consecutive_no_progress → 误判 Zombie
    //     把正常任务杀掉。
    auto h2 = store.register_task("agentX", "task-e", "", "sess").get();
    CHECK(h2 == h, "复用同一 handle");
    const uint64_t e2 = store.current_epoch(h);
    CHECK(e2 == e1 + 1, "续接后 epoch +1（上一代线程据此退出）");

    // 模拟老线程校验：e1 != 当前 → 应退出
    CHECK(store.current_epoch(h) != e1, "上一代线程的 epoch 校验失败 → 退出");

    // 多轮
    drive_to_suspended(store, h);
    (void)store.register_task("agentX", "task-e", "", "sess").get();
    CHECK(store.current_epoch(h) == e1 + 2, "每次续接都推进 epoch");
}

// ── turn_started_at_ms: HardTimeout 按本轮计时 ───────────────────────────────
static void test_turn_clock() {
    SECTION("本轮计时（HardTimeout 基准）");

    PendingTaskStore store(PendingStoreConfig{});
    auto h = store.register_task("agentX", "task-t", "", "sess").get();

    auto s1 = store.get(h);
    CHECK(s1.has_value() && s1->turn_started_at_ms > 0,
          "turn_started_at_ms 已初始化（为 0 会让 HardTimeout 用 1970 做基准，"
          "首次 evaluate 立刻判超时）");
    CHECK(s1->turn_started_at_ms == s1->created_at_ms,
          "首轮 turn_started_at_ms == created_at_ms");

    const int64_t first_created = s1->created_at_ms;
    drive_to_suspended(store, h);

    // 模拟用户思考若干时间后才答复
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    (void)store.register_task("agentX", "task-t", "", "sess").get();

    auto s2 = store.get(h);
    CHECK(s2.has_value(), "续接后可取");
    CHECK(s2->created_at_ms == first_created,
          "created_at_ms 不变（记录的诞生时刻）");
    // ★★ 关键: 本轮计时必须刷新
    //   ⚠ 沿用 created_at_ms 会把"用户思考的那几分钟"算进远端执行时限，
    //     多轮确认的任务在第二轮直接判 HardTimeout。
    CHECK(s2->turn_started_at_ms > first_created,
          "turn_started_at_ms 已刷新（用户思考时间不算进远端执行时限）");
}

int main() {
    std::cout << "=== Remote A2A 续接（handle 复用）测试 ===" << std::endl;

    test_reuse_on_suspended();
    test_no_reuse_on_final();
    test_no_reuse_cross_agent();
    test_context_id_fill();
    test_epoch_guard();
    test_turn_clock();

    std::cout << "\n===============================" << std::endl;
    std::cout << "PASS: " << g_pass << "  FAIL: " << g_fail << std::endl;
    return g_fail == 0 ? 0 : 1;
}
