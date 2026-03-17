#include "hunterclaw/cli/config_commands.h"
#include "hunterclaw/cli/json_output.h"
#include "hunterclaw/core/config_loader.h"  // ✅ 使用 config_loader.h 而非 config.h
#include <iostream>
#include <fstream>
#include <cstdlib>

namespace hunterclaw {
namespace cli {

// ============================================================================
// ConfigGetCommand
// ============================================================================

std::vector<CommandOption> ConfigGetCommand::get_options() const {
    return {
        {"key", "k", "Configuration key to get", true, ""},
        {"default", "d", "Default value if key not found", false, ""}
    };
}

int ConfigGetCommand::execute(const CommandArgs& cmd_args) {
    std::string key = cmd_args.get_option_value("key");
    std::string default_value = cmd_args.get_option_value("default");
    
    // 加载配置
    auto config = core::Config::load();
    if (!config) {
        JsonOutput output{false, "error", "Failed to load configuration"};
        output.print();
        return 1;
    }
    
    // 获取值
    std::string value;
    if (config->has(key)) {
        value = config->get_string(key);
    } else if (!default_value.empty()) {
        value = default_value;
    } else {
        JsonOutput output{false, "not_found", "Configuration key not found: " + key};
        output.print();
        return 1;
    }
    
    JsonOutput output{true, "success", ""};
    output.data["key"] = key;
    output.data["value"] = value;
    output.print();
    
    return 0;
}

// ============================================================================
// ConfigSetCommand
// ============================================================================

std::vector<CommandOption> ConfigSetCommand::get_options() const {
    return {
        {"key", "k", "Configuration key to set", true, ""},
        {"value", "v", "Value to set", true, ""},
        {"global", "g", "Set in global config", false, "false"}
    };
}

int ConfigSetCommand::execute(const CommandArgs& cmd_args) {
    std::string key = cmd_args.get_option_value("key");
    std::string value = cmd_args.get_option_value("value");
    bool global = cmd_args.get_option_value("global") == "true";
    
    // 加载配置
    auto config = core::Config::load();
    if (!config) {
        config = std::make_shared<core::Config>();
    }
    
    // 设置值
    config->set(key, value);
    
    // 保存配置
    std::string config_path = global ? 
        core::Config::get_global_config_path() : 
        core::Config::get_local_config_path();
    
    if (!config->save(config_path)) {
        JsonOutput output{false, "error", "Failed to save configuration"};
        output.print();
        return 1;
    }
    
    JsonOutput output{true, "success", "Configuration updated"};
    output.data["key"] = key;
    output.data["value"] = value;
    output.data["path"] = config_path;
    output.print();
    
    return 0;
}

// ============================================================================
// ConfigListCommand
// ============================================================================

std::vector<CommandOption> ConfigListCommand::get_options() const {
    return {
        {"format", "f", "Output format (json|table)", false, "json"}
    };
}

int ConfigListCommand::execute(const CommandArgs& cmd_args) {
    std::string format = cmd_args.get_option_value("format");
    
    // 加载配置
    auto config = core::Config::load();
    if (!config) {
        JsonOutput output{false, "error", "Failed to load configuration"};
        output.print();
        return 1;
    }
    
    // 获取所有键值对
    auto all_config = config->get_all();
    
    if (format == "table") {
        // 表格输出
        std::cout << "Configuration:\n";
        std::cout << std::string(60, '=') << "\n";
        std::cout << "Key                              | Value\n";
        std::cout << std::string(60, '-') << "\n";
        
        for (const auto& [key, value] : all_config) {
            std::cout << std::left << std::setw(33) << key << "| " << value << "\n";
        }
        
        std::cout << std::string(60, '=') << "\n";
        std::cout << "Total: " << all_config.size() << " keys\n";
    } else {
        // JSON 输出
        JsonOutput output{true, "success", ""};
        output.data["config"] = all_config;
        output.data["count"] = all_config.size();
        output.print();
    }
    
    return 0;
}

// ============================================================================
// ConfigUnsetCommand
// ============================================================================

std::vector<CommandOption> ConfigUnsetCommand::get_options() const {
    return {
        {"key", "k", "Configuration key to unset", true, ""}
    };
}

int ConfigUnsetCommand::execute(const CommandArgs& cmd_args) {
    std::string key = cmd_args.get_option_value("key");
    
    // 加载配置
    auto config = core::Config::load();
    if (!config) {
        JsonOutput output{false, "error", "Failed to load configuration"};
        output.print();
        return 1;
    }
    
    // 检查键是否存在
    if (!config->has(key)) {
        JsonOutput output{false, "not_found", "Configuration key not found: " + key};
        output.print();
        return 1;
    }
    
    // 删除键
    config->unset(key);
    
    // 保存配置
    if (!config->save()) {
        JsonOutput output{false, "error", "Failed to save configuration"};
        output.print();
        return 1;
    }
    
    JsonOutput output{true, "success", "Configuration key removed"};
    output.data["key"] = key;
    output.print();
    
    return 0;
}

// ============================================================================
// ConfigResetCommand
// ============================================================================

std::vector<CommandOption> ConfigResetCommand::get_options() const {
    return {
        {"confirm", "y", "Confirm reset without prompt", false, "false"}
    };
}

int ConfigResetCommand::execute(const CommandArgs& cmd_args) {
    bool confirm = cmd_args.get_option_value("confirm") == "true";
    
    if (!confirm) {
        std::cout << "This will reset all configuration to defaults. Continue? [y/N] ";
        std::string response;
        std::getline(std::cin, response);
        
        if (response != "y" && response != "Y" && response != "yes") {
            JsonOutput output{false, "cancelled", "Reset cancelled"};
            output.print();
            return 1;
        }
    }
    
    // 创建默认配置
    auto config = core::Config::create_default();
    
    // 保存配置
    if (!config->save()) {
        JsonOutput output{false, "error", "Failed to save configuration"};
        output.print();
        return 1;
    }
    
    JsonOutput output{true, "success", "Configuration reset to defaults"};
    output.print();
    
    return 0;
}

// ============================================================================
// ConfigPathCommand
// ============================================================================

std::vector<CommandOption> ConfigPathCommand::get_options() const {
    return {
        {"global", "g", "Show global config path", false, "false"}
    };
}

int ConfigPathCommand::execute(const CommandArgs& cmd_args) {
    bool global = cmd_args.get_option_value("global") == "true";
    
    std::string config_path = global ?
        core::Config::get_global_config_path() :
        core::Config::get_local_config_path();
    
    JsonOutput output{true, "success", ""};
    output.data["path"] = config_path;
    output.data["exists"] = std::filesystem::exists(config_path);
    output.print();
    
    return 0;
}

// ============================================================================
// ConfigEditCommand
// ============================================================================

std::vector<CommandOption> ConfigEditCommand::get_options() const {
    return {
        {"editor", "e", "Editor to use (default: $EDITOR)", false, ""}
    };
}

int ConfigEditCommand::execute(const CommandArgs& cmd_args) {
    std::string editor = cmd_args.get_option_value("editor");
    
    if (editor.empty()) {
        const char* env_editor = std::getenv("EDITOR");
        if (env_editor) {
            editor = env_editor;
        } else {
            editor = "vi";  // 默认编辑器
        }
    }
    
    std::string config_path = core::Config::get_local_config_path();
    
    // 确保配置文件存在
    if (!std::filesystem::exists(config_path)) {
        auto config = core::Config::create_default();
        config->save(config_path);
    }
    
    // 打开编辑器
    std::string command = editor + " " + config_path;
    int result = std::system(command.c_str());
    
    if (result != 0) {
        JsonOutput output{false, "error", "Failed to open editor"};
        output.print();
        return 1;
    }
    
    JsonOutput output{true, "success", "Configuration edited"};
    output.data["path"] = config_path;
    output.print();
    
    return 0;
}

} // namespace cli
} // namespace hunterclaw
