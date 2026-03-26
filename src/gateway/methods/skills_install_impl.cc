// 🎯 严格对齐 OpenClaw: src/agents/skills-install.ts
// HunterClaw Skills 安装执行逻辑实现

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <chrono>
#include <sys/wait.h>
#include <unistd.h>
#include <sys/types.h>
#include <pwd.h>
#include <nlohmann/json.hpp>

#include "skills_install_download.h"
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
    bool prefer_brew = true;   // 对齐 OpenClaw: preferBrew (default: true)
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

// 🎯 对齐 OpenClaw: summarizeInstallOutput (skills-install-output.ts)
static std::string summarize_install_output(const std::string& text) {
    // trim
    std::string raw = text;
    raw.erase(0, raw.find_first_not_of(" \t\n\r"));
    if (!raw.empty()) raw.erase(raw.find_last_not_of(" \t\n\r") + 1);
    if (raw.empty()) return "";
    
    // 按行分割
    std::vector<std::string> lines;
    std::istringstream ss(raw);
    std::string line;
    while (std::getline(ss, line)) {
        // trim line
        line.erase(0, line.find_first_not_of(" \t\r"));
        if (!line.empty()) line.erase(line.find_last_not_of(" \t\r") + 1);
        if (!line.empty()) lines.push_back(line);
    }
    if (lines.empty()) return "";
    
    // 优先找 error: 行 (对齐 OpenClaw: /^error\b/i)
    std::string preferred;
    for (const auto& l : lines) {
        std::string lower = l;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
        if (lower.rfind("error", 0) == 0) { preferred = l; break; }
    }
    // 次选: err!/error:/failed
    if (preferred.empty()) {
        for (const auto& l : lines) {
            std::string lower = l;
            std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
            if (lower.find("err!") != std::string::npos ||
                lower.find("error:") != std::string::npos ||
                lower.find("failed") != std::string::npos) {
                preferred = l; break;
            }
        }
    }
    // 最后一行
    if (preferred.empty()) preferred = lines.back();
    
    // 规范化空白
    std::string normalized;
    bool prev_space = false;
    for (char c : preferred) {
        if (std::isspace(c)) {
            if (!prev_space && !normalized.empty()) { normalized += ' '; prev_space = true; }
        } else {
            normalized += c; prev_space = false;
        }
    }
    if (!normalized.empty() && normalized.back() == ' ') normalized.pop_back();
    
    // 截断到 200 字符
    if (normalized.size() > 200) normalized = normalized.substr(0, 199) + "\xe2\x80\xa6";  // …
    return normalized;
}

// 🎯 对齐 OpenClaw: formatInstallFailureMessage (skills-install-output.ts)
static std::string format_install_failure_message(const CommandResult& result) {
    std::string code_str = (result.code >= 0) ? ("exit " + std::to_string(result.code)) : "unknown exit";
    std::string summary = summarize_install_output(result.stderr_output);
    if (summary.empty()) summary = summarize_install_output(result.stdout_output);
    if (summary.empty()) {
        return "Install failed (" + code_str + ")";
    }
    return "Install failed (" + code_str + "): " + summary;
}

// 🎯 对齐 OpenClaw: createInstallSuccess (line 210-218)
static SkillInstallResult create_install_success(const CommandResult& cmd_result) {
    SkillInstallResult result;
    result.ok = true;
    result.message = "Installed";
    // 对齐 OpenClaw: trim stdout/stderr
    result.stdout_output = cmd_result.stdout_output;
    result.stdout_output.erase(0, result.stdout_output.find_first_not_of(" \t\n\r"));
    if (!result.stdout_output.empty()) result.stdout_output.erase(result.stdout_output.find_last_not_of(" \t\n\r") + 1);
    result.stderr_output = cmd_result.stderr_output;
    result.stderr_output.erase(0, result.stderr_output.find_first_not_of(" \t\n\r"));
    if (!result.stderr_output.empty()) result.stderr_output.erase(result.stderr_output.find_last_not_of(" \t\n\r") + 1);
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
    
    // 🎯 对齐 OpenClaw: formatInstallFailureMessage (skills-install-output.ts)
    return create_install_failure(
        format_install_failure_message(result),
        result.stdout_output,
        result.stderr_output,
        result.code
    );
}

// 🎯 对齐 OpenClaw: formatScanFindingDetail (line 44-52)
static std::string format_scan_finding_detail(
    const std::string& root_dir,
    const std::string& file,
    int line_num,
    const std::string& message
) {
    // 对齐 OpenClaw: path.relative(rootDir, finding.file)
    std::string relative_path = file;
    if (!root_dir.empty() && relative_path.rfind(root_dir, 0) == 0) {
        relative_path = relative_path.substr(root_dir.size());
        if (!relative_path.empty() && relative_path[0] == '/') {
            relative_path = relative_path.substr(1);
        }
    }
    // 对齐 OpenClaw: 如果相对路径为空或以 ".." 开头，使用 basename
    if (relative_path.empty() || relative_path == "." ||
        relative_path.rfind("..", 0) == 0) {
        size_t last_slash = file.find_last_of('/');
        relative_path = (last_slash != std::string::npos) ? file.substr(last_slash + 1) : file;
    }
    return message + " (" + relative_path + ":" + std::to_string(line_num) + ")";
}

// 🎯 对齐 OpenClaw: collectSkillInstallScanWarnings (line 58-85)
// 对齐 scanDirectoryWithSummary: 区分 critical (危险) 和 warn (可疑)
struct ScanSummary {
    int critical = 0;
    int warn = 0;
    struct Finding {
        std::string message;
        std::string file;
        int line;
        std::string severity;  // "critical" | "warn"
    };
    std::vector<Finding> findings;
};

static ScanSummary scan_directory_with_summary(const std::string& skill_dir) {
    ScanSummary summary;
    if (skill_dir.empty() || !std::filesystem::exists(skill_dir)) return summary;
    
    // critical 模式: 直接代码执行
    static const std::vector<std::pair<std::string, std::string>> critical_patterns = {
        {"child_process", "uses child_process (arbitrary command execution)"},
        {"__import__('os')", "uses __import__('os') (arbitrary command execution)"},
        {"subprocess.call", "uses subprocess.call (arbitrary command execution)"},
        {"subprocess.run", "uses subprocess.run (arbitrary command execution)"},
        {"os.system(", "uses os.system (arbitrary command execution)"},
        {"os.popen(", "uses os.popen (arbitrary command execution)"},
    };
    // warn 模式: 可疑但不一定危险
    static const std::vector<std::pair<std::string, std::string>> warn_patterns = {
        {"eval(", "uses eval()"},
        {"exec(", "uses exec()"},
        {"Function(", "uses Function() constructor"},
    };
    
    try {
        for (const auto& entry : std::filesystem::recursive_directory_iterator(skill_dir)) {
            if (!entry.is_regular_file()) continue;
            const auto& path = entry.path();
            const std::string ext = path.extension().string();
            if (ext != ".js" && ext != ".ts" && ext != ".py" && ext != ".sh" &&
                ext != ".mjs" && ext != ".cjs") continue;
            
            std::ifstream f(path);
            if (!f.is_open()) continue;
            std::string line;
            int line_num = 0;
            while (std::getline(f, line)) {
                ++line_num;
                for (const auto& [pattern, msg] : critical_patterns) {
                    if (line.find(pattern) != std::string::npos) {
                        summary.critical++;
                        summary.findings.push_back({msg, path.string(), line_num, "critical"});
                        goto next_file;  // 每个文件只报告一次
                    }
                }
                for (const auto& [pattern, msg] : warn_patterns) {
                    if (line.find(pattern) != std::string::npos) {
                        summary.warn++;
                        summary.findings.push_back({msg, path.string(), line_num, "warn"});
                        goto next_file;
                    }
                }
            }
            next_file:;
        }
    } catch (...) {}
    
    return summary;
}

// 🎯 对齐 OpenClaw: collectSkillInstallScanWarnings(entry) — 扫描 entry.skill.baseDir
static std::vector<std::string> collect_skill_install_scan_warnings(
    const std::string& skill_name,
    const std::string& skill_base_dir  // 对齐 OpenClaw: entry.skill.baseDir (非 workspaceDir)
) {
    std::vector<std::string> warnings;
    
    // 对齐 OpenClaw: const skillDir = path.resolve(entry.skill.baseDir)
    std::string skill_dir = skill_base_dir;
    // 规范化路径
    try {
        if (!skill_dir.empty() && std::filesystem::exists(skill_dir)) {
            skill_dir = std::filesystem::canonical(skill_dir).string();
        }
    } catch (...) {}
    
    try {
        auto summary = scan_directory_with_summary(skill_dir);
        if (summary.critical > 0) {
            // 对齐 OpenClaw: formatScanFindingDetail (line 44-52)
            std::string details;
            for (const auto& f : summary.findings) {
                if (f.severity != "critical") continue;
                if (!details.empty()) details += "; ";
                details += format_scan_finding_detail(skill_dir, f.file, f.line, f.message);
            }
            warnings.push_back(
                "WARNING: Skill \"" + skill_name + "\" contains dangerous code patterns: " + details
            );
        } else if (summary.warn > 0) {
            warnings.push_back(
                "Skill \"" + skill_name + "\" has " + std::to_string(summary.warn) +
                " suspicious code pattern(s). Run \"hunterclaw security audit --deep\" for details."
            );
        }
    } catch (const std::exception& e) {
        warnings.push_back(
            "Skill \"" + skill_name + "\" code safety scan failed (" + std::string(e.what()) +
            "). Installation continues; run \"hunterclaw security audit --deep\" after install."
        );
    }
    
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
    
    // ✅ 对齐 OpenClaw: 加载技能条目 (line 394-406)
    // loadWorkspaceSkillEntries + find entry by skill.name
    json entry_spec;
    {
        // 尝试从 workspace_dir 下的 skills.json 或 skill.json 加载
        std::vector<std::string> skill_files = {
            workspace_dir + "/skills.json",
            workspace_dir + "/.hunterclaw/skills.json",
        };
        for (const auto& sf : skill_files) {
            if (std::filesystem::exists(sf)) {
                try {
                    std::ifstream f(sf);
                    json skills_json;
                    f >> skills_json;
                    // 在 skills 数组中查找 (entry.skill.name 对齐 OpenClaw)
                    if (skills_json.is_array()) {
                        for (const auto& s : skills_json) {
                            // 对齐 OpenClaw: entry.skill.name
                            if (s.value("name", "") == skill_name) {
                                entry_spec = s;
                                break;
                            }
                        }
                    } else if (skills_json.is_object()) {
                        // 单个 skill 文件
                        if (skills_json.value("name", "") == skill_name) {
                            entry_spec = skills_json;
                        }
                    }
                    if (!entry_spec.empty()) break;
                } catch (...) {}
            }
        }
        // 如果还是找不到，尝试 workspace_dir/<skill_name>/skill.json
        if (entry_spec.empty()) {
            std::string single_skill_file = workspace_dir + "/" + skill_name + "/skill.json";
            if (std::filesystem::exists(single_skill_file)) {
                try {
                    std::ifstream f(single_skill_file);
                    f >> entry_spec;
                } catch (...) {}
            }
        }
    }

    // ✅ 对齐 OpenClaw: 查找安装规格 (line 407)
    // find_install_spec(entry, install_id)
    json spec;
    // 🎯 对齐 OpenClaw: entry not found 时直接返回 (line 394-406)
    if (entry_spec.empty()) {
        return SkillInstallResult{
            false, "Skill not found: " + skill_name, "", "", -1, {}
        };
    }
    
    // 🎯 对齐 OpenClaw: findInstallSpec (line 407)
    // 对齐 OpenClaw: entry.metadata?.install ?? []
    // 注意: OpenClaw 访问 entry.metadata.install，不是 entry.install
    if (!entry_spec.empty() && entry_spec.contains("metadata") &&
        entry_spec["metadata"].contains("install")) {
        const auto& install_arr = entry_spec["metadata"]["install"];
        if (install_arr.is_array()) {
            int idx = 0;
            for (const auto& req : install_arr) {
                // resolveInstallId: spec.id ?? `${spec.kind}-${index}`
                std::string resolved_id;
                if (req.contains("id") && req["id"].is_string()) {
                    resolved_id = req["id"].get<std::string>();
                    // trim
                    resolved_id.erase(0, resolved_id.find_first_not_of(" \t\n\r"));
                    if (!resolved_id.empty()) resolved_id.erase(resolved_id.find_last_not_of(" \t\n\r") + 1);
                }
                if (resolved_id.empty()) {
                    resolved_id = req.value("kind", "") + "-" + std::to_string(idx);
                }
                if (resolved_id == install_id) {
                    spec = req;
                    break;
                }
                ++idx;
            }
        }
    }
    // 兼容旧格式: 直接在 entry_spec 顶层的 install 字段
    if (spec.empty() && entry_spec.contains("install")) {
        const auto& install_arr = entry_spec["install"];
        if (install_arr.is_array()) {
            int idx = 0;
            for (const auto& req : install_arr) {
                std::string resolved_id;
                if (req.contains("id") && req["id"].is_string()) {
                    resolved_id = req["id"].get<std::string>();
                    resolved_id.erase(0, resolved_id.find_first_not_of(" \t\n\r"));
                    if (!resolved_id.empty()) resolved_id.erase(resolved_id.find_last_not_of(" \t\n\r") + 1);
                }
                if (resolved_id.empty()) {
                    resolved_id = req.value("kind", "") + "-" + std::to_string(idx);
                }
                if (resolved_id == install_id) { spec = req; break; }
                ++idx;
            }
        }
    }
    
    // 🎯 对齐 OpenClaw: 收集安全警告 (line 408)
    // 对齐 OpenClaw: collectSkillInstallScanWarnings(entry) — 扫描 entry.skill.baseDir
    // entry_spec 中的 baseDir 字段 (对应 entry.skill.baseDir)
    std::string skill_base_dir = entry_spec.value("baseDir", workspace_dir);
    auto warnings = collect_skill_install_scan_warnings(skill_name, skill_base_dir);
    
    if (spec.empty()) {
        return with_warnings(
            SkillInstallResult{false, "Installer not found: " + install_id, "", "", -1, {}},
            warnings
        );
    }
    
    // 🎯 对齐 OpenClaw: download 特殊处理 (line 421-424)
    if (spec.value("kind", "") == "download") {
        std::string config_dir;
        const char* home = getenv("HOME");
        if (home) {
            config_dir = std::string(home) + "/.hunterclaw";
        } else {
            config_dir = "./.hunterclaw";
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
    
    // 🎯 对齐 OpenClaw: resolveSkillsInstallPreferences(config) (line 426)
    SkillsInstallPreferences prefs;
    prefs.prefer_brew = true;   // 对齐 OpenClaw: preferBrew default = true
    prefs.node_manager = "npm"; // 对齐 OpenClaw: nodeManager default = "npm"
    if (config.contains("skills") && config["skills"].contains("install")) {
        const auto& install_cfg = config["skills"]["install"];
        // 对齐 OpenClaw: preferBrew
        if (install_cfg.contains("preferBrew") && install_cfg["preferBrew"].is_boolean()) {
            prefs.prefer_brew = install_cfg["preferBrew"].get<bool>();
        }
        // 对齐 OpenClaw: nodeManager (trim + lowercase)
        if (install_cfg.contains("nodeManager") && install_cfg["nodeManager"].is_string()) {
            std::string mgr = install_cfg["nodeManager"].get<std::string>();
            // trim
            mgr.erase(0, mgr.find_first_not_of(" \t\n\r"));
            if (!mgr.empty()) mgr.erase(mgr.find_last_not_of(" \t\n\r") + 1);
            // lowercase
            std::transform(mgr.begin(), mgr.end(), mgr.begin(), ::tolower);
            if (mgr == "pnpm" || mgr == "yarn" || mgr == "bun" || mgr == "npm") {
                prefs.node_manager = mgr;
            }
        }
    }
    
    // 🎯 对齐 OpenClaw: 构建安装命令 (line 427-439)
    auto command = build_install_command(spec, prefs);
    if (!command.error.empty()) {
        return with_warnings(
            create_install_failure(command.error),
            warnings
        );
    }
    
    // 🎯 对齐 OpenClaw: hasBinary("brew") ? "brew" : resolveBrewExecutable() (line 441)
    // 注意: OpenClaw 不管 preferBrew 都会查找 brewExe，preferBrew 只影响 prefs
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
