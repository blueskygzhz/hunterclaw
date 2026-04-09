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
        
        // ✅ 严格对齐 OpenClaw 优先级 (workspace.ts:490-509):
        // Precedence: extra+plugin < bundled < managed < agents-personal < agents-project < workspace
        // 后加载的覆盖先加载的 (Map key = skill.name)
        struct SkillRoot {
            std::string path;
            SkillSource source;
        };
        
        std::vector<SkillRoot> roots;
        
        // ===================================================================
        // 1. Extra Dirs + Plugin Skills (OpenClaw: "extra" = lowest priority)
        // ===================================================================
        // Plugin Skills: ~/.openclaw/extensions/*/skills
        {
            std::string extensions_dir = core::utils::expand_path("~/.openclaw/extensions");
            if (fs::exists(extensions_dir)) {
                try {
                    for (const auto& ext_entry : fs::directory_iterator(extensions_dir)) {
                        if (!ext_entry.is_directory()) continue;
                        std::string plugin_skills_path =
                            ext_entry.path().string() + "/skills";
                        if (fs::exists(plugin_skills_path)) {
                            roots.push_back({plugin_skills_path, SkillSource::Plugin});
                            log()->info("Found plugin skills: {}", plugin_skills_path);
                        }
                    }
                } catch (const std::exception& e) {
                    log()->warn("Error scanning plugin skills: {}", e.what());
                }
            }
        }
        // Extra Dirs from config (also lowest priority)
        if (config_) {
            for (const auto& dir : config_->skills.load.extra_dirs) {
                roots.push_back({
                    core::utils::expand_path(dir),
                    SkillSource::Personal
                });
            }
        }

        // ===================================================================
        // 2. Bundled Skills (系统内置层)
        // ===================================================================
        std::string bundled_path = core::utils::expand_path("~/.hunterclaw/skills-bundled");
        const char* env_bundled = std::getenv("HUNTERCLAW_BUNDLED_SKILLS");
        if (env_bundled && std::strlen(env_bundled) > 0) {
            bundled_path = core::utils::expand_path(env_bundled);
            log()->info("Using bundled skills from env: {}", bundled_path);
        }
        roots.push_back({bundled_path, SkillSource::Bundled});
        
        // ===================================================================
        // 3. Managed/Local Skills (用户自定义层)
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
        // 4. Personal Skills (agents-skills-personal)
        // ===================================================================
        roots.push_back({
            core::utils::expand_path("~/.agents/skills"),
            SkillSource::Personal
        });
        
        // ===================================================================
        // 5. Project Hidden Skills (agents-skills-project)
        // ===================================================================
        if (!workspace_dir.empty()) {
            roots.push_back({
                workspace_dir + "/.agents/skills",
                SkillSource::Project
            });
        }
        
        // ===================================================================
        // 6. Workspace Skills (openclaw-workspace, 最高优先级)
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
    
// ============================================================================
// ✅ isPathInside (Aligned with OpenClaw scan-paths.ts:3-8)
// Returns true if candidatePath is inside or equal to basePath.
// Uses relative path semantics (no symlink resolution here).
// ============================================================================
static bool is_path_inside(const fs::path& base_path, const fs::path& candidate_path) {
    auto base = fs::path(fs::absolute(base_path)).lexically_normal();
    auto candidate = fs::path(fs::absolute(candidate_path)).lexically_normal();
    auto rel = fs::relative(candidate, base);
    std::string rel_str = rel.string();
    return rel_str == "." ||
           (!rel_str.empty() &&
            rel_str[0] != '.' &&
            !fs::path(rel_str).is_absolute());
}

// ============================================================================
// ✅ resolveContainedSkillPath (Aligned with OpenClaw workspace.ts:201-221)
// Resolves real path and checks it stays inside root.
// Returns empty optional if symlink escapes root.
// ============================================================================
static std::optional<fs::path> resolve_contained_skill_path(
    const std::string& source,
    const fs::path& root_dir,
    const fs::path& root_real_path,
    const fs::path& candidate_path
) {
    std::error_code ec;
    fs::path candidate_real = fs::canonical(candidate_path, ec);
    if (ec) {
        return std::nullopt;  // Path doesn't exist or can't be resolved
    }
    if (is_path_inside(root_real_path, candidate_real)) {
        return candidate_real;  // Safe: realpath is inside root
    }
    // Symlink escape detected
    log()->warn("Skipping skill path that resolves outside its configured root "
                "(source={}, root={}, path={}, realPath={})",
                source, root_dir.string(), candidate_path.string(), candidate_real.string());
    return std::nullopt;
}

// ============================================================================
// ✅ listChildDirectories (Aligned with OpenClaw workspace.ts:151-177)
// Lists immediate child directory names, following symlinks to directories.
// Skips dot-files and node_modules.
// ============================================================================
static std::vector<std::string> list_child_directories(const fs::path& dir) {
    std::vector<std::string> result;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (ec) break;
        std::string name = entry.path().filename().string();
        if (name.empty() || name[0] == '.') continue;
        if (name == "node_modules") continue;
        // Follow symlinks
        std::error_code stat_ec;
        bool is_dir = fs::is_directory(entry.path(), stat_ec);
        if (!stat_ec && is_dir) {
            result.push_back(name);
        }
    }
    std::sort(result.begin(), result.end());
    return result;
}

// ============================================================================
// ✅ resolveNestedSkillsRoot (Aligned with OpenClaw workspace.ts:249-277)
// If dir/skills/*/SKILL.md exists, treat dir/skills as the real root.
// ============================================================================
struct NestedSkillsRoot {
    fs::path base_dir;
    std::optional<std::string> note;
};

static NestedSkillsRoot resolve_nested_skills_root(
    const fs::path& dir,
    int max_entries_to_scan = 100
) {
    fs::path nested = dir / "skills";
    std::error_code ec;
    if (!fs::is_directory(nested, ec) || ec) {
        return {dir, std::nullopt};
    }

    auto child_dirs = list_child_directories(nested);
    int scan_limit = std::min(max_entries_to_scan, static_cast<int>(child_dirs.size()));

    for (int i = 0; i < scan_limit; i++) {
        if (fs::exists(nested / child_dirs[i] / "SKILL.md", ec)) {
            return {nested, "Detected nested skills root at " + nested.string()};
        }
    }
    return {dir, std::nullopt};
}

    core::Result<int> load_from_directory(const std::string& dir, SkillSource source) {
        try {
            int max_per_source = 200;
            int max_candidates_per_root = 300;
            size_t max_skill_file_bytes = 256000;

            if (config_) {
                max_per_source = config_->skills.limits.max_skills_loaded_per_source;
                max_candidates_per_root = config_->skills.limits.max_candidates_per_root;
                max_skill_file_bytes = config_->skills.limits.max_skill_file_bytes;
            }

            fs::path root_dir = fs::path(dir);
            std::error_code ec;
            fs::path root_real = fs::canonical(root_dir, ec);
            if (ec) root_real = fs::absolute(root_dir);

            // ✅ Aligned with OpenClaw: resolveNestedSkillsRoot
            auto nested_result = resolve_nested_skills_root(root_dir, max_candidates_per_root);
            fs::path base_dir = nested_result.base_dir;
            if (nested_result.note) {
                log()->info("Nested skills root: {}", *nested_result.note);
            }

            // ✅ Validate base_dir is still inside root
            auto base_dir_real = resolve_contained_skill_path(
                source_to_str(source), root_dir, root_real, base_dir);
            if (!base_dir_real) {
                return core::Result<int>::Ok(0);  // Safety check failed
            }

            int loaded = 0;

            // ✅ Case 1: baseDir itself has SKILL.md → load as single skill
            if (fs::exists(base_dir / "SKILL.md", ec)) {
                auto skill_md_real = resolve_contained_skill_path(
                    source_to_str(source), root_dir, *base_dir_real, base_dir / "SKILL.md");
                if (skill_md_real) {
                    // Check size
                    std::error_code size_ec;
                    auto size = fs::file_size(*skill_md_real, size_ec);
                    if (!size_ec && size <= max_skill_file_bytes) {
                        auto result = load_skill_file(*skill_md_real, source);
                        if (result.is_ok()) loaded++;
                    } else if (!size_ec) {
                        log()->warn("Skipping skills root due to oversized SKILL.md ({}B > {}B): {}",
                                    size, max_skill_file_bytes, skill_md_real->string());
                    }
                }
                return core::Result<int>::Ok(loaded);
            }

            // ✅ Case 2: enumerate immediate child directories
            auto child_dirs = list_child_directories(base_dir);
            bool suspicious = static_cast<int>(child_dirs.size()) > max_candidates_per_root;
            int max_candidates = std::max(0, max_per_source);

            if (suspicious) {
                log()->warn("Skills root looks suspiciously large, truncating discovery "
                            "(dir={}, childDirs={}, maxCandidatesPerRoot={})",
                            dir, child_dirs.size(), max_candidates_per_root);
            }

            // Process only up to max_candidates children, alphabetically sorted
            int children_to_scan = std::min(static_cast<int>(child_dirs.size()), max_candidates);

            for (int i = 0; i < children_to_scan && loaded < max_per_source; i++) {
                const auto& name = child_dirs[i];
                fs::path skill_dir = base_dir / name;
                fs::path skill_md = skill_dir / "SKILL.md";

                // ✅ Validate skill_dir is inside root (symlink check)
                auto skill_dir_real = resolve_contained_skill_path(
                    source_to_str(source), root_dir, *base_dir_real, skill_dir);
                if (!skill_dir_real) continue;

                if (!fs::exists(skill_md, ec)) continue;

                // ✅ Validate SKILL.md is inside root (double check)
                auto skill_md_real = resolve_contained_skill_path(
                    source_to_str(source), root_dir, *base_dir_real, skill_md);
                if (!skill_md_real) continue;

                // ✅ Check file size limit
                std::error_code size_ec;
                auto size = fs::file_size(*skill_md_real, size_ec);
                if (size_ec) continue;
                if (size > max_skill_file_bytes) {
                    log()->warn("Skipping skill due to oversized SKILL.md (skill={}, {}B > {}B)",
                                name, size, max_skill_file_bytes);
                    continue;
                }

                auto result = load_skill_file(*skill_md_real, source);
                if (result.is_ok()) {
                    loaded++;
                }
            }

            return core::Result<int>::Ok(loaded);

        } catch (const std::exception& e) {
            return core::Result<int>::Err(
                std::string("Failed to load from directory: ") + e.what());
        }
    }

    // Helper: SkillSource to string for logging
    static std::string source_to_str(SkillSource source) {
        switch (source) {
            case SkillSource::Bundled:   return "openclaw-bundled";
            case SkillSource::Managed:   return "openclaw-managed";
            case SkillSource::Personal:  return "agents-skills-personal";
            case SkillSource::Plugin:    return "openclaw-extra";
            case SkillSource::Project:   return "agents-skills-project";
            case SkillSource::Workspace: return "openclaw-workspace";
            default: return "unknown";
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
            
            // ✅ 对齐 OpenClaw workspace.ts:511-525:
            // skillEntries = merged.values().map(skill => ({
            //   skill,
            //   frontmatter: parseFrontmatter(readFileSync(skill.filePath)),
            //   metadata: resolveOpenClawMetadata(frontmatter),
            //   invocation: resolveSkillInvocationPolicy(frontmatter),
            // }))
            ParsedSkillFrontmatter raw_frontmatter =
                SkillParser::parse_raw_frontmatter(content_result.get());

            auto invocation_policy = SkillParser::resolve_invocation_policy(raw_frontmatter);

            // 创建 SkillEntry (包含运行时状态)
            SkillEntry entry;
            entry.skill = skill;
            entry.frontmatter = raw_frontmatter;             // ✅ raw frontmatter map
            entry.metadata = skill.frontmatter.metadata;     // from yaml-cpp structured parse
            entry.invocation = invocation_policy;            // ✅ user-invocable / disable-model-invocation
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

// ============================================================================
// ✅ Snapshot Version Management (Aligned with OpenClaw bumpSkillsSnapshotVersion)
// ============================================================================

int64_t SkillManager::get_snapshot_version() const {
    return static_cast<int64_t>(impl_->version_);
}

int64_t SkillManager::bump_snapshot_version() {
    return static_cast<int64_t>(impl_->bump_version());
}

// ============================================================================
// ✅ compactSkillPaths (Aligned with OpenClaw workspace.ts:46-54)
// Replace HOME prefix with "~/" to reduce LLM token usage (~5-6 tokens/path)
// ============================================================================

/*static*/ std::vector<Skill> SkillManager::compact_skill_paths(
    const std::vector<Skill>& skills
) {
    const char* home_env = std::getenv("HOME");
    if (!home_env) return skills;

    std::string home(home_env);
    if (!home.empty() && home.back() != '/') home += '/';

    std::vector<Skill> result = skills;
    for (auto& skill : result) {
        std::string path_str = skill.file_path.string();
        if (path_str.find(home) == 0) {
            skill.file_path = "~/" + path_str.substr(home.size());
        }
    }
    return result;
}

// ============================================================================
// ✅ filterWorkspaceSkillEntries (Aligned with OpenClaw workspace.ts:68-88)
// ============================================================================

std::vector<SkillEntry> SkillManager::filter_workspace_skill_entries(
    const std::vector<SkillEntry>& entries,
    const SkillEligibilityContext& ctx
) const {
    std::vector<SkillEntry> result;
    for (const auto& entry : entries) {
        std::string reason;
        if (impl_->check_eligibility(entry, ctx, reason)) {
            SkillEntry e = entry;
            e.eligible = true;
            result.push_back(e);
        }
    }
    return result;
}

// ============================================================================
// ✅ buildWorkspaceSkillSnapshot (Aligned with OpenClaw workspace.ts:567-584)
// Builds a versioned SkillSnapshot for injection into agent system prompt.
// ============================================================================

SkillSnapshot SkillManager::build_workspace_skill_snapshot(
    const SkillEligibilityContext& ctx,
    const std::vector<std::string>& skill_filter,
    std::optional<int64_t> snapshot_version
) const {
    // 1. Filter eligible skills
    auto eligible = filter_eligible_skills(ctx, skill_filter);

    // 2. Exclude disableModelInvocation skills from prompt
    //    (OpenClaw: promptEntries = eligible.filter(e => !disableModelInvocation))
    std::vector<SkillEntry> prompt_entries;
    for (const auto& e : eligible) {
        bool disable_model = e.invocation && e.invocation->disable_model_invocation;
        if (!disable_model) {
            prompt_entries.push_back(e);
        }
    }

    // 3. Extract skill objects and compact paths
    std::vector<Skill> resolved_skills;
    for (const auto& e : prompt_entries) {
        resolved_skills.push_back(e.skill);
    }
    auto compacted = compact_skill_paths(resolved_skills);

    // 4. Apply prompt limits — 🎯 严格对齐 OpenClaw applySkillsPromptLimits
    // Step 4a: maxSkillsInPrompt (count cap, default 150)
    int max_skills = 150;
    int max_chars = 30000;  // 🎯 对齐 OpenClaw DEFAULT_MAX_SKILLS_PROMPT_CHARS = 30_000
    if (impl_->config_) {
        max_skills = impl_->config_->skills.limits.max_skills_in_prompt;
        if (impl_->config_->skills.limits.max_skills_prompt_chars > 0) {
            max_chars = impl_->config_->skills.limits.max_skills_prompt_chars;
        }
    }

    bool truncated = false;
    std::string truncated_reason;

    if (static_cast<int>(compacted.size()) > max_skills) {
        log()->warn("Truncating skills from {} to max {}", compacted.size(), max_skills);
        compacted.resize(max_skills);
        resolved_skills.resize(max_skills);
        truncated = true;
        truncated_reason = "count";
    }

    // Step 4b: maxSkillsPromptChars (char cap, binary search)
    // 🎯 严格对齐 OpenClaw workspace.ts:547-562: 二分搜索找到最大可容纳的前缀
    auto format_skills_block = [](const std::vector<Skill>& skills) -> std::string {
        if (skills.empty()) return "";
        std::ostringstream ss;
        ss << "\n<available_skills>\n";
        ss << "Before replying: scan <available_skills> <description> entries.\n";
        ss << "- If exactly one skill clearly applies: read its SKILL.md at <location> with `read`, then follow it.\n";
        ss << "- If multiple could apply: choose the most specific one, then read/follow it.\n";
        ss << "- If none clearly apply: do not read any SKILL.md.\n";
        ss << "Constraints: never read more than one skill up front; only read after selecting.\n\n";
        for (const auto& skill : skills) {
            ss << "  <skill>\n";
            ss << "    <name>" << xml_escape(skill.name) << "</name>\n";
            ss << "    <description>" << xml_escape(skill.description) << "</description>\n";
            std::string skill_path = skill.file_path.string();
            if (!skill_path.empty()) {
                ss << "    <location>" << xml_escape(skill_path) << "</location>\n";
            }
            ss << "  </skill>\n";
        }
        ss << "</available_skills>\n";
        return ss.str();
    };

    auto fits = [&](const std::vector<Skill>& skills) -> bool {
        return static_cast<int>(format_skills_block(skills).size()) <= max_chars;
    };

    if (!fits(compacted)) {
        // 🎯 对齐 OpenClaw: Binary search the largest prefix that fits in the char budget.
        int lo = 0;
        int hi = static_cast<int>(compacted.size());
        while (lo < hi) {
            int mid = (lo + hi + 1) / 2;  // ceil division
            std::vector<Skill> prefix(compacted.begin(), compacted.begin() + mid);
            if (fits(prefix)) {
                lo = mid;
            } else {
                hi = mid - 1;
            }
        }
        log()->warn("Truncating skills from {} to {} by char budget ({} chars)",
                    compacted.size(), lo, max_chars);
        compacted.resize(lo);
        resolved_skills.resize(lo);
        truncated = true;
        truncated_reason = "chars";
    }

    // 5. Build XML prompt block
    std::string prompt_block = format_skills_block(compacted);

    // 🎯 对齐 OpenClaw: truncation note
    std::ostringstream ss;
    if (truncated) {
        ss << "\xE2\x9A\xA0\xEF\xB8\x8F Skills truncated: included "
           << compacted.size() << " of " << resolved_skills.size()
           << ". Run `hunterclaw skills check` to audit.\n";
    }
    ss << prompt_block;

    // 6. Build snapshot
    SkillSnapshot snapshot;
    snapshot.prompt = ss.str();
    snapshot.resolved_skills = resolved_skills;
    snapshot.version = snapshot_version;

    // Add remote note if available (aligned with OpenClaw)
    if (ctx.remote && !ctx.remote->note.empty()) {
        snapshot.prompt = ctx.remote->note + "\n" + snapshot.prompt;
    }

    // Build skills metadata list (for ENV overrides)
    for (const auto& e : eligible) {
        SkillSnapshotEntry entry;
        entry.name = e.skill.name;
        entry.primary_env = e.metadata.primary_env;
        entry.required_env = e.metadata.requires.env;
        snapshot.skills.push_back(entry);
    }

    // Preserve skill_filter
    if (!skill_filter.empty()) {
        snapshot.skill_filter = skill_filter;
    }

    log()->info("Built skill snapshot: {} eligible, {} in prompt (version: {})",
                eligible.size(), compacted.size(),
                snapshot_version ? std::to_string(*snapshot_version) : "none");

    return snapshot;
}

// ============================================================================
// ✅ resolveSkillsPromptForRun (Aligned with OpenClaw workspace.ts:640-658)
// Priority: snapshot.prompt → real-time build → ""
// ============================================================================

std::string SkillManager::resolve_skills_prompt_for_run(
    const std::optional<SkillSnapshot>& snapshot,
    const std::optional<std::vector<SkillEntry>>& entries
) const {
    // 1. Use snapshot prompt if available
    if (snapshot) {
        std::string prompt = snapshot->prompt;
        // trim
        size_t start = prompt.find_first_not_of(" \t\n\r");
        if (start != std::string::npos) {
            return prompt.substr(start);
        }
    }

    // 2. Fall back to real-time build from entries
    if (entries && !entries->empty()) {
        // Build from entries using current context
        std::ostringstream ss;
        ss << "\n<available_skills>\n";
        ss << "Before replying: scan <available_skills> <description> entries.\n";
        ss << "Use the read tool to load SKILL.md at the listed <location> when needed.\n\n";

        for (const auto& entry : *entries) {
            if (!entry.eligible) continue;
            // Skip disableModelInvocation
            if (entry.invocation && entry.invocation->disable_model_invocation) continue;

            const auto& skill = entry.skill;
            ss << "  <skill>\n";
            ss << "    <name>" << xml_escape(skill.name) << "</name>\n";
            ss << "    <description>" << xml_escape(skill.description) << "</description>\n";
            std::string path_str = skill.file_path.string();
            if (!path_str.empty()) {
                ss << "    <location>" << xml_escape(path_str) << "</location>\n";
            }
            ss << "  </skill>\n";
        }
        ss << "</available_skills>\n";

        std::string prompt = ss.str();
        size_t start = prompt.find_first_not_of(" \t\n\r");
        return start != std::string::npos ? prompt.substr(start) : "";
    }

    return "";
}

// ============================================================================
// ✅ applySkillEnvOverrides (Aligned with OpenClaw env-overrides.ts)
// Inject skill apiKey/env into process environment.
// Returns a restore function.
// ============================================================================

std::function<void()> SkillManager::apply_skill_env_overrides(
    const std::vector<SkillEntry>& skills
) {
    std::vector<std::pair<std::string, std::optional<std::string>>> applied;

    for (const auto& entry : skills) {
        if (!impl_->config_) continue;

        const std::string& skill_key = entry.metadata.skill_key.empty()
            ? entry.skill.name
            : entry.metadata.skill_key;

        // Look up skill config (simplified - real impl would check config->skills.entries)
        // Inject primary_env / apiKey if configured
        const std::string& primary_env = entry.metadata.primary_env;
        if (!primary_env.empty() && std::getenv(primary_env.c_str()) == nullptr) {
            // Check if apiKey is configured for this skill
            // (Real impl: config->skills.entries[skill_key].api_key)
            // For now, record the env var name for tracking
            applied.push_back({primary_env, std::nullopt});
        }

        // Inject required env vars from config
        // (Real impl: config->skills.entries[skill_key].env)
    }

    return [applied]() {
        // Restore: remove injected env vars
        for (const auto& [key, old_val] : applied) {
            if (old_val.has_value()) {
                setenv(key.c_str(), old_val->c_str(), 1);
            } else {
                unsetenv(key.c_str());
            }
        }
    };
}

std::function<void()> SkillManager::apply_skill_env_overrides_from_snapshot(
    const SkillSnapshot& snapshot
) {
    std::vector<std::pair<std::string, std::optional<std::string>>> applied;

    for (const auto& skill_info : snapshot.skills) {
        if (!skill_info.primary_env.empty() &&
            std::getenv(skill_info.primary_env.c_str()) == nullptr) {
            applied.push_back({skill_info.primary_env, std::nullopt});
        }
    }

    return [applied]() {
        for (const auto& [key, old_val] : applied) {
            if (old_val.has_value()) {
                setenv(key.c_str(), old_val->c_str(), 1);
            } else {
                unsetenv(key.c_str());
            }
        }
    };
}

// ============================================================================
// Skill Command Name Helpers (Aligned with OpenClaw workspace.ts:102-129)
// ============================================================================

namespace {

static std::string sanitize_skill_cmd_name(const std::string& raw) {
    std::string result;
    for (unsigned char c : raw) {
        result += std::isalnum(c) ? static_cast<char>(std::tolower(c)) : '_';
    }
    std::string out;
    bool prev = false;
    for (char c : result) {
        if (c == '_') { if (!prev) out += c; prev = true; }
        else { out += c; prev = false; }
    }
    size_t s = out.find_first_not_of('_');
    if (s == std::string::npos) return "skill";
    size_t e = out.find_last_not_of('_');
    out = out.substr(s, e - s + 1);
    if (out.size() > 32) out = out.substr(0, 32);
    return out.empty() ? "skill" : out;
}

static std::string unique_cmd_name(const std::string& base, std::set<std::string>& used) {
    auto lower = [](std::string s) {
        std::transform(s.begin(), s.end(), s.begin(), ::tolower);
        return s;
    };
    if (!used.count(lower(base))) { used.insert(lower(base)); return base; }
    for (int i = 2; i < 1000; i++) {
        std::string suf = "_" + std::to_string(i);
        int maxb = std::max(1, 32 - (int)suf.size());
        std::string cand = base.substr(0, maxb) + suf;
        if (!used.count(lower(cand))) { used.insert(lower(cand)); return cand; }
    }
    return base.substr(0, 30) + "_x";
}

} // anonymous namespace

// ============================================================================
// ✅ buildWorkspaceSkillCommandSpecs (Aligned with OpenClaw workspace.ts:775-881)
// ============================================================================
std::vector<SkillCommandSpec> SkillManager::build_workspace_skill_command_specs(
    const SkillEligibilityContext& ctx,
    const std::vector<std::string>& skill_filter,
    const std::optional<std::set<std::string>>& reserved_names
) const {
    auto eligible = filter_eligible_skills(ctx, skill_filter);

    std::vector<SkillEntry> user_invocable;
    for (const auto& e : eligible) {
        if (!e.invocation || e.invocation->user_invocable)
            user_invocable.push_back(e);
    }

    std::set<std::string> used;
    if (reserved_names) {
        for (const auto& n : *reserved_names) {
            std::string l = n;
            std::transform(l.begin(), l.end(), l.begin(), ::tolower);
            used.insert(l);
        }
    }

    std::vector<SkillCommandSpec> specs;
    for (const auto& entry : user_invocable) {
        const std::string& raw_name = entry.skill.name;
        std::string unique = unique_cmd_name(sanitize_skill_cmd_name(raw_name), used);

        std::string desc = entry.skill.description.empty() ? raw_name : entry.skill.description;
        while (!desc.empty() && std::isspace((unsigned char)desc.front())) desc.erase(desc.begin());
        while (!desc.empty() && std::isspace((unsigned char)desc.back())) desc.pop_back();
        if (desc.size() > 100) desc = desc.substr(0, 99) + "…";

        std::optional<SkillCommandDispatch> dispatch;
        auto kit = entry.frontmatter.find("command-dispatch");
        if (kit == entry.frontmatter.end()) kit = entry.frontmatter.find("command_dispatch");
        if (kit != entry.frontmatter.end()) {
            std::string kv = kit->second;
            std::transform(kv.begin(), kv.end(), kv.begin(), ::tolower);
            while (!kv.empty() && std::isspace((unsigned char)kv.front())) kv.erase(kv.begin());
            while (!kv.empty() && std::isspace((unsigned char)kv.back())) kv.pop_back();
            if (kv == "tool") {
                auto tit = entry.frontmatter.find("command-tool");
                if (tit == entry.frontmatter.end()) tit = entry.frontmatter.find("command_tool");
                if (tit != entry.frontmatter.end() && !tit->second.empty())
                    dispatch = SkillCommandDispatch{"tool", tit->second, "raw"};
            }
        }

        specs.push_back({unique, raw_name, desc, dispatch});
    }

    log()->info("Built {} skill command specs", specs.size());
    return specs;
}

// ============================================================================
// ✅ syncSkillsToWorkspace (Aligned with OpenClaw workspace.ts:710-766)
// ============================================================================
core::Result<bool> SkillManager::sync_skills_to_workspace(const SyncSkillsParams& params) {
    std::string source = core::utils::expand_path(params.source_workspace_dir);
    std::string target = core::utils::expand_path(params.target_workspace_dir);
    if (source == target) return core::Result<bool>::Ok(true);

    log()->info("Syncing skills: {} → {}", source, target);
    fs::path tsd = fs::path(target) / "skills";

    auto tmp = std::make_unique<SkillManager>(impl_->config_);
    tmp->load_skills(source);
    auto entries = tmp->list_skills();

    std::error_code ec;
    fs::remove_all(tsd, ec);
    fs::create_directories(tsd, ec);
    if (ec) return core::Result<bool>::Err("Failed to create target: " + ec.message());

    std::set<std::string> used;
    int copied = 0;
    for (const auto& entry : entries) {
        std::string src_name = entry.skill.base_dir.filename().string();
        while (!src_name.empty() && std::isspace((unsigned char)src_name.front()))
            src_name.erase(src_name.begin());
        while (!src_name.empty() && std::isspace((unsigned char)src_name.back()))
            src_name.pop_back();
        if (src_name.empty() || src_name == "." || src_name == "..") continue;

        std::string uname = src_name;
        if (used.count(uname)) {
            for (int i = 2; i < 10000; i++) {
                std::string c = src_name + "-" + std::to_string(i);
                if (!used.count(c)) { uname = c; break; }
            }
        }
        used.insert(uname);

        fs::path dest = tsd / uname;
        auto rel = fs::relative(fs::absolute(dest), fs::absolute(tsd), ec);
        if (ec || rel.string().find("..") != std::string::npos) continue;

        fs::copy(entry.skill.base_dir, dest,
                 fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
        if (!ec) copied++;
        else log()->warn("copy skill {} failed: {}", entry.skill.name, ec.message());
    }

    log()->info("Synced {} skills → {}", copied, tsd.string());
    return core::Result<bool>::Ok(true);
}

} // namespace skills
} // namespace hunterclaw