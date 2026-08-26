// ============================================================================
// test_remote_a2a_auth.cc
// M3 鉴权层测试：auth（配置解析）+ credential_resolver（凭据→headers）
//
// 构建：
//   /usr/local/gcc-12.2.0/bin/g++ -std=c++17 -Iinclude \
//     -Iexternal/json/single_include -Ithird_party \
//     tests/test_remote_a2a_auth.cc \
//     src/remote_a2a/auth.cc src/remote_a2a/credential_resolver.cc \
//     -o /tmp/test_remote_a2a_auth && /tmp/test_remote_a2a_auth
// ============================================================================

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>

#include "hunterclaw/remote_a2a/auth.h"
#include "hunterclaw/remote_a2a/credential_resolver.h"

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

// base64("u:p") == "dTpw"
static const char* kBasicUP = "dTpw";

// ============================================================================
// 配置解析
// ============================================================================
static void test_reject_plaintext() {
    SECTION("配置解析：明文凭据拒绝");

    // [不可回退] 明文凭据禁止进配置文件。
    CHECK(!parse_auth_spec(json::parse(R"({"type":"bearer","token":"sk-abc"})")).ok,
          "明文 token 拒绝");
    CHECK(!parse_auth_spec(
              json::parse(R"({"type":"basic","username":"u","password":"p"})")).ok,
          "明文 password 拒绝");
    CHECK(!parse_auth_spec(json::parse(R"({"type":"apikey","api_key":"k"})")).ok,
          "明文 api_key 拒绝");
    CHECK(!parse_auth_spec(json::parse(R"({"type":"bearer","secret":"s"})")).ok,
          "明文 secret 拒绝");
    CHECK(!parse_auth_spec(json::parse(R"({"type":"bearer","access_token":"t"})")).ok,
          "明文 access_token 拒绝");

    // 错误信息必须指向正确做法
    auto r = parse_auth_spec(json::parse(R"({"type":"bearer","token":"x"})"));
    CHECK(r.error.find("token_env") != std::string::npos, "错误信息提示 token_env");
    CHECK(r.error.find("secret_ref") != std::string::npos, "错误信息提示 secret_ref");
}

static void test_parse_each_kind() {
    SECTION("配置解析：四种鉴权");

    // none
    {
        auto r = parse_auth_spec(json::parse(R"({"type":"none"})"));
        CHECK(r.ok && r.value->kind == AuthKind::None, "none");
        auto r2 = parse_auth_spec(json(nullptr));
        CHECK(r2.ok && r2.value->kind == AuthKind::None, "null → none");
        auto r3 = parse_auth_spec(json::object());
        CHECK(r3.ok && r3.value->kind == AuthKind::None, "空对象 → none");
    }
    // bearer
    {
        auto r = parse_auth_spec(
            json::parse(R"({"type":"bearer","token_env":"MY_TOKEN"})"));
        CHECK(r.ok && r.value->kind == AuthKind::Bearer, "bearer 解析");
        CHECK(r.value->source == SecretSource::Env, "来源 Env");
        CHECK(r.value->env_name == "MY_TOKEN", "env 名");
        // 缺来源
        CHECK(!parse_auth_spec(json::parse(R"({"type":"bearer"})")).ok,
              "bearer 缺来源拒绝");
    }
    // apikey
    {
        auto r = parse_auth_spec(json::parse(
            R"({"type":"apikey","header_name":"X-API-Key","token_env":"K"})"));
        CHECK(r.ok && r.value->kind == AuthKind::ApiKey, "apikey 解析");
        CHECK(r.value->header_name == "X-API-Key", "header_name");
        // [不可回退] header_name 必填，不从卡推断（卡是对端可控的）
        CHECK(!parse_auth_spec(json::parse(R"({"type":"apikey","token_env":"K"})")).ok,
              "apikey 缺 header_name 拒绝");
    }
    // basic
    {
        auto r = parse_auth_spec(json::parse(
            R"({"type":"basic","username":"u","password_env":"P"})"));
        CHECK(r.ok && r.value->kind == AuthKind::Basic, "basic 解析");
        CHECK(r.value->username == "u", "username");
        CHECK(r.value->password_env == "P", "password_env");
        CHECK(!parse_auth_spec(json::parse(R"({"type":"basic","password_env":"P"})")).ok,
              "basic 缺 username 拒绝");
        CHECK(!parse_auth_spec(json::parse(R"({"type":"basic","username":"u"})")).ok,
              "basic 缺密码来源拒绝");
    }
    // custom
    {
        auto r = parse_auth_spec(json::parse(
            R"({"type":"custom","scheme_name":"Token","token_env":"T"})"));
        CHECK(r.ok && r.value->kind == AuthKind::Custom, "custom 解析");
        CHECK(r.value->scheme_name == "Token", "scheme_name");
        CHECK(!parse_auth_spec(json::parse(R"({"type":"custom","token_env":"T"})")).ok,
              "custom 缺 scheme_name 拒绝");
        // scheme 带空格会破坏 Authorization 头结构
        CHECK(!parse_auth_spec(json::parse(
                  R"({"type":"custom","scheme_name":"Bad Scheme","token_env":"T"})")).ok,
              "scheme_name 含空格拒绝");
    }
    // 一期不支持的
    CHECK(!parse_auth_spec(json::parse(R"({"type":"oauth2"})")).ok, "oauth2 拒绝");
    CHECK(!parse_auth_spec(json::parse(R"({"type":"mtls"})")).ok, "mtls 拒绝");
    CHECK(!parse_auth_spec(json::parse(R"({"type":"weird"})")).ok, "未知类型拒绝");
    CHECK(!parse_auth_spec(json::parse(R"("notobject")")).ok, "非对象拒绝");
}

static void test_parse_sources_and_extras() {
    SECTION("配置解析：来源与附加头");

    // 三种来源
    CHECK(parse_auth_spec(json::parse(R"({"type":"bearer","token_env":"E"})"))
              .value->source == SecretSource::Env, "token_env → Env");
    CHECK(parse_auth_spec(json::parse(R"({"type":"bearer","token_file":"/f"})"))
              .value->source == SecretSource::File, "token_file → File");
    CHECK(parse_auth_spec(json::parse(R"({"type":"bearer","secret_ref":"vault://x"})"))
              .value->source == SecretSource::SecretRef, "secret_ref → SecretRef");

    // additional_headers
    {
        auto r = parse_auth_spec(json::parse(R"({
            "type":"bearer","token_env":"T",
            "additional_headers":{"X-Tenant-Id":"acme","X-Trace":"1"}
        })"));
        CHECK(r.ok && r.value->additional_headers.size() == 2, "附加头解析");
        CHECK(r.value->additional_headers.at("X-Tenant-Id") == "acme", "附加头值");
    }
    // [不可回退] 不允许用 additional_headers 覆盖 Authorization
    CHECK(!parse_auth_spec(json::parse(R"({
              "type":"bearer","token_env":"T",
              "additional_headers":{"Authorization":"Bearer evil"}
          })")).ok, "禁止覆盖 Authorization");
    CHECK(!parse_auth_spec(json::parse(R"({
              "type":"bearer","token_env":"T",
              "additional_headers":{"authorization":"x"}
          })")).ok, "大小写不敏感禁止覆盖");

    // authenticate_card_request
    {
        auto r = parse_auth_spec(json::parse(
            R"({"type":"bearer","token_env":"T","authenticate_card_request":true})"));
        CHECK(r.ok && r.value->authenticate_card_request, "authenticate_card_request");
    }
}

// ============================================================================
// 卡的 securityScheme 推断
// ============================================================================
static void test_card_scheme_inference() {
    SECTION("卡 securityScheme 推断");

    CHECK(infer_kind_from_card_scheme(
              json::parse(R"({"type":"http","scheme":"bearer"})")).value ==
              AuthKind::Bearer, "http/bearer");
    CHECK(infer_kind_from_card_scheme(
              json::parse(R"({"type":"http","scheme":"basic"})")).value ==
              AuthKind::Basic, "http/basic");
    CHECK(infer_kind_from_card_scheme(
              json::parse(R"({"type":"http","scheme":"token"})")).value ==
              AuthKind::Custom, "http/其他 → Custom");
    CHECK(infer_kind_from_card_scheme(
              json::parse(R"({"type":"apiKey","in":"header","name":"X-K"})")).value ==
              AuthKind::ApiKey, "apiKey in=header");
    // in 缺省视为 header
    CHECK(infer_kind_from_card_scheme(json::parse(R"({"type":"apiKey"})")).value ==
              AuthKind::ApiKey, "apiKey 缺 in → header");
    // 1.x proto 用 location 字段
    CHECK(infer_kind_from_card_scheme(
              json::parse(R"({"type":"apiKey","location":"header"})")).ok,
          "apiKey location=header");

    // [不可回退] ★ apiKey in=query/cookie 必须【显式失败】。
    //   ADK 只 logger.warning（_auth_headers.py:97-104），凭据被静默丢弃，
    //   请求以无鉴权发出 → 远端 401 → 运维看到 401 而非"配置不支持"。
    {
        auto r = infer_kind_from_card_scheme(
            json::parse(R"({"type":"apiKey","in":"query"})"));
        CHECK(!r.ok, "apiKey in=query 显式失败");
        CHECK(r.error.find("header") != std::string::npos, "错误信息说明仅支持 header");
    }
    CHECK(!infer_kind_from_card_scheme(
              json::parse(R"({"type":"apiKey","in":"cookie"})")).ok,
          "apiKey in=cookie 显式失败");
    CHECK(!infer_kind_from_card_scheme(
              json::parse(R"({"type":"apiKey","location":"query"})")).ok,
          "proto location=query 显式失败");

    CHECK(infer_kind_from_card_scheme(json::parse(R"({"type":"oauth2"})")).value ==
              AuthKind::OAuth2, "oauth2");
    CHECK(!infer_kind_from_card_scheme(json::parse(R"({"type":"???"})")).ok,
          "未知 type 失败");
    CHECK(!infer_kind_from_card_scheme(json::parse(R"({"type":"http"})")).ok,
          "http 缺 scheme 失败");
    CHECK(!infer_kind_from_card_scheme(json::parse(R"("x")")).ok, "非对象失败");
}

static void test_auth_consistency() {
    SECTION("鉴权一致性检查（只警告不阻断）");

    AuthSpec local;
    local.kind = AuthKind::Bearer;

    // 卡未声明（ADK 示例卡的真实形态）→ 有警告但不阻断
    auto w1 = check_auth_consistency(json::object(), local);
    CHECK(w1.size() == 1, "卡未声明 securitySchemes → 1 条警告");

    // 卡声明匹配 → 无警告
    auto w2 = check_auth_consistency(
        json::parse(R"({"s":{"type":"http","scheme":"bearer"}})"), local);
    CHECK(w2.empty(), "声明匹配 → 无警告");

    // 卡声明不匹配 → 警告
    auto w3 = check_auth_consistency(
        json::parse(R"({"s":{"type":"http","scheme":"basic"}})"), local);
    CHECK(!w3.empty(), "声明不匹配 → 有警告");

    // 卡要求鉴权但本地无凭据
    AuthSpec none;
    auto w4 = check_auth_consistency(
        json::parse(R"({"s":{"type":"http","scheme":"bearer"}})"), none);
    CHECK(!w4.empty(), "卡要求鉴权但本地无凭据 → 警告");

    // 本地无凭据且卡也无声明 → 无警告
    CHECK(check_auth_consistency(json::object(), none).empty(), "两边都无 → 无警告");
}

// ============================================================================
// ResolvedCredential → headers
// ============================================================================
static void test_headers_mapping() {
    SECTION("凭据 → HTTP headers");

    // bearer
    {
        auto c = ResolvedCredential::make_bearer("abc123");
        auto h = c.to_headers();
        CHECK(h.size() == 1 && h.at("Authorization") == "Bearer abc123", "bearer 头");
    }
    // [不可回退] ★ 空 token 【不产出】 header。
    //   対标 _auth_headers.py:52-55：不判空会发出字面量 "Bearer None"，
    //   远端返回 401 而非 400，排查困难。
    {
        CHECK(ResolvedCredential::make_bearer("").to_headers().empty(),
              "空 token → 无 header");
    }
    // apikey
    {
        auto h = ResolvedCredential::make_api_key("X-API-Key", "k1").to_headers();
        CHECK(h.size() == 1 && h.at("X-API-Key") == "k1", "apikey 头");
        CHECK(ResolvedCredential::make_api_key("X-K", "").to_headers().empty(),
              "apikey 空 token → 无 header");
        CHECK(ResolvedCredential::make_api_key("", "k").to_headers().empty(),
              "apikey 空 header 名 → 无 header");
    }
    // basic
    {
        auto h = ResolvedCredential::make_basic("u", "p").to_headers();
        CHECK(h.at("Authorization") == std::string("Basic ") + kBasicUP,
              "basic base64 编码正确");
        CHECK(ResolvedCredential::make_basic("u", "").to_headers().empty(),
              "basic 空密码 → 无 header");
        CHECK(ResolvedCredential::make_basic("", "p").to_headers().empty(),
              "basic 空用户 → 无 header");
    }
    // custom
    {
        auto h = ResolvedCredential::make_custom("Token", "t1").to_headers();
        CHECK(h.at("Authorization") == "Token t1", "custom 头");
        CHECK(ResolvedCredential::make_custom("Token", "").to_headers().empty(),
              "custom 空 token → 无 header");
    }
    // none
    {
        CHECK(ResolvedCredential::make_none().to_headers().empty(), "none 无头");
    }
    // error 凭据不产出任何头
    {
        auto c = ResolvedCredential::make_error("boom");
        CHECK(!c.valid(), "error 凭据 invalid");
        CHECK(c.error() == "boom", "error 文本");
        CHECK(c.to_headers().empty(), "error 凭据无头");
    }
    // additional_headers 合并
    {
        auto c = ResolvedCredential::make_bearer("t");
        c.add_extra({{"X-Tenant-Id", "acme"}});
        auto h = c.to_headers();
        CHECK(h.size() == 2, "附加头合并");
        CHECK(h.at("X-Tenant-Id") == "acme", "附加头值");
        CHECK(h.at("Authorization") == "Bearer t", "Authorization 仍在");
    }
    // 移动语义（禁拷贝，只能移动）
    {
        auto a = ResolvedCredential::make_bearer("mv");
        auto b = std::move(a);
        CHECK(b.to_headers().at("Authorization") == "Bearer mv", "移动后可用");
        CHECK(a.to_headers().empty(), "移动后源失效");
    }
}

static void test_base64() {
    SECTION("base64 编码边界");

    // 三种 padding 情形
    CHECK(ResolvedCredential::make_basic("u", "p").to_headers().at("Authorization") ==
              "Basic dTpw", "2 字节 → 1 padding");
    CHECK(ResolvedCredential::make_basic("a", "bc").to_headers().at("Authorization") ==
              "Basic YTpiYw==", "4 字节 → 2 padding");
    CHECK(ResolvedCredential::make_basic("ab", "c").to_headers().at("Authorization") ==
              "Basic YWI6Yw==", "4 字节 变体");
    CHECK(ResolvedCredential::make_basic("abc", "de").to_headers().at("Authorization") ==
              "Basic YWJjOmRl", "6 字节 → 无 padding");
}

// ============================================================================
// 秘密读取
// ============================================================================
static void test_secret_reading() {
    SECTION("秘密来源读取");

    auto r = CredentialResolver::create();

    // 环境变量
    ::setenv("RA2A_TEST_TOKEN", "env-secret-123", 1);
    {
        AuthSpec s;
        s.kind = AuthKind::Bearer;
        s.source = SecretSource::Env;
        s.env_name = "RA2A_TEST_TOKEN";
        auto c = r->resolve(s);
        CHECK(c.ok, "env 读取成功");
        CHECK(c.value->to_headers().at("Authorization") == "Bearer env-secret-123",
              "env 值正确");
        CHECK(r->precheck(s).ok, "precheck 通过");
    }
    // 环境变量未设置
    {
        AuthSpec s;
        s.kind = AuthKind::Bearer;
        s.source = SecretSource::Env;
        s.env_name = "RA2A_DEFINITELY_NOT_SET_XYZ";
        auto c = r->resolve(s);
        CHECK(!c.ok, "env 未设置 → Err");
        CHECK(c.error.find("RA2A_DEFINITELY_NOT_SET_XYZ") != std::string::npos,
              "错误信息含变量名");
        CHECK(!r->precheck(s).ok, "precheck 失败");
    }
    // 空环境变量视为未设置
    {
        ::setenv("RA2A_EMPTY", "", 1);
        AuthSpec s;
        s.kind = AuthKind::Bearer;
        s.source = SecretSource::Env;
        s.env_name = "RA2A_EMPTY";
        CHECK(!r->resolve(s).ok, "空 env 视为未设置");
    }
    // 文件（含尾随换行，必须被 trim）
    {
        const char* path = "/tmp/ra2a_test_secret.txt";
        {
            std::ofstream f(path);
            f << "file-secret-456\n\n";
        }
        AuthSpec s;
        s.kind = AuthKind::Bearer;
        s.source = SecretSource::File;
        s.file_path = path;
        auto c = r->resolve(s);
        CHECK(c.ok, "file 读取成功");
        CHECK(c.value->to_headers().at("Authorization") == "Bearer file-secret-456",
              "file 尾随换行被 trim");
        ::remove(path);
    }
    // 文件不存在
    {
        AuthSpec s;
        s.kind = AuthKind::Bearer;
        s.source = SecretSource::File;
        s.file_path = "/tmp/ra2a_no_such_file_xyz";
        CHECK(!r->resolve(s).ok, "file 不存在 → Err");
    }
    // secret_ref 内联形式
    {
        AuthSpec s;
        s.kind = AuthKind::Bearer;
        s.source = SecretSource::SecretRef;
        s.secret_ref = "env:RA2A_TEST_TOKEN";
        auto c = r->resolve(s);
        CHECK(c.ok && c.value->to_headers().at("Authorization") ==
                          "Bearer env-secret-123", "secret_ref env: 形式");
    }
    // secret_ref 未支持的 scheme → 显式报错（不静默返回空）
    {
        AuthSpec s;
        s.kind = AuthKind::Bearer;
        s.source = SecretSource::SecretRef;
        s.secret_ref = "vault://a2a/x";
        auto c = r->resolve(s);
        CHECK(!c.ok, "vault:// 显式报错");
        CHECK(c.error.find("暂不支持") != std::string::npos, "错误信息明确");
    }
    // Basic 走 password_* 来源
    {
        ::setenv("RA2A_TEST_PW", "pw1", 1);
        AuthSpec s;
        s.kind = AuthKind::Basic;
        s.username = "u";
        s.password_source = SecretSource::Env;
        s.password_env = "RA2A_TEST_PW";
        auto c = r->resolve(s);
        CHECK(c.ok, "basic 读取成功");
        CHECK(!c.value->to_headers().at("Authorization").empty(), "basic 头非空");
        CHECK(r->precheck(s).ok, "basic precheck 通过");
    }
    // none 无需秘密
    {
        AuthSpec s;
        s.kind = AuthKind::None;
        CHECK(r->resolve(s).ok, "none resolve 成功");
        CHECK(r->precheck(s).ok, "none precheck 通过");
    }
    // 一期不支持的 kind
    {
        AuthSpec s;
        s.kind = AuthKind::OAuth2;
        CHECK(!r->resolve(s).ok, "oauth2 resolve 失败");
        CHECK(!r->precheck(s).ok, "oauth2 precheck 失败");
    }
}

// ============================================================================
int main() {
    std::cout << "=== Remote A2A 鉴权层测试（M3）===" << std::endl;

    test_reject_plaintext();
    test_parse_each_kind();
    test_parse_sources_and_extras();
    test_card_scheme_inference();
    test_auth_consistency();
    test_headers_mapping();
    test_base64();
    test_secret_reading();

    std::cout << "\n===============================" << std::endl;
    std::cout << "PASS: " << g_pass << "  FAIL: " << g_fail << std::endl;
    return g_fail == 0 ? 0 : 1;
}
