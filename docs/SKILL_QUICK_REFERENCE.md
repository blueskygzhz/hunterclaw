# HunterClaw Skill 系统快速参考

> **30 秒快速上手** | **100% OpenClaw 兼容**

---

## 🚀 快速开始

### 1️⃣ 创建 SkillManager

```cpp
#include "hunterclaw/skills/skill.h"

auto config = std::make_shared<core::Config>();
auto skill_mgr = std::make_shared<skills::SkillManager>(config);

// 加载 Skills
skill_mgr->load_skills("/path/to/workspace");
```

### 2️⃣ 过滤符合条件的 Skills

```cpp
// 构建资格检查上下文
skills::SkillEligibilityContext ctx;
ctx.current_os = "darwin";
ctx.has_bin = [](const std::string& bin) { /* 检查 PATH */ };
ctx.has_env = [](const std::string& env) { return std::getenv(env.c_str()) != nullptr; };

// 过滤 (支持通配符)
std::vector<std::string> filter = {"github*", "docker*"};
auto eligible = skill_mgr->filter_eligible_skills(ctx, filter);
```

### 3️⃣ 构建 LLM Prompt

```cpp
std::string prompt = skill_mgr->build_skills_prompt(ctx, filter);
// 注入到 system prompt
```

---

## 📂 Skill 目录结构

```
~/.hunterclaw/skills-bundled/    ← 内置 Skills (优先级: 1)
~/.hunterclaw/skills/            ← 管理的 Skills (优先级: 2)
~/.agents/skills/                ← 个人 Skills (优先级: 3)
<workspace>/.agents/skills/      ← 项目 Skills (优先级: 4)
<workspace>/skills/              ← 工作区 Skills (优先级: 5 - 最高)
```

**优先级机制**: 后加载的覆盖前面的 (同名 Skill)

---

## 📝 SKILL.md 格式

```yaml
---
name: github
description: GitHub CLI and API operations
homepage: https://cli.github.com
metadata:
  hunterclaw:
    emoji: "🐙"
    requires:
      bins: ["gh"]           # 必需 (ALL)
      any_bins: ["git"]      # 任一 (OR)
      env: ["GITHUB_TOKEN"]  # 环境变量
      config: ["github.token"]  # 配置路径
    install:
      - id: brew
        kind: brew
        formula: gh
        bins: ["gh"]
        os: ["darwin", "linux"]
        label: "Install GitHub CLI (brew)"
---

# GitHub Skill

使用 `gh` CLI 操作 GitHub...

## 用法

\`\`\`bash
gh pr list
\`\`\`
```

---

## 🔍 资格检查逻辑

| 检查项 | 逻辑 | 示例 |
|--------|------|------|
| `always` | 强制包含 | `always: true` |
| `os` | 操作系统匹配 | `["darwin", "linux"]` |
| `bins` | **ALL** 必须存在 | `["gh", "git"]` |
| `any_bins` | **OR** 至少一个存在 | `["npm", "pnpm", "yarn"]` |
| `env` | 环境变量存在 | `["GITHUB_TOKEN"]` |
| `config` | 配置路径 truthy | `["github.token"]` |

---

## 🎯 Skill 过滤器

```cpp
// Agent 配置
agent_config.skills = {
    "*",           // 全部允许
    "github",      // 精确匹配
    "github*",     // 前缀通配符: github, github-actions, github-api...
};
```

---

## 🛠️ 安装 Skill 依赖

```cpp
// 自动选择最合适的安装方式
skill_mgr->install_skill("mcporter");

// 指定安装方式
skill_mgr->install_skill("mcporter", "node");
```

**支持的安装类型**:
- `brew`: Homebrew
- `node`: npm/pnpm/yarn/bun
- `go`: go install
- `uv`: Python uv
- `download`: HTTP 下载

---

## 🔒 安全扫描

```cpp
auto scan_result = skill_mgr->scan_skill("custom-skill");

if (scan_result.is_ok() && scan_result.value.has_critical()) {
    std::cerr << "⚠️ CRITICAL security issues found!\n";
    
    for (const auto& warning : scan_result.value.warnings) {
        std::cerr << "[" << warning.severity << "] " 
                 << warning.message << " at line " << warning.line << "\n";
    }
}
```

**检测的危险模式** (16 种):
- Critical: `rm -rf`, `dd`, fork bomb, `eval`, remote exec
- High: PATH 修改, sudo, chmod, 网络监听
- Medium: 环境变量, HTTP 请求, temp 文件
- Low: git, docker 操作 (信息性)

---

## ⚙️ 配置选项

```yaml
skills:
  # 限制
  limits:
    max_skills_in_prompt: 150           # LLM Prompt 最多 Skill 数
    max_skills_prompt_chars: 30000      # LLM Prompt 最多字符数
    max_skills_loaded_per_source: 200   # 每个源最多加载 Skill 数
    max_skill_file_bytes: 256000        # 单个 SKILL.md 最大字节数
  
  # 加载
  load:
    extra_dirs:                         # 额外 Skill 目录
      - "~/custom-skills"
    watch: true                         # 热重载
    watch_debounce_ms: 500
  
  # 安装偏好
  install:
    prefer_brew: true                   # macOS/Linux 优先 Homebrew
    node_manager: "pnpm"                # npm, pnpm, yarn, bun
```

---

## 📊 API 速查

### SkillManager

| 方法 | 说明 |
|------|------|
| `load_skills(workspace_dir)` | 加载 Skills |
| `reload_skills()` | 重新加载 |
| `list_skills()` | 列出所有 Skills |
| `get_skill(name)` | 获取单个 Skill |
| `filter_eligible_skills(ctx, filter)` | 过滤符合条件的 Skills |
| `build_skills_prompt(ctx, filter)` | 构建 LLM Prompt |
| `install_skill(name, install_id)` | 安装依赖 |
| `scan_skill(name)` | 安全扫描 |
| `start_watch()` | 启动热重载 |
| `stop_watch()` | 停止热重载 |

### SkillEntry

```cpp
struct SkillEntry {
    Skill skill;                      // Skill 信息
    OpenClawSkillMetadata metadata;   // 元数据
    bool eligible;                    // 是否符合条件
    std::string ineligible_reason;    // 不符合原因
};
```

### Skill

```cpp
struct Skill {
    std::string name;         // 名称
    std::string description;  // 描述
    std::string content;      // Markdown 内容
    fs::path file_path;       // 文件路径
    fs::path base_dir;        // 基础目录
    SkillSource source;       // 来源类型
    SkillFrontmatter frontmatter;  // YAML 元数据
};
```

---

## 🎯 典型使用场景

### 场景 1: Agent 自动注入 Skills

```cpp
class MyAgent : public Agent {
    std::string build_system_prompt() const {
        std::ostringstream ss;
        
        // 基础 system prompt
        ss << "You are a helpful AI assistant...\n\n";
        
        // 注入 Skills
        skills::SkillEligibilityContext ctx;
        ctx.current_os = get_current_os();
        ctx.has_bin = [](const std::string& bin) { return check_bin_exists(bin); };
        ctx.has_env = [](const std::string& env) { return std::getenv(env.c_str()) != nullptr; };
        
        std::string skills_prompt = skill_mgr_->build_skills_prompt(ctx, config_.skills);
        ss << skills_prompt;
        
        return ss.str();
    }
};
```

### 场景 2: 用户选择性安装

```cpp
// 列出所有 Skills 和安装状态
for (const auto& entry : skill_mgr->list_skills()) {
    bool installed = skill_installer.check_dependencies(entry);
    
    std::cout << (installed ? "✅" : "❌") << " " 
              << entry.skill.name << ": " 
              << entry.skill.description << "\n";
    
    if (!installed && entry.metadata.install.size() > 0) {
        std::cout << "   Install options:\n";
        for (const auto& spec : entry.metadata.install) {
            std::cout << "   - [" << spec.id << "] " << spec.label << "\n";
        }
    }
}
```

### 场景 3: 安全审计

```cpp
// 扫描所有 Personal Skills
auto skills = skill_mgr->list_skills();

for (const auto& entry : skills) {
    if (entry.skill.source == SkillSource::Personal) {
        auto scan_result = skill_mgr->scan_skill(entry.skill.name);
        
        if (scan_result.is_ok()) {
            auto& result = scan_result.value;
            
            if (result.has_critical()) {
                std::cerr << "⚠️ " << entry.skill.name << " has CRITICAL issues!\n";
            } else if (!result.warnings.empty()) {
                std::cout << "ℹ️ " << entry.skill.name << " has " 
                         << result.warnings.size() << " warnings\n";
            }
        }
    }
}
```

---

## 🔧 常见问题

### Q: Skill 不生效?

**A**: 检查资格:
```cpp
auto entry = skill_mgr->get_skill("github");
if (entry && !entry->eligible) {
    std::cout << "Reason: " << entry->ineligible_reason << "\n";
}
```

### Q: 如何调试 Skill 加载?

**A**: 启用详细日志:
```cpp
spdlog::set_level(spdlog::level::debug);
skill_mgr->load_skills(workspace_dir);
```

### Q: 如何创建自定义 Skill?

**A**: 在 `~/.agents/skills/my-skill/SKILL.md`:
```yaml
---
name: my-skill
description: My custom skill
metadata:
  hunterclaw:
    emoji: "🔧"
---

# My Skill

使用方法...
```

---

## 📚 更多文档

- **完整对齐分析**: `/docs/SKILL_ALIGNMENT.md`
- **架构文档**: `/docs/architecture.md`
- **Agent 核心**: `/docs/agent-core.md`

---

**快速上手,立即使用！** 🚀
