#pragma once

#include "hunterclaw/cli/command.h"
#include "hunterclaw/core/result.h"
#include <vector>
<string>
#include <memory>

namespace hunterclaw {
namespace cli {

/**
 * @brief Gateway CLI 命令
 * 
 * 对齐 OpenClaw gateway CLI
 * 
 * 命令列表:
 * - gateway start       启动 Gateway
 * - gateway stop        停止 Gateway
 * - gateway restart     重启 Gateway
 * - gateway status      查看状态
 * - gateway logs        查看日志
 * - gateway config      配置管理
 * - gateway health      健康检查
 */

/**
 * @brief Gateway Start 命令
 */
class GatewayStartCommand : public Command {
public:
    std::string name() const override { return "start"; }
    std::string description() const override { return "Start Gateway server"; }
    
    core::Result<void> execute(const std::vector<std::string>& args) override;
    
private:
    std::vector<CommandOption> get_options() const;
};

/**
 * @brief Gateway Stop 命令
 */
class GatewayStopCommand : public Command {
public:
    std::string name() const override { return "stop"; }
    std::string description() const override { return "Stop Gateway server"; }
    
    core::Result<void> execute(const std::vector<std::string>& args) override;
    
private:
    std::vector<CommandOption> get_options() const;
};

/**
 * @brief Gateway Restart 命令
 */
class GatewayRestartCommand : public Command {
public:
    std::string name() const override { return "restart"; }
    std::string description() const override { return "Restart Gateway server"; }
    
    core::Result<void> execute(const std::vector<std::string>& args) override;
    
private:
    std::vector<CommandOption> get_options() const;
};

/**
 * @brief Gateway Status 命令
 */
class GatewayStatusCommand : public Command {
public:
    std::string name() const override { return "status"; }
    std::string description() const override { return "Show Gateway status"; }
    
    core::Result<void> execute(const std::vector<std::string>& args) override;
    
private:
    std::vector<CommandOption> get_options() const;
};

/**
 * @brief Gateway Logs 命令
 */
class GatewayLogsCommand : public Command {
public:
    std::string name() const override { return "logs"; }
    std::string description() const override { return "Show Gateway logs"; }
    
    core::Result<void> execute(const std::vector<std::string>& args) override;
    
private:
    std::vector<CommandOption> get_options() const;
};

/**
 * @brief Gateway Config 命令
 */
class GatewayConfigCommand : public Command {
public:
    std::string name() const override { return "config"; }
    std::string description() const override { return "Manage Gateway configuration"; }
    
    core::Result<void> execute(const std::vector<std::string>& args) override;
    
private:
    std::vector<CommandOption> get_options() const;
};

/**
 * @brief Gateway Health 命令
 */
class GatewayHealthCommand : public Command {
public:
    std::string name() const override { return "health"; }
    std::string description() const override { return "Check Gateway health"; }
    
    core::Result<void> execute(const std::vector<std::string>& args) override;
    
private:
    std::vector<CommandOption> get_options() const;
};

/**
 * @brief 注册所有 Gateway 命令
 */
void register_gateway_commands();

} // namespace cli
} // namespace hunterclaw
