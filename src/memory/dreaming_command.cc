#include "hunterclaw/memory/dreaming_command.h"
#include <algorithm>
#include <sstream>

// 严格对齐 OpenClaw extensions/memory-core/src/dreaming-command.ts (128 行)

namespace hunterclaw {
namespace memory {

// (对齐 dreaming-command.ts:37-39)
static std::string format_enabled(bool value) {
    return value ? "on" : "off";
}

// (对齐 dreaming-command.ts:41-47)
static std::string format_phase_guide() {
    return "- implementation detail: each sweep runs light -> REM -> deep.\n"
           "- deep is the only stage that writes durable entries to MEMORY.md.\n"
           "- DREAMS.md is for human-readable dreaming summaries and diary entries.";
}

// (对齐 dreaming-command.ts:49-64)
static std::string format_status(const DreamingConfigAccess& config) {
    std::ostringstream oss;
    oss << "Dreaming status:\n";
    oss << "- enabled: " << format_enabled(config.is_enabled());
    auto tz = config.get_timezone();
    if (tz) oss << " (" << *tz << ")";
    oss << "\n";
    oss << "- sweep cadence: " << config.get_cron() << "\n";
    oss << "- promotion policy: score>=" << config.get_min_score()
        << ", recalls>=" << config.get_min_recall_count()
        << ", uniqueQueries>=" << config.get_min_unique_queries();
    return oss.str();
}

// (对齐 dreaming-command.ts:66-76)
static std::string format_usage(const std::string& include_status) {
    return "Usage: /dreaming status\n"
           "Usage: /dreaming on|off\n\n" +
           include_status + "\n\n"
           "Phases:\n" +
           format_phase_guide();
}

// (对齐 dreaming-command.ts:78-80)
static bool requires_admin_to_mutate(const std::optional<std::string>& scopes) {
    if (!scopes) return false;
    return scopes->find("operator.admin") == std::string::npos;
}

// (对齐 dreaming-command.ts:82-127)
DreamingCommandResult handle_dreaming_command(
    const DreamingCommandContext& ctx,
    const DreamingConfigAccess& config
) {
    // Parse first token
    std::string args = ctx.args;
    // Trim
    auto s = args.find_first_not_of(" \t\n\r");
    if (s != std::string::npos) args = args.substr(s);
    auto e = args.find_last_not_of(" \t\n\r");
    if (e != std::string::npos) args = args.substr(0, e + 1);

    // Extract first token, lowercase
    std::string first_token;
    auto space = args.find_first_of(" \t");
    first_token = (space != std::string::npos) ? args.substr(0, space) : args;
    std::transform(first_token.begin(), first_token.end(), first_token.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    std::string status = format_status(config);

    // (对齐:95-102) help/empty/options/phases
    if (first_token.empty() || first_token == "help" || first_token == "options" || first_token == "phases") {
        return {format_usage(status)};
    }

    // (对齐:104-106) status
    if (first_token == "status") {
        return {status};
    }

    // (对齐:108-122) on/off
    if (first_token == "on" || first_token == "off") {
        if (requires_admin_to_mutate(ctx.gateway_client_scopes)) {
            return {"⚠️ /dreaming on|off requires operator.admin for gateway clients."};
        }
        bool enabled = (first_token == "on");
        config.set_enabled(enabled);
        std::string new_status = format_status(config);
        return {
            std::string("Dreaming ") + (enabled ? "enabled" : "disabled") + ".\n\n" + new_status
        };
    }

    // (对齐:124) fallback
    return {format_usage(status)};
}

}  // namespace memory
}  // namespace hunterclaw
