#include "hunterclaw/skills/skill.h"
#include "hunterclaw/core/utils.h"
#include <spdlog/spdlog.h>
#include <thread>
#include <chrono>

namespace hunterclaw {
namespace skills {

namespace {
    auto log() {
        static auto logger = spdlog::get("skills");
        if (!logger) {
            logger = spdlog::stdout_logger_mt("skills");
        }
        return logger;
    }
    
    // 执行系统命令并捕获输出 (带超时控制)
    core::Result<std::string> exec_command(
        const std::string& cmd,
        core::Duration timeout = std::chrono::seconds(300)
    ) {
        log()->info("Executing: {}", cmd);
        
        // ✅ 使用 timeout 命令实现超时控制 (Linux/macOS)
        #ifndef _WIN32
        int timeout_secs = std::chrono::duration_cast<std::chrono::seconds>(timeout).count();
        std::string timeout_cmd = "timeout " + std::to_string(timeout_secs) + "s " + cmd;
        #else
        // Windows: 不支持 timeout,直接执行
        std::string timeout_cmd = cmd;
        #endif
        
        FILE* pipe = popen(timeout_cmd.c_str(), "r");
        if (!pipe) {
            return core::Result<std::string>::Err("Failed to execute command");
        }
        
        std::string output;
        char buffer[256];
        while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
            output += buffer;
        }
        
        int exit_code = pclose(pipe);
        
        // ✅ 超时检测 (exit code 124 = timeout)
        if (exit_code == 124 || exit_code == -1) {
            return core::Result<std::string>::Err(
                "Command timed out after " + std::to_string(timeout_secs) + "s"
            );
        }
        
        if (exit_code != 0) {
            return core::Result<std::string>::Err(
                "Command failed with exit code " + std::to_string(exit_code) + 
                ":\n" + output
            );
        }
        
        return core::Result<std::string>::Ok(output);
    }
    
    // 获取当前操作系统
    std::string get_current_os() {
        #ifdef __APPLE__
        return "darwin";
        #elif defined(_WIN32)
        return "win32";
        #elif defined(__linux__)
        return "linux";
        #else
        return "unknown";
        #endif
    }
}

// SkillInstaller::Impl
struct SkillInstaller::Impl {
    InstallPreferences prefs_;
    
    explicit Impl(const InstallPreferences& prefs) : prefs_(prefs) {}
    
    core::Result<bool> install(
        const SkillEntry& entry,
        const std::optional<std::string>& install_id,
        core::Duration timeout
    ) {
        if (entry.metadata.install.empty()) {
            return core::Result<bool>::Err("No install specifications available");
        }
        
        // 选择安装规格
        const SkillInstallSpec* spec = nullptr;
        
        if (install_id) {
            // 使用指定的 install ID
            for (const auto& s : entry.metadata.install) {
                if (s.id == *install_id) {
                    spec = &s;
                    break;
                }
            }
            
            if (!spec) {
                return core::Result<bool>::Err("Install ID not found: " + *install_id);
            }
        } else {
            // 自动选择最合适的
            spec = select_best_install_spec(entry.metadata.install);
            if (!spec) {
                return core::Result<bool>::Err("No suitable install spec found");
            }
        }
        
        log()->info("Installing skill '{}' using '{}'", entry.skill.name, spec->label);
        
        // 根据类型执行安装
        switch (spec->kind) {
            case SkillInstallSpec::Kind::Brew:
                return install_brew(*spec, timeout);
                
            case SkillInstallSpec::Kind::Node:
                return install_node(*spec, timeout);
                
            case SkillInstallSpec::Kind::Go:
                return install_go(*spec, timeout);
                
            case SkillInstallSpec::Kind::Uv:
                return install_uv(*spec, timeout);
                
            case SkillInstallSpec::Kind::Download:
                return install_download(*spec, timeout);
                
            default:
                return core::Result<bool>::Err("Unsupported install kind");
        }
    }
    
    bool check_dependencies(const SkillEntry& entry) const {
        // 检查所有必需的二进制文件是否存在
        for (const auto& bin : entry.metadata.requires.bins) {
            if (!check_bin_exists(bin)) {
                log()->debug("Missing required binary: {}", bin);
                return false;
            }
        }
        
        // 检查 any_bins (至少一个存在)
        if (!entry.metadata.requires.any_bins.empty()) {
            bool has_any = false;
            for (const auto& bin : entry.metadata.requires.any_bins) {
                if (check_bin_exists(bin)) {
                    has_any = true;
                    break;
                }
            }
            if (!has_any) {
                log()->debug("Missing any of required binaries");
                return false;
            }
        }
        
        return true;
    }
    
private:
    // 选择最合适的安装规格
    const SkillInstallSpec* select_best_install_spec(
        const std::vector<SkillInstallSpec>& specs
    ) const {
        std::string current_os = get_current_os();
        
        for (const auto& spec : specs) {
            // 检查 OS 兼容性
            if (!spec.os.empty()) {
                bool os_match = false;
                for (const auto& os : spec.os) {
                    if (os == current_os) {
                        os_match = true;
                        break;
                    }
                }
                if (!os_match) continue;
            }
            
            // 优先级: prefer_brew -> 第一个匹配的
            if (prefs_.prefer_brew && spec.kind == SkillInstallSpec::Kind::Brew) {
                return &spec;
            }
        }
        
        // 返回第一个兼容的
        for (const auto& spec : specs) {
            if (spec.os.empty() || std::find(spec.os.begin(), spec.os.end(), current_os) != spec.os.end()) {
                return &spec;
            }
        }
        
        return nullptr;
    }
    
    core::Result<bool> install_brew(const SkillInstallSpec& spec, core::Duration timeout) {
        if (spec.formula.empty()) {
            return core::Result<bool>::Err("Brew formula not specified");
        }
        
        // 检查 brew 是否安装
        if (!check_bin_exists("brew")) {
            return core::Result<bool>::Err("Homebrew not installed. Visit: https://brew.sh");
        }
        
        std::string cmd = "brew install " + spec.formula;
        auto result = exec_command(cmd, timeout);
        
        if (result.is_err()) {
            return core::Result<bool>::Err("Brew install failed: " + result.error);
        }
        
        log()->info("Successfully installed via Homebrew: {}", spec.formula);
        return core::Result<bool>::Ok(true);
    }
    
    core::Result<bool> install_node(const SkillInstallSpec& spec, core::Duration timeout) {
        if (spec.package.empty()) {
            return core::Result<bool>::Err("Node package not specified");
        }
        
        // 根据偏好选择包管理器
        std::string manager = prefs_.node_manager;
        
        if (!check_bin_exists(manager)) {
            log()->warn("{} not found, falling back to npm", manager);
            manager = "npm";
            
            if (!check_bin_exists("npm")) {
                return core::Result<bool>::Err("No Node package manager found (npm/pnpm/yarn/bun)");
            }
        }
        
        std::string cmd;
        if (manager == "npm" || manager == "pnpm" || manager == "yarn") {
            cmd = manager + " install -g " + spec.package;
        } else if (manager == "bun") {
            cmd = "bun add -g " + spec.package;
        } else {
            cmd = "npm install -g " + spec.package;
        }
        
        auto result = exec_command(cmd, timeout);
        
        if (result.is_err()) {
            return core::Result<bool>::Err("Node install failed: " + result.error);
        }
        
        log()->info("Successfully installed via {}: {}", manager, spec.package);
        return core::Result<bool>::Ok(true);
    }
    
    core::Result<bool> install_go(const SkillInstallSpec& spec, core::Duration timeout) {
        if (spec.module.empty()) {
            return core::Result<bool>::Err("Go module not specified");
        }
        
        if (!check_bin_exists("go")) {
            return core::Result<bool>::Err("Go not installed. Visit: https://go.dev");
        }
        
        std::string cmd = "go install " + spec.module;
        auto result = exec_command(cmd, timeout);
        
        if (result.is_err()) {
            return core::Result<bool>::Err("Go install failed: " + result.error);
        }
        
        log()->info("Successfully installed via Go: {}", spec.module);
        return core::Result<bool>::Ok(true);
    }
    
    core::Result<bool> install_uv(const SkillInstallSpec& spec, core::Duration timeout) {
        if (spec.package.empty()) {
            return core::Result<bool>::Err("Python package not specified");
        }
        
        if (!check_bin_exists("uv")) {
            return core::Result<bool>::Err("uv not installed. Visit: https://docs.astral.sh/uv");
        }
        
        std::string cmd = "uv tool install " + spec.package;
        auto result = exec_command(cmd, timeout);
        
        if (result.is_err()) {
            return core::Result<bool>::Err("uv install failed: " + result.error);
        }
        
        log()->info("Successfully installed via uv: {}", spec.package);
        return core::Result<bool>::Ok(true);
    }
    
    core::Result<bool> install_download(const SkillInstallSpec& spec, core::Duration timeout) {
        if (spec.url.empty()) {
            return core::Result<bool>::Err("Download URL not specified");
        }
        
        log()->info("Downloading from: {}", spec.url);
        
        // 1. 确定目标目录
        fs::path target_dir;
        if (!spec.target_dir.empty()) {
            target_dir = fs::path(spec.target_dir);
        } else {
            const char* home = std::getenv("HOME");
            if (!home) {
                return core::Result<bool>::Err("HOME not set");
            }
            target_dir = fs::path(home) / ".hunterclaw" / "bin";
        }
        
        if (!fs::exists(target_dir)) {
            std::error_code ec;
            fs::create_directories(target_dir, ec);
            if (ec) {
                return core::Result<bool>::Err("Failed to create dir: " + ec.message());
            }
        }
        
        // 2. 下载文件
        std::string filename = spec.archive;
        if (filename.empty()) {
            size_t pos = spec.url.find_last_of('/');
            filename = (pos != std::string::npos) ? spec.url.substr(pos + 1) : "download.bin";
        }
        
        fs::path temp_file = target_dir / filename;
        std::string cmd;
        
        if (check_bin_exists("curl")) {
            cmd = "curl -L -o \"" + temp_file.string() + "\" \"" + spec.url + "\"";
        } else if (check_bin_exists("wget")) {
            cmd = "wget -O \"" + temp_file.string() + "\" \"" + spec.url + "\"";
        } else {
            return core::Result<bool>::Err("curl/wget not found");
        }
        
        auto result = exec_command(cmd, timeout);
        if (result.is_err()) {
            return core::Result<bool>::Err("Download failed: " + result.error);
        }
        
        // 3. 解压
        if (spec.extract) {
            log()->info("Extracting: {}", temp_file.string());
            std::string ext = temp_file.extension().string();
            std::string extract_cmd;
            
            if (ext == ".tar" || ext == ".gz" || ext == ".tgz" || ext == ".bz2") {
                extract_cmd = "tar -xf \"" + temp_file.string() + "\" -C \"" + target_dir.string() + "\"";
                if (spec.strip_components > 0) {
                    extract_cmd += " --strip-components=" + std::to_string(spec.strip_components);
                }
            } else if (ext == ".zip") {
                if (!check_bin_exists("unzip")) {
                    return core::Result<bool>::Err("unzip not found");
                }
                extract_cmd = "unzip -q -o \"" + temp_file.string() + "\" -d \"" + target_dir.string() + "\"";
            } else {
                return core::Result<bool>::Err("Unsupported format: " + ext);
            }
            
            auto extract_result = exec_command(extract_cmd, timeout);
            if (extract_result.is_err()) {
                return core::Result<bool>::Err("Extract failed: " + extract_result.error);
            }
            
            fs::remove(temp_file);
        } else {
            #ifndef _WIN32
            exec_command("chmod +x \"" + temp_file.string() + "\"", std::chrono::seconds(5));
            #endif
        }
        
        log()->info("Installed to: {}", target_dir.string());
        return core::Result<bool>::Ok(true);
    }
    
    bool check_bin_exists(const std::string& bin_name) const {
        std::string path_env = std::getenv("PATH") ? std::getenv("PATH") : "";
        
        #ifdef _WIN32
        const char delimiter = ';';
        std::vector<std::string> extensions = {".exe", ".bat", ".cmd"};
        #else
        const char delimiter = ':';
        std::vector<std::string> extensions = {""};
        #endif
        
        std::istringstream path_stream(path_env);
        std::string path_dir;
        
        while (std::getline(path_stream, path_dir, delimiter)) {
            for (const auto& ext : extensions) {
                fs::path full_path = fs::path(path_dir) / (bin_name + ext);
                if (fs::exists(full_path) && !fs::is_directory(full_path)) {
                    return true;
                }
            }
        }
        
        return false;
    }
};

// SkillInstaller 实现
SkillInstaller::SkillInstaller(const InstallPreferences& prefs)
    : impl_(std::make_unique<Impl>(prefs)) {}

SkillInstaller::~SkillInstaller() = default;

core::Result<bool> SkillInstaller::install(
    const SkillEntry& entry,
    const std::optional<std::string>& install_id,
    core::Duration timeout
) {
    return impl_->install(entry, install_id, timeout);
}

bool SkillInstaller::check_dependencies(const SkillEntry& entry) const {
    return impl_->check_dependencies(entry);
}

} // namespace skills
} // namespace hunterclaw
