// 🎯 严格对齐 OpenClaw: src/agents/skills/workspace.ts
// HunterClaw Skills Manager 头文件

#ifndef HUNTERCLAW_SKILLS_MANAGER_H
#define HUNTERCLAW_SKILLS_MANAGER_H

#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace hunterclaw {
namespace skills {

// 🎯 对齐 OpenClaw types.ts line 66-71: SkillEntry
struct SkillEntry {
    std::string name;               // 技能名称
    std::string file_path;          // SKILL.md 文件路径 (压缩后)
    std::string base_dir;           // 技能基础目录
    std::string source;             // 来源 (workspace, bundled, plugin)
    nlohmann::json frontmatter;     // YAML frontmatter
    nlohmann::json metadata;        // openclaw metadata
};

// 🎯 对齐 OpenClaw line 660-669: loadWorkspaceSkillEntries
/**
 * 从 workspace 目录加载所有技能条目
 * 
 * @param workspace_dir Workspace 目录路径
 * @param config HunterClaw 配置对象 (JSON)
 * @return 技能条目列表
 */
std::vector<SkillEntry> load_workspace_skill_entries(
    const std::string& workspace_dir,
    const nlohmann::json& config = nlohmann::json::object()
);

// 🎯 对齐 OpenClaw skills-install.ts line 92-98: 查找安装规格
/**
 * 在技能的 metadata.install 数组中查找指定 ID 的安装规格
 * 
 * @param entry 技能条目
 * @param install_id 安装规格 ID (空字符串表示返回第一个)
 * @return 安装规格 JSON (未找到返回空对象)
 */
nlohmann::json find_install_spec(
    const SkillEntry& entry,
    const std::string& install_id = ""
);

// 🎯 对齐 OpenClaw skills-status.ts line 169-225: 评估技能资格
/**
 * 检查技能是否符合当前环境的资格
 * 
 * @param entry 技能条目
 * @param config HunterClaw 配置对象
 * @return true 如果技能可用
 */
bool is_skill_eligible(
    const SkillEntry& entry,
    const nlohmann::json& config = nlohmann::json::object()
);

// 🎯 对齐 OpenClaw skills.ts line 26-55: 收集技能 bins
/**
 * 收集所有技能需要的二进制文件列表
 * 
 * @param entries 技能条目列表
 * @return 去重并排序的二进制文件名列表
 */
std::vector<std::string> collect_skill_bins(
    const std::vector<SkillEntry>& entries
);

}  // namespace skills
}  // namespace hunterclaw

#endif  // HUNTERCLAW_SKILLS_MANAGER_H
