#ifndef HUNTERCLAW_SESSIONS_SESSION_ID_RESOLUTION_H
#define HUNTERCLAW_SESSIONS_SESSION_ID_RESOLUTION_H

#include <string>
#include <optional>
#include <vector>
#include <utility>
#include <algorithm>
#include "hunterclaw/sessions/session_manager.h"

namespace hunterclaw {
namespace sessions {

/**
 * Resolve the preferred session key when multiple sessions match a session ID.
 * Aligned with OpenClaw: resolvePreferredSessionKeyForSessionIdMatches()
 *
 * Logic:
 * 1. If only one match → return it
 * 2. Try structural match (key ends with :<sessionId> or equals sessionId)
 * 3. If still ambiguous → return the most recently updated session
 * 4. If tie → return nullopt (ambiguous)
 */
inline std::optional<std::string> resolve_preferred_session_key_for_session_id_matches(
    const std::vector<std::pair<std::string, SessionEntry>>& matches,
    const std::string& session_id
) {
    if (matches.empty()) return std::nullopt;
    if (matches.size() == 1) return matches[0].first;

    // Normalize session ID
    std::string lowered_session_id = session_id;
    // Trim
    size_t s = lowered_session_id.find_first_not_of(" \t\r\n");
    if (s == std::string::npos) return std::nullopt;
    size_t e = lowered_session_id.find_last_not_of(" \t\r\n");
    lowered_session_id = lowered_session_id.substr(s, e - s + 1);
    std::transform(lowered_session_id.begin(), lowered_session_id.end(),
                   lowered_session_id.begin(), ::tolower);

    // Try structural matches
    std::vector<std::pair<std::string, SessionEntry>> structural_matches;
    for (const auto& [store_key, entry] : matches) {
        std::string lower_key = store_key;
        std::transform(lower_key.begin(), lower_key.end(), lower_key.begin(), ::tolower);

        // Check if key ends with :<sessionId>
        const std::string suffix = ":" + lowered_session_id;
        bool ends_with_suffix = lower_key.size() >= suffix.size() &&
            lower_key.substr(lower_key.size() - suffix.size()) == suffix;

        // Check if key equals sessionId
        bool equals_session_id = lower_key == lowered_session_id;

        if (ends_with_suffix || equals_session_id) {
            structural_matches.emplace_back(store_key, entry);
        }
    }

    if (structural_matches.size() == 1) {
        return structural_matches[0].first;
    }

    // Sort by updatedAt descending
    auto sorted_matches = matches;
    std::sort(sorted_matches.begin(), sorted_matches.end(),
        [](const auto& a, const auto& b) {
            return a.second.updated_at > b.second.updated_at;
        }
    );

    // Return freshest only if it's strictly newer than the second
    if (sorted_matches.size() >= 2) {
        if (sorted_matches[0].second.updated_at > sorted_matches[1].second.updated_at) {
            return sorted_matches[0].first;
        }
    }

    return std::nullopt;  // Ambiguous
}

} // namespace sessions
} // namespace hunterclaw

#endif // HUNTERCLAW_SESSIONS_SESSION_ID_RESOLUTION_H
