#ifndef HUNTERCLAW_SESSIONS_SESSION_LABEL_H
#define HUNTERCLAW_SESSIONS_SESSION_LABEL_H

#include <string>
#include <variant>

namespace hunterclaw {
namespace sessions {

// Max length for session labels (aligned with OpenClaw: SESSION_LABEL_MAX_LENGTH = 512)
constexpr size_t SESSION_LABEL_MAX_LENGTH = 512;

// Result type for parseSessionLabel (aligned with OpenClaw ParsedSessionLabel)
struct ParsedSessionLabelOk {
    std::string label;
};

struct ParsedSessionLabelErr {
    std::string error;
};

using ParsedSessionLabel = std::variant<ParsedSessionLabelOk, ParsedSessionLabelErr>;

/**
 * Parse and validate a session label.
 * Aligned with OpenClaw: parseSessionLabel()
 */
inline ParsedSessionLabel parse_session_label(const std::string& raw) {
    // Trim whitespace
    size_t start = raw.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) {
        return ParsedSessionLabelErr{"invalid label: empty"};
    }
    size_t end = raw.find_last_not_of(" \t\r\n");
    std::string trimmed = raw.substr(start, end - start + 1);

    if (trimmed.empty()) {
        return ParsedSessionLabelErr{"invalid label: empty"};
    }
    if (trimmed.size() > SESSION_LABEL_MAX_LENGTH) {
        return ParsedSessionLabelErr{
            "invalid label: too long (max " + std::to_string(SESSION_LABEL_MAX_LENGTH) + ")"
        };
    }
    return ParsedSessionLabelOk{trimmed};
}

} // namespace sessions
} // namespace hunterclaw

#endif // HUNTERCLAW_SESSIONS_SESSION_LABEL_H
