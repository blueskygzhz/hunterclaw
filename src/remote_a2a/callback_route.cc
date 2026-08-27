// ============================================================================
// remote_a2a/callback_route.cc
// CallbackEndpoint → HttpGateway 胶水层。
// ============================================================================

#include "hunterclaw/remote_a2a/callback_route.h"

// ⚠ http_gateway.h 只前向声明 httplib::Request/Response（为了不把 httplib
//   拖进所有依赖它的头文件）。本文件需要访问 req.remote_addr 等成员，
//   故必须包含真实头。这也正是把胶水层单独放这里的原因 ——
//   remote_a2a 核心的其余部分不该碰 httplib。
#include <httplib.h>

namespace hunterclaw {
namespace remote_a2a {

namespace {

/// 提取对端标识（用于失败节流分桶）。
///
/// ⚠ X-Forwarded-For 是【客户端可伪造】的 —— 攻击者可以每次换一个值来
///   绕过节流。但完全不用它，反向代理后的所有请求会挤在同一个桶里
///   （一个来源的暴力尝试会把正常远端也节流掉）。
///
/// 折中：优先用 XFF 的最左值（原始客户端），但同时把 remote_addr
/// 拼进 key —— 伪造 XFF 只能在同一个 remote_addr 下扩桶，
/// 而 remote_addr 是 TCP 层的，伪造不了。
std::string extract_peer(const httplib::Request& req) {
    std::string base = req.remote_addr;
    if (base.empty()) base = "<noaddr>";

    if (req.has_header("X-Forwarded-For")) {
        std::string xff = req.get_header_value("X-Forwarded-For");
        const size_t comma = xff.find(',');
        if (comma != std::string::npos) xff = xff.substr(0, comma);
        // 去空白
        size_t b = 0, e = xff.size();
        while (b < e && (xff[b] == ' ' || xff[b] == '\t')) ++b;
        while (e > b && (xff[e - 1] == ' ' || xff[e - 1] == '\t')) --e;
        xff = xff.substr(b, e - b);
        // ⚠ 长度上限：XFF 可以很长，拼进 map key 会放大内存占用
        if (xff.size() > 64) xff = xff.substr(0, 64);
        if (!xff.empty()) return base + "|" + xff;
    }
    return base;
}

}  // namespace

bool mount_remote_a2a_callback(gateway::HttpGateway& gw,
                              const std::shared_ptr<RemoteA2aRegistry>& reg) {
    if (!reg) return false;

    auto endpoint = reg->callback_endpoint();
    // ★ 未启用回调 → 既不挂路由也不开旁路。
    //   一个存在的旁路路径就是一处攻击面，没人用时不该存在。
    if (!endpoint) return false;

    // ── ① 路由 ──────────────────────────────────────────────────────────
    // ⚠ 用 httplib 的正则捕获而非 ":handle" 占位符：
    //   handle 的白名单校验由 extract_callback_handle 负责，
    //   这里的正则只要保证「不吃掉多余的路径段」即可。
    const std::string pattern =
        std::string(CALLBACK_PATH_PREFIX) + R"(([A-Za-z0-9_-]+))";

    // ⚠ weak 捕获 endpoint：路由的生命周期跟着 gateway，
    //   若强引用 registry 会让 registry 永远不被释放。
    std::weak_ptr<CallbackEndpoint> weak_ep = endpoint;

    gw.register_route("POST", pattern,
                     [weak_ep](const httplib::Request& req, httplib::Response& res) {
        auto ep = weak_ep.lock();
        if (!ep) {
            // registry 已销毁（配置热重载等）。
            // ★ 回 503 而非 200：让远端重试，别把结果丢了。
            res.status = 503;
            res.set_content(R"({"error":"remote_a2a callback unavailable"})",
                           "application/json");
            return;
        }

        const std::string auth =
            req.has_header("Authorization") ? req.get_header_value("Authorization") : "";

        CallbackOutcome out =
            ep->handle_callback(req.path, auth, req.body, extract_peer(req));

        res.status = out.http_status;
        res.set_content(out.body.dump(), "application/json");

        // ⚠ 拒绝时关闭连接 —— 対标 http_gateway.cc:258 的
        //   AuthMiddleware 失败处理（不给暴力尝试者复用连接的便利）。
        if (out.http_status >= 400) {
            res.set_header("Connection", "close");
        }
    });

    // ── ② 鉴权旁路 ──────────────────────────────────────────────────────
    // ★ 缺这一步会让所有回调 401（AuthMiddleware 只放行 /health、/metrics）。
    //   回调带的是我们发给该任务的一次性 token，不是 gateway 共享 token。
    gw.add_auth_bypass_prefix(CALLBACK_PATH_PREFIX);

    return true;
}

}  // namespace remote_a2a
}  // namespace hunterclaw
