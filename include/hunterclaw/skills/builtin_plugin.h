#pragma once

#include "hunterclaw/skills/plugin_interface.h"
#include "hunterclaw/skills/skill.h"
#include <map>
#include <shared_mutex>

namespace hunterclaw {
namespace skills {

/**
 * @brief Builtin Skills Plugin (系统内置 Skills)
 * 
 * 负责加载和管理系统内置的 Skills
 * 
 * OpenClaw 对齐:
 * - 从 ~/.hunterclaw/skills-bundled/ 加载
 * - 从 ~/.openclaw/skills/ 加载 (OpenClaw 兼容)
 * - 支持热重载
 * 
 * 架构:
 * ```
 * BuiltinSkillsPlugin → loadSkillsFromDir() → SKILL.md 解析
 *         ↓
 *   list_skills()
 *   get_skill(name)
 *   refresh_skills()
 * ```
 */
class BuiltinSkillsPlugin : public SkillPlugin {
public:
    BuiltinSkillsPlugin();
    ~BuiltinSkillsPlugin() override;
    
    // SkillPlugin 接口
    core::Result<bool> init(const std::map<std::string, std::string>& config) override;
    void shutdown() override;
    
    std::string get_id() const override { return "builtin-skills"; }
    std::string get_name() const override { return "Builtin Skills"; }
    std::string get_version() const override { return "1.0.0"; }
    
    std::vector<Skill> list_skills() override;
    std::optional<Skill> get_skill(const std::string& name) override;
    core::Result<bool> refresh_skills() override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    
    /**
     * @brief 从目录加载 Skills
     * @param skill_dir Skills 目录
     * @param source Skill 来源
     * @return 加载的 Skills 列表
     * 
     * OpenClaw: loadSkillsFromDir()
     */
    std::vector<Skill> load_skills_from_dir(
        const fs::path& skill_dir,
        SkillSource source
    );
};

/**
 * @brief Builtin Skills Plugin 工厂函数
 * 
 * C ABI 导出函数,供 SkillPluginLoader 调用
 */
extern "C" {
    SkillPlugin* create_skill_plugin();
    void destroy_skill_plugin(SkillPlugin* plugin);
}

} // namespace skills
} // namespace hunterclaw
