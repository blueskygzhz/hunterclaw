# HunterClaw Tool 系统快速参考

## 🚀 快速开始 (30秒)

```cpp
#include "hunterclaw/tools/tool.h"
#include "hunterclaw/tools/builtin_tools.h"

using namespace hunterclaw::tools;

// 1. 创建 ToolManager
ToolContext ctx;
ctx.workspace_dir = "/workspace";
auto tool_mgr = ToolManager::create(ctx);

// 2. 注册内置工具
BuiltinTools::register_all(*tool_mgr, ctx);

// 3. 启用所有增强功能
tool_mgr->enable_builtin_hooks(true, true);     // Hook 系统
tool_mgr->enable_loop_detection(true);          // 循环检测
tool_mgr->apply_profile("standard");            // 策略: 标准模式
tool_mgr->enable_policy_pipeline(true);
tool_mgr->enable_result_normalization(true);    // 结果规范化

// 4. 执行工具
auto result = tool_mgr->execute_tool("read_file", {{"path", "main.cc"}});
```

---

## 📋 功能速查表

| 功能 | 启用方法 | 说明 |
|------|---------|------|
| **Hook 系统** | `enable_builtin_hooks(true, true)` | 参数验证、日志、性能监控 |
| **循环检测** | `enable_loop_detection(true)` | 防止工具重复调用 |
| **策略管道** | `apply_profile("standard")` + `enable_policy_pipeline(true)` | 权限控制、安全策略 |
| **结果规范化** | `enable_result_normalization(true)` | 图片消毒、敏感信息移除 |
| **MCP 集成** | `enable_mcp(config)` + `refresh_mcp_tools()` | 动态工具发现 |

---

## 🎯 常用场景

### 场景 1: 安全沙箱 (生产环境)

```cpp
auto tool_mgr = ToolManager::create(ctx);
BuiltinTools::register_all(*tool_mgr, ctx);

// 受限模式: 只允许只读工具
tool_mgr->apply_profile("restricted");
tool_mgr->enable_policy_pipeline(true);
tool_mgr->enable_result_normalization(true);

// ✅ 允许: read_file, list_dir, search_content
// ❌ 拒绝: write_file, exec
```

### 场景 2: 标准开发 (日常使用)

```cpp
auto tool_mgr = ToolManager::create(ctx);
BuiltinTools::register_all(*tool_mgr, ctx);

// 标准模式: 限制危险操作
tool_mgr->apply_profile("standard");
tool_mgr->enable_policy_pipeline(true);
tool_mgr->enable_loop_detection(true);

// ✅ 允许: 大部分工具
// ⚠️ 需审批: exec (危险命令)
// 🔄 检测: 工具循环调用
```

### 场景 3: 完全访问 (开发调试)

```cpp
auto tool_mgr = ToolManager::create(ctx);
BuiltinTools::register_all(*tool_mgr, ctx);

// 开发模式: 完全访问 + 危险检测
tool_mgr->apply_profile("developer");
tool_mgr->enable_policy_pipeline(true);
tool_mgr->enable_builtin_hooks(true, true);

// ✅ 允许: 所有工具
// ⚠️ 检测: 危险命令 (提示警告)
// 📊 监控: 完整日志和性能统计
```

---

## 🛠️ 自定义配置

### 自定义策略

```cpp
// 1. 获取策略管道
auto pipeline = tool_mgr->get_policy_pipeline();

// 2. 添加白名单
std::set<std::string> allowed = {"read_file", "write_file", "list_dir"};
pipeline->register_policy(std::make_shared<WhitelistPolicy>(allowed));

// 3. 添加文件路径限制
std::vector<std::string> allowed_paths = {"/workspace", "/tmp"};
pipeline->register_policy(std::make_shared<FilePathPolicy>(allowed_paths));

// 4. 添加速率限制
RateLimitPolicy::RateLimitConfig rate_cfg;
rate_cfg.max_calls = 100;
rate_cfg.time_window_seconds = 60;
pipeline->register_policy(std::make_shared<RateLimitPolicy>(rate_cfg));

// 5. 启用策略
tool_mgr->enable_policy_pipeline(true);
```

### 自定义 Hook

```cpp
auto hook_mgr = tool_mgr->get_hook_manager();

// Before Hook: 参数修改
hook_mgr->register_before_hook("custom", 
    [](const std::string& tool_name, const core::JsonValue& params, const ToolContext& ctx) {
        BeforeHookOutcome outcome;
        outcome.blocked = false;
        outcome.params = params;
        
        // 强制添加 workspace 前缀
        if (params.has_key("path")) {
            std::string path = params["path"].as_string();
            if (path[0] != '/') {
                outcome.params["path"] = ctx.workspace_dir + "/" + path;
            }
        }
        
        return outcome;
    }
);

// After Hook: 自定义日志
hook_mgr->register_after_hook("custom_log",
    [](const std::string& tool_name, const core::JsonValue& params, 
       const ToolResult& result, double duration_ms, const std::string& error) {
        if (tool_name == "exec") {
            log()->warn("Exec tool used: {}", params["command"].as_string());
        }
    }
);
```

### 自定义结果处理

```cpp
auto result_pipeline = tool_mgr->get_result_pipeline();

// 自定义处理器: 添加时间戳
result_pipeline->add_processor("timestamp",
    [](const ToolResult& result) -> ToolResult {
        ToolResult processed = result;
        processed.details["timestamp"] = std::time(nullptr);
        return processed;
    }
);

// 自定义处理器: 压缩大型结果
result_pipeline->add_processor("compress",
    [](const ToolResult& result) -> ToolResult {
        ToolResult processed = result;
        if (result.content.size() > 10000) {
            // 压缩逻辑...
        }
        return processed;
    }
);
```

---

## 🔍 调试与监控

### 查看已注册工具

```cpp
auto tools = tool_mgr->list_tools();
for (const auto& name : tools) {
    log()->info("Tool: {}", name);
}
```

### 查看循环检测历史

```cpp
auto detector = tool_mgr->get_loop_detector();
auto history = detector->get_history();

for (const auto& record : history) {
    log()->info("{}: {} (success: {})", 
        record.timestamp, record.tool_name, record.success);
}
```

### 查看策略列表

```cpp
auto pipeline = tool_mgr->get_policy_pipeline();
auto policies = pipeline->list_policies();

for (const auto& name : policies) {
    log()->info("Policy: {}", name);
}
```

### 查看结果处理器

```cpp
auto result_pipeline = tool_mgr->get_result_pipeline();
auto processors = result_pipeline->list_processors();

for (const auto& name : processors) {
    log()->info("Processor: {}", name);
}
```

---

## ⚠️ 常见问题

### Q: 工具被策略拒绝怎么办?

```cpp
// 方式1: 切换到更宽松的 Profile
tool_mgr->apply_profile("developer");

// 方式2: 临时禁用策略
tool_mgr->enable_policy_pipeline(false);
auto result = tool_mgr->execute_tool("exec", {...});
tool_mgr->enable_policy_pipeline(true);

// 方式3: 添加工具到白名单
auto pipeline = tool_mgr->get_policy_pipeline();
std::set<std::string> allowed = {"exec"};
pipeline->register_policy(std::make_shared<WhitelistPolicy>(allowed));
```

### Q: 检测到循环调用怎么办?

```cpp
// 方式1: 清除循环检测历史
auto detector = tool_mgr->get_loop_detector();
detector->clear_history();

// 方式2: 调整检测阈值
LoopDetectorConfig config;
config.exact_repeat_threshold = 5;  // 默认3, 增加到5
detector->set_config(config);

// 方式3: 临时禁用循环检测
tool_mgr->enable_loop_detection(false);
```

### Q: 性能开销太大怎么办?

```cpp
// 最小化配置
tool_mgr->enable_builtin_hooks(false, false);  // 关闭详细日志
tool_mgr->enable_loop_detection(false);        // 关闭循环检测
tool_mgr->enable_result_normalization(false);  // 关闭结果处理

// 只保留策略检查
tool_mgr->apply_profile("restricted");
tool_mgr->enable_policy_pipeline(true);
```

---

## 📊 性能基准

| 配置 | 单次调用耗时 | 额外开销 |
|------|-------------|---------|
| **最小配置** | ~10-15ms | +0ms |
| **+ Hook** | ~12-17ms | +2ms |
| **+ 循环检测** | ~13-18ms | +1ms |
| **+ 策略管道** | ~14-19ms | +1ms |
| **+ 结果规范化** | ~17-22ms | +3ms |
| **完整配置** | ~20-30ms | +10ms |

*测试环境: Intel i7, 16GB RAM, Ubuntu 22.04*

---

## 🎯 Profile 对比

| Profile | 安全级别 | 性能开销 | 适用场景 |
|---------|---------|---------|---------|
| **restricted** | 🔒🔒🔒🔒🔒 | 低 | 生产环境、不可信 Agent |
| **standard** | 🔒🔒🔒🔒 | 中 | 日常开发、常规任务 |
| **developer** | 🔒🔒 | 高 | 开发调试、完整监控 |
| **production** | 🔒🔒🔒🔒🔒 | 中 | 生产部署、严格限制 |

---

## 📖 深入学习

- [完整对齐报告](./TOOL_COMPLETE_ALIGNMENT.md) - 对齐成果总结
- [Hook 使用指南](./TOOL_HOOKS_USAGE.md) - Hook 系统详解
- [对齐分析](./TOOL_ALIGNMENT_ANALYSIS.md) - 对齐过程分析
- [加载指南](./TOOL_LOADING_GUIDE.md) - Tool 加载机制

---

**快速上手,安全高效!** 🚀
