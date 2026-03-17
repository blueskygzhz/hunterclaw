#pragma once

#include "hunterclaw/cli/command.h"
#include "hunterclaw/cron/service.h"
#include <memory>

namespace hunterclaw {
namespace cli {

// ============================================================================
// Cron CLI Commands (对齐 OpenClaw)
// ============================================================================

// hunterclaw cron:list - 列出所有定时任务
class CronListCommand : public Command {
public:
    explicit CronListCommand(std::shared_ptr<cron::CronService> service);
    
    std::string name() const override { return "cron:list"; }
    std::string description() const override { return "List all cron jobs"; }
    
    void configure(CLI::App& app) override;
    core::Result<int> execute(const CommandContext& ctx) override;

private:
    std::shared_ptr<cron::CronService> service_;
    bool show_json_ = false;
    bool show_disabled_ = false;
};

// hunterclaw cron:add <name> <schedule> <command> - 添加定时任务
class CronAddCommand : public Command {
public:
    explicit CronAddCommand(std::shared_ptr<cron::CronService> manager);
    
    std::string name() const override { return "cron:add"; }
    std::string description() const override { return "Add a new cron job"; }
    
    void configure(CLI::App& app) override;
    core::Result<int> execute(const CommandContext& ctx) override;

private:
    std::shared_ptr<cron::CronService> manager_;
    std::string job_name_;
    std::string schedule_;
    std::string command_;
    std::string description_;
};

// hunterclaw cron:remove <name> - 删除定时任务
class CronRemoveCommand : public Command {
public:
    explicit CronRemoveCommand(std::shared_ptr<cron::CronService> manager);
    
    std::string name() const override { return "cron:remove"; }
    std::string description() const override { return "Remove a cron job"; }
    
    void configure(CLI::App& app) override;
    core::Result<int> execute(const CommandContext& ctx) override;

private:
    std::shared_ptr<cron::CronService> manager_;
    std::string job_name_;
    bool force_ = false;
};

// hunterclaw cron:info <name> - 显示任务详情
class CronInfoCommand : public Command {
public:
    explicit CronInfoCommand(std::shared_ptr<cron::CronService> manager);
    
    std::string name() const override { return "cron:info"; }
    std::string description() const override { return "Show cron job details"; }
    
    void configure(CLI::App& app) override;
    core::Result<int> execute(const CommandContext& ctx) override;

private:
    std::shared_ptr<cron::CronService> manager_;
    std::string job_name_;
    bool show_json_ = false;
};

// hunterclaw cron:enable <name> - 启用任务
class CronEnableCommand : public Command {
public:
    explicit CronEnableCommand(std::shared_ptr<cron::CronService> manager);
    
    std::string name() const override { return "cron:enable"; }
    std::string description() const override { return "Enable a cron job"; }
    
    void configure(CLI::App& app) override;
    core::Result<int> execute(const CommandContext& ctx) override;

private:
    std::shared_ptr<cron::CronService> manager_;
    std::string job_name_;
};

// hunterclaw cron:disable <name> - 禁用任务
class CronDisableCommand : public Command {
public:
    explicit CronDisableCommand(std::shared_ptr<cron::CronService> manager);
    
    std::string name() const override { return "cron:disable"; }
    std::string description() const override { return "Disable a cron job"; }
    
    void configure(CLI::App& app) override;
    core::Result<int> execute(const CommandContext& ctx) override;

private:
    std::shared_ptr<cron::CronService> manager_;
    std::string job_name_;
};

// hunterclaw cron:run <name> - 立即执行任务
class CronRunCommand : public Command {
public:
    explicit CronRunCommand(std::shared_ptr<cron::CronService> manager);
    
    std::string name() const override { return "cron:run"; }
    std::string description() const override { return "Run a cron job immediately"; }
    
    void configure(CLI::App& app) override;
    core::Result<int> execute(const CommandContext& ctx) override;

private:
    std::shared_ptr<cron::CronService> manager_;
    std::string job_name_;
};

// ============================================================================
// Register all cron commands
// ============================================================================

void register_cron_commands(
    CLI::App& app,
    std::shared_ptr<cron::CronService> manager
);

} // namespace cli
} // namespace hunterclaw
