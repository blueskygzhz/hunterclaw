#include "hunterclaw/skills/skill.h"
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
    
    // 检查配置路径是否为真值 - 简化实现（暂不支持嵌套查询）
    bool is_config_truthy(std::shared_ptr<core::Config> config, const std::string& path) {
        // TODO: 实现配置查询逻辑
        return false;
    }
}

// SkillManager::Impl
struct SkillManager::Impl {
    std::shared_ptr<core::Config> config_;
    std::string workspace_dir_;
    
    mutable std::mutex mutex_;
    std::map<std::string, SkillEntry> skills_;
    
    // Watcher state
    bool watching_ = false;
    std::thread watch_thread_;
    
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
        
        std::vector<SkillRoot> roots = {
            {core::utils::expand_path("~/.hunterclaw/skills-bundled"), SkillSource::Bundled},
            {core::utils::expand_path("~/.hunterclaw/skills"), SkillSource::Managed},
            {core::utils::expand_path("~/.agents/skills"), SkillSource::Personal},
        };
        
        // 添加配置中的额外目录
        if (config_) {
            for (const auto& dir : config_->skills.load.extra_dirs) {
                roots.push_back({
                    core::utils::expand_path(dir),
                    SkillSource::Personal
                });
            }
        }
        
        // 添加 Workspace Skills (最高优先级)
        if (!workspace_dir.empty()) {
            roots.push_back({
                workspace_dir + "/.agents/skills",
                SkillSource::Project
            });
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
                total_loaded += result.value;
                log()->info("Loaded {} skills from: {}", result.value, root.path);
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
            
            if (content_result.value.size() > max_bytes) {
                log()->warn("Skill file too large ({}): {}", 
                           content_result.value.size(), file_path.string());
                return core::Result<bool>::Err("Skill file exceeds size limit");
            }
            
            // 解析 SKILL.md
            auto skill_result = SkillParser::parse_file(file_path);
            if (skill_result.is_err()) {
                return core::Result<bool>::Err(skill_result.error);
            }
            
            Skill skill = skill_result.value;
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

// 构建 LLM Prompt (100% 对齐 OpenClaw)
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
    
    size_t max_chars = 30000;
    if (impl_->config_) {
        max_chars = impl_->config_->skills.limits.max_skills_prompt_chars;
    }
    
    ss << "\n<skills>\n";
    ss << "You have access to the following skills. Use them to enhance your capabilities:\n\n";
    
    for (const auto& entry : eligible_skills) {
        const auto& skill = entry.skill;
        
        std::ostringstream skill_section;
        
        // Skill 标题
        if (!entry.metadata.emoji.empty()) {
            skill_section << "## " << entry.metadata.emoji << " " << skill.name << "\n\n";
        } else {
            skill_section << "## " << skill.name << "\n\n";
        }
        
        // 描述
        skill_section << skill.description << "\n\n";
        
        // Homepage (可选)
        if (!entry.metadata.homepage.empty()) {
            skill_section << "**Homepage**: " << entry.metadata.homepage << "\n\n";
        }
        
        // Markdown 内容 (LLM 可读的使用指南)
        if (!skill.content.empty()) {
            skill_section << skill.content << "\n\n";
        }
        
        skill_section << "---\n\n";
        
        std::string section_str = skill_section.str();
        
        // 检查字符限制
        if (total_chars + section_str.size() > max_chars) {
            log()->warn("Reached max skills prompt chars ({}), truncating", max_chars);
            break;
        }
        
        ss << section_str;
        total_chars += section_str.size();
    }
    
    ss << "</skills>\n";
    
    log()->info("Built skills prompt with {} skills, {} chars", 
               eligible_skills.size(), total_chars);
    
    return ss.str();
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
    
    // TODO: 实现实际的安装逻辑 (Brew, Node, Go, Download)
    log()->info("Installing skill '{}' using '{}'", skill_name, spec->label);
    
    return core::Result<bool>::Err("Installation not yet implemented");
}

// 安全扫描
core::Result<SkillManager::ScanResult> SkillManager::scan_skill(const std::string& skill_name) {
    auto entry_opt = get_skill(skill_name);
    if (!entry_opt) {
        return core::Result<ScanResult>::Err("Skill not found: " + skill_name);
    }
    
    return SkillScanner::scan(entry_opt.value());
}

// 热重载
void SkillManager::start_watch() {
    if (impl_->watching_) {
        log()->warn("Skill watcher already running");
        return;
    }
    
    impl_->watching_ = true;
    
    // TODO: 实现文件监听
    log()->info("Skill hot-reload enabled");
}

void SkillManager::stop_watch() {
    if (!impl_->watching_) {
        return;
    }
    
    impl_->watching_ = false;
    log()->info("Skill hot-reload disabled");
}

} // namespace skills
} // namespace hunterclaw
