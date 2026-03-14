#include "hunterclaw/skills/skill.h"
#include "hunterclaw/skills/file_watcher.h"
#include <iostream>
#include <thread>
#include <chrono>
#include <spdlog/spdlog.h>

using namespace hunterclaw;
using namespace hunterclaw::skills;

// 简单的热重载测试
int main() {
    // 设置日志
    spdlog::set_level(spdlog::level::debug);
    
    std::cout << "=== HunterClaw Skill Hot-Reload Demo ===" << std::endl;
    std::cout << std::endl;
    
    // 创建配置
    core::Config config;
    config.skills.load.watch = true;
    config.skills.load.watch_debounce_ms = 250;
    config.skills.load.extra_dirs = {};
    
    // 创建 SkillManager
    auto skill_mgr = SkillManager::create(config.skills);
    
    // 设置 workspace
    std::string workspace_dir = "./test_workspace";
    
    // 创建测试目录
    fs::create_directories(workspace_dir + "/skills/test-skill");
    
    // 创建初始 SKILL.md
    std::string skill_path = workspace_dir + "/skills/test-skill/SKILL.md";
    {
        std::ofstream file(skill_path);
        file << "---\n";
        file << "name: test-skill\n";
        file << "description: A test skill for hot-reload demo\n";
        file << "---\n\n";
        file << "# Test Skill\n\n";
        file << "Initial content.\n";
    }
    
    std::cout << "Created test skill: " << skill_path << std::endl;
    std::cout << std::endl;
    
    // 加载 skills
    std::cout << "[1] Loading skills..." << std::endl;
    auto load_result = skill_mgr->load_skills(workspace_dir);
    if (load_result.is_err()) {
        std::cerr << "Failed to load skills: " << load_result.error << std::endl;
        return 1;
    }
    
    auto skills = skill_mgr->list_skills();
    std::cout << "Loaded " << skills.size() << " skills" << std::endl;
    for (const auto& entry : skills) {
        std::cout << "  - " << entry.skill.name << ": " 
                  << entry.skill.description << std::endl;
    }
    std::cout << std::endl;
    
    // 启动热重载
    std::cout << "[2] Starting hot-reload..." << std::endl;
    skill_mgr->start_watch();
    std::cout << "Hot-reload enabled. Watching for changes..." << std::endl;
    std::cout << std::endl;
    
    // 等待一段时间
    std::cout << "[3] Waiting 2 seconds..." << std::endl;
    std::this_thread::sleep_for(std::chrono::seconds(2));
    
    // 修改 SKILL.md
    std::cout << "[4] Modifying skill file..." << std::endl;
    {
        std::ofstream file(skill_path, std::ios::app);
        file << "\n## Updated Content\n\n";
        file << "This content was added at runtime!\n";
    }
    std::cout << "Modified: " << skill_path << std::endl;
    std::cout << std::endl;
    
    // 等待热重载生效 (防抖 + 处理时间)
    std::cout << "[5] Waiting for hot-reload (debounce: 250ms)..." << std::endl;
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    
    // 检查是否重新加载
    std::cout << "[6] Checking reloaded skills..." << std::endl;
    skills = skill_mgr->list_skills();
    std::cout << "Skills count: " << skills.size() << std::endl;
    for (const auto& entry : skills) {
        std::cout << "  - " << entry.skill.name << std::endl;
        if (entry.skill.content.find("Updated Content") != std::string::npos) {
            std::cout << "    ✅ Content updated successfully!" << std::endl;
        }
    }
    std::cout << std::endl;
    
    // 添加新 skill
    std::cout << "[7] Adding new skill..." << std::endl;
    fs::create_directories(workspace_dir + "/skills/new-skill");
    std::string new_skill_path = workspace_dir + "/skills/new-skill/SKILL.md";
    {
        std::ofstream file(new_skill_path);
        file << "---\n";
        file << "name: new-skill\n";
        file << "description: A dynamically added skill\n";
        file << "---\n\n";
        file << "# New Skill\n\n";
        file << "This skill was added at runtime!\n";
    }
    std::cout << "Created: " << new_skill_path << std::endl;
    std::cout << std::endl;
    
    // 等待热重载
    std::cout << "[8] Waiting for hot-reload..." << std::endl;
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    
    // 检查新 skill
    std::cout << "[9] Checking for new skill..." << std::endl;
    skills = skill_mgr->list_skills();
    std::cout << "Skills count: " << skills.size() << std::endl;
    
    bool found_new_skill = false;
    for (const auto& entry : skills) {
        std::cout << "  - " << entry.skill.name << std::endl;
        if (entry.skill.name == "new-skill") {
            found_new_skill = true;
            std::cout << "    ✅ New skill detected!" << std::endl;
        }
    }
    
    if (!found_new_skill) {
        std::cout << "    ❌ New skill not found (hot-reload may not be working)" << std::endl;
    }
    std::cout << std::endl;
    
    // 停止热重载
    std::cout << "[10] Stopping hot-reload..." << std::endl;
    skill_mgr->stop_watch();
    std::cout << "Hot-reload stopped." << std::endl;
    std::cout << std::endl;
    
    // 清理
    std::cout << "[11] Cleaning up..." << std::endl;
    fs::remove_all(workspace_dir);
    std::cout << "Test workspace removed." << std::endl;
    std::cout << std::endl;
    
    std::cout << "=== Demo Complete ===" << std::endl;
    
    return 0;
}
