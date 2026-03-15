#include "hunterclaw/cron/storage.h"
#include "hunterclaw/core/utils.h"
#include <spdlog/spdlog.h>
#include <fstream>
#include <filesystem>
#include <nlohmann/json.hpp>

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace hunterclaw {
namespace cron {

namespace {
    auto log() {
        static auto logger = spdlog::get("cron");
        if (!logger) {
            logger = spdlog::stdout_logger_mt("cron");
        }
        return logger;
    }
}

// ============================================================================
// CronStorage Implementation
// ============================================================================

CronStorage::CronStorage(const std::string& base_dir)
    : base_dir_(base_dir) {
    
    // 创建目录结构
    // ~/.hunterclaw/cron/
    // ├── jobs.json
    // ├── run_logs.json
    // └── backups/
    
    fs::create_directories(base_dir_);
    fs::create_directories(base_dir_ + "/backups");
    
    log()->info("CronStorage initialized at: {}", base_dir_);
}

// ============================================================================
// Jobs Storage
// ============================================================================

core::Result<std::vector<Job>> CronStorage::load_jobs() {
    std::string jobs_path = base_dir_ + "/jobs.json";
    
    if (!fs::exists(jobs_path)) {
        log()->info("Jobs file not found, starting with empty job list");
        return core::Result<std::vector<Job>>::Ok({});
    }
    
    try {
        std::ifstream file(jobs_path);
        if (!file.is_open()) {
            return core::Result<std::vector<Job>>::Err(
                "Failed to open jobs file: " + jobs_path
            );
        }
        
        json data;
        file >> data;
        file.close();
        
        if (!data.is_array()) {
            return core::Result<std::vector<Job>>::Err(
                "Invalid jobs file format: expected array"
            );
        }
        
        std::vector<Job> jobs;
        for (const auto& job_json : data) {
            auto job_result = deserialize_job(job_json);
            if (job_result.is_ok()) {
                jobs.push_back(job_result.unwrap());
            } else {
                log()->warn("Failed to deserialize job: {}", job_result.err());
            }
        }
        
        log()->info("Loaded {} jobs from {}", jobs.size(), jobs_path);
        return core::Result<std::vector<Job>>::Ok(std::move(jobs));
        
    } catch (const std::exception& e) {
        return core::Result<std::vector<Job>>::Err(
            std::string("Failed to load jobs: ") + e.what()
        );
    }
}

core::Result<void> CronStorage::save_jobs(const std::vector<Job>& jobs) {
    std::string jobs_path = base_dir_ + "/jobs.json";
    std::string backup_path = base_dir_ + "/backups/jobs_" + 
        std::to_string(std::time(nullptr)) + ".json";
    
    try {
        // 备份现有文件
        if (fs::exists(jobs_path)) {
            fs::copy(jobs_path, backup_path, fs::copy_options::overwrite_existing);
            log()->debug("Backed up jobs to: {}", backup_path);
        }
        
        // 序列化为 JSON
        json data = json::array();
        for (const auto& job : jobs) {
            data.push_back(serialize_job(job));
        }
        
        // 写入文件 (atomic write)
        std::string temp_path = jobs_path + ".tmp";
        std::ofstream file(temp_path);
        if (!file.is_open()) {
            return core::Result<void>::Err(
                "Failed to open temp file: " + temp_path
            );
        }
        
        file << data.dump(2);  // Pretty print with 2 spaces
        file.close();
        
        // Atomic rename
        fs::rename(temp_path, jobs_path);
        
        log()->info("Saved {} jobs to {}", jobs.size(), jobs_path);
        
        // 清理旧备份 (保留最近10个)
        cleanup_old_backups("jobs_", 10);
        
        return core::Result<void>::Ok();
        
    } catch (const std::exception& e) {
        return core::Result<void>::Err(
            std::string("Failed to save jobs: ") + e.what()
        );
    }
}

// ============================================================================
// Run Logs Storage
// ============================================================================

core::Result<std::vector<RunLog>> CronStorage::load_run_logs(
    const std::optional<std::string>& job_id,
    size_t limit,
    size_t offset
) {
    std::string logs_path = base_dir_ + "/run_logs.json";
    
    if (!fs::exists(logs_path)) {
        log()->debug("Run logs file not found, returning empty list");
        return core::Result<std::vector<RunLog>>::Ok({});
    }
    
    try {
        std::ifstream file(logs_path);
        if (!file.is_open()) {
            return core::Result<std::vector<RunLog>>::Err(
                "Failed to open run logs file: " + logs_path
            );
        }
        
        json data;
        file >> data;
        file.close();
        
        if (!data.is_array()) {
            return core::Result<std::vector<RunLog>>::Err(
                "Invalid run logs format: expected array"
            );
        }
        
        std::vector<RunLog> logs;
        for (const auto& log_json : data) {
        }
        
        size_t end = std::min(offset + limit, logs.size());
        std::vector<RunLog> result(
            logs.begin() + offset,
            logs.begin() + end
        );
        
        log()->debug("Loaded {} run logs (offset={}, limit={})",
                    result.size(), offset, limit);
        
        return core::Result<std::vector<RunLog>>::Ok(std::move(result));
        
    } catch (const std::exception& e) {
        return core::Result<std::vector<RunLog>>::Err(
            std::string("Failed to load run logs: ") + e.what()
        );
    }
}

core::Result<void> CronStorage::save_run_log(const RunLog& log_entry) {
    std::string logs_path = base_dir_ + "/run_logs.json";
    
    try {
        // 加载现有日志
        json data = json::array();
        
        if (fs::exists(logs_path)) {
            std::ifstream file(logs_path);
            if (file.is_open()) {
                file >> data;
                file.close();
            }
        }
        
        // 添加新日志
        data.push_back(serialize_run_log(log_entry));
        
        // 限制日志数量 (保留最近1000条)
        const size_t max_logs = 1000;
        if (data.size() > max_logs) {
            // 删除最旧的日志
            data.erase(data.begin(), data.begin() + (data.size() - max_logs));
        }
        
        // 写入文件
        std::string temp_path = logs_path + ".tmp";
        std::ofstream file(temp_path);
        if (!file.is_open()) {
            return core::Result<void>::Err(
                "Failed to open temp file: " + temp_path
            );
        }
        
        file << data.dump(2);
        file.close();
        
        fs::rename(temp_path, logs_path);
        
        log()->debug("Saved run log for job: {}", log_entry.job_id);
        
        return core::Result<void>::Ok();
        
    } catch (const std::exception& e) {
        return core::Result<void>::Err(
            std::string("Failed to save run log: ") + e.what()
        );
    }
}

core::Result<void> CronStorage::cleanup_old_logs(int64_t before_ms) {
    std::string logs_path = base_dir_ + "/run_logs.json";
    
    if (!fs::exists(logs_path)) {
        return core::Result<void>::Ok();  // Nothing to cleanup
    }
    
    try {
        std::ifstream file(logs_path);
        json data;
        file >> data;
        file.close();
        
        // 过滤掉旧日志
        json filtered = json::array();
        size_t removed = 0;
        
        for (const auto& log_json : data) {
            int64_t started_at = log_json.value("started_at_ms", 0LL);
            if (started_at >= before_ms) {
                filtered.push_back(log_json);
            } else {
                removed++;
            }
        }
        
        // 写回文件
        std::ofstream out_file(logs_path);
        out_file << filtered.dump(2);
        out_file.close();
        
        log()->info("Cleaned up {} old run logs (before {})", removed, before_ms);
        
        return core::Result<void>::Ok();
        
    } catch (const std::exception& e) {
        return core::Result<void>::Err(
            std::string("Failed to cleanup logs: ") + e.what()
        );
    }
}

// ============================================================================
// Serialization
// ============================================================================

json CronStorage::serialize_job(const Job& job) {
    json j;
    j["id"] = job.id;
    j["name"] = job.name;
    j["enabled"] = job.enabled;
    j["created_at_ms"] = job.created_at_ms;
    
    // Schedule
    j["schedule"] = serialize_schedule(job.schedule);
    
    // Action
    j["action"]["type"] = static_cast<int>(job.action.type);
    if (job.action.command.has_value()) {
        j["action"]["command"] = *job.action.command;
    }
    if (job.action.function_name.has_value()) {
        j["action"]["function_name"] = *job.action.function_name;
    }
    if (job.action.args.has_value()) {
        j["action"]["args"] = *job.action.args;
    }
    
    // Delivery
    if (job.delivery.has_value()) {
        j["delivery"] = serialize_delivery(*job.delivery);
    }
    
    // Metadata
    if (job.tags.has_value()) {
        j["tags"] = *job.tags;
    }
    if (job.metadata.has_value()) {
        j["metadata"] = *job.metadata;
    }
    
    return j;
}

core::Result<Job> CronStorage::deserialize_job(const json& j) {
    try {
        Job job;
        job.id = j.at("id").get<std::string>();
        job.name = j.value("name", "");
        job.enabled = j.value("enabled", true);
        job.created_at_ms = j.value("created_at_ms", 0LL);
        
        // Schedule
        auto sched_result = deserialize_schedule(j.at("schedule"));
        if (!sched_result.is_ok()) {
            return core::Result<Job>::Err(sched_result.err());
        }
        job.schedule = sched_result.unwrap();
        
        // Action
        auto& action_json = j.at("action");
        job.action.type = static_cast<ActionType>(action_json.at("type").get<int>());
        if (action_json.contains("command")) {
            job.action.command = action_json.at("command").get<std::string>();
        }
        if (action_json.contains("function_name")) {
            job.action.function_name = action_json.at("function_name").get<std::string>();
        }
        if (action_json.contains("args")) {
            job.action.args = action_json.at("args");
        }
        
        // Delivery
        if (j.contains("delivery")) {
            auto delivery_result = deserialize_delivery(j.at("delivery"));
            if (delivery_result.is_ok()) {
                job.delivery = delivery_result.unwrap();
            }
        }
        
        // Metadata
        if (j.contains("tags")) {
            job.tags = j.at("tags").get<std::vector<std::string>>();
        }
        if (j.contains("metadata")) {
            job.metadata = j.at("metadata");
        }
        
        return core::Result<Job>::Ok(std::move(job));
        
    } catch (const std::exception& e) {
        return core::Result<Job>::Err(
            std::string("Failed to deserialize job: ") + e.what()
        );
    }
}

json CronStorage::serialize_schedule(const Schedule& sched) {
    json j;
    j["kind"] = static_cast<int>(sched.kind);
    
    if (sched.at.has_value()) j["at"] = *sched.at;
    if (sched.every_ms.has_value()) j["every_ms"] = *sched.every_ms;
    if (sched.expr.has_value()) j["expr"] = *sched.expr;
    if (sched.tz.has_value()) j["tz"] = *sched.tz;
    if (sched.anchor_ms.has_value()) j["anchor_ms"] = *sched.anchor_ms;
    if (sched.stagger_ms.has_value()) j["stagger_ms"] = *sched.stagger_ms;
    
    return j;
}

core::Result<Schedule> CronStorage::deserialize_schedule(const json& j) {
    try {
        Schedule sched;
        sched.kind = static_cast<ScheduleKind>(j.at("kind").get<int>());
        
        if (j.contains("at")) sched.at = j.at("at").get<std::string>();
        if (j.contains("every_ms")) sched.every_ms = j.at("every_ms").get<int64_t>();
        if (j.contains("expr")) sched.expr = j.at("expr").get<std::string>();
        if (j.contains("tz")) sched.tz = j.at("tz").get<std::string>();
        if (j.contains("anchor_ms")) sched.anchor_ms = j.at("anchor_ms").get<int64_t>();
        if (j.contains("stagger_ms")) sched.stagger_ms = j.at("stagger_ms").get<int64_t>();
        
        return core::Result<Schedule>::Ok(std::move(sched));
        
    } catch (const std::exception& e) {
        return core::Result<Schedule>::Err(
            std::string("Failed to deserialize schedule: ") + e.what()
        );
    }
}



// ============================================================================
// Backup Management
// ============================================================================

// TODO: 实现备份管理功能
/*
        }
        
        // 按修改时间排序 (最新的在前)
        std::sort(backups.begin(), backups.end(), [](const fs::path& a, const fs::path& b) {
            return fs::last_write_time(a) > fs::last_write_time(b);
        });
        
        // 删除超出保留数量的备份
        for (size_t i = keep_count; i < backups.size(); ++i) {
            fs::remove(backups[i]);
            log()->debug("Removed old backup: {}", backups[i].string());
        }
        
    } catch (const std::exception& e) {
        log()->error("Failed to cleanup backups: {}", e.what());
    }
}
*/

} // namespace cron
} // namespace hunterclaw
