// ============================================================================
// acp_conversation_id.cc
// 🎯 严格对齐 OpenClaw: src/acp/conversation-id.ts
// ============================================================================

#include "hunterclaw/acp/acp_conversation_id.h"
#include <openssl/sha.h>
#include <algorithm>
#include <cstring>

namespace hunterclaw {
namespace acp {

// ── normalize_conversation_id ────────────────────────────────────────────────
// 🎯 对齐 conversation-id.ts: normalizeConversationId(id)
// 剥离已知平台前缀:
//   cursor: → 去除
//   windsurf: → 去除
//   codex: → 去除
//   acp: → 去除
// 返回纯 session ID，无效输入返回 nullopt。
std::optional<std::string> normalize_conversation_id(const std::string& id) {
    if (id.empty()) return std::nullopt;

    std::string result = id;

    // 平台前缀剥离（按优先级，只剥离一层）
    static const char* prefixes[] = {
        "cursor:", "windsurf:", "codex:", "acp:", nullptr
    };

    for (const char** p = prefixes; *p; ++p) {
        size_t plen = std::strlen(*p);
        if (result.size() > plen &&
            result.compare(0, plen, *p) == 0) {
            result = result.substr(plen);
            break;
        }
    }

    // 剥离后若非空且非空白 → 有效
    if (result.empty()) return std::nullopt;
    if (result.find_first_not_of(" \t\n\r") == std::string::npos) {
        return std::nullopt;
    }

    return result;
}

// ── conversation_id_from_metadata ────────────────────────────────────────────
// 🎯 对齐 conversation-id.ts: conversationIdFromMetadata(meta)
std::optional<std::string> conversation_id_from_metadata(
    const nlohmann::json& meta) {

    if (meta.is_null() || !meta.is_object()) return std::nullopt;

    // 查找 conversationId 字段（多种命名风格）
    for (auto& k : {"conversationId", "conversation_id", "conversationID"}) {
        if (meta.contains(k) && meta[k].is_string()) {
            std::string cid = meta[k].get<std::string>();
            return normalize_conversation_id(cid);
        }
    }

    // 检查 sessionKey 是否带 acp: 前缀
    for (auto& k : {"sessionKey", "session", "key"}) {
        if (meta.contains(k) && meta[k].is_string()) {
            std::string sk = meta[k].get<std::string>();
            if (sk.size() > 4 && sk.compare(0, 4, "acp:") == 0) {
                return normalize_conversation_id(sk);
            }
        }
    }

    return std::nullopt;
}

// ── generate_deterministic_conversation_id ──────────────────────────────────
// 🎯 对齐 conversation-id.ts: generateDeterministicConversationId(sessionKey)
// SHA-256(session_key) → 取前 16 hex chars → "acp:<hex16>"
// 对齐 OC: sessionKey 加盐 "openclaw-acp-conversation" 前缀
std::string generate_deterministic_conversation_id(
    const std::string& session_key) {

    static const std::string salt = "openclaw-acp-conversation:";
    std::string input = salt + session_key;

    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(input.data()),
           input.size(), hash);

    // 取前 16 字符 hex → "acp:<hex16>"
    char hex[17];
    for (int i = 0; i < 8; ++i) {
        snprintf(hex + 2 * i, 3, "%02x", hash[i]);
    }
    hex[16] = '\0';

    return std::string("acp:") + hex;
}

} // namespace acp
} // namespace hunterclaw
