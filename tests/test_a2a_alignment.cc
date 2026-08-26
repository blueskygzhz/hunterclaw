// ============================================================================
// A2A 对齐验证 — 対齐 OpenClaw sessions-send-tokens.ts / auto-reply/tokens.ts
//                        / session-key-utils.ts / sessions-send-helpers.ts
// ============================================================================
// 用例取自真源的语义边界与已知回归（openclaw#19537 等），
// 重点覆盖本次修正的三个 bug：
//   1. token 取值（"__X__" -> 裸值）
//   2. token 判定（子串包含 -> token-only 精确匹配）
//   3. delivery route 解析（空 accountId 必须被拒）
//
// 构建：
//   /usr/local/gcc-12.2.0/bin/g++ -std=c++17 -Iinclude
//     -Iexternal/json/single_include -Ithird_party
//     tests/test_a2a_alignment.cc src/tools/support_tokens.cc
//     src/tools/a2a_context.cc -o /tmp/test_a2a_alignment
//   /tmp/test_a2a_alignment
// ============================================================================

#include <iostream>
#include <string>

#include "hunterclaw/agents/run_wait.h"
#include "hunterclaw/auto_reply/tokens.h"
#include "hunterclaw/sessions/input_provenance.h"
#include "hunterclaw/sessions/session_key_utils.h"
#include "hunterclaw/tools/a2a_context.h"
#include "hunterclaw/agents/agent_lanes.h"
#include "hunterclaw/tools/support_tokens.h"

using namespace hunterclaw;
using namespace hunterclaw::tools;

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond, name)                                \
    do {                                                 \
        if (cond) {                                      \
            ++g_pass;                                    \
        } else {                                         \
            ++g_fail;                                    \
            std::cout << "  [FAIL] " << (name) << "\n";  \
        }                                                \
    } while (0)

int main() {
    // =======================================================================
    std::cout << "== Token 常量取值（対齐 sessions-send-tokens.ts:9,11 + tokens.ts:5,7）==\n";
    // 原实现用 "__ANNOUNCE_SKIP__" 等包裹形式，与真源不符 —— prompt 里告诉模型
    // 回复 "ANNOUNCE_SKIP"，判定却找 "__ANNOUNCE_SKIP__"，永远不匹配。
    CHECK(support_tokens::ANNOUNCE_SKIP_TOKEN == "ANNOUNCE_SKIP",
          "ANNOUNCE_SKIP_TOKEN 为裸值");
    CHECK(support_tokens::REPLY_SKIP_TOKEN == "REPLY_SKIP",
          "REPLY_SKIP_TOKEN 为裸值");
    CHECK(support_tokens::SILENT_REPLY_TOKEN == "NO_REPLY",
          "SILENT_REPLY_TOKEN 为 NO_REPLY（対齐 tokens.ts:7）");
    CHECK(support_tokens::HEARTBEAT_TOKEN == "HEARTBEAT_OK",
          "HEARTBEAT_TOKEN 为 HEARTBEAT_OK（対齐 tokens.ts:5）");
    // 与 HunterClaw 其它子系统一致（agent_instance.cc / boot.cc / chat.cc 用裸值）
    CHECK(support_tokens::SILENT_REPLY_TOKEN.find("__") == std::string::npos,
          "token 不含 __ 包裹（与 agent_instance.cc 等既有代码一致）");

    // =======================================================================
    std::cout << "== is_reply_skip / is_announce_skip（対齐 tokens.ts:21-28 严格相等）==\n";
    CHECK(support_tokens::is_reply_skip("REPLY_SKIP"), "精确匹配");
    CHECK(support_tokens::is_reply_skip("  REPLY_SKIP \n"), "忽略首尾空白");
    CHECK(!support_tokens::is_reply_skip("reply_skip"),
          "区分大小写（真源用 === 严格相等）");
    CHECK(!support_tokens::is_reply_skip("REPLY_SKIP 的含义是停止对话"),
          "**关键回归**：正文提及 token 不算跳过（原子串实现会误判）");
    CHECK(!support_tokens::is_reply_skip("好的，我继续。REPLY_SKIP"),
          "结尾带 token 的实质回复不算跳过（対齐 #19537）");
    CHECK(support_tokens::is_announce_skip("ANNOUNCE_SKIP"), "announce 精确匹配");
    CHECK(!support_tokens::is_announce_skip("我打算 ANNOUNCE_SKIP 掉这条"),
          "**关键回归**：正文提及不算 announce 跳过");

    // =======================================================================
    std::cout << "== is_non_deliverable_sessions_reply（対齐 tokens.ts:31-33 token-only）==\n";
    CHECK(support_tokens::is_non_deliverable_sessions_reply("NO_REPLY"),
          "NO_REPLY 不可投递");
    CHECK(support_tokens::is_non_deliverable_sessions_reply("HEARTBEAT_OK"),
          "HEARTBEAT_OK 不可投递");
    CHECK(support_tokens::is_non_deliverable_sessions_reply("REPLY_SKIP"),
          "REPLY_SKIP 不可投递");
    CHECK(support_tokens::is_non_deliverable_sessions_reply("ANNOUNCE_SKIP"),
          "ANNOUNCE_SKIP 不可投递");
    // 対齐 tokens.ts:29 的 `TOKEN(?:\s+TOKEN)*`
    CHECK(support_tokens::is_non_deliverable_sessions_reply("NO_REPLY NO_REPLY"),
          "重复 token（空白分隔）仍视为 token-only");
    CHECK(support_tokens::is_non_deliverable_sessions_reply("no_reply"),
          "isSilentReplyText 大小写不敏感（対齐正则 'i' flag）");
    // 対齐 tokens.ts:47-49,65 stripEdgePunctuation
    CHECK(support_tokens::is_non_deliverable_sessions_reply("**NO_REPLY**"),
          "Markdown 包裹的 token 仍视为静默");
    CHECK(support_tokens::is_non_deliverable_sessions_reply("NO_REPLY。"),
          "全角句号结尾的 token 仍视为静默");
    // ⚠ 最关键的一组：实质回复绝不能被吞掉（openclaw#19537 的原始 bug）
    CHECK(!support_tokens::is_non_deliverable_sessions_reply(
              "任务完成，详见文档。若无需回复请用 NO_REPLY"),
          "**关键回归**：正文提及 NO_REPLY 的实质回复必须投递");
    CHECK(!support_tokens::is_non_deliverable_sessions_reply("NO_REPLYING"),
          "token 作为词的前缀不算静默");
    CHECK(!support_tokens::is_non_deliverable_sessions_reply(""), "空串不算静默");
    // 対齐 tokens.ts:61-62 —— 表情等 Symbol 属实质内容，不可被 strip 成空
    CHECK(!support_tokens::is_non_deliverable_sessions_reply("🎉"),
          "表情符号是实质内容（不可当作静默）");

    // =======================================================================
    std::cout << "== strip_support_tokens（対齐 tokens.ts:236-238 仅剥结尾）==\n";
    CHECK(support_tokens::strip_support_tokens("任务完成 NO_REPLY") == "任务完成",
          "剥离结尾 token");
    CHECK(support_tokens::strip_support_tokens("任务完成 *NO_REPLY") == "任务完成",
          "剥离结尾 token 并去掉紧邻的 Markdown 强调符");
    // ⚠ 真源的一处**刻意不对称**，不要"顺手对齐"：
    //   isSilentReplyText("**NO_REPLY**") == true   （tokens.ts:65 会 stripEdgePunctuation）
    //   stripSilentToken("**NO_REPLY**")  不剥      （tokens.ts:42 结尾是 \s*$，匹配不了尾部 **）
    // 两者并不矛盾：不可投递判定发生在投递**之前**，这种形态会被
    // is_non_deliverable_sessions_reply 直接拦下，永远走不到 strip。
    // 若把 strip 也放宽成剥标点，反而会破坏 "见 **NO_REPLY** 说明" 这类正文。
    CHECK(support_tokens::is_non_deliverable_sessions_reply("**NO_REPLY**"),
          "**NO_REPLY** 由不可投递判定拦下（先于 strip）");
    CHECK(support_tokens::strip_support_tokens("任务完成 **NO_REPLY**") ==
              "任务完成 **NO_REPLY**",
          "strip 不处理尾部带 ** 的形态（対齐真源 \\s*$ 边界）");
    CHECK(support_tokens::strip_support_tokens("任务完成 NO_REPLY NO_REPLY") == "任务完成",
          "剥离结尾重复 token");
    // **关键回归**：原实现做全局 erase，会把正文里的 token 名字也抹掉
    CHECK(support_tokens::strip_support_tokens("回复 NO_REPLY 即可静默") ==
              "回复 NO_REPLY 即可静默",
          "**关键回归**：正文中间的 token 不被删除（原全局 erase 会破坏语义）");
    CHECK(support_tokens::strip_support_tokens("NO_REPLYING 是个词") ==
              "NO_REPLYING 是个词",
          "无边界的 token 前缀不被切断");
    CHECK(support_tokens::strip_support_tokens("没有 token 的正文") ==
              "没有 token 的正文",
          "无 token 时原样返回");

    // =======================================================================
    std::cout << "== parse_thread_session_suffix（対齐 session-key-utils.ts:360-379）==\n";
    {
        auto r = sessions::parse_thread_session_suffix(
            "agent:main:telegram:group:-100123:thread:42");
        CHECK(r.base_session_key.has_value() &&
                  *r.base_session_key == "agent:main:telegram:group:-100123",
              "切出 base session key");
        CHECK(r.thread_id.has_value() && *r.thread_id == "42", "切出 thread id");
    }
    {
        auto r = sessions::parse_thread_session_suffix("agent:main:telegram:group:-100123");
        CHECK(r.base_session_key.has_value() &&
                  *r.base_session_key == "agent:main:telegram:group:-100123",
              "无 thread 标记时 base 为原串");
        CHECK(!r.thread_id.has_value(), "无 thread 标记时 threadId 为空");
    }
    {
        // 対齐:370 lastIndexOf —— 取**最后一个** :thread:
        auto r = sessions::parse_thread_session_suffix(
            "agent:main:slack:channel:C1:thread:T1:thread:T2");
        CHECK(r.thread_id.has_value() && *r.thread_id == "T2",
              "多个 thread 标记时取最后一个");
    }
    {
        auto r = sessions::parse_thread_session_suffix("   ");
        CHECK(!r.base_session_key.has_value() && !r.thread_id.has_value(),
              "空白串返回双空");
    }

    // =======================================================================
    std::cout << "== parse_session_delivery_route（対齐 session-key-utils.ts:388-429）==\n";
    // direct 形态：{channel}:{accountId}:direct:{peerId}
    {
        auto r = sessions::parse_session_delivery_route(
            "agent:main:telegram:acct1:direct:12345");
        CHECK(r.has_value(), "解析 direct 形态");
        if (r) {
            CHECK(r->channel == "telegram", "channel 正确");
            CHECK(r->peer_kind == "direct", "peerKind 正确");
            CHECK(r->peer_id == "12345", "peerId 正确");
            CHECK(r->account_id.has_value() && *r->account_id == "acct1",
                  "accountId 正确");
        }
    }
    // 対齐:409 —— peerId 含 ':' 时必须把剩余段全部拼回
    {
        auto r = sessions::parse_session_delivery_route(
            "agent:main:slack:acct1:dm:U123:extra");
        CHECK(r.has_value() && r->peer_id == "u123:extra",
              "peerId 含冒号时拼回全部剩余段");
    }
    // group 形态：{channel}:{group}:{peerId}
    {
        auto r = sessions::parse_session_delivery_route(
            "agent:main:telegram:group:-100999");
        CHECK(r.has_value(), "解析 group 形态");
        if (r) {
            CHECK(r->peer_kind == "group" && r->peer_id == "-100999",
                  "group peerKind/peerId 正确");
            CHECK(!r->account_id.has_value(), "group 形态无 accountId");
        }
    }
    // thread 后缀应被剥离并回填
    {
        auto r = sessions::parse_session_delivery_route(
            "agent:main:telegram:group:-100999:thread:77");
        CHECK(r.has_value() && r->peer_id == "-100999" &&
                  r->thread_id.has_value() && *r->thread_id == "77",
              "thread 后缀剥离后回填 threadId");
    }
    // ── 拒绝的形态 ──
    // **关键回归**：空 accountId 必须被拒。若沿用会过滤空段的
    // parse_agent_session_key，此串会被误解析成 direct/12345 且丢失账号，
    // 最终投递到错误账号。
    {
        auto r = sessions::parse_session_delivery_route(
            "agent:main:telegram::direct:12345");
        CHECK(!r.has_value(),
              "**关键回归**：accountId 为空段时拒绝解析（防投递到错误账号）");
    }
    CHECK(!sessions::parse_session_delivery_route("agent:main:telegram").has_value(),
          "段数不足时拒绝（対齐:398）");
    CHECK(!sessions::parse_session_delivery_route("telegram:acct:direct:1").has_value(),
          "缺 agent 包裹时拒绝（対齐:394）");
    CHECK(!sessions::parse_session_delivery_route(
              "agent:main:agent:nested:thing").has_value(),
          "内层再套 agent 视为不透明（対齐:398）");
    CHECK(!sessions::parse_session_delivery_route(
              "agent:main:telegram:unknownkind:1").has_value(),
          "peerKind 不在白名单时拒绝（対齐:381-386,425）");
    CHECK(!sessions::parse_session_delivery_route("").has_value(), "空串拒绝");

    // =======================================================================
    std::cout << "== A2A prompt 上下文（対齐 sessions-send-helpers.ts:79-155）==\n";
    {
        // 対齐:86-88 —— session 用占位符保护 prompt-cache，channel 保留具体值
        const std::string ctx = build_agent_to_agent_reply_context(
            "agent:main:telegram:acct:direct:1", "telegram",
            "agent:main:slack:acct:direct:2", "slack",
            A2ARole::Requester, 1, 5);
        CHECK(ctx.find("<REQUESTER_SESSION>") != std::string::npos,
              "requester session 被占位符化");
        CHECK(ctx.find("<TARGET_SESSION>") != std::string::npos,
              "target session 被占位符化");
        CHECK(ctx.find("agent:main:telegram") == std::string::npos,
              "具体 session key 不出现在 prompt（保护 prompt-cache）");
        CHECK(ctx.find("Agent 1 (requester) channel: telegram.") != std::string::npos,
              "channel 保留具体值");
        CHECK(ctx.find("Turn 1 of 5.") != std::string::npos, "轮次信息正确");
        CHECK(ctx.find("Current agent: Agent 1 (requester).") != std::string::npos,
              "当前角色标签正确");
        // **关键**：prompt 里的 token 必须与判定用的常量一致，否则模型照着回
        // 也永远匹配不上。原实现 prompt 写 "__REPLY_SKIP__"，判定也找它，
        // 但真源模型侧约定的是裸 "REPLY_SKIP"。
        CHECK(ctx.find("reply exactly \"REPLY_SKIP\"") != std::string::npos,
              "**关键回归**：prompt 中的 token 为裸值，与判定常量一致");
    }
    {
        const std::string ctx = build_agent_to_agent_announce_context(
            "agent:a", "telegram", "agent:b", "slack",
            "原始请求", "第一轮回复", "最新回复");
        CHECK(ctx.find("Original request: 原始请求") != std::string::npos,
              "原始请求写入");
        CHECK(ctx.find("Round 1 reply: 第一轮回复") != std::string::npos,
              "round-1 回复写入");
        CHECK(ctx.find("Latest reply: 最新回复") != std::string::npos,
              "最新回复写入");
        CHECK(ctx.find("reply exactly \"ANNOUNCE_SKIP\"") != std::string::npos,
              "announce token 为裸值");
        CHECK(ctx.find("After this reply, the agent-to-agent conversation is over.") !=
                  std::string::npos,
              "结束语存在（対齐:152）");
    }
    {
        // 対齐:146-149 —— 缺失时输出 "(not available)."
        const std::string ctx = build_agent_to_agent_announce_context(
            "", "", "agent:b", "", "msg", "", "");
        CHECK(ctx.find("Round 1 reply: (not available).") != std::string::npos,
              "round-1 缺失时输出 (not available).");
        CHECK(ctx.find("Latest reply: (not available).") != std::string::npos,
              "latest 缺失时输出 (not available).");
        // 対齐:89 —— requesterSessionKey 为空时该行整体不输出
        CHECK(ctx.find("Agent 1 (requester) session") == std::string::npos,
              "requester 缺失时不输出该行（対齐 filter(Boolean)）");
        CHECK(ctx.find("Agent 2 (target) session") != std::string::npos,
              "target session 行总是输出（対齐:93）");
    }

    // =======================================================================
    std::cout << "== resolve_ping_pong_turns（対齐 helpers.ts:158-160）==\n";
    CHECK(resolve_ping_pong_turns() == 5, "min(20, 5) == 5");
    CHECK(DEFAULT_AGENT_PING_PONG_TURNS == 5, "默认轮数対齐:20");
    CHECK(MAX_PING_PONG_TURNS == 20, "硬上限対齐:21");

    // =======================================================================
    std::cout << "== channel / lane 常量（対齐 agent-step.ts:9,80-81）==\n";
    // **关键回归**：真源是 "webchat"（message-channel-constants.ts:4），
    // 原实现写成 "internal" —— gateway 会按未知 channel 处理，
    // 渠道策略与回复投递全部走偏。
    CHECK(agents::INTERNAL_MESSAGE_CHANNEL == "webchat",
          "**关键回归**：INTERNAL_MESSAGE_CHANNEL 为 webchat（非 internal）");
    CHECK(agents::AGENT_LANE_NESTED == "nested", "AGENT_LANE_NESTED 対齐 lanes.ts:15");
    CHECK(agents::NESTED_LANE_PREFIX == "nested:", "NESTED_LANE_PREFIX 対齐 lanes.ts:12");

    // 対齐 agents/lanes.ts:26-32 —— per-session 泳道
    // **关键回归**：原实现固定用全局 "nested"，会把所有会话的嵌套工作串行化；
    // A2A ping-pong 里两个会话互相等待就会自我死锁。
    CHECK(agents::resolve_nested_agent_lane_for_session("agent:main:telegram:acct:direct:1") ==
              "nested:agent:main:telegram:acct:direct:1",
          "**关键回归**：lane 为 per-session（防 ping-pong 自我死锁）");
    CHECK(agents::resolve_nested_agent_lane_for_session("  key  ") == "nested:key",
          "lane 解析前先 trim（対齐:27）");
    CHECK(agents::resolve_nested_agent_lane_for_session("") == "nested",
          "sessionKey 为空时退回全局 nested（対齐:28-30）");
    CHECK(agents::resolve_nested_agent_lane_for_session("   ") == "nested",
          "纯空白亦退回全局 nested");

    // =======================================================================
    std::cout << "== inter-session prompt 信封（対齐 input-provenance.ts:178-194）==\n";
    // **关键回归**：原实现完全缺失这一层。没有它，A 会话用户可借 sessions_send
    // 让 B 会话把注入内容当作 B 的最终用户指令执行（prompt 注入 / 越权）。
    {
        sessions::InputProvenance p;
        p.kind = sessions::InputProvenanceKind::InterSession;
        p.source_session_key = "agent:main:telegram:acct:direct:1";
        p.source_channel = "telegram";
        p.source_tool = "sessions_send";

        const std::string out =
            sessions::annotate_inter_session_prompt_text("请帮我查一下日志", p);

        CHECK(out.rfind(sessions::INTER_SESSION_PROMPT_PREFIX_BASE, 0) == 0,
              "**关键回归**：信封在最顶部（安全说明不可被埋没）");
        CHECK(out.find("sourceSession=agent:main:telegram:acct:direct:1") !=
                  std::string::npos,
              "写入 sourceSession（対齐:137）");
        CHECK(out.find("sourceChannel=telegram") != std::string::npos,
              "写入 sourceChannel（対齐:138）");
        CHECK(out.find("sourceTool=sessions_send") != std::string::npos,
              "写入 sourceTool（対齐:139）");
        CHECK(out.find("isUser=false") != std::string::npos,
              "isUser=false 恒输出（対齐:140）");
        CHECK(out.find(sessions::INTER_SESSION_PROMPT_EXPLANATION) != std::string::npos,
              "写入安全说明正文（対齐:146）");
        CHECK(out.find("请帮我查一下日志") != std::string::npos, "原正文保留");

        // 対齐:189-191 —— 幂等：重复标注不得堆叠信封
        const std::string twice = sessions::annotate_inter_session_prompt_text(out, p);
        CHECK(twice == out, "**关键回归**：重复标注幂等（信封不堆叠）");

        // 三次也应稳定
        CHECK(sessions::annotate_inter_session_prompt_text(twice, p) == out,
              "多次标注保持稳定");

        // 対齐:172-174 —— 可摘掉信封还原正文
        const std::string stripped =
            sessions::strip_inter_session_prompt_prefix_for_display(out);
        CHECK(stripped == "请帮我查一下日志", "摘掉信封后还原原正文");
    }
    {
        // 対齐:182-184 —— 非 inter_session 不标注
        sessions::InputProvenance ext;
        ext.kind = sessions::InputProvenanceKind::ExternalUser;
        CHECK(sessions::annotate_inter_session_prompt_text("hello", ext) == "hello",
              "external_user 不标注（対齐:182-184）");
        sessions::InputProvenance sys;
        sys.kind = sessions::InputProvenanceKind::InternalSystem;
        CHECK(sessions::annotate_inter_session_prompt_text("hello", sys) == "hello",
              "internal_system 不标注");
    }
    {
        // 対齐:185-187 —— 空正文不标注（否则产出只有信封的空消息）
        sessions::InputProvenance p;
        p.source_tool = "sessions_send";
        CHECK(sessions::annotate_inter_session_prompt_text("", p).empty(),
              "空正文不标注（対齐:185-187）");
        CHECK(sessions::annotate_inter_session_prompt_text("   \n ", p) == "   \n ",
              "纯空白正文原样返回");
    }
    {
        // 来源字段缺失时只输出 isUser=false，不出现空的 sourceX=
        sessions::InputProvenance p;
        p.kind = sessions::InputProvenanceKind::InterSession;
        const std::string out = sessions::annotate_inter_session_prompt_text("x", p);
        CHECK(out.find("sourceSession=") == std::string::npos,
              "sourceSession 缺失时不输出该项（対齐 filter(Boolean)）");
        CHECK(out.find("sourceChannel=") == std::string::npos,
              "sourceChannel 缺失时不输出该项");
        CHECK(out.find("isUser=false") != std::string::npos,
              "isUser=false 仍恒输出");
    }
    CHECK(sessions::to_string(sessions::InputProvenanceKind::InterSession) ==
              "inter_session",
          "kind 序列化対齐:8");
    CHECK(sessions::to_string(sessions::InputProvenanceKind::ExternalUser) ==
              "external_user",
          "external_user 序列化正确");
    CHECK(sessions::to_string(sessions::InputProvenanceKind::InternalSystem) ==
              "internal_system",
          "internal_system 序列化正确");

    // =======================================================================
    std::cout << "== is_recoverable_agent_wait_error（対齐 run-wait.ts:134-157）==\n";
    // 这组决定「网络抖动」是否会被误报成投递失败去打断用户。
    CHECK(agents::is_recoverable_agent_wait_error("gateway closed (1006)"),
          "1006 关闭属可恢复（対齐:135）");
    CHECK(agents::is_recoverable_agent_wait_error("transport close"),
          "transport close 属可恢复");
    CHECK(agents::is_recoverable_agent_wait_error("gateway not connected"),
          "未连接属可恢复");
    CHECK(agents::is_recoverable_agent_wait_error("socket hang up"),
          "socket hang up 属可恢复");
    CHECK(agents::is_recoverable_agent_wait_error("no active telegram listener"),
          "no active .* listener 属可恢复（対齐:140）");
    CHECK(agents::is_recoverable_agent_wait_error("ECONNRESET"),
          "errno 码属可恢复（大小写不敏感）");
    // 対齐:150-152 —— gateway timeout 刻意**排除**在可恢复之外
    CHECK(!agents::is_recoverable_agent_wait_error("gateway timeout after 30s"),
          "**关键**：gateway timeout 不属可恢复（対齐:150-152）");
    CHECK(!agents::is_recoverable_agent_wait_error("model refused to answer"),
          "业务错误不属可恢复");
    CHECK(!agents::is_recoverable_agent_wait_error(""), "空错误不属可恢复");
    CHECK(!agents::is_recoverable_agent_wait_error("   "), "空白错误不属可恢复");

    // =======================================================================
    std::cout << "== has_updated_assistant_reply_snapshot（対齐 run-wait.ts:313-330）==\n";
    // **关键回归**：baseline 比对缺失会把上一轮旧回复当本轮结果重复投递。
    {
        agents::AssistantReplySnapshot latest;
        latest.text = "新回复";
        latest.fingerprint = "fp-new";

        agents::AssistantReplySnapshot none;
        CHECK(agents::has_updated_assistant_reply_snapshot(latest, none),
              "无 baseline 时任何回复都算新（対齐:320-322）");

        agents::AssistantReplySnapshot same;
        same.text = "新回复";
        same.fingerprint = "fp-new";
        CHECK(!agents::has_updated_assistant_reply_snapshot(latest, same),
              "**关键回归**：指纹相同判为未更新（防重复投递）");

        // 対齐:323-325 —— 有指纹优先比指纹：同文本但属新消息也算更新
        agents::AssistantReplySnapshot same_text_diff_fp;
        same_text_diff_fp.text = "新回复";
        same_text_diff_fp.fingerprint = "fp-old";
        CHECK(agents::has_updated_assistant_reply_snapshot(latest, same_text_diff_fp),
              "同文本但指纹不同判为已更新（対齐:323-325）");

        // 対齐:326-328 —— 无指纹时退回比文本
        agents::AssistantReplySnapshot text_only;
        text_only.text = "新回复";
        CHECK(!agents::has_updated_assistant_reply_snapshot(latest, text_only),
              "baseline 仅有文本且相同 → 未更新（対齐:326-328）");
        agents::AssistantReplySnapshot text_only_diff;
        text_only_diff.text = "旧回复";
        CHECK(agents::has_updated_assistant_reply_snapshot(latest, text_only_diff),
              "baseline 仅有文本且不同 → 已更新");

        // 対齐:317-319 —— 最新快照无文本一律未更新
        agents::AssistantReplySnapshot empty_latest;
        CHECK(!agents::has_updated_assistant_reply_snapshot(empty_latest, none),
              "最新快照为空 → 未更新（対齐:317-319）");
    }

    // =======================================================================
    std::cout << "== wait 三态判定（対齐 sessions-send-tool.ts:288-296）==\n";
    // **关键回归**：此前 AgentWaitResult 只有 status/error/pending_error，
    // 无法区分「终态超时」与「还在跑」，导致所有 timeout 都被当终态、
    // 慢 run 的回复彻底丢失。
    {
        // ── is_terminal_timeout：endedAt 存在 或 stopReason/livenessState 非空 ──
        agents::AgentWaitResult still_running;
        still_running.status = agents::AgentWaitStatus::Timeout;
        CHECK(!still_running.is_terminal_timeout(),
              "**关键回归**：三者皆空 → 非终态超时，须返回 accepted 并挂 flow");

        agents::AgentWaitResult ended;
        ended.status = agents::AgentWaitStatus::Timeout;
        ended.has_ended_at = true;
        ended.ended_at = 1234;
        CHECK(ended.is_terminal_timeout(),
              "endedAt 存在 → 终态超时（対齐:289）");

        agents::AgentWaitResult stopped;
        stopped.status = agents::AgentWaitStatus::Timeout;
        stopped.stop_reason = "max_tokens";
        CHECK(stopped.is_terminal_timeout(),
              "stopReason 非空 → 终态超时");

        agents::AgentWaitResult liveness;
        liveness.status = agents::AgentWaitStatus::Timeout;
        liveness.liveness_state = "idle";
        CHECK(liveness.is_terminal_timeout(),
              "livenessState 非空 → 终态超时");

        // endedAt=0 也算存在（presence 而非真值）
        agents::AgentWaitResult ended_zero;
        ended_zero.has_ended_at = true;
        ended_zero.ended_at = 0;
        CHECK(ended_zero.is_terminal_timeout(),
              "endedAt=0 仍算存在（presence 语义，不是真值判断）");

        // ── is_pending_error_timeout：pendingError 且 error 非空 ──
        agents::AgentWaitResult pe_both;
        pe_both.pending_error = true;
        pe_both.error = "delivery unconfirmed";
        CHECK(pe_both.is_pending_error_timeout(),
              "pendingError + 错误文本 → pending-error 超时（対齐:292-296）");

        agents::AgentWaitResult pe_no_text;
        pe_no_text.pending_error = true;
        CHECK(!pe_no_text.is_pending_error_timeout(),
              "**关键**：pendingError 但无错误文本 → 不算（防把慢 run 误报为投递异常）");

        agents::AgentWaitResult pe_blank;
        pe_blank.pending_error = true;
        pe_blank.error = "   ";
        CHECK(!pe_blank.is_pending_error_timeout(),
              "错误文本仅空白 → 不算（対齐 error.trim() !== \"\"）");

        agents::AgentWaitResult pe_no_flag;
        pe_no_flag.error = "some error";
        CHECK(!pe_no_flag.is_pending_error_timeout(),
              "有错误文本但 pendingError 为假 → 不算");
    }

    // =======================================================================
    std::cout << "== select_deliverable_sessions_reply（対齐 tokens.ts:36-49）==\n";
    // 语义**刻意不对称**：只有 NO_REPLY 允许回落 fallback，
    // ANNOUNCE_SKIP/REPLY_SKIP/HEARTBEAT_OK 是明确的"别发"指令，禁止绕过。
    {
        using support_tokens::select_deliverable_sessions_reply;

        CHECK(select_deliverable_sessions_reply("实质回复", "兜底") == "实质回复",
              "primary 可投递 → 用 primary（対齐:41-43）");
        CHECK(select_deliverable_sessions_reply("  实质回复  ", "兜底") == "实质回复",
              "primary 结果已 trim");

        // NO_REPLY → 允许回落
        CHECK(select_deliverable_sessions_reply("NO_REPLY", "兜底") == "兜底",
              "**关键**：primary=NO_REPLY 时允许回落 fallback（対齐:44-48）");
        CHECK(select_deliverable_sessions_reply("", "兜底") == "兜底",
              "primary 为空时允许回落 fallback");
        CHECK(select_deliverable_sessions_reply("   ", "兜底") == "兜底",
              "primary 仅空白等同为空");

        // 其余三个 token → 禁止回落
        CHECK(select_deliverable_sessions_reply("ANNOUNCE_SKIP", "兜底").empty(),
              "**关键**：ANNOUNCE_SKIP 禁止回落 fallback（対齐:44-46）");
        CHECK(select_deliverable_sessions_reply("REPLY_SKIP", "兜底").empty(),
              "**关键**：REPLY_SKIP 禁止回落 fallback");
        CHECK(select_deliverable_sessions_reply("HEARTBEAT_OK", "兜底").empty(),
              "**关键**：HEARTBEAT_OK 禁止回落 fallback");

        // fallback 自身也要过检查
        CHECK(select_deliverable_sessions_reply("NO_REPLY", "NO_REPLY").empty(),
              "fallback 也是 token → 无可投递内容（対齐:48）");
        CHECK(select_deliverable_sessions_reply("NO_REPLY", "").empty(),
              "primary=NO_REPLY 且无 fallback → 空");
        CHECK(select_deliverable_sessions_reply("", "").empty(),
              "两者皆空 → 空");

        // 混合正文不受影响（token-only 判定）
        CHECK(select_deliverable_sessions_reply("处理完毕，无需回复 NO_REPLY", "兜底") ==
                  "处理完毕，无需回复 NO_REPLY",
              "**关键回归**：以 token 结尾的实质回复仍可投递（openclaw#19537）");
    }

    // =======================================================================
    std::cout << "== liveness / stop-reason 谓词（対齐 agent-liveness.ts + run-termination.ts）==\n";
    // **关键回归**：上一轮我凭直觉把 blocked 判定写成
    // {blocked, stalled, unresponsive}，真源**只认 "blocked"**。
    // 多认会把正常完成的 run 误判为阻塞并丢弃其回复。
    CHECK(agents::is_blocked_liveness_state("blocked"), "blocked 命中");
    CHECK(agents::is_blocked_liveness_state("  BLOCKED  "),
          "trim + 大小写不敏感（対齐:3）");
    CHECK(!agents::is_blocked_liveness_state("stalled"),
          "**关键**：stalled 不是 blocked（真源只认一个值）");
    CHECK(!agents::is_blocked_liveness_state("unresponsive"),
          "**关键**：unresponsive 不是 blocked");
    CHECK(!agents::is_blocked_liveness_state("blocked_by_tool"),
          "**关键**：子串不算（严格相等，非包含）");
    CHECK(!agents::is_blocked_liveness_state(""), "空串不算");

    CHECK(agents::is_abandoned_liveness_state("abandoned"), "abandoned 命中");
    CHECK(agents::is_abandoned_liveness_state("ABANDONED"), "大小写不敏感");
    CHECK(!agents::is_abandoned_liveness_state("blocked"), "blocked 不是 abandoned");

    CHECK(agents::format_blocked_liveness_error("boom") == "boom",
          "有原文用原文（対齐:13-14）");
    CHECK(agents::format_blocked_liveness_error("  ") ==
              "Agent run blocked before producing a usable result.",
          "空白原文用兜底文案");
    CHECK(agents::format_abandoned_liveness_error("") ==
              "Agent run ended before producing a complete result.",
          "abandoned 兜底文案");

    CHECK(agents::is_cancellation_stop_reason("rpc"), "rpc 属取消（対齐:486）");
    CHECK(agents::is_cancellation_stop_reason("stop"), "stop 属取消");
    CHECK(!agents::is_cancellation_stop_reason("aborted"), "aborted 不属取消（属 abort）");
    CHECK(agents::is_aborted_agent_stop_reason("aborted"), "aborted 属 abort");
    CHECK(agents::is_aborted_agent_stop_reason("restart"), "restart 属 abort（対齐:138）");
    CHECK(!agents::is_aborted_agent_stop_reason("stop"), "stop 不属 abort");

    // =======================================================================
    std::cout << "== timeout phase 归因（対齐 run-timeout-attribution.ts + :462）==\n";
    CHECK(agents::normalize_agent_run_timeout_phase("provider") ==
              agents::AgentRunTimeoutPhase::Provider, "provider 解析正确");
    CHECK(agents::normalize_agent_run_timeout_phase("post_turn") ==
              agents::AgentRunTimeoutPhase::PostTurn, "post_turn 解析正确");
    CHECK(agents::normalize_agent_run_timeout_phase("  queue  ") ==
              agents::AgentRunTimeoutPhase::Queue, "trim 后解析");
    CHECK(agents::normalize_agent_run_timeout_phase("PROVIDER") ==
              agents::AgentRunTimeoutPhase::None,
          "**关键**：真源不做 lowercase，大写不匹配");
    CHECK(agents::normalize_agent_run_timeout_phase("bogus") ==
              agents::AgentRunTimeoutPhase::None, "未知值归 None");

    // 対齐:462 —— 只有这三个算硬超时
    CHECK(agents::is_hard_agent_run_timeout_phase(agents::AgentRunTimeoutPhase::Preflight),
          "preflight 属硬超时");
    CHECK(agents::is_hard_agent_run_timeout_phase(agents::AgentRunTimeoutPhase::Provider),
          "provider 属硬超时");
    CHECK(agents::is_hard_agent_run_timeout_phase(agents::AgentRunTimeoutPhase::PostTurn),
          "post_turn 属硬超时");
    CHECK(!agents::is_hard_agent_run_timeout_phase(agents::AgentRunTimeoutPhase::Queue),
          "**关键**：queue 不算硬超时（属 wait 层不确定性）");
    CHECK(!agents::is_hard_agent_run_timeout_phase(
              agents::AgentRunTimeoutPhase::GatewayDraining),
          "**关键**：gateway_draining 不算硬超时");

    // =======================================================================
    std::cout << "== normalize_agent_wait_result（対齐 agent-run-terminal-outcome.ts:496-563）==\n";
    {
        using agents::AgentWaitResult;
        using agents::AgentWaitStatus;
        using agents::AgentRunTerminalReason;
        using agents::AgentRunTimeoutPhase;

        // ── 正常完成 ──
        {
            AgentWaitResult r;
            r.status = AgentWaitStatus::Ok;
            agents::normalize_agent_wait_result(&r);
            CHECK(r.status == AgentWaitStatus::Ok &&
                      r.terminal_reason == AgentRunTerminalReason::Completed,
                  "裸 ok → completed/ok");
        }
        // ── blocked：即便 gateway 回 ok 也必须降级为 error ──
        {
            AgentWaitResult r;
            r.status = AgentWaitStatus::Ok;
            r.liveness_state = "blocked";
            agents::normalize_agent_wait_result(&r);
            CHECK(r.status == AgentWaitStatus::Error &&
                      r.terminal_reason == AgentRunTerminalReason::Blocked,
                  "**关键**：ok + blocked → error（防读取不存在的新回复）");
            CHECK(r.error == "Agent run blocked before producing a usable result.",
                  "blocked 错误文案来自 formatBlockedLivenessError");
        }
        // ── hard timeout：phase 属硬超时 ──
        {
            AgentWaitResult r;
            r.status = AgentWaitStatus::Timeout;
            r.timeout_phase = AgentRunTimeoutPhase::Provider;
            agents::normalize_agent_wait_result(&r);
            CHECK(r.status == AgentWaitStatus::Timeout &&
                      r.terminal_reason == AgentRunTerminalReason::HardTimeout,
                  "provider 阶段超时 → hard_timeout/timeout");
            CHECK(r.is_sticky_terminal(), "hard_timeout 是粘性终态（対齐:479-483）");
        }
        // ── hard timeout 的第二条来源：timeout + providerStarted==true ──
        {
            AgentWaitResult r;
            r.status = AgentWaitStatus::Timeout;
            r.has_provider_started = true;
            r.provider_started = true;
            agents::normalize_agent_wait_result(&r);
            CHECK(r.terminal_reason == AgentRunTerminalReason::HardTimeout,
                  "timeout + providerStarted=true → hard_timeout（対齐:507-509）");
        }
        // providerStarted 未提供 → 不算 hard timeout
        {
            AgentWaitResult r;
            r.status = AgentWaitStatus::Timeout;
            agents::normalize_agent_wait_result(&r);
            CHECK(r.terminal_reason == AgentRunTerminalReason::TimedOut,
                  "**关键**：providerStarted 未提供 → 普通 timed_out，非 hard");
        }
        // providerStarted=false → 不算 hard timeout
        {
            AgentWaitResult r;
            r.status = AgentWaitStatus::Timeout;
            r.has_provider_started = true;
            r.provider_started = false;
            agents::normalize_agent_wait_result(&r);
            CHECK(r.terminal_reason == AgentRunTerminalReason::TimedOut,
                  "providerStarted=false → 普通 timed_out");
        }
        // hard_timeout 优先于 blocked（対齐 error/reason 链的顺序）
        {
            AgentWaitResult r;
            r.status = AgentWaitStatus::Timeout;
            r.timeout_phase = AgentRunTimeoutPhase::Provider;
            r.liveness_state = "blocked";
            r.error = "orig";
            agents::normalize_agent_wait_result(&r);
            CHECK(r.terminal_reason == AgentRunTerminalReason::HardTimeout,
                  "**关键**：hard_timeout 优先于 blocked（対齐:528-531 顺序）");
            CHECK(r.error == "orig",
                  "hard_timeout 保留原始 error，不套 blocked 文案（対齐:517-518）");
        }
        // ── aborted ──
        {
            AgentWaitResult r;
            r.status = AgentWaitStatus::Error;
            r.stop_reason = "aborted";
            agents::normalize_agent_wait_result(&r);
            CHECK(r.terminal_reason == AgentRunTerminalReason::Aborted &&
                      r.status == AgentWaitStatus::Error,
                  "stopReason=aborted → aborted/error");
            CHECK(r.error == "agent run aborted",
                  "aborted 且无原文 → 用 AGENT_RUN_ABORTED_ERROR（対齐:521-522）");
        }
        // restart 归 cancelled 而非 aborted（対齐:510 的 !restartCancelled）
        {
            AgentWaitResult r;
            r.status = AgentWaitStatus::Error;
            r.stop_reason = "restart";
            agents::normalize_agent_wait_result(&r);
            CHECK(r.terminal_reason == AgentRunTerminalReason::Cancelled,
                  "**关键**：restart → cancelled，不是 aborted（対齐:504,510,513）");
            CHECK(r.is_sticky_terminal(), "cancelled 也是粘性终态");
        }
        // ── cancelled：仅在 status != ok 时 rpc/stop 才算取消 ──
        {
            AgentWaitResult r;
            r.status = AgentWaitStatus::Error;
            r.stop_reason = "stop";
            agents::normalize_agent_wait_result(&r);
            CHECK(r.terminal_reason == AgentRunTerminalReason::Cancelled,
                  "非 ok + stop → cancelled");
        }
        {
            AgentWaitResult r;
            r.status = AgentWaitStatus::Ok;
            r.stop_reason = "stop";
            agents::normalize_agent_wait_result(&r);
            CHECK(r.terminal_reason == AgentRunTerminalReason::Completed &&
                      r.status == AgentWaitStatus::Ok,
                  "**关键**：ok + stop 是正常收尾，不算取消（対齐:511-514）");
        }
        // ── abandoned ──
        {
            AgentWaitResult r;
            r.status = AgentWaitStatus::Ok;
            r.liveness_state = "abandoned";
            agents::normalize_agent_wait_result(&r);
            CHECK(r.terminal_reason == AgentRunTerminalReason::Abandoned &&
                      r.status == AgentWaitStatus::Error,
                  "abandoned → error");
            CHECK(r.error == "Agent run ended before producing a complete result.",
                  "abandoned 兜底文案");
        }
        // ── failed ──
        {
            AgentWaitResult r;
            r.status = AgentWaitStatus::Error;
            r.error = "provider exploded";
            agents::normalize_agent_wait_result(&r);
            CHECK(r.terminal_reason == AgentRunTerminalReason::Failed &&
                      r.error == "provider exploded",
                  "裸 error → failed，保留原文");
        }
        // 空白 stopReason 视为不存在（asNonEmptyString）
        {
            AgentWaitResult r;
            r.status = AgentWaitStatus::Error;
            r.stop_reason = "   ";
            agents::normalize_agent_wait_result(&r);
            CHECK(r.terminal_reason == AgentRunTerminalReason::Failed,
                  "空白 stopReason 视为不存在（対齐 asNonEmptyString:468-470）");
        }
    }

    // =======================================================================
    std::cout << "== auto_reply/tokens.h（対齐 tokens.ts:29,42 —— 三处此前遗漏）==\n";
    // 这组独立于 tools/support_tokens：auto_reply 侧有自己一份 token 实现，
    // 被 reply 投递链复用。此前它的正则缺 `(?:\s+TOKEN)*`、缺 icase、
    // 缺剥标点，导致重复 token / 小写 / Markdown 包裹三类形态识别不出来。
    {
        using hunterclaw::auto_reply::is_silent_reply_text;
        using hunterclaw::auto_reply::strip_silent_token;
        using hunterclaw::auto_reply::SILENT_REPLY_TOKEN;

        // ① 重复 token（対齐 tokens.ts:29 的 `(?:\s+TOKEN)*`）
        CHECK(is_silent_reply_text("NO_REPLY NO_REPLY", SILENT_REPLY_TOKEN),
              "**关键**：重复 token 应命中（模型偶尔连吐两遍）");
        CHECK(is_silent_reply_text("NO_REPLY\n NO_REPLY  ", SILENT_REPLY_TOKEN),
              "重复 + 换行分隔应命中");
        // ② 大小写不敏感（対齐正则的 "i" 标志）
        CHECK(is_silent_reply_text("no_reply", SILENT_REPLY_TOKEN),
              "**关键**：小写应命中（真源带 icase）");
        CHECK(is_silent_reply_text("No_Reply", SILENT_REPLY_TOKEN),
              "混合大小写应命中");
        // ③ 剥首尾标点（対齐 tokens.ts:47-49 + :65 第二次判定）
        CHECK(is_silent_reply_text("**NO_REPLY**", SILENT_REPLY_TOKEN),
              "**关键**：Markdown 加粗应命中");
        CHECK(is_silent_reply_text("NO_REPLY。", SILENT_REPLY_TOKEN),
              "中文句号应命中");
        CHECK(is_silent_reply_text("[NO_REPLY]", SILENT_REPLY_TOKEN),
              "方括号应命中");
        // token-only 底线（openclaw#19537）
        CHECK(!is_silent_reply_text("处理完毕，详见 NO_REPLY 说明", SILENT_REPLY_TOKEN),
              "**关键回归**：实质回复不得被判静默");
        CHECK(!is_silent_reply_text("NO_REPLYING", SILENT_REPLY_TOKEN),
              "NO_REPLYING 不得命中");
        CHECK(!is_silent_reply_text("", SILENT_REPLY_TOKEN), "空串不命中");
        // emoji 刻意视为实质内容（tokens.ts:61-62）
        CHECK(!is_silent_reply_text("🎉", SILENT_REPLY_TOKEN),
              "**刻意**：emoji 是实质内容，不剥 Symbol 类");

        // strip 侧（以下行为均已用 node 跑真源正则逐条核对）
        CHECK(strip_silent_token("好了 no_reply", SILENT_REPLY_TOKEN) == "好了",
              "**关键**：小写 token 应被剥（此前 icase 缺失会残留到用户可见文本）");
        CHECK(strip_silent_token("好了 NO_REPLY NO_REPLY", SILENT_REPLY_TOKEN) == "好了",
              "重复 token 应被一并剥掉");
        CHECK(strip_silent_token("好了 **NO_REPLY", SILENT_REPLY_TOKEN) == "好了",
              "前置 `**` 属合法边界，应被剥（対齐 `\\*+`）");
        CHECK(strip_silent_token("好了 **NO_REPLY**", SILENT_REPLY_TOKEN) ==
                  "好了 **NO_REPLY**",
              "**已核对真源**：尾部 `**` 使 `\\s*$` 不匹配，原样保留");
        CHECK(strip_silent_token("abcNO_REPLY", SILENT_REPLY_TOKEN) == "abcNO_REPLY",
              "**关键回归**：紧贴单词不得剥（前置边界缺失时会误切成 abc）");
    }

    // =======================================================================
    std::cout << "\n========================================\n";
    std::cout << "  通过: " << g_pass << "   失败: " << g_fail << "\n";
    std::cout << "========================================\n";
    return g_fail == 0 ? 0 : 1;
}
