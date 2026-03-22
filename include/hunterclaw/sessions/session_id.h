#ifndef HUNTERCLAW_SESSIONS_SESSION_ID_H
#define HUNTERCLAW_SESSIONS_SESSION_ID_H

#include <string>
#include <regex>

namespace hunterclaw {
namespace sessions {

// UUID格式正则 (对齐 OpenClaw: SESSION_ID_RE)
// 格式: xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx (8-4-4-4-12 hex digits)
inline const std::regex SESSION_ID_RE{
    "^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}$"
};

/**
 * Check if a string looks like a session UUID.
 * Aligned with OpenClaw: looksLikeSessionId()
 */
inline bool looks_like_session_id(const std::string& value) {
    // Trim whitespace
    size_t start = value.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return false;
    size_t end = value.find_last_not_of(" \t\r\n");
    std::string trimmed = value.substr(start, end - start + 1);
    return std::regex_match(trimmed, SESSION_ID_RE);
}

} // namespace sessions
} // namespace hunterclaw

#endif // HUNTERCLAW_SESSIONS_SESSION_ID_H
