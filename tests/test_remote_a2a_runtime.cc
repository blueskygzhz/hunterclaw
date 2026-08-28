// ============================================================================
// test_remote_a2a_runtime.cc
// 进程级 RemoteA2aRuntime 测试
//
// ⚠ 本套测试有个特殊约束：RemoteA2aRuntime 是【单例且 init 幂等】，
//   一个进程内只能初始化一次。因此测试顺序有依赖：
//   前半部分测「未启用」路径（不会把 inited_ 置成 enabled），
//   最后才测「启用」路径。
//   这不是测试写得糙，而是被单例语义强制的 —— 也正因如此，
//   下面每个 case 都标注了它对全局状态的影响。
// ============================================================================

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>

#include "hunterclaw/remote_a2a/runtime.h"

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
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f << c.dump();
    f.close();
    return path;
}

// ============================================================================
// 环境变量逃生阀
// ============================================================================
static void test_env_disable() {
    SECTION("HUNTERCLAW_REMOTE_A2A 逃生阀");

    // ★ 判据必须与 MCP 一致（agent_instance.cc:193 用的是 != "0"）——
    //   运维习惯不一致会踩坑
    unsetenv("HUNTERCLAW_REMOTE_A2A");
    CHECK(!RemoteA2aRuntime::disabled_by_env(), "未设置 → 启用");

    setenv("HUNTERCLAW_REMOTE_A2A", "0", 1);
    CHECK(RemoteA2aRuntime::disabled_by_env(), "=0 → 禁用 [不可回退]");

    setenv("HUNTERCLAW_REMOTE_A2A", "1", 1);
    CHECK(!RemoteA2aRuntime::disabled_by_env(), "=1 → 启用");

    setenv("HUNTERCLAW_REMOTE_A2A", "", 1);
    CHECK(!RemoteA2aRuntime::disabled_by_env(), "空串 → 启用（与 MCP 一致）");

    setenv("HUNTERCLAW_REMOTE_A2A", "false", 1);
    CHECK(!RemoteA2aRuntime::disabled_by_env(),
          "'false' 不算禁用（只认 '0'，与 MCP 一致）");

    unsetenv("HUNTERCLAW_REMOTE_A2A");
}

// ============================================================================
// 未初始化状态
// ============================================================================
static void test_uninitialized() {
    SECTION("未初始化状态");

    auto& rt = RemoteA2aRuntime::instance();
    // ★ init 之前一切查询都必须安全（agent_instance 可能先于 main 的 init
    //   被调到 —— 比如测试环境或异常启动顺序）
    CHECK(!rt.enabled(), "未 init → 未启用");
    CHECK(!rt.callback_needed(), "未 init → 无需回调");
    CHECK(rt.registry() == nullptr, "未 init → registry 为空");
    CHECK(rt.warnings().empty(), "未 init → 无警告");

    // ★ [不可回退] 未 init 时 register_into 必须返回 Ok(0) 而非 Err ——
    //   绝大多数部署不用 remote A2A，返回 Err 会让 agent_instance
    //   在每个会话都刷一条警告
    hunterclaw::tools::ToolContext ctx;
    ctx.session_key = "s1";
    auto mgr = hunterclaw::tools::ToolManager::create(ctx);
    auto r = rt.register_into(*mgr);
    CHECK(r.is_ok() && r.value.value_or(-1) == 0,
          "未 init → register_into 返回 Ok(0) [不可回退]");
    CHECK(mgr->list_tools().empty(), "未注册任何工具");

    // attach_callback 也必须安全
    bool mounter_called = false;
    CHECK(!rt.attach_callback([&](const std::shared_ptr<RemoteA2aRegistry>&) {
        mounter_called = true;
        return true;
    }), "未 init → attach_callback 返回 false");
    CHECK(!mounter_called, "未 init → mounter 未被调用 [不可回退]");

    // shutdown 幂等且安全
    rt.shutdown();
    rt.shutdown();
    CHECK(true, "未 init 时 shutdown 不崩溃");
}

// ============================================================================
// 配置缺失 / 禁用
// ============================================================================
static void test_init_disabled() {
    SECTION("init：配置缺失/禁用");

    auto& rt = RemoteA2aRuntime::instance();

    // ★ 配置缺失不是错误 —— 返回 Ok 且 enabled=false
    auto r = rt.init(json());
    CHECK(r.is_ok(), "配置缺失 → Ok [不可回退]");
    CHECK(r.value.value_or(-1) == 0, "计数为 0");
    CHECK(!rt.enabled(), "enabled=false");

    // ★ [不可回退] init 幂等：第二次调用不重建。
    //   重建会让已登记的让渡任务失去引擎（handle 反查不到）。
    auto r2 = rt.init(json{{"enabled", true},
                          {"agents", json::array({json{{"name", "x"},
                                                       {"card", "https://x.example.com"}}})}});
    CHECK(r2.is_ok(), "重复 init 返回 Ok");
    CHECK(!rt.enabled(), "重复 init 不改变已有状态 [不可回退]");
    CHECK(rt.registry() == nullptr, "重复 init 不创建 registry [不可回退]");

    // 未启用时 register_into 仍返回 Ok(0)
    hunterclaw::tools::ToolContext ctx;
    ctx.session_key = "s2";
    auto mgr = hunterclaw::tools::ToolManager::create(ctx);
    CHECK(rt.register_into(*mgr).value.value_or(-1) == 0, "未启用 → Ok(0)");
}

// ============================================================================
// 主流程（★ 必须放最后：会把单例置成 enabled）
//
// ⚠ 由于 init 幂等，上面的 test_init_disabled 已经把 inited_ 置为 true，
//   本函数无法再真正初始化。故改为直接构造 registry 验证接线逻辑，
//   并单独验证 init 的各条判定（通过 parse + build 的组合，
//   与 init 内部实现一致）。
// ============================================================================
static void test_registry_path() {
    SECTION("registry 接线（绕过单例）");

    const std::string p = write_card("/tmp/hc_rt_card.json", "rtx",
                                    "https://rtx.example.com/a2a");

    json node;
    node["enabled"] = true;
    node["agents"] = json::array(
        {json{{"name", "rtx"}, {"card", p}, {"resolveDns", false}}});

    auto parsed = parse_remote_a2a_config(node);
    CHECK(parsed.is_ok(), "配置解析成功");
    if (!parsed.is_ok()) {
        std::remove(p.c_str());
        return;
    }

    auto reg = RemoteA2aRegistry::create(parsed.get());
    auto built = reg->build();
    CHECK(built.is_ok() && built.get() == 1, "构造 1 个远端");

    // ★ 关键：同一个 registry 注册进【两个不同会话】的 ToolManager，
    //   两边的工具应各自携带自己的 session_key（隔离在工具层，
    //   而非存储层 —— store 是共享的）
    hunterclaw::tools::ToolContext ctx_a;
    ctx_a.session_key = "sess-A";
    auto mgr_a = hunterclaw::tools::ToolManager::create(ctx_a);
    CHECK(reg->register_tools(*mgr_a).value.value_or(0) == 3, "会话 A 注册 3 个工具");

    hunterclaw::tools::ToolContext ctx_b;
    ctx_b.session_key = "sess-B";
    auto mgr_b = hunterclaw::tools::ToolManager::create(ctx_b);
    CHECK(reg->register_tools(*mgr_b).value.value_or(0) == 3, "会话 B 注册 3 个工具");

    // 两个会话各有独立的工具实例
    CHECK(mgr_a->get_tool("remote_a2a__rtx") != mgr_b->get_tool("remote_a2a__rtx"),
          "两会话的工具实例不同 [不可回退]");

    // ★★ [不可回退] store 共享，但跨会话读取被拦下 ——
    //   这是「共享存储 + 工具层隔离」这一设计的核心验证
    auto store = reg->store();
    auto h_a = store->register_task("rtx", "t-a", "", "sess-A").get();
    store->update_terminal(h_a, TerminalReason::Completed, "A 的结果", "");

    auto status_a = mgr_a->get_tool("remote_a2a_status");
    auto status_b = mgr_b->get_tool("remote_a2a_status");

    auto ra = status_a->execute(hunterclaw::core::JsonValue(json{{"handle", h_a}}));
    CHECK(ra.success, "会话 A 能读自己的任务");

    auto rb = status_b->execute(hunterclaw::core::JsonValue(json{{"handle", h_a}}));
    CHECK(!rb.success, "会话 B 读不到 A 的任务 [不可回退]");

    // 会话 B 的列表里看不到 A 的任务
    auto lb = status_b->execute(hunterclaw::core::JsonValue(json::object()));
    CHECK(lb.content[0].text.find(h_a) == std::string::npos,
          "会话 B 列表不含 A 的 handle [不可回退]");

    // ★ 共享 store 意味着只有一份卡缓存：两个会话的工具指向同一 client
    CHECK(reg->runtimes().size() == 1, "只有一份 runtime（卡只拉一次）");

    reg->stop_sweeper();
    std::remove(p.c_str());
}

// ============================================================================
// 回调需求判定
// ============================================================================
static void test_callback_needed() {
    SECTION("回调需求判定");

    const std::string p = write_card("/tmp/hc_rt_cb.json", "cbx",
                                    "https://cbx.example.com/a2a");

    // 未启用回调
    {
        json node;
        node["enabled"] = true;
        node["agents"] = json::array(
            {json{{"name", "cbx"}, {"card", p}, {"resolveDns", false}}});
        auto reg = RemoteA2aRegistry::create(parse_remote_a2a_config(node).get());
        reg->build();
        CHECK(!reg->callback_enabled(), "未配 enableCallback → 不需要");
        CHECK(reg->callback_endpoint() == nullptr, "不创建端点");
        reg->stop_sweeper();
    }

    // 启用回调
    {
        json node;
        node["enabled"] = true;
        node["callbackBaseUrl"] = "https://hc.example.com";
        node["agents"] = json::array({json{{"name", "cbx"},
                                           {"card", p},
                                           {"resolveDns", false},
                                           {"enableCallback", true}}});
        auto reg = RemoteA2aRegistry::create(parse_remote_a2a_config(node).get());
        reg->build();
        CHECK(reg->callback_enabled(), "配了 enableCallback → 需要");
        CHECK(reg->callback_endpoint() != nullptr, "创建端点");
        reg->stop_sweeper();
    }

    std::remove(p.c_str());
}

// ============================================================================
int main() {
    std::cout << "===== remote_a2a runtime 接线测试 =====" << std::endl;

    // ⚠ 顺序有依赖：单例 init 幂等，禁用路径必须先跑
    test_env_disable();
    test_uninitialized();
    test_init_disabled();
    test_registry_path();
    test_callback_needed();

    std::cout << "\n===== 结果: " << g_pass << " 通过, " << g_fail << " 失败 ====="
              << std::endl;
    return g_fail == 0 ? 0 : 1;
}
