// ============================================================================
// test_remote_a2a_tools.cc
// M10 agent_tool + M11 agent_registry + M12 config 测试
//
// 构建：
//   /usr/local/gcc-12.2.0/bin/g++ -std=c++17 -Iinclude \
//     -Iexternal/json/single_include -Ithird_party \
//     tests/test_remote_a2a_tools.cc \
//     src/remote_a2a/*.cc src/infra/http_client.cc \
//     src/tools/tool_manager.cc ... -lpthread
//   （实际见脚本；tool_manager 依赖较多，测试用轻量 stub）
//
// ★ 全部离线：卡走本地文件，不发一个网络包。
// ============================================================================

#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>

#include "hunterclaw/remote_a2a/agent_tool.h"
#include "hunterclaw/remote_a2a/config.h"

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

static std::string write_temp(const std::string& path, const std::string& body) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f << body;
    f.close();
    return path;
}

// ============================================================================
// 工具名生成
// ============================================================================
static void test_tool_name() {
    SECTION("工具名生成");

    {
        std::vector<std::string> r;
        CHECK(safe_remote_a2a_tool_name("research", r) == "remote_a2a__research",
              "常规名");
    }

    // 非法字符替换
    {
        std::vector<std::string> r;
        const std::string n = safe_remote_a2a_tool_name("my agent!@#", r);
        CHECK(n == "remote_a2a__my-agent", "非法字符替换并折叠");
    }
    // 中文名 → 全部替换后折叠
    {
        std::vector<std::string> r;
        const std::string n = safe_remote_a2a_tool_name("研究助手", r);
        CHECK(n.rfind("remote_a2a__", 0) == 0, "中文名有合法前缀");
        // 折叠+去首尾 '-' 后为空 → 回落 "agent"
        CHECK(n == "remote_a2a__agent", "全非法字符回落 agent");
    }
    // 空名回落
    {
        std::vector<std::string> r;
        CHECK(safe_remote_a2a_tool_name("", r) == "remote_a2a__agent", "空名回落");
        CHECK(safe_remote_a2a_tool_name("   ", r) == "remote_a2a__agent-2",
              "空白名回落并去重");
    }

    // ★ [不可回退] 长度上限 64 —— 部分 LLM provider 对函数名长度有硬限制
    {
        std::vector<std::string> r;
        const std::string n = safe_remote_a2a_tool_name(std::string(200, 'a'), r);
        CHECK(n.size() <= TOOL_NAME_MAX_TOTAL, "总长不超 64 [不可回退]");
    }

    // 去重
    {
        std::vector<std::string> r;
        const std::string a = safe_remote_a2a_tool_name("dup", r);
        const std::string b = safe_remote_a2a_tool_name("dup", r);
        const std::string c = safe_remote_a2a_tool_name("dup", r);
        CHECK(a == "remote_a2a__dup", "首个无后缀");
        CHECK(b == "remote_a2a__dup-2", "第二个加 -2");
        CHECK(c == "remote_a2a__dup-3", "第三个加 -3");
    }
    // 大小写不敏感去重（避免 provider 侧大小写归一后撞名）
    {
        std::vector<std::string> r;
        safe_remote_a2a_tool_name("Dup", r);
        const std::string b = safe_remote_a2a_tool_name("dup", r);
        CHECK(b == "remote_a2a__dup-2", "大小写不敏感去重 [不可回退]");
    }

    // ★ [不可回退] 名字必须稳定：同样输入序列必须得到同样输出，
    //   否则 transcript 里历史 tool_call 的名字会对不上现有工具
    {
        std::vector<std::string> r1, r2;
        std::vector<std::string> a, b;
        for (int i = 0; i < 3; ++i) a.push_back(safe_remote_a2a_tool_name("x", r1));
        for (int i = 0; i < 3; ++i) b.push_back(safe_remote_a2a_tool_name("x", r2));
        CHECK(a == b, "同输入序列得同输出 [不可回退]");
    }

    // 与保留名冲突
    {
        std::vector<std::string> r{TOOL_STATUS, TOOL_CANCEL};
        const std::string n = safe_remote_a2a_tool_name("status", r);
        CHECK(n != TOOL_STATUS, "不与 status 工具撞名");
        CHECK(n == "remote_a2a__status", "前缀本身已避开撞名");
    }
}

// ============================================================================
// Message 构造
// ============================================================================
static void test_build_message() {
    SECTION("Message 构造");

    auto m = build_user_message("hello", "", "");
    CHECK(m.role == Role::User, "role = user");
    CHECK(m.parts.size() == 1 && m.parts[0].text == "hello", "文本 part");
    CHECK(!m.message_id.empty(), "messageId 非空");

    // ★ [不可回退] messageId 必须唯一 —— 远端用它去重，
    //   撞 id 会让第二条消息被当重复投递直接丢弃
    auto m2 = build_user_message("hello", "", "");
    CHECK(m.message_id != m2.message_id, "messageId 唯一 [不可回退]");
    // 同一文本重复构造也必须不同 id
    auto m3 = build_user_message("hello", "", "");
    CHECK(m2.message_id != m3.message_id, "同文本 id 仍不同 [不可回退]");

    // 续接任务
    auto mc = build_user_message("more", "task-9", "ctx-9");
    CHECK(mc.task_id == "task-9" && mc.context_id == "ctx-9", "续接字段透传");

    // 序列化含 is_user_input
    auto j = to_json(m);
    CHECK(j["parts"][0]["metadata"]["is_user_input"] == true, "标记 is_user_input");
}

// ============================================================================
// 归属校验
// ============================================================================
static void test_ownership() {
    SECTION("handle 归属校验");

    auto s = PendingTaskStore::create();
    auto h1 = s->register_task("a", "t1", "", "sess-A").get();
    auto h2 = s->register_task("a", "t2", "", "sess-B").get();

    CHECK(check_handle_ownership(*s, h1, "sess-A").empty(), "本会话 handle 通过");

    // ★ [不可回退] 跨会话必须拒绝 —— handle 出现在工具返回文本里，
    //   模型可能把它带到别的会话；不校验则 A 能读走 B 的结果
    CHECK(!check_handle_ownership(*s, h2, "sess-A").empty(),
          "跨会话 handle 被拒 [不可回退]");
    CHECK(!check_handle_ownership(*s, "bogus", "sess-A").empty(), "不存在的 handle 被拒");
    CHECK(!check_handle_ownership(*s, "", "sess-A").empty(), "空 handle 被拒");

    // ★ [不可回退] 「不存在」与「属于别人」的错误文案必须一致 ——
    //   区分开会变成探测其他会话是否有某 handle 的探针
    const std::string e_other = check_handle_ownership(*s, h2, "sess-A");
    const std::string e_none = check_handle_ownership(*s, "bogus", "sess-A");
    CHECK(e_other == e_none, "不区分不存在与无权限 [不可回退]");
}

// ============================================================================
// 配置解析 —— 基本
// ============================================================================
static void test_config_basic() {
    SECTION("配置解析 —— 基本");

    // 缺失 / 非 object → 未启用（不是错误）
    {
        auto r = parse_remote_a2a_config(json());
        CHECK(r.is_ok() && !r.get().enabled, "null → 未启用 [不可回退]");
    }
    {
        auto r = parse_remote_a2a_config(json::array());
        CHECK(r.is_ok() && !r.get().enabled, "数组 → 未启用");
    }
    {
        auto r = parse_remote_a2a_config(json{{"enabled", false}});
        CHECK(r.is_ok() && !r.get().enabled, "显式 false → 未启用");
    }

    // enabled 但无 agents → Err
    {
        auto r = parse_remote_a2a_config(json{{"enabled", true}});
        CHECK(r.is_err(), "enabled 但无 agents → Err");
    }
    {
        auto r = parse_remote_a2a_config(
            json{{"enabled", true}, {"agents", json::array()}});
        CHECK(r.is_err(), "空 agents 数组 → Err");
    }

    // 正常单 agent
    {
        json n;
        n["enabled"] = true;
        n["agents"] = json::array({json{{"name", "research"},
                                        {"card", "https://r.example.com"}}});
        auto r = parse_remote_a2a_config(n);
        CHECK(r.is_ok(), "单 agent 解析成功");
        CHECK(r.get().agents.size() == 1, "1 个 agent");
        const auto& a = r.get().agents[0];
        CHECK(a.name == "research", "name");
        CHECK(a.card.source == "https://r.example.com", "card source");
        CHECK(a.client.name == "research", "client name 同步");
        // ★ 策略默认最小权限
        CHECK(!a.policy.requires_approval, "默认不需审批");
        CHECK(!a.policy.owner_only, "默认非 owner_only");
    }
}

// ============================================================================
// 配置解析 —— 单项错误不拖垮整体
// ============================================================================
static void test_config_partial_failure() {
    SECTION("配置解析 —— 单项失败隔离");

    json n;
    n["enabled"] = true;
    n["agents"] = json::array({
        json{{"name", "good"}, {"card", "https://g.example.com"}},
        json{{"card", "https://noname.example.com"}},            // 缺 name
        json{{"name", "nocard"}},                                 // 缺 card
        json{{"name", "good"}, {"card", "https://dup.example.com"}},  // 重名
        json{{"name", "badauth"},
             {"card", "https://b.example.com"},
             {"auth", {{"kind", "bearer"}, {"token", "PLAINTEXT"}}}},  // 明文凭据
        json{{"name", "disabled"}, {"card", "https://d.example.com"},
             {"enabled", false}},
        json::array(),                                            // 非对象
        json{{"name", "good2"}, {"card", "https://g2.example.com"}},
    });

    auto r = parse_remote_a2a_config(n);
    CHECK(r.is_ok(), "整体解析成功");
    // ★ [不可回退] 单项错误只跳过该项（対标 MCP：一个 server 离线
    //   不该让所有工具消失）
    CHECK(r.get().agents.size() == 2, "只保留 2 个有效 agent [不可回退]");
    CHECK(r.get().agents[0].name == "good", "good 保留");
    CHECK(r.get().agents[1].name == "good2", "good2 保留");
    CHECK(r.get().warnings.size() >= 5, "每个被跳过项都有 warning");

    // ★ [不可回退] 明文凭据必须被拒（auth.cc:83-94）
    bool warned_plaintext = false;
    for (const auto& w : r.get().warnings) {
        if (w.find("badauth") != std::string::npos) warned_plaintext = true;
    }
    CHECK(warned_plaintext, "明文凭据的 agent 被跳过并告警 [不可回退]");

    // 全部无效 → Err
    {
        json bad;
        bad["enabled"] = true;
        bad["agents"] = json::array({json{{"name", "x"}}, json{{"card", "y"}}});
        CHECK(parse_remote_a2a_config(bad).is_err(), "全部无效 → Err");
    }
}

// ============================================================================
// 配置解析 —— camelCase 键名归一
// ============================================================================
static void test_config_camel_keys() {
    SECTION("配置解析 —— camelCase 归一");

    json n;
    n["enabled"] = true;
    n["agents"] = json::array({json{
        {"name", "a"},
        {"card", "https://a.example.com"},
        // ★ [不可回退] 配置文件用 camelCase，而 parse_auth_spec 只认 snake_case。
        //   不做归一的话用户写 tokenEnv 会得到「必须指定 token_env」的自相矛盾报错。
        {"auth", {{"kind", "bearer"}, {"tokenEnv", "MY_TOKEN"}}},
        {"requiresApproval", true},
        {"ownerOnly", true},
        {"yieldMs", 45000},
        {"maxTaskMs", 600000},
        {"forceNoStreaming", true},
    }});

    auto r = parse_remote_a2a_config(n);
    CHECK(r.is_ok(), "camelCase 配置解析成功");
    if (r.is_ok()) {
        const auto& a = r.get().agents[0];
        CHECK(a.card.auth.kind == AuthKind::Bearer, "tokenEnv 被识别 [不可回退]");
        CHECK(a.card.auth.env_name == "MY_TOKEN", "env 名透传");
        CHECK(a.policy.requires_approval, "requiresApproval");
        CHECK(a.policy.owner_only, "ownerOnly");
        CHECK(a.exec.yield_ms == 45000, "yieldMs");
        CHECK(a.exec.max_task_ms == 600000, "maxTaskMs");
        CHECK(a.client.force_no_streaming, "forceNoStreaming");
    }

    // snake_case 也要能用（向后兼容）
    {
        json s;
        s["enabled"] = true;
        s["agents"] = json::array({json{{"name", "a"},
                                        {"card", "https://a.example.com"},
                                        {"auth", {{"type", "bearer"},
                                                  {"token_env", "T"}}},
                                        {"requires_approval", true}}});
        auto rr = parse_remote_a2a_config(s);
        CHECK(rr.is_ok() && rr.get().agents[0].policy.requires_approval,
              "snake_case 兼容");
    }

    // ★ [不可回退] camel 归一不能绕过明文凭据检测：apiKey 也要被拒
    {
        json s;
        s["enabled"] = true;
        s["agents"] = json::array({json{{"name", "a"},
                                        {"card", "https://a.example.com"},
                                        {"auth", {{"kind", "apikey"},
                                                  {"headerName", "X-K"},
                                                  {"apiKey", "PLAIN"}}}}});
        auto rr = parse_remote_a2a_config(s);
        CHECK(rr.is_err(), "全部 agent 因明文凭据被跳过 → Err [不可回退]");
    }
}

// ============================================================================
// 配置解析 —— 数值钳制
// ============================================================================
static void test_config_clamp() {
    SECTION("配置解析 —— 数值钳制");

    json n;
    n["enabled"] = true;
    n["defaults"] = json{{"probe", json{{"baseIntervalMs", 40000},
                                        {"maxIntervalMs", 5000},   // 反了
                                        {"maxNoProgress", 1}}}};   // 低于下限
    n["agents"] = json::array({json{{"name", "a"}, {"card", "https://a.example.com"}}});

    auto r = parse_remote_a2a_config(n);
    CHECK(r.is_ok(), "解析成功");
    if (r.is_ok()) {
        const auto& p = r.get().agents[0].exec.probe;
        // ★ [不可回退] max_no_progress 下限 2（probe_policy.h:26）——
        //   设 1 会因单次抖动误判 Zombie
        CHECK(p.max_no_progress >= 2, "maxNoProgress 下限 2 [不可回退]");
        // ★ [不可回退] base > max 时退避会失效（interval_for 恒返回 max）
        CHECK(p.base_interval_ms <= p.max_interval_ms,
              "base 不大于 max [不可回退]");
        bool warned = false;
        for (const auto& w : r.get().warnings) {
            if (w.find("baseIntervalMs") != std::string::npos) warned = true;
        }
        CHECK(warned, "base/max 颠倒有告警");
    }

    // store 钳制
    {
        json s;
        s["enabled"] = true;
        s["store"] = json{{"maxEntries", 999999}, {"terminalTtlMs", 1}};
        s["agents"] = json::array({json{{"name", "a"}, {"card", "https://a.x.com"}}});
        auto rr = parse_remote_a2a_config(s);
        CHECK(rr.is_ok(), "store 配置解析成功");
        CHECK(rr.get().store.max_entries <= 4096, "maxEntries 上限");
        CHECK(rr.get().store.terminal_ttl_ms >= 60000, "terminalTtlMs 下限");
    }

    // 负值/零值回落默认
    {
        json s;
        s["enabled"] = true;
        s["agents"] = json::array({json{{"name", "a"},
                                        {"card", "https://a.x.com"},
                                        {"maxTaskMs", -1},
                                        {"timeoutMs", 0}}});
        auto rr = parse_remote_a2a_config(s);
        CHECK(rr.is_ok(), "非法数值解析成功");
        CHECK(rr.get().agents[0].exec.max_task_ms > 0, "负 maxTaskMs 回落默认");
        CHECK(rr.get().agents[0].card.timeout_ms > 0, "零 timeoutMs 回落默认");
    }

    // force 双开 → 以禁用为准
    {
        json s;
        s["enabled"] = true;
        s["agents"] = json::array({json{{"name", "a"},
                                        {"card", "https://a.x.com"},
                                        {"forceStreaming", true},
                                        {"forceNoStreaming", true}}});
        auto rr = parse_remote_a2a_config(s);
        CHECK(rr.is_ok(), "双开解析成功");
        CHECK(!rr.get().agents[0].client.force_streaming, "force 双开以禁用为准");
        CHECK(rr.get().agents[0].client.force_no_streaming, "禁用生效");
        CHECK(!rr.get().warnings.empty(), "双开有告警");
    }
}

// ============================================================================
// 配置解析 —— 回调基址校验
// ============================================================================
static void test_config_callback() {
    SECTION("配置解析 —— 回调基址");

    auto make = [](const std::string& base) {
        json n;
        n["enabled"] = true;
        if (!base.empty()) n["callbackBaseUrl"] = base;
        n["agents"] = json::array({json{{"name", "a"},
                                        {"card", "https://a.example.com"},
                                        {"enableCallback", true}}});
        return n;
    };

    // 启用回调但无基址 → Err
    CHECK(parse_remote_a2a_config(make("")).is_err(), "无 callbackBaseUrl → Err");

    // ★ [不可回退] 回调必须 https —— 回调 body 带任务结果，
    //   且 Bearer token 随请求头明文发出
    CHECK(parse_remote_a2a_config(make("http://pub.example.com")).is_err(),
          "公网 http 回调被拒 [不可回退]");

    // loopback 允许 http（本地开发）
    CHECK(parse_remote_a2a_config(make("http://127.0.0.1:8080")).is_ok(),
          "loopback http 允许");
    CHECK(parse_remote_a2a_config(make("http://localhost:8080")).is_ok(),
          "localhost http 允许");

    // https 正常
    {
        auto r = parse_remote_a2a_config(make("https://hc.example.com"));
        CHECK(r.is_ok(), "https 回调通过");
        CHECK(r.get().agents[0].exec.callback_base_url == "https://hc.example.com",
              "基址下发到 agent");
        CHECK(r.get().agents[0].exec.enable_callback, "回调已启用");
    }

    // 非法 URL
    CHECK(parse_remote_a2a_config(make("ftp://x.com")).is_err(), "非 http/https 被拒");
    CHECK(parse_remote_a2a_config(make("not-a-url")).is_err(), "非法 URL 被拒");

    // ★ 未启用回调时不校验基址（不该因为一个用不到的字段拒绝启动）
    {
        json n;
        n["enabled"] = true;
        n["callbackBaseUrl"] = "http://insecure.example.com";
        n["agents"] = json::array({json{{"name", "a"}, {"card", "https://a.x.com"}}});
        CHECK(parse_remote_a2a_config(n).is_ok(),
              "未启用回调时不校验基址 [不可回退]");
    }
}

// ============================================================================
// 工具的 Schema 与安全边界
// ============================================================================
static void test_tool_schema() {
    SECTION("工具 Schema 与安全边界");

    const std::string p = write_temp(
        "/tmp/hc_tool_card.json",
        R"({"name":"n","description":"desc","url":"https://h.example.com/a2a",
            "skills":[{"id":"s1","name":"搜索","description":"网络搜索"}]})");

    CardResolverConfig ccfg;
    ccfg.source = p;
    ccfg.resolve_dns = false;
    auto cards = CardResolver::create(ccfg);

    RemoteAgentClientConfig rcfg;
    rcfg.name = "helper";
    auto client = RemoteAgentClient::create(rcfg, cards);
    auto store = PendingTaskStore::create();
    auto engine = ExecutionEngine::create(client, store, ExecutionConfig{});

    hunterclaw::tools::ToolContext ctx;
    ctx.session_key = "sess-X";

    AgentToolPolicy pol;
    pol.requires_approval = true;
    pol.owner_only = true;

    RemoteA2aAgentTool tool("remote_a2a__helper", "helper", client, engine, pol, ctx);

    CHECK(tool.name() == "remote_a2a__helper", "工具名");
    // ★ [不可回退] 策略只从本地配置读，绝不从卡读（卡是对端可控的）
    CHECK(tool.requires_approval(), "requires_approval 来自本地配置 [不可回退]");
    CHECK(tool.owner_only(), "owner_only 来自本地配置 [不可回退]");

    auto s = tool.parameters();
    CHECK(s.type == "object", "schema type");
    CHECK(s.properties.count("message") == 1, "有 message 参数");
    CHECK(s.required.size() == 1 && s.required[0] == "message", "message 必填");
    CHECK(s.properties.count("task_id") == 1, "有 task_id（续接用）");
    CHECK(s.properties.count("yield_ms") == 1, "有 yield_ms");
    CHECK(s.properties.count("background") == 1, "有 background");

    // ★★ [不可回退] 绝不暴露 session_key 参数 ——
    //   params 由模型构造，允许指定 session_key 等于让模型把任务
    //   挂到别人的会话上，进度与结果会投递给其他用户
    CHECK(s.properties.count("session_key") == 0,
          "不暴露 session_key 参数 [不可回退]");
    CHECK(s.properties.count("sessionKey") == 0, "不暴露 sessionKey 参数 [不可回退]");

    // description() 是热路径（每次组装 prompt 都调）→ 不做网络 IO
    const std::string d0 = tool.description();
    CHECK(!d0.empty(), "未预热时有兜底描述");

    tool.warm_up();
    const std::string d1 = tool.description();
    CHECK(d1.find("helper") != std::string::npos, "预热后描述含 agent 名");
    CHECK(d1.find("搜索") != std::string::npos, "预热后描述含 skill");
    CHECK(d1.find(TOOL_STATUS) != std::string::npos, "描述提示跟进方式");

    // 描述长度受控（防远端撑爆 context）
    CHECK(d1.size() <= pol.max_description_chars + 8, "描述长度受控");

    // 空 message 被拒
    {
        auto r = tool.execute(hunterclaw::core::JsonValue(json{{"message", ""}}));
        CHECK(!r.success, "空 message 被拒");
    }
    {
        auto r = tool.execute(hunterclaw::core::JsonValue(json::object()));
        CHECK(!r.success, "缺 message 被拒");
    }

    std::remove(p.c_str());
}

// ============================================================================
// status / cancel 工具
// ============================================================================
static void test_status_cancel_tools() {
    SECTION("status / cancel 工具");

    auto store = PendingTaskStore::create();
    hunterclaw::tools::ToolContext ctx;
    ctx.session_key = "sess-A";

    // lookup 返回 nullptr（模拟远端已移出配置）
    EngineLookupFn null_lookup = [](const std::string&) { return nullptr; };

    RemoteA2aStatusTool st(store, null_lookup, ctx);
    CHECK(st.name() == TOOL_STATUS, "status 工具名");
    CHECK(!st.requires_approval(), "status 不需审批（只读）");

    auto ss = st.parameters();
    CHECK(ss.properties.count("handle") == 1, "有 handle 参数");
    CHECK(ss.properties.count("wait_ms") == 1, "有 wait_ms 参数");
    CHECK(ss.required.empty(), "handle 可省（用于列出全部）");
    CHECK(ss.properties.count("session_key") == 0,
          "status 不暴露 session_key [不可回退]");

    // 空会话 → 提示无任务
    {
        auto r = st.execute(hunterclaw::core::JsonValue(json::object()));
        CHECK(r.success, "空列表也是成功");
        CHECK(r.content[0].text.find("没有在途") != std::string::npos, "提示无任务");
    }

    // 列出本会话任务
    {
        auto h1 = store->register_task("a", "t1", "", "sess-A").get();
        store->register_task("b", "t2", "", "sess-B").get();
        auto r = st.execute(hunterclaw::core::JsonValue(json::object()));
        CHECK(r.success, "列出成功");
        CHECK(r.content[0].text.find(h1) != std::string::npos, "含本会话 handle");
        // ★ [不可回退] 绝不列出其他会话的任务
        CHECK(r.content[0].text.find("t2") == std::string::npos,
              "不泄漏其他会话任务 [不可回退]");
    }

    // 跨会话 handle 被拒
    {
        auto hb = store->register_task("b", "t3", "", "sess-B").get();
        auto r = st.execute(hunterclaw::core::JsonValue(json{{"handle", hb}}));
        CHECK(!r.success, "跨会话 handle 被拒 [不可回退]");
    }

    // 引擎不可用时退化为只读（不阻塞）
    {
        auto h = store->register_task("a", "t9", "", "sess-A").get();
        store->update_terminal(h, TerminalReason::Completed, "done-9", "");
        auto r = st.execute(hunterclaw::core::JsonValue(json{{"handle", h}}));
        CHECK(r.success, "引擎不可用仍返回状态");
        CHECK(r.content[0].text.find("done-9") != std::string::npos, "含已有结果");
    }

    // cancel 工具
    RemoteA2aCancelTool ct(store, null_lookup, ctx, true);
    CHECK(ct.name() == TOOL_CANCEL, "cancel 工具名");
    CHECK(ct.requires_approval(), "cancel 审批策略生效");
    auto cs = ct.parameters();
    CHECK(cs.required.size() == 1 && cs.required[0] == "handle", "handle 必填");

    // 空 handle
    CHECK(!ct.execute(hunterclaw::core::JsonValue(json::object())).success,
          "cancel 缺 handle 被拒");

    // ★ [不可回退] 引擎不可用时仍要本地标记取消 ——
    //   否则任务永久挂在在途列表，且占着容量让 register_task 拒绝新任务
    {
        auto h = store->register_task("a", "t10", "", "sess-A").get();
        auto r = ct.execute(hunterclaw::core::JsonValue(json{{"handle", h}}));
        CHECK(r.success, "引擎不可用 cancel 仍成功");
        auto t = store->get(h);
        CHECK(t->reason == TerminalReason::Canceled,
              "本地已标记取消 [不可回退]");
        CHECK(t->phase == PendingPhase::Terminal, "phase 转 Terminal");
    }

    // 跨会话 cancel 被拒
    {
        auto hb = store->register_task("b", "t11", "", "sess-B").get();
        CHECK(!ct.execute(hunterclaw::core::JsonValue(json{{"handle", hb}})).success,
              "跨会话 cancel 被拒 [不可回退]");
        CHECK(store->get(hb)->phase == PendingPhase::Running,
              "跨会话任务未被误取消 [不可回退]");
    }
}

// ============================================================================
int main() {
    std::cout << "===== remote_a2a M10/M11/M12 测试 =====" << std::endl;

    test_tool_name();
    test_build_message();
    test_ownership();

    test_config_basic();
    test_config_partial_failure();
    test_config_camel_keys();
    test_config_clamp();
    test_config_callback();

    test_tool_schema();
    test_status_cancel_tools();

    std::cout << "\n===== 结果: " << g_pass << " 通过, " << g_fail << " 失败 ====="
              << std::endl;
    return g_fail == 0 ? 0 : 1;
}
