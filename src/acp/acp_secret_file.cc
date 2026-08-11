// ============================================================================
// acp_secret_file.cc
// 🎯 严格对齐 OpenClaw: src/acp/secret-file.ts
// ============================================================================

#include "hunterclaw/acp/acp_secret_file.h"
#include <fstream>
#include <sstream>
#include <cstdlib>
#include <openssl/sha.h>

namespace hunterclaw {
namespace acp {

// ── get_secret_file_path ─────────────────────────────────────────────────────
// 🎯 对齐 secret-file.ts: getSecretFilePath()
// 返回 ~/.openclaw/acp-secret 完整路径
std::string get_secret_file_path() {
    const char* home = std::getenv("HOME");
    if (!home || !*home) {
        // fallback: /tmp
        return "/tmp/.openclaw/acp-secret";
    }
    return std::string(home) + "/.openclaw/acp-secret";
}

// ── read_secret_file ─────────────────────────────────────────────────────────
// 🎯 对齐 secret-file.ts: readSecretFile()
// 读取优先级:
//   1. HUNTERCLAW_ACP_SECRET 环境变量
//   2. ~/.openclaw/acp-secret 文件内容（trim whitespace/newline）
//   3. 文件不存在 → nullopt
std::optional<std::string> read_secret_file() {
    // 1. 环境变量优先
    const char* env = std::getenv("HUNTERCLAW_ACP_SECRET");
    if (env && *env) {
        std::string s(env);
        // trim
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) {
            s.pop_back();
        }
        if (!s.empty()) return s;
    }

    // 2. 文件读取
    std::string path = get_secret_file_path();
    std::ifstream file(path);
    if (!file.is_open()) return std::nullopt;

    std::ostringstream buf;
    buf << file.rdbuf();
    std::string content = buf.str();

    // Trim whitespace and newlines
    while (!content.empty() && (content.back() == '\n' || content.back() == '\r' || content.back() == ' ')) {
        content.pop_back();
    }

    if (content.empty()) return std::nullopt;
    return content;
}

// ── derive_secret_key ────────────────────────────────────────────────────────
// 🎯 对齐 secret-file.ts: deriveSecretKey(agentName, token)
// SHA-256("acp:" + agentName + ":" + token) → hex string
std::string derive_secret_key(
    const std::string& agent_name,
    const std::string& auth_token) {

    std::string input = "acp:" + agent_name + ":" + auth_token;

    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(input.data()),
           input.size(), hash);

    // Convert to hex string
    char hex[2 * SHA256_DIGEST_LENGTH + 1];
    for (int i = 0; i < SHA256_DIGEST_LENGTH; ++i) {
        snprintf(hex + 2 * i, 3, "%02x", hash[i]);
    }
    hex[2 * SHA256_DIGEST_LENGTH] = '\0';

    return std::string(hex);
}

} // namespace acp
} // namespace hunterclaw
