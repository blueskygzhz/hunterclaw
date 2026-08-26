// ============================================================================
// test_acp_alignment.cc
// ACP 与 OpenClaw 真源的语义一致性验证
//
// 覆盖：
//   * approval-classifier.ts  → acp_client（工具名防伪造 / cwd 范围 / 审批分类）
//   * policy.ts               → acp_client（enabled / dispatch / allowedAgents）
//   * client-helpers.ts       → acp_client（option kind / 权限解析流程）
//   * tool-status.ts          → acp_tool_status（终态映射）
//   * conversation-id.ts      → acp_conversation_id（normalizeConversationText）
//   * session-mapper.ts       → acp_session_mapper（解析顺序 / reset 方法名）
//
// 编译（示例）：
//   g++-12 -std=c++17 -Iinclude -Iexternal/json/single_include \
//     -Iexternal/ixwebsocket -Ithird_party -Iexternal \
//     -o /tmp/t_acp tests/test_acp_alignment.cc \
//     src/acp/acp_client.cc src/acp/acp_tool_status.cc \
//     src/acp/acp_conversation_id.cc src/acp/acp_session_mapper.cc ...
// ============================================================================

#include <iostream>
#include <string>

#include "hunterclaw/acp/acp_client.h"
#include "hunterclaw/acp/acp_conversation_id.h"
#include "hunterclaw/acp/acp_session_mapper.h"
#include "hunterclaw/acp/acp_tool_status.h"
#include "hunterclaw/acp/acp_translator.h"

using nlohmann::json;

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond, msg)                                              \
    do {                                                              \
        if (cond) {                                                   \
            ++g_pass;                                                 \
        } else {                                                      \
            ++g_fail;                                                 \
            std::cout << "  ✗ FAIL: " << (msg) << "\n";               \
        }                                                             \
    } while (0)

namespace hc = hunterclaw;
namespace acp = hunterclaw::acp;

int main() {
    // =======================================================================
    std::cout << "== normalize_tool_name（対齐 approval-classifier.ts:66-72）==\n";
    CHECK(acp::normalize_tool_name("Read") == "read", "lowercase 归一");
    CHECK(acp::normalize_tool_name("  exec  ") == "exec", "trim");
    CHECK(acp::normalize_tool_name("web_search") == "web_search", "下划线合法");
    CHECK(acp::normalize_tool_name("a.b-c_d") == "a.b-c_d", ". - _ 均合法");
    CHECK(acp::normalize_tool_name("").empty(), "空串非法");
    CHECK(acp::normalize_tool_name("has space").empty(),
          "**关键**：含空格非法（対齐 /^[a-z0-9._-]+$/）");
    CHECK(acp::normalize_tool_name("semi;colon").empty(), "含分号非法");
    CHECK(acp::normalize_tool_name("path/slash").empty(), "含斜杠非法");
    CHECK(acp::normalize_tool_name(std::string(129, 'a')).empty(),
          "**关键**：超过 128 字符非法（対齐:68）");
    CHECK(acp::normalize_tool_name(std::string(128, 'a')).size() == 128,
          "恰好 128 字符合法");

    // =======================================================================
    std::cout << "== resolve_tool_name_for_permission 防伪造（対齐 :82-112）==\n";
    // **这组是安全核心**：此前实现直接信任调用方传入的 tool_name，
    // 等于让对端自报身份。真源从 _meta / rawInput / title 三源解析并交叉校验。
    {
        // 单源：各自可用
        CHECK(acp::resolve_tool_name_for_permission(
                  json{{"_meta", {{"toolName", "read"}}}}) == "read",
              "_meta.toolName 可解析");
        CHECK(acp::resolve_tool_name_for_permission(
                  json{{"title", "read: /tmp/a.txt"}}) == "read",
              "title 冒号前为工具名（対齐:78）");
        CHECK(acp::resolve_tool_name_for_permission(
                  json{{"rawInput", {{"tool", "search"}}}}) == "search",
              "rawInput.tool 可解析");

        // 优先级 meta > title > rawInput（対齐:111）
        CHECK(acp::resolve_tool_name_for_permission(
                  json{{"_meta", {{"toolName", "read"}}},
                       {"title", "read: x"}}) == "read",
              "meta 与 title 一致时取 meta");

        // **冲突即拒绝**（対齐:102-110）
        CHECK(acp::resolve_tool_name_for_permission(
                  json{{"_meta", {{"toolName", "exec"}}},
                       {"title", "read: /etc/shadow"}}).empty(),
              "**关键**：meta=exec 与 title=read 冲突 → 拒绝（防伪造）");
        CHECK(acp::resolve_tool_name_for_permission(
                  json{{"rawInput", {{"tool", "exec"}}},
                       {"_meta", {{"toolName", "read"}}}}).empty(),
              "**关键**：rawInput 与 meta 冲突 → 拒绝");
        CHECK(acp::resolve_tool_name_for_permission(
                  json{{"rawInput", {{"tool", "exec"}}},
                       {"title", "read: x"}}).empty(),
              "**关键**：rawInput 与 title 冲突 → 拒绝");

        // 源里有值但格式非法 → 直接拒绝（対齐:99-101）
        CHECK(acp::resolve_tool_name_for_permission(
                  json{{"_meta", {{"toolName", "bad name!"}}},
                       {"title", "read: x"}}).empty(),
              "**关键**：meta 格式非法即拒绝，不退回 title");

        // 无任何来源
        CHECK(acp::resolve_tool_name_for_permission(json::object()).empty(),
              "无来源 → 空");
        CHECK(acp::resolve_tool_name_for_permission(json{{"title", "no-colon"}}) ==
                  "no-colon",
              "title 无冒号时整体作为工具名");
    }

    // =======================================================================
    std::cout << "== is_tool_path_scoped_to_cwd（対齐 :167-194）==\n";
    {
        const std::string cwd = "/tmp/work";
        CHECK(acp::is_tool_path_scoped_to_cwd("/tmp/work/a.txt", cwd),
              "cwd 内绝对路径");
        CHECK(acp::is_tool_path_scoped_to_cwd("a.txt", cwd),
              "相对路径按 cwd 解析");
        CHECK(acp::is_tool_path_scoped_to_cwd("./sub/b.txt", cwd),
              "./ 前缀相对路径");
        CHECK(!acp::is_tool_path_scoped_to_cwd("/etc/shadow", cwd),
              "**关键**：cwd 外绝对路径拒绝");
        CHECK(!acp::is_tool_path_scoped_to_cwd("../escape.txt", cwd),
              "**关键**：.. 逃逸拒绝");
        CHECK(!acp::is_tool_path_scoped_to_cwd("/tmp/work/../../etc/passwd", cwd),
              "**关键**：多级 .. 逃逸拒绝（需归一化后判定）");
        CHECK(acp::is_tool_path_scoped_to_cwd("file:///tmp/work/c.txt", cwd),
              "file:// URL 解析（対齐:172-179）");
        CHECK(!acp::is_tool_path_scoped_to_cwd("file:///etc/hosts", cwd),
              "file:// 越界拒绝");
        CHECK(acp::is_tool_path_scoped_to_cwd("file:///tmp/work/a%20b.txt", cwd),
              "file:// percent-decode（%20 → 空格）");
        CHECK(!acp::is_tool_path_scoped_to_cwd("", cwd), "空路径拒绝");
        CHECK(!acp::is_tool_path_scoped_to_cwd("   ", cwd), "空白路径拒绝");
    }

    // =======================================================================
    std::cout << "== classify_acp_tool_approval（対齐 :197-245）==\n";
    {
        const std::string cwd = "/tmp/work";

        // ── read：必须路径全部在 cwd 内才自动批准（対齐:207-222）──
        {
            auto c = acp::classify_acp_tool_approval(
                json{{"_meta", {{"toolName", "read"}}},
                     {"rawInput", {{"path", "/tmp/work/a.txt"}}}}, cwd);
            CHECK(c.approval_class == acp::AcpApprovalClass::ReadonlyScoped &&
                      c.auto_approve,
                  "read + cwd 内路径 → readonly_scoped 自动批准");
        }
        {
            auto c = acp::classify_acp_tool_approval(
                json{{"_meta", {{"toolName", "read"}}},
                     {"rawInput", {{"path", "/etc/shadow"}}}}, cwd);
            CHECK(c.approval_class == acp::AcpApprovalClass::Other &&
                      !c.auto_approve,
                  "**关键安全回归**：read /etc/shadow → other，不自动批准");
        }
        {
            // 対齐:214-216 —— 无路径候选时 rawPaths.length===0 → autoApprove=false
            auto c = acp::classify_acp_tool_approval(
                json{{"_meta", {{"toolName", "read"}}}}, cwd);
            CHECK(!c.auto_approve,
                  "**关键**：read 但解析不出路径 → 不自动批准（対齐:215）");
        }

        // ── 搜索类：任一越界即降级，但无路径可自动批准（対齐:223-234）──
        {
            auto c = acp::classify_acp_tool_approval(
                json{{"_meta", {{"toolName", "search"}}}}, cwd);
            CHECK(c.approval_class == acp::AcpApprovalClass::ReadonlySearch &&
                      c.auto_approve,
                  "search 无路径 → readonly_search 自动批准（対齐:233）");
        }
        {
            auto c = acp::classify_acp_tool_approval(
                json{{"_meta", {{"toolName", "search"}}},
                     {"rawInput", {{"path", "/etc"}}}}, cwd);
            CHECK(c.approval_class == acp::AcpApprovalClass::Other &&
                      !c.auto_approve,
                  "**关键**：search 路径越界 → other（対齐:230-232）");
        }
        {
            // locations 也要参与判定（対齐:224-229 includeLocations=true）
            auto c = acp::classify_acp_tool_approval(
                json{{"_meta", {{"toolName", "web_search"}}},
                     {"locations", json::array({json{{"path", "/etc/passwd"}}})}}, cwd);
            CHECK(!c.auto_approve,
                  "**关键**：locations 里的越界路径同样拒绝");
        }

        // ── exec_capable（対齐:235-237）──
        for (const char* t : {"exec", "spawn", "shell", "bash", "process",
                              "code_execution", "nodes"}) {
            auto c = acp::classify_acp_tool_approval(
                json{{"_meta", {{"toolName", t}}}}, cwd);
            CHECK(c.approval_class == acp::AcpApprovalClass::ExecCapable &&
                      !c.auto_approve,
                  std::string(t) + " → exec_capable 不自动批准");
        }
        CHECK(acp::classify_acp_tool_approval(
                  json{{"_meta", {{"toolName", "nodes"}}}}, cwd).approval_class ==
                  acp::AcpApprovalClass::ExecCapable,
              "**关键**：nodes 属 exec_capable（此前漏，対齐:23）");

        // ── control_plane：必须 **不** 自动批准（対齐:238-240）──
        for (const char* t : {"cron", "gateway", "sessions_spawn",
                              "sessions_send", "session_status"}) {
            auto c = acp::classify_acp_tool_approval(
                json{{"_meta", {{"toolName", t}}}}, cwd);
            CHECK(c.approval_class == acp::AcpApprovalClass::ControlPlane &&
                      !c.auto_approve,
                  std::string("**关键安全回归**：") + t +
                      " → control_plane 且**不**自动批准");
        }
        CHECK(acp::classify_acp_tool_approval(
                  json{{"_meta", {{"toolName", "cron"}}}}, cwd).approval_class ==
                  acp::AcpApprovalClass::ControlPlane,
              "**关键**：cron 属 control_plane（此前漏，対齐:26）");
        CHECK(acp::classify_acp_tool_approval(
                  json{{"_meta", {{"toolName", "gateway"}}}}, cwd).approval_class ==
                  acp::AcpApprovalClass::ControlPlane,
              "**关键**：gateway 属 control_plane（此前漏，対齐:27）");

        // ── 真源里 "run"/"terminal" **不在** exec 集合内（此前多加）──
        CHECK(acp::classify_acp_tool_approval(
                  json{{"_meta", {{"toolName", "run"}}}}, cwd).approval_class !=
                  acp::AcpApprovalClass::ExecCapable,
              "**关键**：run 不在 EXEC_CAPABLE_TOOL_IDS（此前多加）");
        CHECK(acp::classify_acp_tool_approval(
                  json{{"_meta", {{"toolName", "terminal"}}}}, cwd).approval_class !=
                  acp::AcpApprovalClass::ExecCapable,
              "**关键**：terminal 不在 EXEC_CAPABLE_TOOL_IDS（此前多加）");

        // ── mutating（対齐:241-243）──
        {
            auto c = acp::classify_acp_tool_approval(
                json{{"_meta", {{"toolName", "write"}}}}, cwd);
            CHECK(c.approval_class == acp::AcpApprovalClass::Mutating &&
                      !c.auto_approve,
                  "write → mutating 不自动批准");
        }

        // ── 名字解析失败 → unknown 且不批准（対齐:201-204）──
        {
            auto c = acp::classify_acp_tool_approval(json::object(), cwd);
            CHECK(c.approval_class == acp::AcpApprovalClass::Unknown &&
                      !c.auto_approve,
                  "**关键**：无法解析工具名 → unknown 不自动批准");
        }
        {
            // 伪造场景：title 声称 read，_meta 实为 exec
            auto c = acp::classify_acp_tool_approval(
                json{{"_meta", {{"toolName", "exec"}}},
                     {"title", "read: /tmp/work/a.txt"}}, cwd);
            CHECK(c.approval_class == acp::AcpApprovalClass::Unknown &&
                      !c.auto_approve,
                  "**关键安全回归**：伪造身份 → unknown，绝不自动批准");
        }

        // ── read 的 title 路径提取（対齐:129-136）──
        {
            auto c = acp::classify_acp_tool_approval(
                json{{"_meta", {{"toolName", "read"}}},
                     {"title", "read: /tmp/work/b.txt"}}, cwd);
            CHECK(c.auto_approve,
                  "read 从 title 尾部提取路径（対齐:136）");
        }
        {
            auto c = acp::classify_acp_tool_approval(
                json{{"_meta", {{"toolName", "read"}}},
                     {"title", "read: file_path: /tmp/work/c.txt, mode: r"}}, cwd);
            CHECK(c.auto_approve,
                  "read 支持逗号段中的 file_path:（対齐:131）");
        }
    }

    // =======================================================================
    std::cout << "== resolve_tool_kind_for_permission（対齐 client-helpers.ts:28-42）==\n";
    CHECK(acp::resolve_tool_kind_for_permission("", acp::AcpApprovalClass::Unknown)
              .empty(),
          "名空 + unknown → 空（対齐:32-34）");
    CHECK(acp::resolve_tool_kind_for_permission("read",
              acp::AcpApprovalClass::ReadonlyScoped) == "readonly_scoped",
          "readonly_scoped 字面量");
    CHECK(acp::resolve_tool_kind_for_permission("search",
              acp::AcpApprovalClass::ReadonlySearch) == "readonly_search",
          "readonly_search 字面量");
    CHECK(acp::resolve_tool_kind_for_permission("exec",
              acp::AcpApprovalClass::ExecCapable) == "exec_capable",
          "exec_capable 字面量");
    CHECK(acp::resolve_tool_kind_for_permission("cron",
              acp::AcpApprovalClass::ControlPlane) == "control_plane",
          "control_plane 字面量");
    CHECK(acp::resolve_tool_kind_for_permission("x",
              acp::AcpApprovalClass::Unknown) == "unknown",
          "名非空 + unknown → \"unknown\"（不是空串）");

    // =======================================================================
    std::cout << "== resolve_acp_tool_terminal_outcome（対齐 tool-status.ts:3-20）==\n";
    {
        using acp::AcpToolTerminalOutcome;
        auto r = acp::resolve_acp_tool_terminal_outcome("completed");
        CHECK(r.has_value() && *r == AcpToolTerminalOutcome::Completed,
              "completed → Completed");
        r = acp::resolve_acp_tool_terminal_outcome("done");
        CHECK(r.has_value() && *r == AcpToolTerminalOutcome::Completed,
              "done → Completed（対齐:5 归一）");
        r = acp::resolve_acp_tool_terminal_outcome("failed");
        CHECK(r.has_value() && *r == AcpToolTerminalOutcome::Failed,
              "failed → Failed");
        r = acp::resolve_acp_tool_terminal_outcome("error");
        CHECK(r.has_value() && *r == AcpToolTerminalOutcome::Failed,
              "error → Failed（対齐:7 归一）");
        r = acp::resolve_acp_tool_terminal_outcome("cancelled");
        CHECK(r.has_value() && *r == AcpToolTerminalOutcome::Cancelled,
              "cancelled → Cancelled");
        r = acp::resolve_acp_tool_terminal_outcome("  COMPLETED  ");
        CHECK(r.has_value() && *r == AcpToolTerminalOutcome::Completed,
              "trim + lowercase 后匹配（対齐:15）");

        // 非终态 / 未知
        CHECK(!acp::resolve_acp_tool_terminal_outcome("pending").has_value(),
              "pending 非终态");
        CHECK(!acp::resolve_acp_tool_terminal_outcome("in_progress").has_value(),
              "in_progress 非终态");
        CHECK(!acp::resolve_acp_tool_terminal_outcome("").has_value(),
              "空串非终态");
        CHECK(!acp::resolve_acp_tool_terminal_outcome("canceled").has_value(),
              "**关键**：canceled（单 l）**不在**映射表内（真源只认 cancelled）");
        CHECK(!acp::resolve_acp_tool_terminal_outcome("has error inside").has_value(),
              "**关键回归**：子串含 error 不算终态（须严格相等，"
              "此前的 classify_tool_result 用子串匹配会误判）");

        CHECK(acp::is_acp_tool_terminal_status("done"), "便捷谓词 true");
        CHECK(!acp::is_acp_tool_terminal_status("running"), "便捷谓词 false");
    }

    // =======================================================================
    std::cout << "== normalize_conversation_text（対齐 conversation-id.ts:2-10）==\n";
    CHECK(acp::normalize_conversation_text(json("  abc  ")) == "abc", "string trim");
    CHECK(acp::normalize_conversation_text(json(123)) == "123", "number → 字符串");
    CHECK(acp::normalize_conversation_text(json(true)) == "true",
          "**关键**：bool → \"true\"（JS 模板字符串语义，不是 \"1\"）");
    CHECK(acp::normalize_conversation_text(json(false)) == "false", "bool false");
    CHECK(acp::normalize_conversation_text(json(nullptr)).empty(), "null → 空");
    CHECK(acp::normalize_conversation_text(json::object()).empty(), "对象 → 空");
    CHECK(acp::normalize_conversation_text(json::array()).empty(), "数组 → 空");
    CHECK(acp::normalize_conversation_text(json("ABC")) == "ABC",
          "**关键**：**不**做 lowercase（大小写敏感 ID 不可破坏）");

    // =======================================================================
    std::cout << "== parse_session_meta（対齐 session-mapper.ts:15-27 + meta.ts）==\n";
    {
        auto m = acp::parse_session_meta(json{{"sessionKey", "k1"}});
        CHECK(m.session_key.has_value() && *m.session_key == "k1", "sessionKey");

        m = acp::parse_session_meta(json{{"session", "k2"}});
        CHECK(m.session_key.has_value() && *m.session_key == "k2", "别名 session");

        m = acp::parse_session_meta(json{{"key", "k3"}});
        CHECK(m.session_key.has_value() && *m.session_key == "k3", "别名 key");

        // **关键**：第一个别名值非法时须继续试下一个（対齐 meta.ts:12-17）
        m = acp::parse_session_meta(json{{"sessionKey", "   "}, {"session", "k4"}});
        CHECK(m.session_key.has_value() && *m.session_key == "k4",
              "**关键**：sessionKey 为空白 → 继续用 session（別名链不断）");
        m = acp::parse_session_meta(json{{"sessionKey", 42}, {"key", "k5"}});
        CHECK(m.session_key.has_value() && *m.session_key == "k5",
              "**关键**：sessionKey 类型不符 → 继续用 key");

        // trim
        m = acp::parse_session_meta(json{{"sessionLabel", "  ops  "}});
        CHECK(m.session_label.has_value() && *m.session_label == "ops", "label trim");

        // bool 仅认真 boolean
        m = acp::parse_session_meta(json{{"resetSession", true}});
        CHECK(m.reset_session.has_value() && *m.reset_session, "resetSession true");
        m = acp::parse_session_meta(json{{"reset", false}});
        CHECK(m.reset_session.has_value() && !*m.reset_session,
              "**关键**：显式 false 须保留（不能退化成未提供）");
        m = acp::parse_session_meta(json{{"resetSession", "true"}});
        CHECK(!m.reset_session.has_value(),
              "**关键**：字符串 \"true\" 不算 boolean（対齐 meta.ts:34）");

        // prefixCwd（此前完全缺失）
        m = acp::parse_session_meta(json{{"prefixCwd", true}});
        CHECK(m.prefix_cwd.has_value() && *m.prefix_cwd,
              "**关键**：prefixCwd 须被解析（此前缺失导致开关失效）");

        m = acp::parse_session_meta(json(nullptr));
        CHECK(!m.session_key.has_value(), "null meta → 全空");
    }

    // =======================================================================
    std::cout << "== resolve_session_key 解析顺序（対齐 session-mapper.ts:30-88）==\n";
    {
        // 记录实际发出的 gateway 调用，验证方法名与参数
        std::vector<std::pair<std::string, json>> calls;
        auto fn = [&calls](const std::string& method, const json& params) -> json {
            calls.emplace_back(method, params);
            if (params.contains("label")) {
                return json{{"key", "resolved-from-label"}};
            }
            if (params.contains("key")) {
                return json{{"key", params["key"]}};
            }
            return json::object();
        };

        acp::SessionKeyResolutionOptions opts;

        // ① meta.sessionLabel 优先，且必须查表
        calls.clear();
        {
            acp::AcpSessionMeta_Parsed m;
            m.session_label = "ops";
            const std::string k = acp::resolve_session_key(m, "fallback", opts, fn);
            CHECK(k == "resolved-from-label", "label 经 sessions.resolve 换算");
            CHECK(calls.size() == 1 && calls[0].first == "sessions.resolve" &&
                      calls[0].second.contains("label"),
                  "**关键**：label 路径必须查表（label 不是 key）");
        }

        // ② meta.sessionKey + requireExisting=false → 不查表
        calls.clear();
        {
            acp::AcpSessionMeta_Parsed m;
            m.session_key = "k-direct";
            const std::string k = acp::resolve_session_key(m, "fallback", opts, fn);
            CHECK(k == "k-direct", "key + 非 requireExisting → 直接用");
            CHECK(calls.empty(),
                  "**关键**：不必要的 sessions.resolve 往返应被省掉");
        }

        // ② key + requireExisting=true → 查表校验
        calls.clear();
        {
            acp::AcpSessionMeta_Parsed m;
            m.session_key = "k-check";
            m.require_existing = true;
            const std::string k = acp::resolve_session_key(m, "fallback", opts, fn);
            CHECK(k == "k-check", "key + requireExisting → 查表校验后返回");
            CHECK(calls.size() == 1 && calls[0].second.contains("key"),
                  "requireExisting 时以 key 参数查表");
        }

        // label 优先于 key（顺序不可调换）
        calls.clear();
        {
            acp::AcpSessionMeta_Parsed m;
            m.session_label = "ops";
            m.session_key = "k-ignored";
            const std::string k = acp::resolve_session_key(m, "fallback", opts, fn);
            CHECK(k == "resolved-from-label",
                  "**关键**：label 优先于 key（対齐:41 在 :51 之前）");
        }

        // ③④ opts 默认值
        calls.clear();
        {
            acp::SessionKeyResolutionOptions o2;
            o2.default_session_label = "def-label";
            acp::AcpSessionMeta_Parsed m;
            const std::string k = acp::resolve_session_key(m, "fallback", o2, fn);
            CHECK(k == "resolved-from-label", "opts.defaultSessionLabel 生效");
        }
        {
            acp::SessionKeyResolutionOptions o3;
            o3.default_session_key = "def-key";
            acp::AcpSessionMeta_Parsed m;
            const std::string k = acp::resolve_session_key(m, "fallback", o3, fn);
            CHECK(k == "def-key", "opts.defaultSessionKey 生效");
        }

        // ⑤ 兜底
        {
            acp::AcpSessionMeta_Parsed m;
            const std::string k = acp::resolve_session_key(m, "fallback", opts, fn);
            CHECK(k == "fallback", "全无 → fallback（対齐:87）");
        }

        // 解析失败必须抛错，不可静默回落
        {
            auto fail_fn = [](const std::string&, const json&) -> json {
                return json::object();   // 无 key 字段
            };
            acp::AcpSessionMeta_Parsed m;
            m.session_label = "nope";
            bool threw = false;
            try {
                acp::resolve_session_key(m, "fallback", opts, fail_fn);
            } catch (const std::exception&) {
                threw = true;
            }
            CHECK(threw,
                  "**关键**：label 解析失败必须抛错（対齐:46），"
                  "静默回落会把消息投到错误会话");
        }
    }

    // =======================================================================
    std::cout << "== reset_session_if_needed（対齐 session-mapper.ts:91-102）==\n";
    {
        std::vector<std::string> methods;
        auto fn = [&methods](const std::string& method, const json&) -> json {
            methods.push_back(method);
            return json::object();
        };
        acp::SessionKeyResolutionOptions opts;

        // 未请求 → 不发请求
        methods.clear();
        {
            acp::AcpSessionMeta_Parsed m;
            CHECK(!acp::reset_session_if_needed(m, "k", opts, fn), "未请求 → false");
            CHECK(methods.empty(), "未请求 → 不发任何请求");
        }
        // 请求 → 发 sessions.reset
        methods.clear();
        {
            acp::AcpSessionMeta_Parsed m;
            m.reset_session = true;
            CHECK(acp::reset_session_if_needed(m, "k", opts, fn), "请求 → true");
            CHECK(methods.size() == 1 && methods[0] == "sessions.reset",
                  "**关键回归**：必须是 sessions.reset（此前误用 sessions.init，"
                  "init 不清历史 → resetSession 静默失效）");
        }
        // opts 兜底
        methods.clear();
        {
            acp::SessionKeyResolutionOptions o2;
            o2.reset_session = true;
            acp::AcpSessionMeta_Parsed m;
            CHECK(acp::reset_session_if_needed(m, "k", o2, fn),
                  "opts.resetSession 兜底生效（対齐:97）");
        }
        // meta 显式 false 覆盖 opts true
        methods.clear();
        {
            acp::SessionKeyResolutionOptions o3;
            o3.reset_session = true;
            acp::AcpSessionMeta_Parsed m;
            m.reset_session = false;
            CHECK(!acp::reset_session_if_needed(m, "k", o3, fn),
                  "**关键**：meta 显式 false 覆盖 opts true（取值链 meta→opts）");
        }
        // 空 session_key 不发请求
        methods.clear();
        {
            acp::AcpSessionMeta_Parsed m;
            m.reset_session = true;
            CHECK(!acp::reset_session_if_needed(m, "", opts, fn),
                  "空 sessionKey → 不发请求");
        }
    }

    // =======================================================================
    std::cout << "== resolve_list_sessions_page_size（対齐 translator.session-list.ts:73-83）==\n";
    // **关键回归**：此前从顶层 params 读 limit，默认 50 且**无上限** ——
    // 客户端传 limit:999999 会被原样接受，一次拉全库。
    {
        using acp::resolve_list_sessions_page_size;
        CHECK(resolve_list_sessions_page_size(json::object()) == 100,
              "未提供 → 默认 100（**不是 50**）");
        CHECK(resolve_list_sessions_page_size(json(nullptr)) == 100,
              "null meta → 默认 100");
        CHECK(resolve_list_sessions_page_size(json{{"limit", 20}}) == 20,
              "限内取值原样");
        CHECK(resolve_list_sessions_page_size(json{{"pageSize", 30}}) == 30,
              "别名 pageSize 生效");
        CHECK(resolve_list_sessions_page_size(json{{"limit", 999999}}) == 100,
              "**关键**：超过上限 clamp 到 100（防一次拉全库）");
        CHECK(resolve_list_sessions_page_size(json{{"limit", 0}}) == 1,
              "**关键**：0 clamp 到 1（対齐 Math.max(1, ...)）");
        CHECK(resolve_list_sessions_page_size(json{{"limit", -5}}) == 1,
              "负数 clamp 到 1");
        CHECK(resolve_list_sessions_page_size(json{{"limit", 20.9}}) == 20,
              "小数向下取整（対齐 Math.floor）");
        CHECK(resolve_list_sessions_page_size(json{{"limit", "20"}}) == 100,
              "**关键**：字符串不算数值 → 用默认值（対齐 readNumber）");
        // 别名优先级：limit 先于 pageSize
        CHECK(resolve_list_sessions_page_size(json{{"limit", 10}, {"pageSize", 50}}) == 10,
              "limit 优先于 pageSize（別名顺序）");
        // limit 非法时继续试 pageSize（対齐 readMetaValue 的别名链）
        CHECK(resolve_list_sessions_page_size(json{{"limit", "x"}, {"pageSize", 40}}) == 40,
              "**关键**：limit 类型不符 → 继续试 pageSize");
    }

    // =======================================================================
    std::cout << "== map_stop_reason_for_state（対齐 translator.ts:1157-1171）==\n";
    // **关键回归**：此前实现把三个 state 的判定压成一张扁平字符串表，
    // 导致 final 状态下的 "refusal"/"content_filter" 被误映射为 refusal，
    // 且 aborted 需要 rawStopReason 也像 abort 才返回 cancelled。
    {
        using acp::map_acp_stop_reason_for_state;

        // final：只认 max_tokens（対齐:1158-1159）
        CHECK(map_acp_stop_reason_for_state("final", "max_tokens", "") == "max_tokens",
              "final + max_tokens → max_tokens");
        CHECK(map_acp_stop_reason_for_state("final", "", "") == "end_turn",
              "final + 空 → end_turn");
        CHECK(map_acp_stop_reason_for_state("final", "stop", "") == "end_turn",
              "final + stop → end_turn");
        CHECK(map_acp_stop_reason_for_state("final", "refusal", "") == "end_turn",
              "**关键回归**：final + refusal → end_turn（真源在 final 只认 "
              "max_tokens；refusal 只能来自 error 状态）");
        CHECK(map_acp_stop_reason_for_state("final", "content_filter", "") == "end_turn",
              "**关键回归**：final + content_filter → end_turn");
        CHECK(map_acp_stop_reason_for_state("final", "length", "") == "end_turn",
              "**关键**：length **不是** max_tokens（真源只认字面 max_tokens）");

        // aborted：无条件 cancelled（対齐:1163-1164）
        CHECK(map_acp_stop_reason_for_state("aborted", "", "") == "cancelled",
              "aborted + 空 → cancelled（无条件）");
        CHECK(map_acp_stop_reason_for_state("aborted", "max_tokens", "") == "cancelled",
              "**关键**：aborted 优先于 max_tokens");
        CHECK(map_acp_stop_reason_for_state("aborted", "whatever", "refusal") ==
                  "cancelled",
              "**关键**：aborted 无条件 cancelled，不看 rawStopReason/errorKind");

        // error：仅 errorKind=="refusal" 才 refusal（対齐:1168-1169）
        CHECK(map_acp_stop_reason_for_state("error", "", "refusal") == "refusal",
              "error + errorKind=refusal → refusal");
        CHECK(map_acp_stop_reason_for_state("error", "", "boom") == "end_turn",
              "error + 其它 errorKind → end_turn");
        CHECK(map_acp_stop_reason_for_state("error", "refusal", "") == "end_turn",
              "**关键**：error 状态看的是 errorKind，**不是** rawStopReason");
        CHECK(map_acp_stop_reason_for_state("error", "max_tokens", "") == "end_turn",
              "error 状态下 max_tokens 也归 end_turn");

        // 未知 state 走 final 分支（真源的 if/else 结构隐含此兜底）
        CHECK(map_acp_stop_reason_for_state("delta", "max_tokens", "") == "max_tokens",
              "未知 state 按 final 语义处理");
    }

    // =======================================================================
    std::cout << "\n========================================\n";
    std::cout << "  通过: " << g_pass << "   失败: " << g_fail << "\n";
    std::cout << "========================================\n";
    return g_fail == 0 ? 0 : 1;
}
