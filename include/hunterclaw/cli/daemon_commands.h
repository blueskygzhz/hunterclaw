#pragma once

#include "hunterclaw/cli/command.h"
#include "hunterclaw/daemon/daemon_service.h"
#include <memory>

namespace hunterclaw {
namespace cli {

/**
 * Daemon Install Command
 * Aligned with OpenClaw's runDaemonInstall() (install.ts:21)
 */
class DaemonInstallCommand : public Command {
public:
    core::Result<void> execute(const std::vector<std::string>& args) override;
    std::string name() const override { return "install"; }
    std::string description() const override { return "Install HunterClaw as a system daemon"; }
    void print_help() const override;
    
private:
    std::vector<CommandOption> get_options() const;
};

/**
 * Daemon Uninstall Command
 * Aligned with OpenClaw's runDaemonUninstall() (lifecycle.ts:183)
 */
class DaemonUninstallCommand : public Command {
public:
    core::Result<void> execute(const std::vector<std::string>& args) override;
    std::string name() const override { return "uninstall"; }
    std::string description() const override { return "Uninstall HunterClaw daemon"; }
    void print_help() const override;
    
private:
    std::vector<CommandOption> get_options() const;
};

/**
 * Daemon Start Command
 * Aligned with OpenClaw's runDaemonStart() (lifecycle.ts:193)
 */
class DaemonStartCommand : public Command {
public:
    core::Result<void> execute(const std::vector<std::string>& args) override;
    std::string name() const override { return "start"; }
    std::string description() const override { return "Start HunterClaw daemon"; }
    void print_help() const override;
    
private:
    std::vector<CommandOption> get_options() const;
};

/**
 * Daemon Stop Command
 * Aligned with OpenClaw's runDaemonStop() (lifecycle.ts:202)
 */
class DaemonStopCommand : public Command {
public:
    core::Result<void> execute(const std::vector<std::string>& args) override;
    std::string name() const override { return "stop"; }
    std::string description() const override { return "Stop HunterClaw daemon"; }
    void print_help() const override;
    
private:
    std::vector<CommandOption> get_options() const;
};

/**
 * Daemon Restart Command
 * Aligned with OpenClaw's runDaemonRestart() (lifecycle.ts:220)
 */
class DaemonRestartCommand : public Command {
public:
    core::Result<void> execute(const std::vector<std::string>& args) override;
    std::string name() const override { return "restart"; }
    std::string description() const override { return "Restart HunterClaw daemon"; }
    void print_help() const override;
    
private:
    std::vector<CommandOption> get_options() const;
};

/**
 * Daemon Status Command
 * Aligned with OpenClaw's runDaemonStatus() (status.ts:7)
 */
class DaemonStatusCommand : public Command {
public:
    core::Result<void> execute(const std::vector<std::string>& args) override;
    std::string name() const override { return "status"; }
    std::string description() const override { return "Show HunterClaw daemon status"; }
    void print_help() const override;
    
private:
    std::vector<CommandOption> get_options() const;
};

/**
 * Daemon Logs Command
 * Platform-specific log viewing
 */
class DaemonLogsCommand : public Command {
public:
    core::Result<void> execute(const std::vector<std::string>& args) override;
    std::string name() const override { return "logs"; }
    std::string description() const override { return "View HunterClaw daemon logs"; }
    void print_help() const override;
    
private:
    std::vector<CommandOption> get_options() const;
};

/**
 * Register all daemon commands
 */
void register_daemon_commands();

} // namespace cli
} // namespace hunterclaw
