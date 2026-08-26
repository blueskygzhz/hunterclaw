#pragma once

// ============================================================================
// remote_a2a/auth.h
// Remote A2A 鉴权配置。
//
// 一期支持：Bearer / ApiKey(header) / Basic / Custom scheme
// 二期支持：OAuth2（复用 mcp_oauth_* 体系）
// 不支持：  mTLS（需扩展 infra::HttpClient）
//
// ── 三条纪律 ────────────────────────────────────────────────────────────────
// ① 明文凭据【禁止】进配置文件 —— 只能 token_env / token_file / secret_ref
// ② apiKey 的 in=query/cookie 【显式失败】，不像 ADK 那样只 warning
//    （_auth_headers.py:97-104 只 logger.warning，导致凭据静默丢弃、
//     请求以无鉴权发出、远端 401，运维看到 401 而非"配置不支持"）
// ③ 空 token 【不产出】 header —— 対标 _auth_headers.py:52-55 的注释：
//    交换失败时 token 为空，不判空会发出字面量 "Bearer None"
// ============================================================================

#include <map>
#include <string>

#include <nlohmann/json.hpp>

#include "hunterclaw/core/types.h"

namespace hunterclaw {
namespace remote_a2a {

enum class AuthKind {
    None,
    Bearer,   // Authorization: Bearer <token>
    ApiKey,   // <header_name>: <token>   ★ 仅 header
    Basic,    // Authorization: Basic base64(user:pass)
    Custom,   // Authorization: <scheme> <token>
    OAuth2,   // 二期
    Mtls,     // 不支持
};

const char* auth_kind_text(AuthKind);

/// ★ 凭据来源。配置里绝不允许明文。
enum class SecretSource {
    Env,        // 环境变量
    File,       // 文件（対标 acp_secret_file 的读法）
    SecretRef,  // gateway/secret_ref.h 的引用
};

struct AuthSpec {
    AuthKind kind = AuthKind::None;
    SecretSource source = SecretSource::Env;

    // 凭据来源（三者取其一）
    std::string env_name;
    std::string file_path;
    std::string secret_ref;

    // ApiKey：必填
    std::string header_name;

    // Basic
    std::string username;
    SecretSource password_source = SecretSource::Env;
    std::string password_env;
    std::string password_file;
    std::string password_ref;

    // Custom
    std::string scheme_name;  // 如 "Token"

    /// 额外请求头（対标 ADK 的 additional_headers）
    std::map<std::string, std::string> additional_headers;

    /// ★ 拉卡时是否也带凭据。
    ///   true → 触发 per-invocation card（authenticated extended card
    ///   的作用域是单个已认证 session，见 A2A 规范 §3.1.1.1）
    bool authenticate_card_request = false;

    bool needs_secret() const {
        return kind == AuthKind::Bearer || kind == AuthKind::ApiKey ||
               kind == AuthKind::Custom || kind == AuthKind::OAuth2;
    }
};

/// 从配置节点解析。
/// ★ 出现明文 token / password / secret / api_key 字段 → 直接 Err。
core::Result<AuthSpec> parse_auth_spec(const nlohmann::json& cfg);

/// 从卡的单个 securityScheme 推断类型（仅用于一致性检查与报错提示）。
/// ★ apiKey 的 in != header → 显式 Err。
core::Result<AuthKind> infer_kind_from_card_scheme(const nlohmann::json& scheme);

/// 卡声明的鉴权方式与本地配置是否兼容。
/// 返回的 vector 为空表示无警告；非空为人类可读的警告文本（不阻断）。
std::vector<std::string> check_auth_consistency(const nlohmann::json& security_schemes,
                                                const AuthSpec& local);

}  // namespace remote_a2a
}  // namespace hunterclaw
