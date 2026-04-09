#pragma once

// ============================================================================
// onboard_skills.h
// 🎯 严格对齐 OpenClaw: src/commands/onboard-skills.ts
//
// 引导式 skills 配置向导接口
// ============================================================================

#include "hunterclaw/core/types.h"
#include <functional>
#include <string>
#include <vector>

namespace hunterclaw {
namespace commands {

// ============================================================================
// WizardPrompter (对齐 OpenClaw: WizardPrompter interface)
// ============================================================================

class WizardPrompter {
public:
    virtual ~WizardPrompter() = default;

    // 🎯 对齐 OpenClaw: prompter.note(text, title)
    virtual void note(const std::string& text, const std::string& title = "") = 0;

    // 🎯 对齐 OpenClaw: prompter.confirm({message, initialValue})
    virtual bool confirm(const std::string& message, bool initial_value = false) = 0;

    // 🎯 对齐 OpenClaw: prompter.multiselect({message, options})
    virtual std::vector<std::string> multiselect(
        const std::string& message,
        const std::vector<std::string>& options) = 0;

    // 🎯 对齐 OpenClaw: prompter.select({message, options})
    virtual std::string select(
        const std::string& message,
        const std::vector<std::string>& options) = 0;

    // 🎯 对齐 OpenClaw: prompter.text({message, validate})
    virtual std::string text(const std::string& message) = 0;

    // 🎯 对齐 OpenClaw: prompter.progress(label) → {stop(msg)}
    struct ProgressHandle {
        std::function<void(const std::string&)> stop;
    };
    virtual ProgressHandle progress(const std::string& label) = 0;
};

// ============================================================================
// setupSkills — 交互式安装向导
// ============================================================================

// 🎯 对齐 OpenClaw: setupSkills(cfg, workspaceDir, runtime, prompter)
// 返回更新后的配置
core::JsonValue setup_skills(
    core::JsonValue config,
    const std::string& workspace_dir,
    WizardPrompter& prompter);

}  // namespace commands
}  // namespace hunterclaw
