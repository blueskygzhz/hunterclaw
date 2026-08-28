// ============================================================================
// test_https_smoke.cc
// HttpClient HTTPS 通路冒烟测试。
//
// ★ 为什么需要这个测试：SSL 代码路径此前【从未被编译过】
//   （src/BUILD 缺 HUNTERCLAW_ENABLE_SSL）。编译通过不等于握手成功 ——
//   OpenSSL 1.0.2 兼容层的三处替换必须用真实握手验证：
//     ① SSLv23_client_method 能否协商出 TLS 1.2+
//     ② SSL_OP_NO_* 位掩码是否真的挡住了 TLS 1.1 及以下
//     ③ 主机名校验是否生效（证书有效但域名不匹配必须失败）
//
// ⚠ 本测试【需要外网】。无网环境会全部 SKIP 而非 FAIL ——
//   否则 CI 会因网络问题红掉，掩盖真正的回归。
//
// 构建:
//   g++ -std=c++17 -DHUNTERCLAW_ENABLE_SSL -Iinclude \
//     -Iexternal/json/single_include tests/test_https_smoke.cc \
//     src/infra/http_client.cc -lssl -lcrypto -lpthread -o /tmp/t_https
// ============================================================================

#include <iostream>
#include <string>

#include "hunterclaw/infra/http_client.h"

using namespace hunterclaw;

static int g_pass = 0;
static int g_fail = 0;
static int g_skip = 0;

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

#define SKIP(name, why)                                                    \
    do {                                                                   \
        ++g_skip;                                                          \
        std::cout << "  SKIP: " << (name) << " (" << (why) << ")"          \
                  << std::endl;                                            \
    } while (0)

#define SECTION(x) std::cout << "\n== " << x << " ==" << std::endl

/// 判断错误是否为「网络不可达」（区别于「SSL 逻辑错误」）。
/// ★ 这个区分是本测试的核心：不能把无网环境当成 SSL 缺陷。
static bool is_network_unavailable(const std::string& err) {
    static const char* patterns[] = {
        "Failed to resolve", "Connection timeout", "Connection failed",
        "Connection refused", "Network is unreachable", "Temporary failure",
    };
    for (const char* p : patterns) {
        if (err.find(p) != std::string::npos) return true;
    }
    return false;
}

// ============================================================================
// ① HTTPS 支持已编译进来（不需要网络）
// ============================================================================
static void test_ssl_compiled_in() {
    SECTION("HTTPS 支持已编译");

    auto client = infra::HttpClient::create();
    infra::HttpRequestOptions opt;
    opt.timeout_ms = 3000;

    // ★★ 关键断言：即使连不上，错误也【不能】是 "HTTPS not supported"。
    //   这正是 src/BUILD 缺 HUNTERCLAW_ENABLE_SSL 时的症状 ——
    //   一个不发包就返回的假错误。
    auto r = client->get("https://127.0.0.1:1/nothing", opt);
    const std::string err = r.is_err() ? r.error : "";
    CHECK(err.find("HTTPS not supported") == std::string::npos,
          "不再返回 'HTTPS not supported' [不可回退]");
    CHECK(err.find("SSL not supported") == std::string::npos,
          "connect_ssl 非 stub 实现 [不可回退]");

    // 连本地 1 端口必然失败，但应是【传输层】错误而非"不支持"
    CHECK(r.is_err() || !r.get().ok, "无效目标确实失败");
}

// ============================================================================
// ② 真实 HTTPS 握手（需要网络）
// ============================================================================
static void test_real_handshake() {
    SECTION("真实 HTTPS 握手");

    auto client = infra::HttpClient::create();
    infra::HttpRequestOptions opt;
    opt.timeout_ms = 15000;

    auto r = client->get("https://www.google.com/generate_204", opt);
    if (r.is_err()) {
        if (is_network_unavailable(r.error)) {
            SKIP("TLS 握手", "无外网: " + r.error);
            return;
        }
        // ★ 非网络原因的失败 = 真缺陷。必须 FAIL 而非 SKIP。
        CHECK(false, "TLS 握手失败(非网络原因): " + r.error);
        return;
    }

    // ★ 兼容层验证 ①：SSLv23_client_method 成功协商出 TLS
    CHECK(r.get().status_code > 0, "拿到 HTTP 状态码（握手成功）[不可回退]");
    std::cout << "    握手成功, status=" << r.get().status_code << std::endl;
}

// ============================================================================
// ③ 证书校验生效（需要网络）
// ============================================================================
static void test_cert_verification() {
    SECTION("证书校验");

    auto client = infra::HttpClient::create();
    infra::HttpRequestOptions opt;
    opt.timeout_ms = 15000;

    // badssl.com 的过期证书 —— 默认 ssl_verify=true 必须拒绝
    {
        auto r = client->get("https://expired.badssl.com/", opt);
        if (r.is_err() && is_network_unavailable(r.error)) {
            SKIP("过期证书拒绝", "无外网");
        } else {
            // ★★ [不可回退] 过期证书必须被拒。
            //   若通过，说明 SSL_CTX_set_verify(SSL_VERIFY_PEER) 或
            //   SSL_get_verify_result 复核失效 —— 那是 MITM 门户大开。
            CHECK(r.is_err(), "过期证书被拒绝 [不可回退]");
            if (r.is_err()) {
                std::cout << "    拒绝原因: " << r.error << std::endl;
                // 错误信息应指向证书问题，而非笼统的握手失败
                const bool informative =
                    r.error.find("erti") != std::string::npos ||  // certificate
                    r.error.find("expired") != std::string::npos ||
                    r.error.find("verif") != std::string::npos;
                CHECK(informative, "错误信息指向证书问题（可排查）");
            }
        }
    }

    // ★ 兼容层验证 ③：主机名不匹配必须被拒
    //   （X509_VERIFY_PARAM_set1_host 在 1.0.2 里存在，此处确认真的生效）
    {
        auto r = client->get("https://wrong.host.badssl.com/", opt);
        if (r.is_err() && is_network_unavailable(r.error)) {
            SKIP("主机名校验", "无外网");
        } else {
            CHECK(r.is_err(), "主机名不匹配被拒绝 [不可回退]");
            if (r.is_err()) {
                std::cout << "    拒绝原因: " << r.error << std::endl;
            }
        }
    }

    // ssl_verify=false 时应放行（自签名场景的逃生阀）
    {
        infra::HttpRequestOptions lax;
        lax.timeout_ms = 15000;
        lax.ssl_verify = false;
        auto r = client->get("https://self-signed.badssl.com/", lax);
        if (r.is_err() && is_network_unavailable(r.error)) {
            SKIP("ssl_verify=false 放行", "无外网");
        } else {
            // ★ 这条也是「不可回退」的另一面：逃生阀必须真的能用，
            //   否则内网自签名场景无法工作，用户会转而去关掉整个校验。
            CHECK(r.is_ok(), "ssl_verify=false 放行自签名证书 [不可回退]");
        }
    }
}

// ============================================================================
// ④ TLS 版本下限（需要网络）
// ============================================================================
static void test_tls_version_floor() {
    SECTION("TLS 版本下限");

    auto client = infra::HttpClient::create();
    infra::HttpRequestOptions opt;
    opt.timeout_ms = 15000;

    // ★★ 兼容层验证 ②：OpenSSL 1.0.2 下用 SSL_OP_NO_TLSv1|NO_TLSv1_1
    //   替代 SSL_CTX_set_min_proto_version(TLS1_2_VERSION)。
    //   badssl 的 tls-v1-1 端点只支持 TLS 1.1 —— 必须连不上。
    //   若连上了，说明位掩码写错或未生效，TLS 下限形同虚设。
    auto r = client->get("https://tls-v1-1.badssl.com:1011/", opt);
    if (r.is_err() && is_network_unavailable(r.error)) {
        SKIP("TLS 1.1 拒绝", "无外网");
        return;
    }
    CHECK(r.is_err(), "TLS 1.1 端点被拒绝（下限=1.2 生效）[不可回退]");
    if (r.is_err()) {
        std::cout << "    拒绝原因: " << r.error << std::endl;
    }
}

// ============================================================================
int main() {
    std::cout << "===== HttpClient HTTPS 冒烟测试 =====" << std::endl;
#ifndef HUNTERCLAW_ENABLE_SSL
    std::cout << "\n!! 编译时未定义 HUNTERCLAW_ENABLE_SSL —— 本测试无意义"
              << std::endl;
    return 1;
#endif

    test_ssl_compiled_in();
    test_real_handshake();
    test_cert_verification();
    test_tls_version_floor();

    std::cout << "\n===== 结果: " << g_pass << " 通过, " << g_fail
              << " 失败, " << g_skip << " 跳过 =====" << std::endl;
    return g_fail == 0 ? 0 : 1;
}
