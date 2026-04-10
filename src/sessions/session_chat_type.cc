#include "hunterclaw/sessions/session_chat_type.h"
#include <algorithm>
#include <regex>
#include <set>
#include <sstream>

namespace hunterclaw {
namespace sessions {

std::string session_key_chat_type_to_string(SessionKeyChatType type) {
    switch (type) {
        case SessionKeyChatType::DIRECT: return "direct";
        case SessionKeyChatType::GROUP: return "group";
        case SessionKeyChatType::CHANNEL: return "channel";
        case SessionKeyChatType::UNKNOWN: return "unknown";
    }
    return "unknown";
}

std::optional<SessionKeyChatType> string_to_session_key_chat_type(const std::string& str) {
    if (str == "direct") return SessionKeyChatType::DIRECT;
    if (str == "group") return SessionKeyChatType::GROUP;
    if (str == "channel") return SessionKeyChatType::CHANNEL;
    if (str == "unknown") return SessionKeyChatType::UNKNOWN;
    return std::nullopt;
}

namespace {
// Helper: to_lower
std::string to_lower(const std::string& s) {
    std::string r = s;
    std::transform(r.begin(), r.end(), r.begin(), ::tolower);
    return r;
}

// Helper: split by delimiter, skip empty
std::vector<std::string> split_filter(const std::string& s, char delim) {
    std::vector<std::string> result;
    std::istringstream stream(s);
    std::string token;
    while (std::getline(stream, token, delim)) {
        if (!token.empty()) result.push_back(token);
    }
    return result;
}

// 对齐 session-chat-type-shared.ts:6-22: deriveBuiltInLegacySessionChatType
std::optional<SessionKeyChatType> derive_builtin_legacy(const std::string& scoped) {
    // group:xxx
    if (std::regex_match(scoped, std::regex("^group:[^:]+$"))) {
        return SessionKeyChatType::GROUP;
    }
    // WhatsApp group: 123-456@g.us
    if (std::regex_match(scoped, std::regex("^[0-9]+(?:-[0-9]+)*@g\\.us$"))) {
        return SessionKeyChatType::GROUP;
    }
    // whatsapp:xxx@g.us (not containing :group:)
    if (std::regex_match(scoped, std::regex("^whatsapp:(?!.*:group:).+@g\\.us$"))) {
        return SessionKeyChatType::GROUP;
    }
    // discord:xxx:guild-xxx:channel-xxx
    if (std::regex_match(scoped, std::regex("^discord:(?:[^:]+:)?guild-[^:]+:channel-[^:]+$"))) {
        return SessionKeyChatType::CHANNEL;
    }
    return std::nullopt;
}

// 对齐: parseAgentSessionKey — strip "agent:<agentId>:" prefix
std::string strip_agent_prefix(const std::string& key) {
    auto parts = split_filter(key, ':');
    if (parts.size() >= 3 && parts[0] == "agent") {
        std::string rest;
        for (size_t i = 2; i < parts.size(); ++i) {
            if (i > 2) rest += ":";
            rest += parts[i];
        }
        return rest;
    }
    return key;
}
}  // namespace

// 对齐 session-chat-type-shared.ts:24-51
SessionKeyChatType derive_session_chat_type_from_scoped_key(
    const std::string& scoped_session_key,
    const std::vector<LegacyChatTypeDeriver>& legacy_derivers
) {
    auto tokens_vec = split_filter(scoped_session_key, ':');
    std::set<std::string> tokens(tokens_vec.begin(), tokens_vec.end());
    
    if (tokens.count("group")) return SessionKeyChatType::GROUP;
    if (tokens.count("channel")) return SessionKeyChatType::CHANNEL;
    if (tokens.count("direct") || tokens.count("dm")) return SessionKeyChatType::DIRECT;
    
    auto builtin = derive_builtin_legacy(scoped_session_key);
    if (builtin.has_value()) return builtin.value();
    
    for (const auto& deriver : legacy_derivers) {
        auto derived = deriver(scoped_session_key);
        if (derived.has_value()) return derived.value();
    }
    
    return SessionKeyChatType::UNKNOWN;
}

// 对齐 session-chat-type-shared.ts:56-68
SessionKeyChatType derive_session_chat_type_from_key(
    const std::string& session_key,
    const std::vector<LegacyChatTypeDeriver>& legacy_derivers
) {
    auto raw = to_lower(session_key);
    // Trim
    auto start = raw.find_first_not_of(" \t\n\r");
    if (start == std::string::npos) return SessionKeyChatType::UNKNOWN;
    raw = raw.substr(start, raw.find_last_not_of(" \t\n\r") - start + 1);
    if (raw.empty()) return SessionKeyChatType::UNKNOWN;
    
    auto scoped = strip_agent_prefix(raw);
    return derive_session_chat_type_from_scoped_key(scoped, legacy_derivers);
}

}  // namespace sessions
}  // namespace hunterclaw
