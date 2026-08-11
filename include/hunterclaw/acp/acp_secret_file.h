#pragma once

// ============================================================================
// acp_secret_file.h
// 🎯 严格对齐 OpenClaw: src/acp/secret-file.ts
//
// 职责:
//   - 读取 ACP secret file（~/.openclaw/acp-secret 或环境变量）
//   - 基于 agent 身份 + auth token 推导预期 secret
//   - 用于 ACP server→client 的身份验证
// ============================================================================

#include <string>
#include <optional>

namespace hunterclaw {
namespace acp {

// ── ACP Secret File ───────────────────────────────────────────────────────────

// 🎯 对齐 secret-file.ts: readSecretFile()
// 读取顺序:
//   HUNTERCLAW_ACP_SECRET 环境变量
//   -> ~/.openclaw/acp-secret
//   -> 文件不存在返回 nullopt
std::optional<std::string> read_secret_file();

// 🎯 对齐 secret-file.ts: deriveSecretKey(agentName, token)
// 基于 agent 名称 + 认证 token 用 SHA-256 派生 secret。
// 用于 ACP client 端验证 server 身份。
std::string derive_secret_key(
    const std::string& agent_name,
    const std::string& auth_token);

// 🎯 对齐 secret-file.ts: getSecretFilePath()
// 返回默认 secret 文件路径（~/.openclaw/acp-secret）
std::string get_secret_file_path();

} // namespace acp
} // namespace hunterclaw
