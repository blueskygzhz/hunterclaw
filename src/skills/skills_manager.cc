// 🎯 严格对齐 OpenClaw: src/agents/skills/workspace.ts
// HunterClaw Skills Manager 实现

#include "hunterclaw/skills/skills_manager.h"

#include <algorithm>
#include <dirent.h>
#include <filesystem>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace hunterclaw {
namespace skills {

namespace {

// 🎯 对齐 OpenClaw line 96-100: 默认限制
constexpr int DEFAULT_MAX_CANDIDATES_PER_ROOT = 300;
constexpr int DEFAULT_MAX_SKILLS_LOADED_PER_SOURCE = 200;
constexpr int DEFAULT_MAX_SKILLS_IN_PROMPT = 150;
constexpr int DEFAULT_MAX_SKILLS_PROMPT_CHARS = 30000;
constexpr int DEFAULT_MAX_SKILL_FILE_BYTES = 256000;

// 🎯 对齐 OpenClaw line 46-54: 压缩技能路径
std::string compact_skill_path(const std::string& path) {
    const char* home = getenv("HOME");
    if (!home) return path;
    
    std::string home_prefix = std::string(home) + "/";
    if (path.find(home_prefix) == 0) {
        return "~/" + path.substr(home_prefix.length());
    }
    return path;
}

// 🎯 对齐 OpenClaw line 199-212: 列出子目录
std::vector<std::string> list_child_directories(const std::string& dir) {
    std::vector<std::string> result;
    
    DIR* d = opendir(dir.c_str());
    if (!d) return result;
    
    struct dirent* entry;
    while ((entry = readdir(d)) != nullptr) {
        if (entry->d_type == DT_DIR) {
            std::string name = entry->d_name;
            if (name != "." && name != ".." && name[0] != '.') {
                result.push_back(name);
            }
        }
    }
    closedir(d);
    
    std::sort(result.begin(), result.end());
    return result;
}

// 🎯 对齐 OpenClaw line 255-277: 检测嵌套技能根目录
struct ResolvedSkillsRoot {
    std::string base_dir;
    std::string note;
};

ResolvedSkillsRoot resolve_nested_skills_root(
    const std::string& dir,
    int max_entries_to_scan
) {
    // 检查 dir/skills 是否存在
    std::string nested = dir + "/skills";
    struct stat st;
    if (stat(nested.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
        return {dir, ""};
    }
    
    // 启发式检测: 如果 dir/skills/*/SKILL.md 存在，则 dir/skills 是真正的根目录
    auto child_dirs = list_child_directories(nested);
    int scan_limit = std::min(max_entries_to_scan, (int)child_dirs.size());
    
    for (int i = 0; i < scan_limit; i++) {
        std::string skill_md = nested + "/" + child_dirs[i] + "/SKILL.md";
        if (access(skill_md.c_str(), F_OK) == 0) {
            return {nested, "Detected nested skills root at " + nested};
        }
    }
    
    return {dir, ""};
}

// 🎯 对齐 OpenClaw frontmatter.ts: 解析 YAML frontmatter
json parse_frontmatter(const std::string& content) {
    json result = json::object();
    
    // 查找 frontmatter 边界 (---)
    if (content.substr(0, 3) != "---") {
        return result;
    }
    
    size_t end_pos = content.find("\n---", 3);
    if (end_pos == std::string::npos) {
        return result;
    }
    
    std::string frontmatter = content.substr(4, end_pos - 4);
    
    // 简化的 YAML 解析 (仅支持基本 key: value)
    std::istringstream stream(frontmatter);
    std::string line;
    std::string current_key;
    
    while (std::getline(stream, line)) {
        // 跳过注释
        if (line.empty() || line[0] == '#') continue;
        
        // 检查是否为 key: value 行
        size_t colon = line.find(':');
        if (colon != std::string::npos) {
            std::string key = line.substr(0, colon);
            std::string value = line.substr(colon + 1);
            
            // trim
            key.erase(0, key.find_first_not_of(" \t"));
            key.erase(key.find_last_not_of(" \t") + 1);
            value.erase(0, value.find_first_not_of(" \t"));
            value.erase(value.find_last_not_of(" \t") + 1);
            
            if (!key.empty()) {
                result[key] = value;
                current_key = key;
            }
        }
    }
    
    return result;
}

// 🎯 对齐 OpenClaw frontmatter.ts line 35-82: 解析 openclaw metadata
json resolve_openclaw_metadata(const json& frontmatter) {
    json metadata = json::object();
    
    // 查找 openclaw 字段
    auto it = frontmatter.find("openclaw");
    if (it == frontmatter.end()) {
        return metadata;
    }
    
    // 简化实现: 假设 openclaw 是 JSON 字符串
    if (it->is_string()) {
        try {
            metadata = json::parse(it->get<std::string>());
        } catch (...) {
            // 解析失败，返回空 metadata
        }
    } else if (it->is_object()) {
        metadata = *it;
    }
    
    return metadata;
}

// 🎯 对齐 OpenClaw line 302-400: 加载单个技能目录
SkillEntry load_skill_from_file(
    const std::string& skill_file,
    const std::string& source,
    int max_file_bytes
) {
    SkillEntry entry;
    entry.source = source;
    entry.file_path = compact_skill_path(skill_file);
    
    // 读取文件
    std::ifstream file(skill_file);
    if (!file.is_open()) {
        return entry;
    }
    
    // 检查文件大小
    file.seekg(0, std::ios::end);
    size_t file_size = file.tellg();
    if (file_size > (size_t)max_file_bytes) {
        // 文件过大，跳过
        return entry;
    }
    file.seekg(0, std::ios::beg);
    
    // 读取内容
    std::string content((std::istreambuf_iterator<char>(file)),
                        std::istreambuf_iterator<char>());
    
    // 解析 frontmatter
    entry.frontmatter = parse_frontmatter(content);
    
    // 解析 openclaw metadata
    entry.metadata = resolve_openclaw_metadata(entry.frontmatter);
    
    // 提取技能名称 (从文件路径或 frontmatter)
    if (entry.frontmatter.contains("name")) {
        entry.name = entry.frontmatter["name"].get<std::string>();
    } else {
        // 从文件路径提取 (skill_dir 名称)
        size_t last_slash = skill_file.find_last_of('/');
        size_t second_last_slash = skill_file.find_last_of('/', last_slash - 1);
        if (second_last_slash != std::string::npos) {
            entry.name = skill_file.substr(
                second_last_slash + 1, 
                last_slash - second_last_slash - 1
            );
        }
    }
    
    // 设置 base_dir (skill_file 的父目录)
    size_t last_slash = skill_file.find_last_of('/');
    entry.base_dir = skill_file.substr(0, last_slash);
    
    return entry;
}

}  // namespace

// 🎯 对齐 OpenClaw line 660-669: loadWorkspaceSkillEntries
std::vector<SkillEntry> load_workspace_skill_entries(
    const std::string& workspace_dir,
    const json& config
) {
    std::vector<SkillEntry> entries;
    
    // 解析限制
    int max_candidates = DEFAULT_MAX_CANDIDATES_PER_ROOT;
    int max_skills_per_source = DEFAULT_MAX_SKILLS_LOADED_PER_SOURCE;
    int max_file_bytes = DEFAULT_MAX_SKILL_FILE_BYTES;
    
    if (config.contains("skills") && config["skills"].contains("limits")) {
        auto limits = config["skills"]["limits"];
        if (limits.contains("maxCandidatesPerRoot")) {
            max_candidates = limits["maxCandidatesPerRoot"].get<int>();
        }
        if (limits.contains("maxSkillsLoadedPerSource")) {
            max_skills_per_source = limits["maxSkillsLoadedPerSource"].get<int>();
        }
        if (limits.contains("maxSkillFileBytes")) {
            max_file_bytes = limits["maxSkillFileBytes"].get<int>();
        }
    }
    
    // 1. 检测技能根目录
    auto resolved = resolve_nested_skills_root(workspace_dir, max_candidates);
    std::string skills_root = resolved.base_dir;
    
    // 2. 列出所有子目录
    auto skill_dirs = list_child_directories(skills_root);
    
    // 3. 加载每个技能
    int loaded = 0;
    for (const auto& dir_name : skill_dirs) {
        if (loaded >= max_skills_per_source) {
            break;
        }
        
        std::string skill_file = skills_root + "/" + dir_name + "/SKILL.md";
        if (access(skill_file.c_str(), F_OK) != 0) {
            continue;  // SKILL.md 不存在
        }
        
        auto entry = load_skill_from_file(skill_file, "workspace", max_file_bytes);
        if (!entry.name.empty()) {
            entries.push_back(entry);
            loaded++;
        }
    }
    
    return entries;
}

// 🎯 对齐 OpenClaw skills-install.ts line 92-98: 查找安装规格
json find_install_spec(
    const SkillEntry& entry,
    const std::string& install_id
) {
    if (!entry.metadata.contains("install")) {
        return json::object();
    }
    
    auto install_specs = entry.metadata["install"];
    if (!install_specs.is_array()) {
        return json::object();
    }
    
    // 如果指定了 install_id，查找匹配的规格
    if (!install_id.empty()) {
        for (const auto& spec : install_specs) {
            if (spec.contains("id") && spec["id"].get<std::string>() == install_id) {
                return spec;
            }
        }
        return json::object();  // 未找到
    }
    
    // 否则返回第一个规格
    if (install_specs.size() > 0) {
        return install_specs[0];
    }
    
    return json::object();
}

// 🎯 对齐 OpenClaw skills-status.ts line 169-225: 评估技能资格
bool is_skill_eligible(
    const SkillEntry& entry,
    const json& config
) {
    // 1. 检查是否被禁用
    if (config.contains("skills") && config["skills"].contains("entries")) {
        auto entries = config["skills"]["entries"];
        std::string skill_key = entry.metadata.value("skillKey", entry.name);
        
        if (entries.contains(skill_key)) {
            auto skill_config = entries[skill_key];
            if (skill_config.contains("enabled") && 
                skill_config["enabled"].get<bool>() == false) {
                return false;  // 技能被禁用
            }
        }
    }
    
    // 2. 检查 OS 兼容性
    if (entry.metadata.contains("os")) {
        auto os_list = entry.metadata["os"];
        if (os_list.is_array() && os_list.size() > 0) {
            // ✅ 对齐 OpenClaw: 检测当前平台
            std::string current_os =
#if defined(__APPLE__)
                "darwin";
#elif defined(_WIN32)
                "win32";
#else
                "linux";
#endif
            
            bool os_matched = false;
            for (const auto& os : os_list) {
                if (os.get<std::string>() == current_os) {
                    os_matched = true;
                    break;
                }
            }
            
            if (!os_matched) {
                return false;  // OS 不兼容
            }
        }
    }
    
    // 3. 检查依赖 (bins, env, config)
    if (entry.metadata.contains("requires")) {
        auto requires = entry.metadata["requires"];
        
        // 检查必需的二进制文件
        if (requires.contains("bins") && requires["bins"].is_array()) {
            for (const auto& bin : requires["bins"]) {
                // ✅ 对齐 OpenClaw: hasBinary() 检查
                // 使用 PATH 遍历查找可执行文件，不依赖 system()
                std::string bin_name = bin.get<std::string>();
                bool bin_found = false;
                {
                    const char* path_env = std::getenv("PATH");
                    if (path_env) {
                        std::istringstream path_stream(path_env);
                        std::string path_dir;
                        while (std::getline(path_stream, path_dir, ':')) {
                            std::filesystem::path full_path = std::filesystem::path(path_dir) / bin_name;
                            if (std::filesystem::exists(full_path) && !std::filesystem::is_directory(full_path)) {
                                bin_found = true;
                                break;
                            }
                        }
                    }
                }
                if (!bin_found) {
                    return false;  // 缺少必需的二进制文件
                }
            }
        }
        
        // 检查环境变量
        if (requires.contains("env") && requires["env"].is_array()) {
            for (const auto& env_var : requires["env"]) {
                std::string var_name = env_var.get<std::string>();
                if (!getenv(var_name.c_str())) {
                    return false;  // 缺少必需的环境变量
                }
            }
        }
    }
    
    return true;  // 技能符合资格
}

// 🎯 对齐 OpenClaw skills.ts line 26-55: 收集技能 bins
std::vector<std::string> collect_skill_bins(
    const std::vector<SkillEntry>& entries
) {
    std::set<std::string> bins_set;
    
    for (const auto& entry : entries) {
        // 1. 收集 metadata.requires.bins
        if (entry.metadata.contains("requires")) {
            auto requires = entry.metadata["requires"];
            
            if (requires.contains("bins") && requires["bins"].is_array()) {
                for (const auto& bin : requires["bins"]) {
                    std::string bin_name = bin.get<std::string>();
                    if (!bin_name.empty()) {
                        bins_set.insert(bin_name);
                    }
                }
            }
            
            // 2. 收集 metadata.requires.anyBins
            if (requires.contains("anyBins") && requires["anyBins"].is_array()) {
                for (const auto& bin : requires["anyBins"]) {
                    std::string bin_name = bin.get<std::string>();
                    if (!bin_name.empty()) {
                        bins_set.insert(bin_name);
                    }
                }
            }
        }
        
        // 3. 收集 metadata.install[].bins
        if (entry.metadata.contains("install") && entry.metadata["install"].is_array()) {
            for (const auto& install_spec : entry.metadata["install"]) {
                if (install_spec.contains("bins") && install_spec["bins"].is_array()) {
                    for (const auto& bin : install_spec["bins"]) {
                        std::string bin_name = bin.get<std::string>();
                        if (!bin_name.empty()) {
                            bins_set.insert(bin_name);
                        }
                    }
                }
            }
        }
    }
    
    // 排序并返回
    std::vector<std::string> result(bins_set.begin(), bins_set.end());
    std::sort(result.begin(), result.end());
    return result;
}

}  // namespace skills
}  // namespace hunterclaw
