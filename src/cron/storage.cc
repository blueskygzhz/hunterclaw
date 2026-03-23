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
                log()->warn("Failed to deserialize job: {}", job_result.error);
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

core::Result<std::vector<RunLogEntry>> CronStorage::load_run_logs(
    const std::string& job_id,
    std::optional<int64_t> since_ms,
    std::optional<int> limit
) {
    std::string logs_path = base_dir_ + "/run_logs.json";
    
    if (!fs::exists(logs_path)) {
        log()->debug("Run logs file not found, returning empty list");
        return core::Result<std::vector<RunLogEntry>>::Ok({});
    }
    
    try {
        std::ifstream file(logs_path);
        if (!file.is_open()) {
            return core::Result<std::vector<RunLogEntry>>::Err(
                "Failed to open run logs file: " + logs_path
            );
        }
        
        json data;
        file >> data;
        file.close();
        
        if (!data.is_array()) {
            return core::Result<std::vector<RunLogEntry>>::Err(
                "Invalid run logs format: expected array"
            );
        }
        
        std::vector<RunLogEntry> logs;
        for (const auto& log_json : data) {
            try {
                auto entry = RunLogEntry::from_json(log_json);
                // Filter by job_id and since_ms if specified
                if (!job_id.empty() && entry.job_id != job_id) {
                    continue;
                }
                if (since_ms.has_value() && entry.ts < *since_ms) {
                    continue;
                }
                logs.push_back(entry);
            } catch (const std::exception& e) {
                log()->warn("Failed to parse run log entry: {}", e.what());
            }
        }
        
        // Apply limit if specified
        if (limit.has_value() && logs.size() > static_cast<size_t>(*limit)) {
            logs.resize(*limit);
        }
        
        log()->debug("Loaded {} run logs for job_id={}", logs.size(), job_id);
        
        return core::Result<std::vector<RunLogEntry>>::Ok(std::move(logs));
        
    } catch (const std::exception& e) {
        return core::Result<std::vector<RunLogEntry>>::Err(
            std::string("Failed to load run logs: ") + e.what()
        );
    }
}

core::Result<void> CronStorage::save_run_log(const RunLogEntry& log_entry) {
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
        data.push_back(log_entry.to_json());
        
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

// ============================================================================
// Serialization
// ============================================================================

json CronStorage::serialize_job(const Job& job) {
    json j;
    j["id"] = job.id;
    j["name"] = job.name;
    j["enabled"] = job.enabled;
    j["created_at_ms"] = job.created_at_ms;
    j["updated_at_ms"] = job.updated_at_ms;
    
    // Schedule
    j["schedule"] = serialize_schedule(job.schedule);
    
    // Session target
    j["session_target"] = static_cast<int>(job.session_target);
    
    // Wake mode
    j["wake_mode"] = static_cast<int>(job.wake_mode);
    
    // ✅ 对齐 OpenClaw: 实现 payload 序列化
    {
        json payload;
        payload["kind"] = (job.payload.kind == Payload::Kind::AgentTurn) ? "agent-turn" : "system-event";
        payload["message"] = job.payload.message;
        if (job.payload.model) payload["model"] = *job.payload.model;
        if (job.payload.thinking) payload["thinking"] = *job.payload.thinking;
        if (job.payload.timeout_seconds) payload["timeoutSeconds"] = *job.payload.timeout_seconds;
        j["payload"] = payload;
    }
    
    // ✅ 对齐 OpenClaw: 实现 delivery 序列化
    if (job.delivery.has_value()) {
        const auto& d = *job.delivery;
        json delivery;
        switch (d.mode) {
            case DeliveryMode::None:     delivery["mode"] = "none"; break;
            case DeliveryMode::Announce: delivery["mode"] = "announce"; break;
            case DeliveryMode::Webhook:  delivery["mode"] = "webhook"; break;
        }
        if (d.channel) delivery["channel"] = *d.channel;
        if (d.to) delivery["to"] = *d.to;
        if (d.account_id) delivery["accountId"] = *d.account_id;
        if (d.webhook_url) delivery["webhookUrl"] = *d.webhook_url;
        j["delivery"] = delivery;
    }
    
    // Metadata
    if (job.agent_id.has_value()) {
        j["agent_id"] = *job.agent_id;
    }
    if (job.session_key.has_value()) {
        j["session_key"] = *job.session_key;
    }
    if (job.lane.has_value()) {
        j["lane"] = *job.lane;
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
        job.updated_at_ms = j.value("updated_at_ms", 0LL);
        
        // Schedule
        auto sched_result = deserialize_schedule(j.at("schedule"));
        if (!sched_result.is_ok()) {
            return core::Result<Job>::Err(sched_result.error);
        }
        job.schedule = sched_result.unwrap();
        
        // Session target
        if (j.contains("session_target")) {
            job.session_target = static_cast<SessionTarget>(j.at("session_target").get<int>());
        } else {
            job.session_target = SessionTarget::Isolated;
        }
        
        // Wake mode
        if (j.contains("wake_mode")) {
            job.wake_mode = static_cast<WakeMode>(j.at("wake_mode").get<int>());
        } else {
            job.wake_mode = WakeMode::Now;
        }
        
        // Payload - TODO: implement payload deserialization
        job.payload = Payload::system_event(""); // Placeholder
        
        // Delivery - TODO: implement delivery deserialization
        
        // Metadata
        if (j.contains("agent_id")) {
            job.agent_id = j.at("agent_id").get<std::string>();
        }
        if (j.contains("session_key")) {
            job.session_key = j.at("session_key").get<std::string>();
        }
        if (j.contains("lane")) {
            job.lane = j.at("lane").get<std::string>();
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
