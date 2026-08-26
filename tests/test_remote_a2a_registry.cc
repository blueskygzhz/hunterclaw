// ============================================================================
// test_remote_a2a_registry.cc
// M11 agent_registry 端到端测试（离线：卡走本地文件）
//
// ⚠ 本测试链入真实 ToolManager 的替代实现（test_stub_tool_manager.cc）——
//   真实 tool_manager.cc 会拖入 MCP/hook/policy 整条依赖链。
//   stub 只实现 register_tool / get_tool / list_tools / context，
//   这正是 register_tools() 用到的全部接口。
// ============================================================================

#include <cstdio>
#include <fstream>
#include <iostream>
#include <set>
#include <string>
#include <thread>

#include "hunterclaw/remote_a2a/agent_registry.h"

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

static std::string write_card(const std::string& path, const std::string& name,
                             const std::string& url) {
    json c;
    c["name"] = name;
    c["description"] = "card for " + name;
    c["url"] = url;
    c["capabilities"] = json::object();
    c["skills"] = json::array({json{{"id", "s1"}, {"name", "skill-1"}}});
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f << c.dump();
    f.close();
    return path;
}

// ============================================================================
static void test_build_and_register() {
    SECTION("build + register_tools");

    const std::string p1 = write_card("/tmp/hc_reg_a.json", "alpha",
                                     "https://alpha.example.com/a2a");
    const std::string p2 = write_card("/tmp/hc_reg_b.json", "beta",
                                     "https://beta.example.com/a2a");

    json node;
    node["enabled"] = true;
    node["agents"] = json::array({
        json{{"name", "alpha"}, {"card", p1}, {"resolveDns", false}},
        json{{"name", "beta"}, {"card", p2}, {"resolveDns", false},
             {"requiresApproval", true}},
    });

    auto parsed = parse_remote_a2a_config(node);
    CHECK(parsed.is_ok(), "配置解析成功");
    if (!parsed.is_ok()) return;

    auto reg = RemoteA2aRegistry::create(parsed.get());
    auto built = reg->build();
    CHECK(built.is_ok() && built.get() == 2, "构造 2 个远端");
    CHECK(reg->agent_count() == 2, "agent_count");

    // 卡已就绪
    for (const auto& rt : reg->runtimes()) {
        CHECK(rt.card_ready, "卡已就绪: " + rt.name);
        CHECK(rt.cards && rt.client && rt.engine, "三件套齐备: " + rt.name);
        CHECK(rt.tool_name.rfind("remote_a2a__", 0) == 0, "工具名前缀: " + rt.name);
    }

    // ★ store 是全局单例（见 agent_registry.h 文件头）
    auto store = reg->store();
    CHECK(store != nullptr, "store 存在");

    hunterclaw::tools::ToolContext ctx;
    ctx.session_key = "sess-1";
    auto mgr = hunterclaw::tools::ToolManager::create(ctx);

    auto n = reg->register_tools(*mgr);
    // ★ [不可回退] N 个发送工具 + 2 个全局跟进工具（不是 3N）——
    //   跟进只需 handle，按 agent 拆会让工具清单膨胀 3 倍，直接吃 context
    CHECK(n.is_ok() && n.get() == 4, "注册 2+2 = 4 个工具 [不可回退]");

    CHECK(mgr->get_tool("remote_a2a__alpha") != nullptr, "alpha 工具已注册");
    CHECK(mgr->get_tool("remote_a2a__beta") != nullptr, "beta 工具已注册");
    CHECK(mgr->get_tool(TOOL_STATUS) != nullptr, "status 工具已注册");
    CHECK(mgr->get_tool(TOOL_CANCEL) != nullptr, "cancel 工具已注册");

    // 策略透传
    CHECK(!mgr->get_tool("remote_a2a__alpha")->requires_approval(),
          "alpha 不需审批");
    CHECK(mgr->get_tool("remote_a2a__beta")->requires_approval(), "beta 需审批");
    // ★ cancel 取「任一远端需审批」—— 宁可多问一次，
    //   也不能让高敏远端的计费任务被无审批取消
    CHECK(mgr->get_tool(TOOL_CANCEL)->requires_approval(),
          "任一远端需审批则 cancel 需审批 [不可回退]");

    // 预热后描述含卡内容
    const std::string d = mgr->get_tool("remote_a2a__alpha")->description();
    CHECK(d.find("card for alpha") != std::string::npos, "描述含卡 description");
    CHECK(d.find("skill-1") != std::string::npos, "描述含 skill");

    // build 幂等
    auto again = reg->build();
    CHECK(again.is_ok() && again.get() == 2, "build 幂等");

    std::remove(p1.c_str());
    std::remove(p2.c_str());
}

// ============================================================================
static void test_card_failure_not_blocking() {
    SECTION("卡拉取失败不阻断注册");

    json node;
    node["enabled"] = true;
    node["agents"] = json::array({
        json{{"name", "offline"}, {"card", "/tmp/hc_no_such_card_xyz.json"},
             {"resolveDns", false}},
    });

    auto parsed = parse_remote_a2a_config(node);
    CHECK(parsed.is_ok(), "配置解析成功");

    auto reg = RemoteA2aRegistry::create(parsed.get());
    auto built = reg->build();

    // ★★ [不可回退] 対标 tool_manager.cc:667-671 的 MCP 处理：
    //   远端离线时工具仍注册，否则远端重启期间启动 HunterClaw
    //   会导致工具永久缺失，直到下次重启才恢复
    CHECK(built.is_ok(), "卡失败仍构造成功 [不可回退]");
    CHECK(reg->agent_count() == 1, "远端仍在运行时列表 [不可回退]");

    const auto rts = reg->runtimes();
    CHECK(!rts.empty() && !rts[0].card_ready, "标记 card_ready=false");
    CHECK(!rts[0].card_error.empty(), "记录 card_error");

    bool warned = false;
    for (const auto& w : reg->warnings()) {
        if (w.find("offline") != std::string::npos) warned = true;
    }
    CHECK(warned, "有告警说明卡拉取失败");

    hunterclaw::tools::ToolContext ctx;
    ctx.session_key = "s";
    auto mgr = hunterclaw::tools::ToolManager::create(ctx);
    auto n = reg->register_tools(*mgr);
    CHECK(n.is_ok() && n.get() == 3, "工具仍注册（1+2）[不可回退]");

    auto tool = mgr->get_tool("remote_a2a__offline");
    CHECK(tool != nullptr, "离线远端的工具存在");
    // 未预热 → 兜底描述（不能是空串，否则 prompt 里工具没有说明）
    CHECK(!tool->description().empty(), "有兜底描述 [不可回退]");

    // 调用时才报错
    auto r = tool->execute(hunterclaw::core::JsonValue(json{{"message", "hi"}}));
    CHECK(!r.success, "调用时报错");
    CHECK(r.error.find("AgentCard") != std::string::npos, "错误指向卡");
}

// ============================================================================
static void test_engine_lookup() {
    SECTION("handle → engine 反查");

    const std::string p = write_card("/tmp/hc_reg_lk.json", "lk",
                                    "https://lk.example.com/a2a");
    json node;
    node["enabled"] = true;
    node["agents"] = json::array(
        {json{{"name", "lk"}, {"card", p}, {"resolveDns", false}}});

    auto reg = RemoteA2aRegistry::create(parse_remote_a2a_config(node).get());
    reg->build();

    auto store = reg->store();
    auto h = store->register_task("lk", "t1", "", "s1").get();

    // ★ 通过 store 查 agent_name 再映射，而不是把引擎指针塞进 handle ——
    //   后者会让 handle 变成不可序列化的东西（二期落盘无法恢复）
    auto e = reg->engine_for_handle(h);
    CHECK(e != nullptr, "反查到引擎");

    // 不存在的 handle
    CHECK(reg->engine_for_handle("bogus") == nullptr, "无效 handle 返回 nullptr");

    // agent 不在配置中的 handle（模拟配置变更后残留的任务）
    auto h2 = store->register_task("removed-agent", "t2", "", "s1").get();
    CHECK(reg->engine_for_handle(h2) == nullptr,
          "agent 已移出配置 → nullptr（status 会退化为只读）");

    std::remove(p.c_str());
}

// ============================================================================
static void test_sweeper() {
    SECTION("后台 sweeper");

    const std::string p = write_card("/tmp/hc_reg_sw.json", "sw",
                                    "https://sw.example.com/a2a");
    json node;
    node["enabled"] = true;
    node["store"] = json{{"runningMaxMs", 60000}, {"terminalTtlMs", 60000}};
    node["agents"] = json::array(
        {json{{"name", "sw"}, {"card", p}, {"resolveDns", false}}});

    auto reg = RemoteA2aRegistry::create(parse_remote_a2a_config(node).get());
    reg->build();

    reg->start_sweeper(1000);
    reg->start_sweeper(1000);  // 幂等

    // ★ [不可回退] stop 必须用 cv 唤醒，不能等 interval 走完 ——
    //   interval 是 60s，析构时等一分钟不可接受
    const auto t0 = std::chrono::steady_clock::now();
    reg->stop_sweeper();
    const auto el = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0)
                        .count();
    CHECK(el < 800, "stop_sweeper 立即返回（cv 唤醒）[不可回退]");

    reg->stop_sweeper();  // 重复 stop 安全
    CHECK(true, "重复 stop 不崩溃");

    std::remove(p.c_str());
}

// ============================================================================
static void test_enable_entry() {
    SECTION("enable_remote_a2a 便捷入口");

    hunterclaw::tools::ToolContext ctx;
    ctx.session_key = "s";

    // 未启用 → Ok(0)，不是错误
    {
        auto mgr = hunterclaw::tools::ToolManager::create(ctx);
        std::shared_ptr<RemoteA2aRegistry> out;
        auto r = enable_remote_a2a(*mgr, json(), &out);
        CHECK(r.is_ok() && r.get() == 0, "配置缺失 → Ok(0) [不可回退]");
        CHECK(out == nullptr, "未启用时不产出 registry");
    }
    {
        auto mgr = hunterclaw::tools::ToolManager::create(ctx);
        std::shared_ptr<RemoteA2aRegistry> out;
        auto r = enable_remote_a2a(*mgr, json{{"enabled", false}}, &out);
        CHECK(r.is_ok() && r.get() == 0, "显式禁用 → Ok(0)");
    }

    // 配置无效 → Err
    {
        auto mgr = hunterclaw::tools::ToolManager::create(ctx);
        std::shared_ptr<RemoteA2aRegistry> out;
        auto r = enable_remote_a2a(*mgr, json{{"enabled", true}}, &out);
        CHECK(r.is_err(), "enabled 但无 agents → Err");
    }

    // 正常路径
    {
        const std::string p = write_card("/tmp/hc_reg_en.json", "en",
                                        "https://en.example.com/a2a");
        json node;
        node["enabled"] = true;
        node["agents"] = json::array(
            {json{{"name", "en"}, {"card", p}, {"resolveDns", false}}});

        auto mgr = hunterclaw::tools::ToolManager::create(ctx);
        std::shared_ptr<RemoteA2aRegistry> out;
        auto r = enable_remote_a2a(*mgr, node, &out);
        CHECK(r.is_ok() && r.get() == 3, "注册 1+2 = 3 个工具");
        CHECK(out != nullptr, "产出 registry");
        CHECK(mgr->get_tool("remote_a2a__en") != nullptr, "工具可取回");

        // ★ registry 销毁后 status 工具仍在 ToolManager 里 ——
        //   lookup 用 weak_ptr，registry 没了应返回 nullptr 而非崩溃
        auto status_tool = mgr->get_tool(TOOL_STATUS);
        out.reset();
        auto rr = status_tool->execute(hunterclaw::core::JsonValue(json::object()));
        CHECK(rr.success || !rr.error.empty(),
              "registry 销毁后 status 工具不崩溃 [不可回退]");

        std::remove(p.c_str());
    }
}

// ============================================================================
static void test_tool_name_collision() {
    SECTION("工具名冲突处理");

    const std::string p1 = write_card("/tmp/hc_reg_c1.json", "x", "https://x1.ex.com/a");
    const std::string p2 = write_card("/tmp/hc_reg_c2.json", "x", "https://x2.ex.com/a");

    json node;
    node["enabled"] = true;
    // 两个 agent 消杀后会撞名（"my x" 与 "my-x" 都变成 "my-x"）
    node["agents"] = json::array({
        json{{"name", "my x"}, {"card", p1}, {"resolveDns", false}},
        json{{"name", "my-x"}, {"card", p2}, {"resolveDns", false}},
    });

    auto reg = RemoteA2aRegistry::create(parse_remote_a2a_config(node).get());
    auto built = reg->build();
    CHECK(built.is_ok() && built.get() == 2, "两个 agent 都构造");

    std::set<std::string> names;
    for (const auto& rt : reg->runtimes()) names.insert(rt.tool_name);
    // ★ [不可回退] 撞名必须去重，否则后注册的会覆盖前者（静默丢工具）
    CHECK(names.size() == 2, "工具名已去重 [不可回退]");

    hunterclaw::tools::ToolContext ctx;
    ctx.session_key = "s";
    auto mgr = hunterclaw::tools::ToolManager::create(ctx);
    auto n = reg->register_tools(*mgr);
    CHECK(n.is_ok() && n.get() == 4, "4 个工具全部注册（无覆盖）[不可回退]");
    CHECK(mgr->list_tools().size() == 4, "ToolManager 里确实有 4 个");

    std::remove(p1.c_str());
    std::remove(p2.c_str());
}

// ============================================================================
int main() {
    std::cout << "===== remote_a2a M11 registry 测试 =====" << std::endl;

    test_build_and_register();
    test_card_failure_not_blocking();
    test_engine_lookup();
    test_sweeper();
    test_enable_entry();
    test_tool_name_collision();

    std::cout << "\n===== 结果: " << g_pass << " 通过, " << g_fail << " 失败 ====="
              << std::endl;
    return g_fail == 0 ? 0 : 1;
}
