#pragma once

// ============================================================================
// remote_a2a/credential_resolver.h
// 凭据解析：AuthSpec + 来源 → HTTP headers。
//
// 対标 ADK 的 build_auth_headers（auth/_auth_headers.py:30-110），
// 映射规则严格照抄（含那个关键的空 token 判空）。
//
// ── 安全约束 ────────────────────────────────────────────────────────────────
// ① ResolvedCredential 禁拷贝（避免凭据在内存中扩散）
// ② 析构时 volatile 写零擦除
// ③ 绝不 log 凭据内容；启动时把凭据注册进日志脱敏表
// ============================================================================

#include <map>
#include <memory>
#include <mutex>
#include <string>

#include "hunterclaw/core/types.h"
#include "hunterclaw/remote_a2a/auth.h"

namespace hunterclaw {
namespace remote_a2a {

/// 已解析的凭据。★ 只在栈上短暂存在，不缓存、不落盘。
class ResolvedCredential {
public:
    ResolvedCredential() = default;
    ~ResolvedCredential();

    // ★ 禁拷贝：避免凭据在内存中扩散
    ResolvedCredential(const ResolvedCredential&) = delete;
    ResolvedCredential& operator=(const ResolvedCredential&) = delete;
    ResolvedCredential(ResolvedCredential&&) noexcept;
    ResolvedCredential& operator=(ResolvedCredential&&) noexcept;

    /// 対标 ADK build_auth_headers。
    /// ★ 任一必需字段为空 → 【不产出】对应 header（而非产出 "Bearer "）。
    std::map<std::string, std::string> to_headers() const;

    bool valid() const { return valid_; }
    const std::string& error() const { return err_; }
    AuthKind kind() const { return kind_; }

    static ResolvedCredential make_none();
    static ResolvedCredential make_bearer(std::string token);
    static ResolvedCredential make_api_key(std::string header, std::string token);
    static ResolvedCredential make_basic(std::string user, std::string pass);
    static ResolvedCredential make_custom(std::string scheme, std::string token);
    static ResolvedCredential make_error(std::string e);

    /// 追加 additional_headers
    void add_extra(const std::map<std::string, std::string>& h);

private:
    void secure_zero();

    AuthKind kind_ = AuthKind::None;
    std::string token_;
    std::string username_;
    std::string password_;
    std::string header_name_;
    std::string scheme_name_;
    std::map<std::string, std::string> extra_;
    bool valid_ = false;
    std::string err_;
};

/// 从环境变量 / 文件 / secret_ref 读取秘密。
class CredentialResolver {
public:
    static std::shared_ptr<CredentialResolver> create();

    /// 解析凭据。kind == None 时返回一个 valid 的空凭据。
    core::Result<ResolvedCredential> resolve(const AuthSpec&);

    /// 启动期预检：来源是否可读。★ 不返回凭据内容，只返回能否读到。
    core::Result<bool> precheck(const AuthSpec&);

    /// 读取秘密原文（供日志脱敏注册用）。★ 调用方必须立即使用后丢弃。
    core::Result<std::string> read_secret_for_redaction(const AuthSpec&);

private:
    core::Result<std::string> read_secret(SecretSource src, const std::string& env_name,
                                          const std::string& file_path,
                                          const std::string& secret_ref);
};

/// 文件读取（去尾部换行）。対标 acp_secret_file 的读法。
core::Result<std::string> read_secret_file_trimmed(const std::string& path);

}  // namespace remote_a2a
}  // namespace hunterclaw
