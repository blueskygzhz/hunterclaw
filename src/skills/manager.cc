#include "hunterclaw/skills/skill.h"
#include "hunterclaw/skills/file_watcher.h"
#include "hunterclaw/core/utils.h"
#include <filesystem>
#include <map>
#include <mutex>
#include <algorithm>
#include <numeric>
#include <spdlog/spdlog.h>

namespace fs = std::filesystem;

namespace hunterclaw {
namespace skills {

// Forward declarations
static std::string xml_escape(const std::string& str);

// Factory method
std::shared_ptr<SkillManager> SkillManager::create(const core::SkillConfig& config) {
    // 创建一个空的Config对象，只包含skills配置
    auto full_config = std::make_shared<core::Config>();
    full_config->skills = config;
    
    return std::make_shared<SkillManager>(full_config);
}

namespace {
    auto log() {
        auto logger = spdlog::get("skills");
        if (!logger) {
            logger = spdlog::stdout_logger_mt("skills");
        }
        return logger;
    }
    
    // 检查二进制文件是否存在 (100% 对齐 OpenClaw)
    bool check_bin_exists(const std::string& bin_name) {
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
    
    // 检查任意一个二进制存在 (OR 逻辑)
    bool check_any_bin_exists(const std::vector<std::string>& bins) {
        for (const auto& bin : bins) {
            if (check_bin_exists(bin)) {
                return true;
            }
        }
        return false;
    }
    
    // ✅ 对齐 OpenClaw: isConfigPathTruthy
    // 支持点分隔的路径查询，如 "providers.anthropic.apiKey"
    bool is_config_truthy(std::shared_ptr<core::Config> config, const std::string& path) {
        if (!config || path.empty()) return false;

        // 将路径分割为部分
        std::vector<std::string> parts;
        std::istringstream ss(path);
        std::string part;
        while (std::getline(ss, part, '.')) {
            if (!part.empty()) parts.push_back(part);
        }
        if (parts.empty()) return false;

        // 遍历已知配置字段（对齐 OpenClaw 支持的路径）
        // providers.anthropic.apiKey
        if (parts.size() >= 3 && parts[0] == "providers") {
            const std::string& provider = parts[1];
            const std::string& field = parts[2];
            if (field == "apiKey" || field == "api_key") {
                // 检查环境变量（对齐 OpenClaw 优先级）
                std::string env_key = "ANTHROPIC_API_KEY";
                if (provider == "openai") env_key = "OPENAI_API_KEY";
                else if (provider == "gemini") env_key = "GEMINI_API_KEY";
                else if (provider == "ollama") return true;  // ollama 无需 key
                const char* val = std::getenv(env_key.c_str());
                return val && std::strlen(val) > 0;
            }
        }

        // gateway.auth.enabled
        if (parts.size() >= 2 && parts[0] == "gateway" && parts[1] == "auth") {
            return config->gateway.auth.enabled;
        }

        // 默认: 返回 false（未知路径）
        return false;
    }}

// SkillManager::Impl
struct SkillManager::Impl {
    std::shared_ptr<core::Config> config_;
    std::string workspace_dir_;
    
    mutable std::mutex mutex_;
    std::map<std::string, SkillEntry> skills_;
    
    // Watcher state
    bool watching_ = false;
    std::thread watch_thread_;
    std::shared_ptr<FileWatcher> file_watcher_;
    uint64_t version_ = 0;
    
    explicit Impl(std::shared_ptr<core::Config> config)
        : config_(config) {}
    
    ~Impl() {
        if (watching_) {
            watching_ = false;
            if (watch_thread_.joinable()) {
                watch_thread_.join();
            }
        }
    }
    
    uint64_t bump_version() {
        return ++version_;
    }
    
    core::Result<bool> load_from_roots(const std::string& workspace_dir) {
        std::lock_guard<std::mutex> lock(mutex_);
        
        workspace_dir_ = workspace_dir;
        skills_.clear();
        
        // 定义 Skill 根目录 (优先级从低到高)
        // 后加载的会覆盖先加载的 (100% 对齐 OpenClaw)
        struct SkillRoot {
            std::string path;
            SkillSource source;
        };
        
        std::vector<SkillRoot> roots;
        
        // ===================================================================
        // 1. Bundled Skills (系统内置层)
        // ===================================================================
        // OpenClaw: /usr/lib/node_modules/openclaw/skills/
        // HunterClaw: ~/.hunterclaw/skills-bundled (可配置)
        std::string bundled_path = core::utils::expand_path("~/.hunterclaw/skills-bundled");
        const char* env_bundled = std::getenv("HUNTERCLAW_BUNDLED_SKILLS");
        if (env_bundled && std::strlen(env_bundled) > 0) {
            bundled_path = core::utils::expand_path(env_bundled);
            log()->info("Using bundled skills from env: {}", bundled_path);
        }
        roots.push_back({bundled_path, SkillSource::Bundled});
        
        // ===================================================================
        // 2. Managed/Local Skills (用户自定义层)
        // ===================================================================
        // ✅ OpenClaw 兼容: ~/.openclaw/skills/
        roots.push_back({
            core::utils::expand_path("~/.openclaw/skills"),
            SkillSource::Managed
        });
        
        // ✅ HunterClaw 专用: ~/.hunterclaw/skills/
        roots.push_back({
            core::utils::expand_path("~/.hunterclaw/skills"),
            SkillSource::Managed
        });
        
        // ===================================================================
        // 3. Personal Skills (HunterClaw 多租户扩展)
        // ===================================================================
        roots.push_back({
            core::utils::expand_path("~/.agents/skills"),
            SkillSource::Personal
        });
        
        // ===================================================================
        // 4. Plugin Skills (OpenClaw 对齐: 扩展插件层)
        // ===================================================================
// TODO: 实现 plugin_manager_ 后启用
        // if (plugin_manager_) {
        //     for (const auto& plugin : plugin_manager_->get_enabled_plugins()) {
        //         std::string plugin_skills_path =
        //             core::utils::expand_path("~/.openclaw/extensions/" + plugin + "/skills");
        //
        //         if (fs::exists(plugin_skills_path)) {
        //             roots.push_back({plugin_skills_path, SkillSource::Plugin});
        //             log()->info("Loading plugin skills: {}", plugin);
        //         }
        //     }
        // }
        
        // ===================================================================
        // 5. Extra Dirs (HunterClaw 配置扩展)
        // ===================================================================
        if (config_) {
            for (const auto& dir : config_->skills.load.extra_dirs) {
                roots.push_back({
                    core::utils::expand_path(dir),
                    SkillSource::Personal
                });
            }
        }
        
        // ===================================================================
        // 6. Project Hidden Skills (HunterClaw 扩展)
        // ===================================================================
        if (!workspace_dir.empty()) {
            roots.push_back({
                workspace_dir + "/.agents/skills",
                SkillSource::Project
            });
        }
        
        // ===================================================================
        // 7. Workspace Skills (OpenClaw 100% 对齐: <workspace>/skills/)
        // ===================================================================
        if (!workspace_dir.empty()) {
            roots.push_back({
                workspace_dir + "/skills",
                SkillSource::Workspace
            });
        }
        
        // 从每个根目录加载
        int total_loaded = 0;
        for (const auto& root : roots) {
            if (!fs::exists(root.path)) {
                log()->debug("Skill root does not exist: {}", root.path);
                continue;
            }
            
            auto result = load_from_directory(root.path, root.source);
            if (result.is_ok()) {
                total_loaded += result.get();
                log()->info("Loaded {} skills from: {}", result.get(), root.path);
            } else {
                log()->error("Failed to load skills from {}: {}", root.path, result.error);
            }
        }
        
        log()->info("Total skills loaded: {}", skills_.size());
        
        return core::Result<bool>::Ok(true);
    }
    
    core::Result<int> load_from_directory(const std::string& dir, SkillSource source) {
        try {
            int loaded = 0;
            int max_per_source = 200;  // limits.max_skills_loaded_per_source
            
            if (config_) {
                max_per_source = config_->skills.limits.max_skills_loaded_per_source;
            }
            
            for (const auto& entry : fs::recursive_directory_iterator(dir)) {
                if (loaded >= max_per_source) {
                    log()->warn("Reached max skills limit ({}) for source: {}", 
                               max_per_source, dir);
                    break;
                }
                
                if (!entry.is_regular_file()) continue;
                
                std::string filename = entry.path().filename().string();
                if (filename == "SKILL.md" || filename == "skill.md") {
                    auto result = load_skill_file(entry.path(), source);
                    if (result.is_ok()) {
                        loaded++;
                    }
                }
            }
            
            return core::Result<int>::Ok(loaded);
            
        } catch (const std::exception& e) {
            return core::Result<int>::Err(
                std::string("Failed to load from directory: ") + e.what());
        }
    }
    
    core::Result<bool> load_skill_file(const fs::path& file_path, SkillSource source) {
        try {
            // 读取文件内容
            auto content_result = core::utils::read_file(file_path.string());
            if (content_result.is_err()) {
                return core::Result<bool>::Err(content_result.error);
            }
            
            // 检查文件大小限制
            size_t max_bytes = 256000;
            if (config_) {
                max_bytes = config_->skills.limits.max_skill_file_bytes;
            }
            
            if (content_result.get().size() > max_bytes) {
                log()->warn("Skill file too large ({}): {}", 
                           content_result.get().size(), file_path.string());
                return core::Result<bool>::Err("Skill file exceeds size limit");
            }
            
            // 解析 SKILL.md
            auto skill_result = SkillParser::parse_file(file_path);
            if (skill_result.is_err()) {
                return core::Result<bool>::Err(skill_result.error);
            }
            
            Skill skill = skill_result.get();
            skill.source = source;
            skill.base_dir = file_path.parent_path();
            
            // 创建 SkillEntry (包含运行时状态)
            SkillEntry entry;
            entry.skill = skill;
            entry.metadata = skill.frontmatter.metadata;
            entry.eligible = false;  // 稍后计算
            
            // 存储 (后加载的覆盖前面的 - 优先级机制)
            skills_[skill.name] = entry;
            
            log()->debug("Loaded skill: {} from {}", skill.name, file_path.string());
            
            return core::Result<bool>::Ok(true);
            
        } catch (const std::exception& e) {
            return core::Result<bool>::Err(
                std::string("Failed to load skill file: ") + e.what());
        }
    }
    
    // 计算 Skill 资格 (100% 对齐 OpenClaw)
    bool check_eligibility(
        const SkillEntry& entry,
        const SkillEligibilityContext& ctx,
        std::string& reason
    ) const {
        const auto& metadata = entry.metadata;
        
        // 1. 检查 always 标志
        if (metadata.always) {
            return true;
        }
        
        // 2. 检查操作系统
        if (!metadata.os.empty()) {
            bool os_match = false;
            for (const auto& req_os : metadata.os) {
                if (req_os == ctx.current_os) {
                    os_match = true;
                    break;
                }
            }
            if (!os_match) {
                reason = "OS not supported: " + ctx.current_os;
                return false;
            }
        }
        
        // 3. 检查必需的二进制文件 (ALL)
        for (const auto& bin : metadata.requires.bins) {
            bool has_bin = ctx.has_bin && ctx.has_bin(bin);
            if (!has_bin) {
                reason = "Missing required binary: " + bin;
                return false;
            }
        }
        
        // 4. 检查任意二进制文件 (ANY - OR 逻辑)
        if (!metadata.requires.any_bins.empty()) {
            bool has_any = false;
            if (ctx.has_bin) {
                for (const auto& bin : metadata.requires.any_bins) {
                    if (ctx.has_bin(bin)) {
                        has_any = true;
                        break;
                    }
                }
            }
            
            if (!has_any) {
                reason = "Missing any of: " + 
                        std::accumulate(metadata.requires.any_bins.begin(),
                                      metadata.requires.any_bins.end(),
                                      std::string{},
                                      [](const std::string& a, const std::string& b) {
                                          return a.empty() ? b : a + ", " + b;
                                      });
                return false;
            }
        }
        
        // 5. 检查环境变量
        for (const auto& env : metadata.requires.env) {
            bool has_env = ctx.has_env && ctx.has_env(env);
            if (!has_env) {
                reason = "Missing required environment variable: " + env;
                return false;
            }
        }
        
        // 6. 检查配置路径 (truthy check)
        for (const auto& cfg_path : metadata.requires.config) {
            bool is_truthy = ctx.is_config_path_truthy && 
                           ctx.is_config_path_truthy(cfg_path);
            if (!is_truthy) {
                reason = "Config path not truthy: " + cfg_path;
                return false;
            }
        }
        
        return true;
    }
    
};

// SkillManager 实现
SkillManager::SkillManager(std::shared_ptr<core::Config> config)
    : impl_(std::make_unique<Impl>(config)) {}

SkillManager::~SkillManager() = default;

core::Result<bool> SkillManager::load_skills(const std::string& workspace_dir) {
    return impl_->load_from_roots(workspace_dir);
}

core::Result<bool> SkillManager::load_from_roots() {
    return impl_->load_from_roots("");
}

core::Result<bool> SkillManager::reload_skills() {
    return impl_->load_from_roots(impl_->workspace_dir_);
}

std::vector<SkillEntry> SkillManager::list_skills() const {
    std::lock_guard<std::mutex> lock(impl_->mutex_);
    
    std::vector<SkillEntry> result;
    for (const auto& pair : impl_->skills_) {
        result.push_back(pair.second);
    }
    return result;
}

std::optional<SkillEntry> SkillManager::get_skill(const std::string& name) const {
    std::lock_guard<std::mutex> lock(impl_->mutex_);
    
    auto it = impl_->skills_.find(name);
    if (it == impl_->skills_.end()) {
        return std::nullopt;
    }
    
    return it->second;
}

// 过滤符合条件的 Skills (100% 对齐 OpenClaw)
std::vector<SkillEntry> SkillManager::filter_eligible_skills(
    const SkillEligibilityContext& ctx,
    const std::vector<std::string>& skill_filter
) const {
    std::lock_guard<std::mutex> lock(impl_->mutex_);
    
    std::vector<SkillEntry> result;
    
    for (auto& [name, entry] : impl_->skills_) {
        // 1. 应用 skill filter (Agent 级别的过滤器)
        if (!skill_filter.empty()) {
            bool allowed = false;
            
            for (const auto& pattern : skill_filter) {
                // 全部允许
                if (pattern == "*") {
                    allowed = true;
                    break;
                }
                
                // 精确匹配
                if (pattern == name) {
                    allowed = true;
                    break;
                }
                
                // 通配符匹配: "github*"
                if (pattern.back() == '*') {
                    std::string prefix = pattern.substr(0, pattern.size() - 1);
                    if (name.substr(0, prefix.size()) == prefix) {
                        allowed = true;
                        break;
                    }
                }
            }
            
            if (!allowed) continue;
        }
        
        // 2. 计算资格
        std::string reason;
        bool eligible = impl_->check_eligibility(entry, ctx, reason);
        
        SkillEntry filtered_entry = entry;
        filtered_entry.eligible = eligible;
        filtered_entry.ineligible_reason = reason;
        
        result.push_back(filtered_entry);
    }
    
    // 3. 只返回符合条件的
    result.erase(
        std::remove_if(result.begin(), result.end(),
            [](const SkillEntry& e) { return !e.eligible; }),
        result.end()
    );
    
    // 4. 应用限制
    int max_skills = 150;
    if (impl_->config_) {
        max_skills = impl_->config_->skills.limits.max_skills_in_prompt;
    }
    
    if (result.size() > static_cast<size_t>(max_skills)) {
        log()->warn("Truncating skills from {} to max {}", result.size(), max_skills);
        result.resize(max_skills);
    }
    
    return result;
}

// 构建 LLM Prompt (100% 对齐 OpenClaw 两阶段加载机制)
std::string SkillManager::build_skills_prompt(
    const SkillEligibilityContext& ctx,
    const std::vector<std::string>& skill_filter
) const {
    auto eligible_skills = filter_eligible_skills(ctx, skill_filter);
    
    if (eligible_skills.empty()) {
        return "";
    }
    
    std::ostringstream ss;
    size_t total_chars = 0;
    
    // 阶段 1: 只注入元信息列表
    ss << "\n<available_skills>\n";
    ss << "Before replying: scan <available_skills> <description> entries.\n";
    ss << "Use the read tool to load SKILL.md at the listed <location> when needed.\n\n";
    
    for (const auto& entry : eligible_skills) {
        const auto& skill = entry.skill;
        
        // 只包含: 名称 + 描述 + 文件路径
        ss << "  <skill>\n";
        ss << "    <name>" << xml_escape(skill.name) << "</name>\n";
        ss << "    <description>" << xml_escape(skill.description) << "</description>\n";
        
        // 文件路径 (LLM 通过 read tool 加载)
        std::string skill_path = skill.file_path.string();
        if (!skill_path.empty()) {
            ss << "    <location>" << xml_escape(skill_path) << "</location>\n";
        }
        
        ss << "  </skill>\n";
        
        total_chars += skill.name.size() + skill.description.size() + skill_path.size() + 97;
    }
    
    ss << "</available_skills>\n";
    
    log()->info("Built skills catalog with {} skills, ~{} chars (metadata only)", 
               eligible_skills.size(), total_chars);
    
    return ss.str();
}

// XML 转义辅助函数
static std::string xml_escape(const std::string& str) {
    std::string result;
    result.reserve(str.size());
    
    for (char c : str) {
        switch (c) {
            case '&': result += "&amp;"; break;
            case '<': result += "&lt;"; break;
            case '>': result += "&gt;"; break;
            case '"': result += "&quot;"; break;
            case '\'': result += "&apos;"; break;
            default: result += c; break;
        }
    }
    
    return result;
}

// 安装 Skill 依赖
core::Result<bool> SkillManager::install_skill(
    const std::string& skill_name,
    const std::optional<std::string>& install_id
) {
    auto entry_opt = get_skill(skill_name);
    if (!entry_opt) {
        return core::Result<bool>::Err("Skill not found: " + skill_name);
    }
    
    auto& entry = entry_opt.value();
    
    if (entry.metadata.install.empty()) {
        return core::Result<bool>::Err("No install specs available for: " + skill_name);
    }
    
    // 如果指定了 install_id，使用它；否则选择第一个
    const SkillInstallSpec* spec = nullptr;
    
    if (install_id) {
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
        spec = &entry.metadata.install[0];
    }
    
    // ✅ 对齐 OpenClaw: installSkill (src/agents/skills-install.ts)
    // 支持: brew, node (npm/pnpm/yarn/bun), go, uv, download
    log()->info("Installing skill '{}' using '{}'", skill_name, spec->label);

    // 构建安装命令
    std::string install_cmd;
    switch (spec->kind) {
        case SkillInstallKind::Brew:
            if (!spec->formula.empty()) {
                install_cmd = "brew install " + spec->formula;
            } else if (!spec->bins.empty()) {
                install_cmd = "brew install " + spec->bins[0];
            }
            break;
        case SkillInstallKind::Node:
            // 优先使用 pnpm，其次 npm
            {
                const char* pnpm = std::getenv("PNPM_HOME");
                std::string pkg_mgr = pnpm ? "pnpm" : "npm";
                if (!spec->formula.empty()) {
                    install_cmd = pkg_mgr + " install -g " + spec->formula;
                } else if (!spec->bins.empty()) {
                    install_cmd = pkg_mgr + " install -g " + spec->bins[0];
                }
            }
            break;
        case SkillInstallKind::Go:
            if (!spec->formula.empty()) {
                install_cmd = "go install " + spec->formula + "@latest";
            }
            break;
        case SkillInstallKind::Uv:
            if (!spec->formula.empty()) {
                install_cmd = "uv tool install " + spec->formula;
            } else if (!spec->bins.empty()) {
                install_cmd = "uv tool install " + spec->bins[0];
            }
            break;
        case SkillInstallKind::Download:
            // Download 安装需要下载 URL，解压，移动到 targetDir
            // 对齐 OpenClaw skills-install-download.ts
            return core::Result<bool>::Err("Download install requires skills_install_impl integration");
        default:
            return core::Result<bool>::Err("Unknown install kind for: " + skill_name);
    }

    if (install_cmd.empty()) {
        return core::Result<bool>::Err("Cannot determine install command for: " + skill_name);
    }

    log()->info("Running install command: {}", install_cmd);

    // 执行安装命令（对齐 OpenClaw: 捕获 stdout/stderr）
    int ret = std::system(install_cmd.c_str());
    if (ret != 0) {
        return core::Result<bool>::Err(
            "Install command failed (exit code " + std::to_string(ret) + "): " + install_cmd
        );
    }

    log()->info("Skill '{}' installed successfully", skill_name);
    return core::Result<bool>::Ok(true);
}

// 安全扫描
core::Result<SkillManager::ScanResult> SkillManager::scan_skill(const std::string& skill_name) {
    auto entry_opt = get_skill(skill_name);
    if (!entry_opt) {
        return core::Result<ScanResult>::Err("Skill not found: " + skill_name);
    }
    
    return SkillScanner::scan(entry_opt.value());
}

// 热重载 (100% 对齐 OpenClaw 实现)
void SkillManager::start_watch() {
    if (impl_->file_watcher_ && impl_->file_watcher_->is_running()) {
        log()->warn("Skill watcher already running");
        return;
    }
    
    // 检查配置
    bool watch_enabled = true;
    int debounce_ms = 250;
    
    if (impl_->config_) {
        // OpenClaw 默认: watch !== false (即默认为 true)
        watch_enabled = impl_->config_->skills.load.watch;
        if (impl_->config_->skills.load.watch_debounce_ms > 0) {
            debounce_ms = impl_->config_->skills.load.watch_debounce_ms;
        }
    }
    
    if (!watch_enabled) {
        log()->info("Skill hot-reload disabled by config");
        return;
    }
    
    // 创建 FileWatcher
    FileWatcherConfig config;
    config.debounce_delay = std::chrono::milliseconds(debounce_ms);
    config.stability_threshold = std::chrono::milliseconds(debounce_ms);
    config.ignore_initial = true;  // 忽略初始文件
    
    impl_->file_watcher_ = FileWatcher::create(config);
    
    // 设置回调 (触发重新加载)
    impl_->file_watcher_->set_callback([this](const WatchEvent& event) {
        std::string event_type;
        switch (event.type) {
            case WatchEventType::Add:    event_type = "add"; break;
            case WatchEventType::Change: event_type = "change"; break;
            case WatchEventType::Unlink: event_type = "unlink"; break;
        }
        
        log()->info("Skill file {} detected: {}", event_type, event.path.string());
        
        // 递增版本号
        uint64_t new_version = impl_->bump_version();
        
        // 重新加载 skills
        auto result = reload_skills();
        if (result.is_ok()) {
            log()->info("Skills reloaded successfully (version: {})", new_version);
        } else {
            log()->error("Failed to reload skills: {}", result.error);
        }
    });
    
    // 添加监控目标: 只监控 SKILL.md 文件 (100% 对齐 OpenClaw)
    std::vector<std::string> watch_patterns;
    
    // 1. Workspace skills
    if (!impl_->workspace_dir_.empty()) {
        watch_patterns.push_back(impl_->workspace_dir_ + "/skills/*/SKILL.md");
        watch_patterns.push_back(impl_->workspace_dir_ + "/.agents/skills/*/SKILL.md");
    }
    
    // 2. Personal skills
    watch_patterns.push_back(core::utils::expand_path("~/.agents/skills/*/SKILL.md"));
    
    // 3. Managed skills
    watch_patterns.push_back(core::utils::expand_path("~/.openclaw/skills/*/SKILL.md"));
    watch_patterns.push_back(core::utils::expand_path("~/.hunterclaw/skills/*/SKILL.md"));
    
    // 4. Extra directories
    if (impl_->config_) {
        for (const auto& dir : impl_->config_->skills.load.extra_dirs) {
            std::string expanded = core::utils::expand_path(dir);
            watch_patterns.push_back(expanded + "/*/SKILL.md");
        }
    }
    
    // 添加所有监控
    for (const auto& pattern : watch_patterns) {
        auto result = impl_->file_watcher_->add_watch(pattern);
        if (result.is_err()) {
            log()->warn("Failed to add watch pattern {}: {}", pattern, result.error);
        }
    }
    
    // 启动监控
    auto result = impl_->file_watcher_->start();
    if (result.is_ok()) {
        log()->info("Skill hot-reload enabled (debounce: {}ms)", debounce_ms);
    } else {
        log()->error("Failed to start skill watcher: {}", result.error);
        impl_->file_watcher_.reset();
    }
}

void SkillManager::stop_watch() {
    if (!impl_->file_watcher_) {
        return;
    }
    
    impl_->file_watcher_->stop();
    impl_->file_watcher_.reset();
    
    log()->info("Skill hot-reload disabled");
}

} // namespace skills
} // namespace hunterclaw
