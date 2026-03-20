#include "hunterclaw/memory/session_memory_provider.h"
#include "hunterclaw/memory/session_files.h"
#include <spdlog/spdlog.h>
#include <filesystem>
#include <chrono>

namespace fs = std::filesystem;

namespace hunterclaw {
namespace memory {

namespace {
    auto log() {
        auto logger = spdlog::get("memory");
        if (!logger) {
            logger = spdlog::stdout_logger_mt("memory");
        }
        return logger;
    }
}

// ============================================================================
// SessionMemoryProvider Implementation
// Aligns with openclaw session-files.ts:
//   - listSessionFilesForAgent()
//   - buildSessionEntry()
// Session memory indexing is handled internally by MemoryIndexManager.
// ============================================================================

SessionMemoryProvider::SessionMemoryProvider(
    const std::string& agent_id,
    const SessionMemoryConfig& config
) : agent_id_(agent_id), config_(config) {
    log()->info("SessionMemoryProvider created: agent_id={}, enabled={}",
                agent_id_, config_.enabled);
}

core::Result<std::vector<std::string>> SessionMemoryProvider::list_session_files() const {
    return list_session_files_for_agent(agent_id_);
}

core::Result<SessionFileEntry> SessionMemoryProvider::build_entry(
    const std::string& abs_path
) const {
    return build_session_entry(abs_path);
}

core::Result<void> SessionMemoryProvider::prune_old_sessions() {
    if (!config_.enabled || !config_.retention_days || !config_.export_dir) {
        return core::Result<void>::Ok();
    }

    // Align with openclaw: pruneOldSessionFiles (builtin-manager.ts:1085)
    int retention_days = config_.retention_days.value();
    fs::path export_dir = config_.export_dir.value();

    log()->info("Pruning sessions older than {} days from {}",
                retention_days, export_dir.string());

    auto now = std::chrono::system_clock::now();
    auto cutoff = now - std::chrono::hours(24 * retention_days);
    auto cutoff_time_t = std::chrono::system_clock::to_time_t(cutoff);

    int deleted_count = 0;

    try {
        if (!fs::exists(export_dir)) {
            return core::Result<void>::Ok();
        }

        for (const auto& entry : fs::directory_iterator(export_dir)) {
            if (!entry.is_regular_file()) {
                continue;
            }

            auto ftime = fs::last_write_time(entry);
            auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                ftime - fs::file_time_type::clock::now() + std::chrono::system_clock::now()
            );
            auto ftime_t = std::chrono::system_clock::to_time_t(sctp);

            if (ftime_t < cutoff_time_t) {
                fs::remove(entry);
                deleted_count++;
                log()->debug("Pruned old session file: {}", entry.path().filename().string());
            }
        }

        log()->info("Pruned {} old session files", deleted_count);
        return core::Result<void>::Ok();

    } catch (const std::exception& e) {
        return core::Result<void>::Err(std::string("Session pruning failed: ") + e.what());
    }
}

} // namespace memory
} // namespace hunterclaw
