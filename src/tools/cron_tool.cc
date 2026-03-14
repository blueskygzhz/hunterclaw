#include "hunterclaw/tools/cron_tool.h"
#include <json/json.h>
<parameter name="chrono">
#include <sstream>
#include <algorithm>

namespace hunterclaw {
namespace tools {

namespace {

// Cron Job 定义
struct CronJob {
    std::string id;
    std::string name;
    std::string schedule;        // Cron 表达式 (e.g., "0 9 * * *")
    std::string command;          // 要执行的命令
    Json::Value params;           // 命令参数
    bool enabled;
    int64_t created_at_ms;
    int64_t last_run_ms;
    int64_t next_run_ms;
    int run_count;
    std::string last_status;      // "success", "error", "pending"
    std::string last_error;
};

// Cron Job 管理器
class CronJobManager {
public:
    static CronJobManager& Instance() {
        static CronJobManager instance;
        return instance;
    }

    std::string CreateJob(const std::string& name, const std::string& schedule,
                          const std::string& command, const Json::Value& params) {
        std::lock_guard<std::mutex> lock(mutex_);
        
        // 生成唯一 ID
        std::string id = GenerateJobId();
        
        CronJob job;
        job.id = id;
        job.name = name;
        job.schedule = schedule;
        job.command = command;
        job.params = params;
        job.enabled = true;
        job.created_at_ms = GetCurrentTimeMs();
        job.last_run_ms = 0;
        job.next_run_ms = CalculateNextRunTime(schedule, job.created_at_ms);
        job.run_count = 0;
        job.last_status = "pending";
        
        jobs_[id] = job;
        jobs_by_name_[name] = id;
        
        return id;
    }

    bool DeleteJob(const std::string& id) {
        std::lock_guard<std::mutex> lock(mutex_);
        
        auto it = jobs_.find(id);
        if (it == jobs_.end()) {
            return false;
        }
        
        jobs_by_name_.erase(it->second.name);
        jobs_.erase(it);
        return true;
    }

    std::vector<CronJob> ListJobs() const {
        std::lock_guard<std::mutex> lock(mutex_);
        
        std::vector<CronJob> result;
        for (const auto& [_, job] : jobs_) {
            result.push_back(job);
        }
        
        // 按创建时间倒序排序
        std::sort(result.begin(), result.end(),
                  [](const CronJob& a, const CronJob& b) {
                      return a.created_at_ms > b.created_at_ms;
                  });
        
        return result;
    }

    std::optional<CronJob> GetJob(const std::string& id) const {
        std::lock_guard<std::mutex> lock(mutex_);
        
        auto it = jobs_.find(id);
        if (it != jobs_.end()) {
            return it->second;
        }
        return std::nullopt;
    }

    bool EnableJob(const std::string& id, bool enabled) {
        std::lock_guard<std::mutex> lock(mutex_);
        
        auto it = jobs_.find(id);
        if (it == jobs_.end()) {
            return false;
        }
        
        it->second.enabled = enabled;
        return true;
    }

    void UpdateJobStatus(const std::string& id, const std::string& status,
                          const std::string& error = "") {
        std::lock_guard<std::mutex> lock(mutex_);
        
        auto it = jobs_.find(id);
        if (it == jobs_.end()) {
            return;
        }
        
        auto& job = it->second;
        job.last_run_ms = GetCurrentTimeMs();
        job.last_status = status;
        job.last_error = error;
        job.run_count++;
        
        // 计算下次运行时间
        job.next_run_ms = CalculateNextRunTime(job.schedule, job.last_run_ms);
    }

private:
    CronJobManager() = default;

    std::string GenerateJobId() {
        std::stringstream ss;
        ss << "cron_" << std::hex << GetCurrentTimeMs() << "_" << (++id_counter_);
        return ss.str();
    }

    int64_t GetCurrentTimeMs() const {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }

    // 简化的 Cron 表达式解析和下次运行时间计算
    // 实际实现应该使用专业的 Cron 库 (如 ccronexpr)
    int64_t CalculateNextRunTime(const std::string& schedule, int64_t from_ms) const {
        // 这里使用简化逻辑:每小时执行一次
        // 实际实现需要完整的 Cron 表达式解析
        const int64_t kOneHour = 60 * 60 * 1000;
        return from_ms + kOneHour;
    }

    mutable std::mutex mutex_;
    std::map<std::string, CronJob> jobs_;
    std::map<std::string, std::string> jobs_by_name_;
    int id_counter_ = 0;
};

// 将 CronJob 转换为 JSON
Json::Value CronJobToJson(const CronJob& job) {
    Json::Value json;
    json["id"] = job.id;
    json["name"] = job.name;
    json["schedule"] = job.schedule;
    json["command"] = job.command;
    json["params"] = job.params;
    json["enabled"] = job.enabled;
    json["createdAtMs"] = Json::Int64(job.created_at_ms);
    json["lastRunMs"] = Json::Int64(job.last_run_ms);
    json["nextRunMs"] = Json::Int64(job.next_run_ms);
    json["runCount"] = job.run_count;
    json["lastStatus"] = job.last_status;
    if (!job.last_error.empty()) {
        json["lastError"] = job.last_error;
    }
    return json;
}

// 格式化时间戳为人类可读格式
std::string FormatTimestamp(int64_t timestamp_ms) {
    if (timestamp_ms == 0) {
        return "Never";
    }
    
    auto now = std::chrono::system_clock::now();
    auto then = std::chrono::system_clock::time_point(std::chrono::milliseconds(timestamp_ms));
    auto diff = std::chrono::duration_cast<std::chrono::seconds>(now - then);
    
    int64_t seconds = diff.count();
    if (seconds < 60) {
        return std::to_string(seconds) + " seconds ago";
    } else if (seconds < 3600) {
        return std::to_string(seconds / 60) + " minutes ago";
    } else if (seconds < 86400) {
        return std::to_string(seconds / 3600) + " hours ago";
    } else {
        return std::to_string(seconds / 86400) + " days ago";
    }
}

std::string FormatNextRun(int64_t next_run_ms) {
    auto now = std::chrono::system_clock::now();
    auto next = std::chrono::system_clock::time_point(std::chrono::milliseconds(next_run_ms));
    auto diff = std::chrono::duration_cast<std::chrono::seconds>(next - now);
    
    int64_t seconds = diff.count();
    if (seconds < 0) {
        return "Overdue";
    } else if (seconds < 60) {
        return "in " + std::to_string(seconds) + " seconds";
    } else if (seconds < 3600) {
        return "in " + std::to_string(seconds / 60) + " minutes";
    } else if (seconds < 86400) {
        return "in " + std::to_string(seconds / 3600) + " hours";
    } else {
        return "in " + std::to_string(seconds / 86400) + " days";
    }
}

} // namespace

CronTool::CronTool(const std::string& name, const std::string& description)
    : BaseTool(name, description) {}

ToolDefinition CronTool::GetDefinition() const {
    ToolDefinition def;
    def.name = name_;
    def.description = description_;
    
    // cron_list - 列出所有定时任务
    {
        ToolFunction func;
        func.name = "cron_list";
        func.description = "列出所有已创建的定时任务,包括任务状态、调度信息、执行历史等";
        
        Json::Value schema;
        schema["type"] = "object";
        schema["properties"] = Json::Value(Json::objectValue);
        schema["required"] = Json::Value(Json::arrayValue);
        
        func.parameters = schema;
        def.functions.push_back(func);
    }
    
    // cron_create - 创建定时任务
    {
        ToolFunction func;
        func.name = "cron_create";
        func.description = "创建新的定时任务,支持 Cron 表达式调度(如每天、每小时等)";
        
        Json::Value schema;
        schema["type"] = "object";
        
        Json::Value properties;
        properties["name"]["type"] = "string";
        properties["name"]["description"] = "任务名称 (必需)";
        properties["schedule"]["type"] = "string";
        properties["schedule"]["description"] = "Cron 表达式 (必需), e.g., '0 9 * * *' = 每天9点";
        properties["command"]["type"] = "string";
        properties["command"]["description"] = "要执行的命令 (必需)";
        properties["params"]["type"] = "object";
        properties["params"]["description"] = "命令参数 (可选)";
        schema["properties"] = properties;
        
        Json::Value required;
        required.append("name");
        required.append("schedule");
        required.append("command");
        schema["required"] = required;
        
        func.parameters = schema;
        def.functions.push_back(func);
    }
    
    // cron_delete - 删除定时任务
    {
        ToolFunction func;
        func.name = "cron_delete";
        func.description = "删除指定的定时任务";
        
        Json::Value schema;
        schema["type"] = "object";
        
        Json::Value properties;
        properties["id"]["type"] = "string";
        properties["id"]["description"] = "任务 ID (必需)";
        schema["properties"] = properties;
        
        Json::Value required;
        required.append("id");
        schema["required"] = required;
        
        func.parameters = schema;
        def.functions.push_back(func);
    }
    
    return def;
}

ToolResult CronTool::Execute(const std::string& function_name,
                              const Json::Value& arguments) {
    if (function_name == "cron_list") {
        return ExecuteCronList(arguments);
    } else if (function_name == "cron_create") {
        return ExecuteCronCreate(arguments);
    } else if (function_name == "cron_delete") {
        return ExecuteCronDelete(arguments);
    }
    
    return ToolResult::Error("Unknown function: " + function_name);
}

ToolResult CronTool::ExecuteCronList(const Json::Value& arguments) {
    auto& manager = CronJobManager::Instance();
    auto jobs = manager.ListJobs();
    
    Json::Value result;
    result["totalJobs"] = static_cast<int>(jobs.size());
    
    int enabled_count = 0;
    Json::Value jobs_json(Json::arrayValue);
    for (const auto& job : jobs) {
        if (job.enabled) {
            enabled_count++;
        }
        jobs_json.append(CronJobToJson(job));
    }
    
    result["enabledJobs"] = enabled_count;
    result["jobs"] = jobs_json;
    
    // 构建人类可读的摘要
    std::stringstream summary;
    summary << "Cron Jobs Summary:\n";
    summary << "Total: " << jobs.size() << " job(s), "
            << enabled_count << " enabled\n\n";
    
    if (jobs.empty()) {
        summary << "No cron jobs configured.\n";
    } else {
        for (const auto& job : jobs) {
            summary << "• " << job.name << " (" << job.id.substr(0, 12) << "...)\n";
            summary << "  Schedule: " << job.schedule << "\n";
            summary << "  Command: " << job.command << "\n";
            summary << "  Status: " << (job.enabled ? "✓ Enabled" : "✗ Disabled") << "\n";
            summary << "  Runs: " << job.run_count << " time(s)\n";
            summary << "  Last: " << FormatTimestamp(job.last_run_ms)
                    << " (" << job.last_status << ")\n";
            summary << "  Next: " << FormatNextRun(job.next_run_ms) << "\n\n";
        }
    }
    
    return ToolResult::Success(result, summary.str());
}

ToolResult CronTool::ExecuteCronCreate(const Json::Value& arguments) {
    // 验证必需参数
    if (!arguments.isMember("name") || !arguments["name"].isString()) {
        return ToolResult::Error("Missing required parameter: name");
    }
    if (!arguments.isMember("schedule") || !arguments["schedule"].isString()) {
        return ToolResult::Error("Missing required parameter: schedule");
    }
    if (!arguments.isMember("command") || !arguments["command"].isString()) {
        return ToolResult::Error("Missing required parameter: command");
    }
    
    std::string name = arguments["name"].asString();
    std::string schedule = arguments["schedule"].asString();
    std::string command = arguments["command"].asString();
    Json::Value params = arguments.get("params", Json::Value(Json::objectValue));
    
    // 验证 Cron 表达式格式 (简化版)
    // 实际实现应该使用专业的 Cron 表达式解析器
    if (schedule.empty()) {
        return ToolResult::Error("Invalid cron schedule: empty");
    }
    
    // 创建定时任务
    auto& manager = CronJobManager::Instance();
    std::string job_id = manager.CreateJob(name, schedule, command, params);
    
    auto job_opt = manager.GetJob(job_id);
    if (!job_opt.has_value()) {
        return ToolResult::Error("Failed to create cron job");
    }
    
    Json::Value result = CronJobToJson(*job_opt);
    
    std::stringstream summary;
    summary << "Successfully created cron job:\n\n";
    summary << "Name: " << name << "\n";
    summary << "ID: " << job_id << "\n";
    summary << "Schedule: " << schedule << "\n";
    summary << "Command: " << command << "\n";
    summary << "Next run: " << FormatNextRun(job_opt->next_run_ms) << "\n";
    
    return ToolResult::Success(result, summary.str());
}

ToolResult CronTool::ExecuteCronDelete(const Json::Value& arguments) {
    // 验证必需参数
    if (!arguments.isMember("id") || !arguments["id"].isString()) {
        return ToolResult::Error("Missing required parameter: id");
    }
    
    std::string job_id = arguments["id"].asString();
    
    // 获取任务信息 (用于返回摘要)
    auto& manager = CronJobManager::Instance();
    auto job_opt = manager.GetJob(job_id);
    
    if (!job_opt.has_value()) {
        return ToolResult::Error("Cron job not found: " + job_id);
    }
    
    std::string job_name = job_opt->name;
    
    // 删除任务
    bool deleted = manager.DeleteJob(job_id);
    if (!deleted) {
        return ToolResult::Error("Failed to delete cron job: " + job_id);
    }
    
    Json::Value result;
    result["deleted"] = true;
    result["id"] = job_id;
    result["name"] = job_name;
    
    std::stringstream summary;
    summary << "Successfully deleted cron job:\n";
    summary << "Name: " << job_name << "\n";
    summary << "ID: " << job_id << "\n";
    
    return ToolResult::Success(result, summary.str());
}

} // namespace tools
} // namespace hunterclaw
