// 🎯 严格对齐 OpenClaw: src/agents/sandbox/shared.ts (47 lines)
// 会话/作用域键的 slug 化与解析，工作空间目录路径计算

#include <string>
#include <filesystem>
#include <algorithm>
#include <openssl/sha.h>
#include <sstream>
#include <iomanip>

namespace hunterclaw {
namespace sandbox {

namespace fs = std::filesystem;

static std::string sha256_hex(const std::string& input) {
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(input.c_str()), input.size(), hash);
    std::ostringstream oss;
    for (int i = 0; i < SHA256_DIGEST_LENGTH; ++i)
        oss << std::hex << std::setfill('0') << std::setw(2) << (int)hash[i];
    return oss.str();
}

// 🎯 对齐 OpenClaw: slugifySessionKey
std::string slugify_session_key(const std::string& value) {
    std::string trimmed = value.empty() ? "session" : value;
    // Trim
    size_t s = trimmed.find_first_not_of(" \t\r\n");
    size_t e = trimmed.find_last_not_of(" \t\r\n");
    if (s == std::string::npos) trimmed = "session";
    else trimmed = trimmed.substr(s, e - s + 1);
    if (trimmed.empty()) trimmed = "session";

    std::string hash = sha256_hex(trimmed).substr(0, 8);
    // Make safe slug
    std::string safe;
    for (char c : trimmed) {
        char lower = std::tolower(c);
        if (std::isalnum(lower) || lower == '.' || lower == '_' || lower == '-')
            safe += lower;
        else
            safe += '-';
    }
    // Strip leading/trailing hyphens
    while (!safe.empty() && safe.front() == '-') safe.erase(safe.begin());
    while (!safe.empty() && safe.back() == '-') safe.pop_back();
    if (safe.size() > 32) safe = safe.substr(0, 32);
    if (safe.empty()) safe = "session";

    return safe + "-" + hash;
}

// 🎯 对齐 OpenClaw: resolveSandboxWorkspaceDir
std::string resolve_sandbox_workspace_dir(const std::string& root, const std::string& session_key) {
    std::string slug = slugify_session_key(session_key);
    return (fs::path(root) / slug).string();
}

// 🎯 对齐 OpenClaw: resolveSandboxScopeKey
std::string resolve_sandbox_scope_key(const std::string& scope, const std::string& session_key) {
    std::string trimmed = session_key.empty() ? "main" : session_key;
    if (scope == "shared") return "shared";
    if (scope == "session") return trimmed;
    // scope == "agent": extract agent ID from session key
    auto colon = trimmed.find(':');
    std::string agent_id = (colon != std::string::npos) ? trimmed.substr(0, colon) : trimmed;
    return "agent:" + agent_id;
}

// 🎯 对齐 OpenClaw: resolveSandboxAgentId
std::string resolve_sandbox_agent_id(const std::string& scope_key) {
    if (scope_key.empty() || scope_key == "shared") return "";
    if (scope_key.rfind("agent:", 0) == 0 && scope_key.size() > 6) {
        return scope_key.substr(6);
    }
    auto colon = scope_key.find(':');
    return (colon != std::string::npos) ? scope_key.substr(0, colon) : scope_key;
}

}  // namespace sandbox
}  // namespace hunterclaw
