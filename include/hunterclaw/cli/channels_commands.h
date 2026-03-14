#pragma once

#include "hunterclaw/cli/command.h"
#include "hunterclaw/core/result.h"
#include <vector>
#include <string>
#include <memory>

namespace hunterclaw {
namespace cli {

/**
 * @brief Channels CLI 命令
 * 
 * 对齐 OpenClaw channels CLI
 * 
 * 命令列表:
 * - channels list       列出所有渠道
 * - channels add        添加渠道
 * - channels remove     移除渠道
 * - channels enable     启用渠道
 * - channels disable    禁用渠道
 * - channels status     查看渠道状态
 * - channels test       测试渠道连接
 */

/**
 * @brief Channels List 命令
 */
class ChannelsListCommand : public Command {
public:
    std::string name() const override { return "list"; }
    std::string description() const override { return "List all channels"; }
    
    core::Result<void> execute(const std::vector<std::string>& args) override;
    
private:
    std::vector<CommandOption> get_options() const;
};

/**
 * @brief Channels Add 命令
 */
class ChannelsAddCommand : public Command {
public:
    std::string name() const override { return "add"; }
    std::string description() const override { return "Add a new channel"; }
    
    core::Result<void> execute(const std::vector<std::string>& args) override;
    
private:
    std::vector<CommandOption> get_options() const;
};

/**
 * @brief Channels Remove 命令
 */
class ChannelsRemoveCommand : public Command {
public:
    std::string name() const override { return "remove"; }
    std::string description() const override { return "Remove a channel"; }
    
    core::Result<void> execute(const std::vector<std::string>& args) override;
    
private:
    std::vector<CommandOption> get_options() const;
};

/**
 * @brief Channels Enable 命令
 */
class ChannelsEnableCommand : public Command {
public:
    std::string name() const override { return "enable"; }
    std::string description() const override { return "Enable a channel"; }
    
    core::Result<void> execute(const std::vector<std::string>& args) override;
    
private:
    std::vector<CommandOption> get_options() const;
};

/**
 * @brief Channels Disable 命令
 */
class ChannelsDisableCommand : public Command {
public:
    std::string name() const override { return "disable"; }
    std::string description() const override { return "Disable a channel"; }
    
    core::Result<void> execute(const std::vector<std::string>& args) override;
    
private:
    std::vector<CommandOption> get_options() const;
};

/**
 * @brief Channels Status 命令
 */
class ChannelsStatusCommand : public Command {
public:
    std::string name() const override { return "status"; }
    std::string description() const override { return "Show channels status"; }
    
    core::Result<void> execute(const std::vector<std::string>& args) override;
    
private:
    std::vector<CommandOption> get_options() const;
};

/**
 * @brief Channels Test 命令
 */
class ChannelsTestCommand : public Command {
public:
    std::string name() const override { return "test"; }
    std::string description() const override { return "Test channel connection"; }
    
    core::Result<void> execute(const std::vector<std::string>& args) override;
    
private:
    std::vector<CommandOption> get_options() const;
};

/**
 * @brief 注册所有 Channels 命令
 */
void register_channels_commands();

} // namespace cli
} // namespace hunterclaw
