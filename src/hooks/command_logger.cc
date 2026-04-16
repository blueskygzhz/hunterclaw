// ✅ 严格对齐 OpenClaw: src/hooks/bundled/command-logger/handler.ts (70行)
// 命令审计日志 Hook 实现

#include "hunterclaw/hooks/command_logger.h"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <cstdlib>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace hunterclaw {
namespace hooks {

// ============================================================================
// Helper — timestamp to ISO 8601
// ============================================================================

static std::string to_iso8601(const std::chrono::system_clock::time_point& tp) {
    auto tt = std::chrono::system_clock::to_time_t(tp);
    std::tm tm = *std::gmtime(&tt);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        tp.time_since_epoch()) % 1000;
    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%S")
        << '.' << std::setfill('0') << std::setw(3) << ms.count() << 'Z';
    return oss.str();
}

// ============================================================================
// resolveCommandLogDir — 对齐 OpenClaw: resolveStateDir + "logs"
// ============================================================================

std::string resolve_command_log_dir() {
    // ✅ 对齐 OpenClaw: resolveStateDir(process.env, os.homedir)
    std::string home;
    const char* home_env = std::getenv("HOME");
    if (home_env) {
        home = home_env;
    } else {
        home = "/tmp";
    }

    // ✅ 对齐 OpenClaw: path.join(stateDir, "logs")
    std::string state_dir = home + "/.hunterclaw";
    return state_dir + "/logs";
}

// ============================================================================
// logCommandEvent — 严格对齐 OpenClaw: logCommand handler (L39-67)
// ============================================================================

void log_command_event(const CommandEvent& event) {
    // ✅ 对齐 OpenClaw: Only trigger on command events (L41-43)
    if (event.type != "command") {
        return;
    }

    try {
        // ✅ 对齐 OpenClaw: Create log directory (L47-49)
        std::string log_dir = resolve_command_log_dir();
        fs::create_directories(log_dir);

        // ✅ 对齐 OpenClaw: Append to command log file (L51-60)
        std::string log_file = log_dir + "/commands.log";

        json log_line = {
            {"timestamp", to_iso8601(event.timestamp)},
            {"action", event.action},
            {"sessionKey", event.session_key},
            {"senderId", event.sender_id.empty() ? "unknown" : event.sender_id},
            {"source", event.command_source.empty() ? "unknown" : event.command_source}
        };

        // ✅ 对齐 OpenClaw: fs.appendFile (L62)
        std::ofstream ofs(log_file, std::ios::app);
        if (ofs.is_open()) {
            ofs << log_line.dump() << "\n";
        }
    } catch (...) {
        // ✅ 对齐 OpenClaw: Silent failure (L63-66)
        // 故障隔离: 日志写入失败不影响主流程
    }
}

}  // namespace hooks
}  // namespace hunterclaw
