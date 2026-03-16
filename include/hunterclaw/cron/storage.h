#pragma once

#include <string>
#include <vector>
#include <optional>
#include "hunterclaw/cron/types.h"
#include "hunterclaw/core/result.h"
#include <nlohmann/json.hpp>

namespace hunterclaw {
namespace cron {

using json = nlohmann::json;

// Forward declarations
class CronStorage {
public:
    CronStorage(const std::string& base_dir);
    ~CronStorage() = default;
    
    // Jobs storage
    core::Result<std::vector<Job>> load_jobs();
    core::Result<void> save_jobs(const std::vector<Job>& jobs);
    
    // Run logs storage
    core::Result<std::vector<RunLog>> load_run_logs(
        const std::string& job_id,
        std::optional<int64_t> since_ms,
        std::optional<int> limit
    );
    core::Result<void> save_run_log(const RunLog& log_entry);
    
    // Backup operations
    bool backup();
    bool restore(const std::string& backup_name);
    std::vector<std::string> list_backups();
    core::Result<void> cleanup_old_backups(const std::string& prefix, int keep_count);

private:
    std::string base_dir_;
    
    // Serialization helpers
    json serialize_job(const Job& job);
    core::Result<Job> deserialize_job(const json& j);
    json serialize_schedule(const Schedule& sched);
    core::Result<Schedule> deserialize_schedule(const json& j);
};

} // namespace cron
} // namespace hunterclaw
