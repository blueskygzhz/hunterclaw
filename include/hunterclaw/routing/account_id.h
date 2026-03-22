#ifndef HUNTERCLAW_ROUTING_ACCOUNT_ID_H
#define HUNTERCLAW_ROUTING_ACCOUNT_ID_H

#include <string>
#include <optional>
#include <algorithm>
#include <regex>
#include <unordered_map>
#include <mutex>

namespace hunterclaw {
namespace routing {

// Default account ID (aligned with OpenClaw: DEFAULT_ACCOUNT_ID)
inline const std::string DEFAULT_ACCOUNT_ID = "default";

/**
 * Normalize an account ID to a safe, canonical form.
 * Aligned with OpenClaw: normalizeAccountId()
 *
 * - Returns DEFAULT_ACCOUNT_ID for empty/null input
 * - Lowercases and collapses invalid chars to "-"
 * - Strips leading/trailing dashes
 * - Truncates to 64 chars
 */
inline std::string normalize_account_id(const std::string& value) {
    std::string trimmed = value;
    size_t s = trimmed.find_first_not_of(" \t\r\n");
    if (s == std::string::npos) return DEFAULT_ACCOUNT_ID;
    size_t e = trimmed.find_last_not_of(" \t\r\n");
    trimmed = trimmed.substr(s, e - s + 1);
    if (trimmed.empty()) return DEFAULT_ACCOUNT_ID;

    static const std::regex valid_id_re{"^[a-zA-Z0-9][a-zA-Z0-9_-]{0,63}$"};
    if (std::regex_match(trimmed, valid_id_re)) {
        std::string lower = trimmed;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
        return lower;
    }

    // Canonicalize: lowercase, replace invalid chars with "-"
    std::string lower = trimmed;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    std::string result;
    for (char c : lower) {
        if (std::isalnum(c) || c == '_' || c == '-') result += c;
        else result += '-';
    }
    // Strip leading/trailing dashes
    size_t rs = result.find_first_not_of('-');
    if (rs == std::string::npos) return DEFAULT_ACCOUNT_ID;
    size_t re = result.find_last_not_of('-');
    result = result.substr(rs, re - rs + 1);
    if (result.size() > 64) result = result.substr(0, 64);
    return result.empty() ? DEFAULT_ACCOUNT_ID : result;
}

/**
 * Normalize an optional account ID.
 * Returns nullopt for empty/invalid input.
 * Aligned with OpenClaw: normalizeOptionalAccountId()
 */
inline std::optional<std::string> normalize_optional_account_id(const std::string& value) {
    std::string trimmed = value;
    size_t s = trimmed.find_first_not_of(" \t\r\n");
    if (s == std::string::npos) return std::nullopt;
    size_t e = trimmed.find_last_not_of(" \t\r\n");
    trimmed = trimmed.substr(s, e - s + 1);
    if (trimmed.empty()) return std::nullopt;

    static const std::regex valid_id_re{"^[a-zA-Z0-9][a-zA-Z0-9_-]{0,63}$"};
    if (std::regex_match(trimmed, valid_id_re)) {
        std::string lower = trimmed;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
        return lower;
    }

    std::string lower = trimmed;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    std::string result;
    for (char c : lower) {
        if (std::isalnum(c) || c == '_' || c == '-') result += c;
        else result += '-';
    }
    size_t rs = result.find_first_not_of('-');
    if (rs == std::string::npos) return std::nullopt;
    size_t re = result.find_last_not_of('-');
    result = result.substr(rs, re - rs + 1);
    if (result.size() > 64) result = result.substr(0, 64);
    return result.empty() ? std::nullopt : std::optional<std::string>{result};
}

} // namespace routing
} // namespace hunterclaw

#endif // HUNTERCLAW_ROUTING_ACCOUNT_ID_H
