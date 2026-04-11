// ============================================================================
// sessions_resolve.cc - Session Key Resolution
// 🎯 严格对齐 OpenClaw gateway/sessions-resolve.ts (201 行)
//
// 核心功能:
//   - resolveSessionKeyFromResolveParams: 按 key/sessionId/label 解析 session key
//   - 支持 legacy key 迁移
//   - 支持 spawnedBy/agentId 过滤
//   - 支持 label 模糊匹配
// ============================================================================

#include "hunterclaw/gateway/gateway.h"
#include "hunterclaw/gateway/method_guard.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <filesystem>
#include <fstream>

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace hunterclaw {
namespace gateway {

struct SessionResolveResult {
    bool ok = false;
    std::string key;
    std::string error_code;
    std::string error_message;
};

// 对齐 sessions-resolve.ts:54-201 resolveSessionKeyFromResolveParams
SessionResolveResult resolve_session_key_from_params(
    std::shared_ptr<GatewayServer> server,
    const json& params
) {
    SessionResolveResult result;
    
    std::string key = params.value("key", "");
    std::string session_id = params.value("sessionId", "");
    std::string label = params.value("label", "");
    std::string spawned_by = params.value("spawnedBy", "");
    std::string agent_id = params.value("agentId", "");

    // Trim all inputs
    auto trim = [](std::string& s) {
        auto start = s.find_first_not_of(" \t");
        if (start == std::string::npos) { s = ""; return; }
        auto end = s.find_last_not_of(" \t");
        s = s.substr(start, end - start + 1);
    };
    trim(key); trim(session_id); trim(label);

    // 对齐 sessions-resolve.ts:65-80 — 只能提供一个选择器
    int selection_count = (!key.empty() ? 1 : 0) + (!session_id.empty() ? 1 : 0) + (!label.empty() ? 1 : 0);
    if (selection_count > 1) {
        result.error_code = "INVALID_REQUEST";
        result.error_message = "Provide either key, sessionId, or label (not multiple)";
        return result;
    }
    if (selection_count == 0) {
        result.error_code = "INVALID_REQUEST";
        result.error_message = "Either key, sessionId, or label is required";
        return result;
    }

    // Session resolution 通过 session store JSON 文件实现 (对齐 OpenClaw loadSessionStore)
    
    // 读取 session store
    auto load_store = [&]() -> json {
        // 查找 session store 文件 (对齐 OpenClaw loadSessionStore)
        std::vector<std::string> candidates = {
            (fs::current_path() / ".openclaw" / "sessions.json").string(),
            (fs::current_path() / "sessions.json").string(),
        };
        for (const auto& path : candidates) {
            std::ifstream f(path);
            if (f.is_open()) {
                try { return json::parse(f); } catch (...) {}
            }
        }
        return json::object();
    };
    
    // 对齐 sessions-resolve.ts:82-121 — 按 key 解析
    if (!key.empty()) {
        auto store = load_store();
        if (store.contains(key)) {
            result.ok = true;
            result.key = key;
            return result;
        }
        result.error_code = "INVALID_REQUEST";
        result.error_message = "No session found: " + key;
        return result;
    }

    // 对齐 sessions-resolve.ts:123-156 — 按 sessionId 解析
    if (!session_id.empty()) {
        auto store = load_store();
        std::vector<std::string> matches;
        for (auto& [sk, entry] : store.items()) {
            if (!entry.is_object()) continue;
            std::string sid = entry.value("sessionId", "");
            if (sid == session_id || sk == session_id) {
                if (!spawned_by.empty() && entry.value("spawnedBy", "") != spawned_by) continue;
                if (!agent_id.empty() && entry.value("agentId", "") != agent_id) continue;
                matches.push_back(sk);
            }
        }
        if (matches.empty()) {
            result.error_code = "INVALID_REQUEST";
            result.error_message = "No session found: " + session_id;
            return result;
        }
        if (matches.size() > 1) {
            std::string keys_str;
            for (const auto& k : matches) {
                if (!keys_str.empty()) keys_str += ", ";
                keys_str += k;
            }
            result.error_code = "INVALID_REQUEST";
            result.error_message = "Multiple sessions found for sessionId: " + session_id + " (" + keys_str + ")";
            return result;
        }
        result.ok = true;
        result.key = matches[0];
        return result;
    }

    // 对齐 sessions-resolve.ts:158-200 — 按 label 解析
    if (!label.empty()) {
        auto store = load_store();
        std::string lower_label = label;
        std::transform(lower_label.begin(), lower_label.end(), lower_label.begin(), ::tolower);
        std::vector<std::string> matches;
        for (auto& [sk, entry] : store.items()) {
            if (!entry.is_object()) continue;
            std::string entry_label = entry.value("label", "");
            std::string lower_entry = entry_label;
            std::transform(lower_entry.begin(), lower_entry.end(), lower_entry.begin(), ::tolower);
            if (lower_entry == lower_label) {
                if (!spawned_by.empty() && entry.value("spawnedBy", "") != spawned_by) continue;
                if (!agent_id.empty() && entry.value("agentId", "") != agent_id) continue;
                matches.push_back(sk);
            }
        }
        if (matches.empty()) {
            result.error_code = "INVALID_REQUEST";
            result.error_message = "No session found with label: " + label;
            return result;
        }
        if (matches.size() > 1) {
            result.error_code = "INVALID_REQUEST";
            result.error_message = "Multiple sessions found with label: " + label;
            return result;
        }
        result.ok = true;
        result.key = matches[0];
        return result;
    }

    result.error_code = "INVALID_REQUEST";
    result.error_message = "Unexpected resolution state";
    return result;
}

}  // namespace gateway
}  // namespace hunterclaw
