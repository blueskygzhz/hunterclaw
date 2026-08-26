// ============================================================================
// remote_a2a/credential_resolver.cc
// 凭据解析实现。映射规则対标 ADK auth/_auth_headers.py:30-110。
// ============================================================================

#include "hunterclaw/remote_a2a/credential_resolver.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <utility>

namespace hunterclaw {
namespace remote_a2a {

namespace {

const char kB64Table[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64_encode(const std::string& in) {
    std::string out;
    out.reserve(((in.size() + 2) / 3) * 4);
    size_t i = 0;
    while (i + 2 < in.size()) {
        const uint32_t v = (static_cast<unsigned char>(in[i]) << 16) |
                           (static_cast<unsigned char>(in[i + 1]) << 8) |
                           static_cast<unsigned char>(in[i + 2]);
        out.push_back(kB64Table[(v >> 18) & 0x3F]);
        out.push_back(kB64Table[(v >> 12) & 0x3F]);
        out.push_back(kB64Table[(v >> 6) & 0x3F]);
        out.push_back(kB64Table[v & 0x3F]);
        i += 3;
    }
    const size_t rem = in.size() - i;
    if (rem == 1) {
        const uint32_t v = static_cast<unsigned char>(in[i]) << 16;
        out.push_back(kB64Table[(v >> 18) & 0x3F]);
        out.push_back(kB64Table[(v >> 12) & 0x3F]);
        out.push_back('=');
        out.push_back('=');
    } else if (rem == 2) {
        const uint32_t v = (static_cast<unsigned char>(in[i]) << 16) |
                           (static_cast<unsigned char>(in[i + 1]) << 8);
        out.push_back(kB64Table[(v >> 18) & 0x3F]);
        out.push_back(kB64Table[(v >> 12) & 0x3F]);
        out.push_back(kB64Table[(v >> 6) & 0x3F]);
        out.push_back('=');
    }
    return out;
}

/// 去尾部空白与换行（秘密文件常带尾随 \n）
std::string trim_secret(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ' ||
                          s.back() == '\t')) {
        s.pop_back();
    }
    size_t b = 0;
    while (b < s.size() && (s[b] == ' ' || s[b] == '\t')) ++b;
    return s.substr(b);
}

void wipe_string(std::string& s) {
    if (s.empty()) return;
    volatile char* p = const_cast<volatile char*>(s.data());
    for (size_t i = 0; i < s.size(); ++i) p[i] = 0;
    s.clear();
    s.shrink_to_fit();
}

}  // namespace

// ── 文件读取 ────────────────────────────────────────────────────────────────

core::Result<std::string> read_secret_file_trimmed(const std::string& path) {
    if (path.empty()) {
        return core::Result<std::string>::Err("秘密文件路径为空");
    }
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) {
        return core::Result<std::string>::Err("无法打开秘密文件: " + path);
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    std::string v = trim_secret(ss.str());
    if (v.empty()) {
        return core::Result<std::string>::Err("秘密文件内容为空: " + path);
    }
    return core::Result<std::string>::Ok(std::move(v));
}

// ── ResolvedCredential ──────────────────────────────────────────────────────

ResolvedCredential::~ResolvedCredential() { secure_zero(); }

ResolvedCredential::ResolvedCredential(ResolvedCredential&& o) noexcept
    : kind_(o.kind_),
      token_(std::move(o.token_)),
      username_(std::move(o.username_)),
      password_(std::move(o.password_)),
      header_name_(std::move(o.header_name_)),
      scheme_name_(std::move(o.scheme_name_)),
      extra_(std::move(o.extra_)),
      valid_(o.valid_),
      err_(std::move(o.err_)) {
    o.kind_ = AuthKind::None;
    o.valid_ = false;
}

ResolvedCredential& ResolvedCredential::operator=(ResolvedCredential&& o) noexcept {
    if (this != &o) {
        secure_zero();
        kind_ = o.kind_;
        token_ = std::move(o.token_);
        username_ = std::move(o.username_);
        password_ = std::move(o.password_);
        header_name_ = std::move(o.header_name_);
        scheme_name_ = std::move(o.scheme_name_);
        extra_ = std::move(o.extra_);
        valid_ = o.valid_;
        err_ = std::move(o.err_);
        o.kind_ = AuthKind::None;
        o.valid_ = false;
    }
    return *this;
}

void ResolvedCredential::secure_zero() {
    wipe_string(token_);
    wipe_string(username_);
    wipe_string(password_);
}

std::map<std::string, std::string> ResolvedCredential::to_headers() const {
    std::map<std::string, std::string> h;
    if (!valid_) return h;

    switch (kind_) {
        case AuthKind::Bearer:
        case AuthKind::OAuth2:
            // ★★ 対标 _auth_headers.py:52-55 的判空注释：
            //   "A failed exchange returns the credential with no access token,
            //    and without this the header is the literal string 'Bearer None'."
            //   发出 "Bearer " 或 "Bearer None" 会让远端返回 401 而非 400，
            //   排查时看到的是"鉴权失败"而真实原因是"凭据没读到"。
            if (token_.empty()) break;
            h["Authorization"] = "Bearer " + token_;
            break;

        case AuthKind::Basic:
            if (username_.empty() || password_.empty()) break;
            h["Authorization"] = "Basic " + base64_encode(username_ + ":" + password_);
            break;

        case AuthKind::ApiKey:
            if (token_.empty() || header_name_.empty()) break;
            h[header_name_] = token_;
            break;

        case AuthKind::Custom:
            if (token_.empty() || scheme_name_.empty()) break;
            h["Authorization"] = scheme_name_ + " " + token_;
            break;

        case AuthKind::None:
        case AuthKind::Mtls:
            break;
    }

    // additional_headers（parse 时已禁止覆盖 Authorization）
    for (const auto& kv : extra_) h[kv.first] = kv.second;
    return h;
}

void ResolvedCredential::add_extra(const std::map<std::string, std::string>& h) {
    for (const auto& kv : h) extra_[kv.first] = kv.second;
}

ResolvedCredential ResolvedCredential::make_none() {
    ResolvedCredential c;
    c.kind_ = AuthKind::None;
    c.valid_ = true;
    return c;
}

ResolvedCredential ResolvedCredential::make_bearer(std::string token) {
    ResolvedCredential c;
    c.kind_ = AuthKind::Bearer;
    c.token_ = std::move(token);
    c.valid_ = true;
    return c;
}

ResolvedCredential ResolvedCredential::make_api_key(std::string header,
                                                    std::string token) {
    ResolvedCredential c;
    c.kind_ = AuthKind::ApiKey;
    c.header_name_ = std::move(header);
    c.token_ = std::move(token);
    c.valid_ = true;
    return c;
}

ResolvedCredential ResolvedCredential::make_basic(std::string user, std::string pass) {
    ResolvedCredential c;
    c.kind_ = AuthKind::Basic;
    c.username_ = std::move(user);
    c.password_ = std::move(pass);
    c.valid_ = true;
    return c;
}

ResolvedCredential ResolvedCredential::make_custom(std::string scheme,
                                                   std::string token) {
    ResolvedCredential c;
    c.kind_ = AuthKind::Custom;
    c.scheme_name_ = std::move(scheme);
    c.token_ = std::move(token);
    c.valid_ = true;
    return c;
}

ResolvedCredential ResolvedCredential::make_error(std::string e) {
    ResolvedCredential c;
    c.valid_ = false;
    c.err_ = std::move(e);
    return c;
}

// ── CredentialResolver ──────────────────────────────────────────────────────

std::shared_ptr<CredentialResolver> CredentialResolver::create() {
    return std::make_shared<CredentialResolver>();
}

core::Result<std::string> CredentialResolver::read_secret(
    SecretSource src, const std::string& env_name, const std::string& file_path,
    const std::string& secret_ref) {
    switch (src) {
        case SecretSource::Env: {
            if (env_name.empty()) {
                return core::Result<std::string>::Err("环境变量名为空");
            }
            const char* v = std::getenv(env_name.c_str());
            if (v == nullptr || *v == '\0') {
                return core::Result<std::string>::Err("环境变量未设置或为空: " +
                                                     env_name);
            }
            return core::Result<std::string>::Ok(trim_secret(v));
        }
        case SecretSource::File:
            return read_secret_file_trimmed(file_path);
        case SecretSource::SecretRef:
            // TODO(二期): 接入 gateway/secret_ref.h 的解析。
            // 一期先支持 "env:NAME" 与 "file:PATH" 两种内联形式，
            // 其余（如 vault://）显式报错而非静默返回空。
            if (secret_ref.rfind("env:", 0) == 0) {
                return read_secret(SecretSource::Env, secret_ref.substr(4), {}, {});
            }
            if (secret_ref.rfind("file:", 0) == 0) {
                return read_secret_file_trimmed(secret_ref.substr(5));
            }
            return core::Result<std::string>::Err(
                "secret_ref '" + secret_ref +
                "' 暂不支持（一期仅支持 env:NAME / file:PATH）");
    }
    return core::Result<std::string>::Err("未知 SecretSource");
}

core::Result<ResolvedCredential> CredentialResolver::resolve(const AuthSpec& spec) {
    switch (spec.kind) {
        case AuthKind::None: {
            auto c = ResolvedCredential::make_none();
            c.add_extra(spec.additional_headers);
            return core::Result<ResolvedCredential>::Ok(std::move(c));
        }

        case AuthKind::Bearer: {
            auto t = read_secret(spec.source, spec.env_name, spec.file_path,
                                 spec.secret_ref);
            if (!t.ok) return core::Result<ResolvedCredential>::Err(t.error);
            auto c = ResolvedCredential::make_bearer(std::move(*t.value));
            c.add_extra(spec.additional_headers);
            return core::Result<ResolvedCredential>::Ok(std::move(c));
        }

        case AuthKind::ApiKey: {
            auto t = read_secret(spec.source, spec.env_name, spec.file_path,
                                 spec.secret_ref);
            if (!t.ok) return core::Result<ResolvedCredential>::Err(t.error);
            auto c = ResolvedCredential::make_api_key(spec.header_name,
                                                      std::move(*t.value));
            c.add_extra(spec.additional_headers);
            return core::Result<ResolvedCredential>::Ok(std::move(c));
        }

        case AuthKind::Basic: {
            auto p = read_secret(spec.password_source, spec.password_env,
                                 spec.password_file, spec.password_ref);
            if (!p.ok) return core::Result<ResolvedCredential>::Err(p.error);
            auto c = ResolvedCredential::make_basic(spec.username, std::move(*p.value));
            c.add_extra(spec.additional_headers);
            return core::Result<ResolvedCredential>::Ok(std::move(c));
        }

        case AuthKind::Custom: {
            auto t = read_secret(spec.source, spec.env_name, spec.file_path,
                                 spec.secret_ref);
            if (!t.ok) return core::Result<ResolvedCredential>::Err(t.error);
            auto c = ResolvedCredential::make_custom(spec.scheme_name,
                                                     std::move(*t.value));
            c.add_extra(spec.additional_headers);
            return core::Result<ResolvedCredential>::Ok(std::move(c));
        }

        case AuthKind::OAuth2:
            return core::Result<ResolvedCredential>::Err(
                "oauth2 一期不支持（二期复用 mcp_oauth_*）");
        case AuthKind::Mtls:
            return core::Result<ResolvedCredential>::Err("mtls 暂不支持");
    }
    return core::Result<ResolvedCredential>::Err("未知 AuthKind");
}

core::Result<bool> CredentialResolver::precheck(const AuthSpec& spec) {
    if (spec.kind == AuthKind::None) return core::Result<bool>::Ok(true);
    if (spec.kind == AuthKind::OAuth2 || spec.kind == AuthKind::Mtls) {
        return core::Result<bool>::Err(std::string(auth_kind_text(spec.kind)) +
                                      " 一期不支持");
    }

    if (spec.kind == AuthKind::Basic) {
        auto p = read_secret(spec.password_source, spec.password_env,
                             spec.password_file, spec.password_ref);
        if (!p.ok) return core::Result<bool>::Err(p.error);
        wipe_string(*p.value);
        return core::Result<bool>::Ok(true);
    }

    auto t = read_secret(spec.source, spec.env_name, spec.file_path, spec.secret_ref);
    if (!t.ok) return core::Result<bool>::Err(t.error);
    wipe_string(*t.value);
    return core::Result<bool>::Ok(true);
}

core::Result<std::string> CredentialResolver::read_secret_for_redaction(
    const AuthSpec& spec) {
    if (spec.kind == AuthKind::None) {
        return core::Result<std::string>::Err("无凭据");
    }
    if (spec.kind == AuthKind::Basic) {
        return read_secret(spec.password_source, spec.password_env, spec.password_file,
                           spec.password_ref);
    }
    return read_secret(spec.source, spec.env_name, spec.file_path, spec.secret_ref);
}

}  // namespace remote_a2a
}  // namespace hunterclaw
