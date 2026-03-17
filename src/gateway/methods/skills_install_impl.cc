// 🎯 严格对齐 OpenClaw: src/agents/skills-install.ts
// HunterClaw Skills 安装执行逻辑实现

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <regex>
#include <chrono>
#include <thread>
#include <sys/wait.h>
#include <unistd.h>
#include <sys/types.h>
#include <pwd.h>
#include <nlohmann/json.hpp>

#include "gateway/methods/skills_install_download.h"  // ✅ 通过 -Isrc 可访问
#include "hunterclaw/skills/skills_manager.h"

using json = nlohmann::json;

namespace hunterclaw {
namespace skills {

// 🎯 对齐 OpenClaw: SkillInstallResult (line 27-34)
struct SkillInstallResult {
    bool ok;
    std::string message;
    std::string stdout_output;
    std::string stderr_output;
    int code;  // 进程退出码，-1 表示执行失败
    std::vector<std::string> warnings;
};

// 🎯 对齐 OpenClaw: CommandResult (line 189-193)
struct CommandResult {
    int code;
    std::string stdout_output;
    std::string stderr_output;
};

// 🎯 对齐 OpenClaw: SkillsInstallPreferences (config.skills.install)
struct SkillsInstallPreferences {
    std::string node_manager;  // npm, pnpm, yarn, bun
};

// ==================== 辅助函数 ====================

// 🎯 对齐 OpenClaw: hasBinary() - 检查二进制文件是否存在
static bool has_binary(const std::string& bin_name) {
    // 使用 which/where 命令检查
    std::string cmd;
#ifdef _WIN32
    cmd = "where " + bin_name + " >nul 2>&1";
#else
    cmd = "which " + bin_name + " >/dev/null 2>&1";
#endif
    return system(cmd.c_str()) == 0;
}

// 🎯 对齐 OpenClaw: runCommandSafely() (line 220-238)
static CommandResult run_command_safely(
    const std::vector<std::string>& argv,
    int timeout_ms,
    const std::map<std::string, std::string>& extra_env = {}
) {
    CommandResult result;
    result.code = -1;
    
    if (argv.empty()) {
        result.stderr_output = "empty command";
        return result;
    }
    
    // 构建命令字符串
    std::ostringstream cmd_builder;
    for (size_t i = 0; i < argv.size(); ++i) {
        if (i > 0) cmd_builder << " ";
        // 简单的 shell 转义 (生产环境需要更健壮的实现)
        if (argv[i].find(' ') != std::string::npos) {
            cmd_builder << "\"" << argv[i] << "\"";
        } else {
            cmd_builder << argv[i];
        }
    }
    
    std::string cmd = cmd_builder.str();
    
    // 设置环境变量
    for (const auto& [key, value] : extra_env) {
        setenv(key.c_str(), value.c_str(), 1);
    }
    
    // 执行命令并捕获输出
    // 注意: 这是简化实现，生产环境应该使用 popen 或 fork+exec+pipe
    cmd += " 2>&1";  // 重定向 stderr 到 stdout
    
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) {
        result.stderr_output = "popen failed";
        return result;
    }
    
    // 读取输出 (带超时)
    char buffer[256];
    std::ostringstream output;
    auto start_time = std::chrono::steady_clock::now();
    
    while (true) {
        // 检查超时
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start_time
        ).count();
        
        if (elapsed > timeout_ms) {
            pclose(pipe);
            result.stderr_output = "command timeout";
            result.code = -1;
            return result;
        }
        
        // 非阻塞读取 (简化实现)
        if (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
            output << buffer;
        } else {
            break;
        }
    }
    
    int status = pclose(pipe);
    result.code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    result.stdout_output = output.str();
    
    return result;
}

// 🎯 对齐 OpenClaw: createInstallFailure (line 195-208)
static SkillInstallResult create_install_failure(
    const std::string& message,
    const std::string& stdout_output = "",
    const std::string& stderr_output = "",
    int code = -1
) {
    SkillInstallResult result;
    result.ok = false;
    result.message = message;
    result.stdout_output = stdout_output;
    result.stderr_output = stderr_output;
    result.code = code;
    return result;
}

// 🎯 对齐 OpenClaw: createInstallSuccess (line 210-218)
static SkillInstallResult create_install_success(const CommandResult& cmd_result) {
    SkillInstallResult result;
    result.ok = true;
    result.message = "Installed";
    result.stdout_output = cmd_result.stdout_output;
    result.stderr_output = cmd_result.stderr_output;
    result.code = cmd_result.code;
    return result;
}

// 🎯 对齐 OpenClaw: withWarnings (line 36-44)
static SkillInstallResult with_warnings(
    const SkillInstallResult& result,
    const std::vector<std::string>& warnings
) {
    if (warnings.empty()) {
        return result;
    }
    
    SkillInstallResult new_result = result;
    new_result.warnings = warnings;
    return new_result;
}

// ==================== 安装器实现 ====================

// 🎯 对齐 OpenClaw: buildNodeInstallCommand (line 101-112)
static std::vector<std::string> build_node_install_command(
    const std::string& package_name,
    const SkillsInstallPreferences& prefs
) {
    if (prefs.node_manager == "pnpm") {
        return {"pnpm", "add", "-g", "--ignore-scripts", package_name};
    } else if (prefs.node_manager == "yarn") {
        return {"yarn", "global", "add", "--ignore-scripts", package_name};
    } else if (prefs.node_manager == "bun") {
        return {"bun", "add", "-g", "--ignore-scripts", package_name};
    } else {
        // 默认使用 npm
        return {"npm", "install", "-g", "--ignore-scripts", package_name};
    }
}

// 🎯 对齐 OpenClaw: buildInstallCommand (line 114-154)
struct BuildCommandResult {
    std::vector<std::string> argv;
    std::string error;
};

static BuildCommandResult build_install_command(
    const json& spec,
    const SkillsInstallPreferences& prefs
) {
    BuildCommandResult result;
    
    std::string kind = spec.value("kind", "");
    
    if (kind == "brew") {
        // 🎯 对齐 OpenClaw: brew install (line 122-127)
        if (!spec.contains("formula") || spec["formula"].empty()) {
            result.error = "missing brew formula";
            return result;
        }
        result.argv = {"brew", "install", spec["formula"].get<std::string>()};
        
    } else if (kind == "node") {
        // 🎯 对齐 OpenClaw: npm/pnpm/yarn/bun (line 128-134)
        if (!spec.contains("package") || spec["package"].empty()) {
            result.error = "missing node package";
            return result;
        }
        result.argv = build_node_install_command(spec["package"].get<std::string>(), prefs);
        
    } else if (kind == "go") {
        // 🎯 对齐 OpenClaw: go install (line 136-141)
        if (!spec.contains("module") || spec["module"].empty()) {
            result.error = "missing go module";
            return result;
        }
        result.argv = {"go", "install", spec["module"].get<std::string>()};
        
    } else if (kind == "uv") {
        // 🎯 对齐 OpenClaw: uv tool install (line 142-147)
        if (!spec.contains("package") || spec["package"].empty()) {
            result.error = "missing uv package";
            return result;
        }
        result.argv = {"uv", "tool", "install", spec["package"].get<std::string>()};
        
    } else if (kind == "download") {
        // 🎯 对齐 OpenClaw: download 特殊处理 (line 148-150)
        result.error = "download install handled separately";
        
    } else {
        result.error = "unsupported installer";
    }
    
    return result;
}

// 🎯 对齐 OpenClaw: resolveBrewExecutable() - 查找 brew 可执行文件
static std::string resolve_brew_executable() {
    // 常见的 brew 路径
    const char* candidates[] = {
        "/opt/homebrew/bin/brew",  // Apple Silicon
        "/usr/local/bin/brew",     // Intel Mac / Linux
        nullptr
    };
    
    for (int i = 0; candidates[i] != nullptr; ++i) {
        if (access(candidates[i], X_OK) == 0) {
            return candidates[i];
        }
    }
    
    return "";
}

// 🎯 对齐 OpenClaw: resolveBrewBinDir (line 156-187)
static std::string resolve_brew_bin_dir(int timeout_ms, const std::string& brew_exe = "") {
    std::string exe = brew_exe;
    
    if (exe.empty()) {
        if (has_binary("brew")) {
            exe = "brew";
        } else {
            exe = resolve_brew_executable();
        }
    }
    
    if (exe.empty()) {
        return "";
    }
    
    // 执行 brew --prefix
    auto result = run_command_safely({exe, "--prefix"}, std::min(timeout_ms, 30000));
    if (result.code == 0) {
        std::string prefix = result.stdout_output;
        // trim
        prefix.erase(0, prefix.find_first_not_of(" \t\n\r"));
        prefix.erase(prefix.find_last_not_of(" \t\n\r") + 1);
        
        if (!prefix.empty()) {
            return prefix + "/bin";
        }
    }
    
    // 🎯 对齐 OpenClaw: 回退到 HOMEBREW_PREFIX 环境变量 (line 172-175)
    const char* env_prefix = getenv("HOMEBREW_PREFIX");
    if (env_prefix && strlen(env_prefix) > 0) {
        return std::string(env_prefix) + "/bin";
    }
    
    // 🎯 对齐 OpenClaw: 回退到常见路径 (line 177-186)
    const char* fallback_paths[] = {
        "/opt/homebrew/bin",
        "/usr/local/bin",
        nullptr
    };
    
    for (int i = 0; fallback_paths[i] != nullptr; ++i) {
        if (access(fallback_paths[i], F_OK) == 0) {
            return fallback_paths[i];
        }
    }
    
    return "";
}

// 🎯 对齐 OpenClaw: resolveBrewMissingFailure (line 247-254)
static SkillInstallResult resolve_brew_missing_failure(const json& spec) {
    std::string formula = spec.value("formula", "this package");
    
    std::string hint;
#ifdef __linux__
    hint = "Homebrew is not installed. Install it from https://brew.sh or install \"" 
           + formula + "\" manually using your system package manager (e.g. apt, dnf, pacman).";
#else
    hint = "Homebrew is not installed. Install it from https://brew.sh";
#endif
    
    return create_install_failure("brew not installed — " + hint);
}

// 🎯 对齐 OpenClaw: ensureUvInstalled (line 256-283)
static SkillInstallResult ensure_uv_installed(
    const json& spec,
    const std::string& brew_exe,
    int timeout_ms
) {
    std::string kind = spec.value("kind", "");
    
    if (kind != "uv" || has_binary("uv")) {
        // 不需要安装
        return SkillInstallResult{true, "", "", "", 0, {}};
    }
    
    if (brew_exe.empty()) {
        return create_install_failure(
            "uv not installed — install manually: https://docs.astral.sh/uv/getting-started/installation/"
        );
    }
    
    // 🎯 对齐 OpenClaw: 使用 brew 安装 uv (line 272-277)
    auto brew_result = run_command_safely({brew_exe, "install", "uv"}, timeout_ms);
    if (brew_result.code == 0) {
        return SkillInstallResult{true, "", "", "", 0, {}};  // 安装成功
    }
    
    return create_install_failure(
        "Failed to install uv (brew)",
        brew_result.stdout_output,
        brew_result.stderr_output,
        brew_result.code
    );
}

// 🎯 对齐 OpenClaw: installGoViaApt (line 285-336)
static SkillInstallResult install_go_via_apt(int timeout_ms) {
    std::vector<std::string> apt_install_argv = {"apt-get", "install", "-y", "golang-go"};
    std::vector<std::string> apt_update_argv = {"apt-get", "update", "-qq"};
    std::string apt_failure_message = 
        "go not installed — automatic install via apt failed. Install manually: https://go.dev/doc/install";
    
    // 🎯 对齐 OpenClaw: 检查是否为 root 用户 (line 291)
    bool is_root = (getuid() == 0);
    
    if (is_root) {
        // 🎯 对齐 OpenClaw: root 用户直接安装 (line 293-302)
        // Best effort: 更新包索引
        run_command_safely(apt_update_argv, timeout_ms);
        
        auto apt_result = run_command_safely(apt_install_argv, timeout_ms);
        if (apt_result.code == 0) {
            return SkillInstallResult{true, "", "", "", 0, {}};
        }
        
        return create_install_failure(
            apt_failure_message,
            apt_result.stdout_output,
            apt_result.stderr_output,
            apt_result.code
        );
    }
    
    // 🎯 对齐 OpenClaw: 非 root 用户需要 sudo (line 305-321)
    if (!has_binary("sudo")) {
        return create_install_failure(
            "go not installed — apt-get is available but sudo is not installed. "
            "Install manually: https://go.dev/doc/install"
        );
    }
    
    // 检查 sudo 是否可用 (无密码)
    auto sudo_check = run_command_safely({"sudo", "-n", "true"}, 5000);
    if (sudo_check.code != 0) {
        return create_install_failure(
            "go not installed — apt-get is available but sudo is not usable "
            "(missing or requires a password). Install manually: https://go.dev/doc/install",
            sudo_check.stdout_output,
            sudo_check.stderr_output,
            sudo_check.code
        );
    }
    
    // Best effort: 更新包索引
    std::vector<std::string> sudo_update_argv = {"sudo"};
    sudo_update_argv.insert(sudo_update_argv.end(), apt_update_argv.begin(), apt_update_argv.end());
    run_command_safely(sudo_update_argv, timeout_ms);
    
    // 使用 sudo 安装
    std::vector<std::string> sudo_install_argv = {"sudo"};
    sudo_install_argv.insert(sudo_install_argv.end(), apt_install_argv.begin(), apt_install_argv.end());
    
    auto apt_result = run_command_safely(sudo_install_argv, timeout_ms);
    if (apt_result.code == 0) {
        return SkillInstallResult{true, "", "", "", 0, {}};
    }
    
    return create_install_failure(
        apt_failure_message,
        apt_result.stdout_output,
        apt_result.stderr_output,
        apt_result.code
    );
}

// 🎯 对齐 OpenClaw: ensureGoInstalled (line 338-367)
static SkillInstallResult ensure_go_installed(
    const json& spec,
    const std::string& brew_exe,
    int timeout_ms
) {
    std::string kind = spec.value("kind", "");
    
    if (kind != "go" || has_binary("go")) {
        // 不需要安装
        return SkillInstallResult{true, "", "", "", 0, {}};
    }
    
    // 🎯 对齐 OpenClaw: 优先使用 brew 安装 (line 347-358)
    if (!brew_exe.empty()) {
        auto brew_result = run_command_safely({brew_exe, "install", "go"}, timeout_ms);
        if (brew_result.code == 0) {
            return SkillInstallResult{true, "", "", "", 0, {}};
        }
        
        return create_install_failure(
            "Failed to install go (brew)",
            brew_result.stdout_output,
            brew_result.stderr_output,
            brew_result.code
        );
    }
    
    // 🎯 对齐 OpenClaw: 回退到 apt-get (line 360-362)
    if (has_binary("apt-get")) {
        return install_go_via_apt(timeout_ms);
    }
    
    // 🎯 对齐 OpenClaw: 无可用安装器 (line 364-366)
    return create_install_failure(
        "go not installed — install manually: https://go.dev/doc/install"
    );
}

// 🎯 对齐 OpenClaw: executeInstallCommand (line 369-390)
static SkillInstallResult execute_install_command(
    const std::vector<std::string>& argv,
    int timeout_ms,
    const std::map<std::string, std::string>& extra_env = {}
) {
    if (argv.empty()) {
        return create_install_failure("invalid install command");
    }
    
    auto result = run_command_safely(argv, timeout_ms, extra_env);
    
    if (result.code == 0) {
        return create_install_success(result);
    }
    
    // 🎯 对齐 OpenClaw: formatInstallFailureMessage (line 387-389)
    std::ostringstream msg;
    msg << "Installation failed (exit code " << result.code << ")";
    if (!result.stderr_output.empty()) {
        msg << ": " << result.stderr_output.substr(0, 200);  // 截断到 200 字符
    }
    
    return create_install_failure(
        msg.str(),
        result.stdout_output,
        result.stderr_output,
        result.code
    );
}

// 🎯 对齐 OpenClaw: collectSkillInstallScanWarnings (line 58-85)
static std::vector<std::string> collect_skill_install_scan_warnings(
    const std::string& skill_name,
    const std::string& skill_dir
) {
    std::vector<std::string> warnings;
    
    // TODO: 实现安全扫描逻辑
    // 对齐 OpenClaw scanDirectoryWithSummary()
    // 1. 扫描危险模式: eval(), exec(), system(), child_process
    // 2. 扫描可疑文件: .env, secrets, credentials
    // 3. 统计 critical/warn 级别
    
    // 示例警告 (如果扫描发现问题):
    // warnings.push_back("WARNING: Skill \"" + skill_name + "\" contains dangerous code patterns");
    
    return warnings;
}

// ==================== 主安装函数 ====================

// 🎯 对齐 OpenClaw: installSkill() (line 392-470)
SkillInstallResult install_skill(
    const std::string& workspace_dir,
    const std::string& skill_name,
    const std::string& install_id,
    int timeout_ms,
    const json& config
) {
    // 🎯 对齐 OpenClaw: 规范化超时 (line 393)
    timeout_ms = std::min(std::max(timeout_ms, 1000), 900000);  // 1秒 到 15分钟
    
    // TODO: 加载技能条目 (line 394-406)
    // const auto entries = load_workspace_skill_entries(workspace_dir);
    // const auto entry = find_entry_by_name(entries, skill_name);
    // if (!entry) {
    //     return create_install_failure("Skill not found: " + skill_name);
    // }
    
    // TODO: 查找安装规格 (line 407)
    // const auto spec = find_install_spec(entry, install_id);
    
    // 临时模拟 spec (示例)
    json spec;
    spec["kind"] = "brew";  // 或 "node", "go", "uv", "download"
    spec["formula"] = "python3";  // 示例
    
    // 🎯 对齐 OpenClaw: 收集安全警告 (line 408)
    auto warnings = collect_skill_install_scan_warnings(skill_name, workspace_dir);
    
    if (spec.empty()) {
        return with_warnings(
            create_install_failure("Installer not found: " + install_id),
            warnings
        );
    }
    
    // 🎯 对齐 OpenClaw: download 特殊处理 (line 421-424)
    if (spec.value("kind", "") == "download") {
        // 获取配置目录 (通常是 ~/.hunterclaw)
        std::string config_dir = "./.hunterclaw";  // TODO: 从配置系统获取
        const char* home = getenv("HOME");
        if (home) {
            config_dir = std::string(home) + "/.hunterclaw";
        }
        
        // 提取参数
        std::string url = spec.value("url", "");
        std::string target_dir = spec.value("targetDir", "");
        std::string archive_type = spec.value("archive", "");
        bool extract = spec.value("extract", true);
        int strip_components = spec.value("stripComponents", 0);
        
        // 调用 download 安装器
        auto download_result = install_download_spec(
            skill_name,
            skill_name,  // skill_key (TODO: 从 entry 获取)
            url,
            target_dir,
            archive_type,
            extract,
            strip_components,
            timeout_ms,
            config_dir
        );
        
        // 转换结果格式
        SkillInstallResult result;
        result.ok = download_result.ok;
        result.message = download_result.message;
        result.stdout_output = download_result.stdout_output;
        result.stderr_output = download_result.stderr_output;
        result.code = download_result.code;
        result.warnings = download_result.warnings;
        
        return with_warnings(result, warnings);
    }
    
    // 🎯 对齐 OpenClaw: 解析安装偏好 (line 426)
    SkillsInstallPreferences prefs;
    if (config.contains("skills") && 
        config["skills"].contains("install") && 
        config["skills"]["install"].contains("nodeManager")) {
        prefs.node_manager = config["skills"]["install"]["nodeManager"].get<std::string>();
    } else {
        prefs.node_manager = "npm";  // 默认
    }
    
    // 🎯 对齐 OpenClaw: 构建安装命令 (line 427-439)
    auto command = build_install_command(spec, prefs);
    if (!command.error.empty()) {
        return with_warnings(
            create_install_failure(command.error),
            warnings
        );
    }
    
    // 🎯 对齐 OpenClaw: 查找 brew 可执行文件 (line 441)
    std::string brew_exe;
    if (has_binary("brew")) {
        brew_exe = "brew";
    } else {
        brew_exe = resolve_brew_executable();
    }
    
    // 🎯 对齐 OpenClaw: brew 安装器检查 (line 442-444)
    if (spec.value("kind", "") == "brew" && brew_exe.empty()) {
        return with_warnings(resolve_brew_missing_failure(spec), warnings);
    }
    
    // 🎯 对齐 OpenClaw: 确保 uv 已安装 (line 446-449)
    auto uv_install_result = ensure_uv_installed(spec, brew_exe, timeout_ms);
    if (!uv_install_result.ok && !uv_install_result.message.empty()) {
        return with_warnings(uv_install_result, warnings);
    }
    
    // 🎯 对齐 OpenClaw: 确保 go 已安装 (line 451-454)
    auto go_install_result = ensure_go_installed(spec, brew_exe, timeout_ms);
    if (!go_install_result.ok && !go_install_result.message.empty()) {
        return with_warnings(go_install_result, warnings);
    }
    
    // 🎯 对齐 OpenClaw: 替换 brew 路径 (line 456-459)
    auto argv = command.argv;
    if (spec.value("kind", "") == "brew" && !brew_exe.empty() && !argv.empty() && argv[0] == "brew") {
        argv[0] = brew_exe;
    }
    
    // 🎯 对齐 OpenClaw: 设置 GOBIN 环境变量 (line 461-467)
    std::map<std::string, std::string> extra_env;
    if (spec.value("kind", "") == "go" && !brew_exe.empty()) {
        std::string brew_bin = resolve_brew_bin_dir(timeout_ms, brew_exe);
        if (!brew_bin.empty()) {
            extra_env["GOBIN"] = brew_bin;
        }
    }
    
    // 🎯 对齐 OpenClaw: 执行安装命令 (line 469)
    return with_warnings(
        execute_install_command(argv, timeout_ms, extra_env),
        warnings
    );
}

}  // namespace skills
}  // namespace hunterclaw
