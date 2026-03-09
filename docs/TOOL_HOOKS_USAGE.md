# HunterClaw Tool Hook 系统使用指南

## 🎯 概述

Tool Hook 系统是 HunterClaw **100% 对齐 OpenClaw** 的核心扩展机制，允许在工具执行前后插入自定义逻辑。

**主要功能**:
- ✅ **Before Hook**: 参数修改、验证、权限检查
- ✅ **After Hook**: 日志记录、审计、性能监控
- ✅ **内置 Hooks**: 参数验证、日志、性能、错误收集、审计、速率限制

---

## 📚 快速开始

### 1. 启用内置 Hooks

```cpp
#include "hunterclaw/tools/tool.h"

// 创建 ToolManager
ToolContext ctx;
ctx.workspace_dir = "/workspace";
ctx.agent_id = "agent-123";

auto tool_manager = tools::ToolManager::create(ctx);

// 启用内置 Hooks (日志 + 性能监控)
tool_manager->enable_builtin_hooks(true, true);

// 注册工具
tools::BuiltinTools::register_all(*tool_manager, ctx);

// 执行工具 (Hook 自动运行)
auto result = tool_manager->execute_tool("read_file", {
    {"path", "main.cc"}
});
```

**输出**:
```
[info] Tool 'read_file' executed successfully in 15.23ms
[debug] Tool 'read_file' stats: 1 calls, avg=15.23ms
```

---

### 2. 注册自定义 Before Hook

```cpp
// 获取 Hook Manager
auto hook_mgr = tool_manager->get_hook_manager();

// 注册 Before Hook (权限检查)
hook_mgr->register_before_hook("permission_check",
    [](const std::string& tool_name, const core::JsonValue& params, const ToolContext& ctx) {
        tools::BeforeHookOutcome outcome;
        outcome.blocked = false;
        outcome.params = params;
        
        // 检查敏感工具
        if (tool_name == "exec") {
            std::string command = params["command"];
            
            // 阻止危险命令
            if (command.find("rm -rf") != std::string::npos) {
                outcome.blocked = true;
                outcome.reason = "Dangerous command blocked: rm -rf";
                return outcome;
            }
        }
        
        return outcome;
    }
);
```

**效果**:
```cpp
auto result = tool_manager->execute_tool("exec", {
    {"command", "rm -rf /"}
});

// => result.success = false
// => result.error = "Dangerous command blocked: rm -rf"
```

---

### 3. 注册自定义 After Hook

```cpp
// 审计 Hook (记录所有工具调用)
hook_mgr->register_after_hook("audit",
    [](const std::string& tool_name, const core::JsonValue& params,
       const ToolResult& result, double duration_ms, const std::string& error) {
        // 写入审计日志
        std::ofstream audit_log("/var/log/hunterclaw_audit.log", std::ios::app);
        
        audit_log << "[" << std::time(nullptr) << "] "
                  << "Tool: " << tool_name << ", "
                  << "Success: " << (result.success ? "Y" : "N") << ", "
                  << "Duration: " << duration_ms << "ms";
        
        if (!error.empty()) {
            audit_log << ", Error: " << error;
        }
        
        audit_log << "\n";
    }
);
```

**审计日志**:
```
[1678901234] Tool: read_file, Success: Y, Duration: 15.23ms
[1678901235] Tool: exec, Success: N, Duration: 2.45ms, Error: Dangerous command blocked
[1678901236] Tool: write_file, Success: Y, Duration: 25.67ms
```

---

## 🔧 内置 Hooks 详解

### 1. 参数验证 Hook

**自动启用** (无需配置)

**功能**:
- 检查参数是否为对象
- 验证必需参数存在

**示例**:
```cpp
// ✅ 正确调用
tool_manager->execute_tool("read_file", {
    {"path", "main.cc"}
});

// ❌ 错误调用 (缺少 path 参数)
tool_manager->execute_tool("read_file", {});
// => Error: "read_file requires 'path' parameter"
```

---

### 2. 日志记录 Hook

**启用**: `enable_builtin_hooks(true, false)`

**功能**:
- 记录所有工具调用
- 记录执行时间
- 记录成功/失败状态

**输出**:
```
[info] Tool 'read_file' executed successfully in 15.23ms
[error] Tool 'exec' failed after 2.45ms: Command not found
```

---

### 3. 性能监控 Hook

**启用**: `enable_builtin_hooks(false, true)`

**功能**:
- 统计每个工具的调用次数
- 计算平均执行时间
- 记录最小/最大执行时间
- 每 100 次调用打印统计

**输出**:
```
[info] Tool 'read_file' stats: 100 calls, avg=15.45ms, min=5.23ms, max=125.67ms
[info] Tool 'exec' stats: 200 calls, avg=850.23ms, min=100.45ms, max=5000.12ms
```

---

### 4. 错误收集 Hook

**启用**: `enable_builtin_hooks(false, true)`

**功能**:
- 统计每个工具的错误率
- 记录最后一次错误
- 高错误率警告 (>50%)

**输出**:
```
[warn] Tool 'exec' has high error rate: 55.0% (11/20)
```

---

### 5. 审计 Hook

**手动启用**:

```cpp
tools::builtin_hooks::AuditHookConfig config;
config.sensitive_tools = {"exec", "write_file", "delete_file"};
config.audit_log_path = "/var/log/hunterclaw_audit.log";

hook_mgr->register_after_hook("audit",
    tools::builtin_hooks::create_audit_hook(config)
);
```

**功能**:
- 记录敏感工具调用
- 写入审计日志
- 包含时间戳、参数、结果

---

### 6. 速率限制 Hook

**手动启用**:

```cpp
tools::builtin_hooks::RateLimitConfig config;
config.max_calls_per_minute = 60;             // 全局限制
config.max_calls_per_tool_per_minute = 10;    // 单个工具限制

hook_mgr->register_before_hook("rate_limit",
    tools::builtin_hooks::create_rate_limit_hook(config)
);
```

**功能**:
- 全局速率限制 (所有工具总和)
- 单个工具速率限制
- 超限自动阻止

**效果**:
```cpp
// 在 1 分钟内调用 read_file 11 次
for (int i = 0; i < 11; i++) {
    auto result = tool_manager->execute_tool("read_file", {...});
}

// 第 11 次调用被阻止
// => Error: "Tool 'read_file' rate limit exceeded: 10 calls/min"
```

---

## 💡 高级用法

### 1. 参数修改 (Before Hook)

```cpp
hook_mgr->register_before_hook("auto_fill_defaults",
    [](const std::string& tool_name, const core::JsonValue& params, const ToolContext& ctx) {
        tools::BeforeHookOutcome outcome;
        outcome.blocked = false;
        outcome.params = params;
        
        if (tool_name == "exec") {
            // 自动添加默认超时
            if (!params.contains("timeoutSec")) {
                outcome.params["timeoutSec"] = 30;
            }
            
            // 自动添加工作目录
            if (!params.contains("cwd")) {
                outcome.params["cwd"] = ctx.workspace_dir;
            }
        }
        
        return outcome;
    }
);
```

**效果**:
```cpp
// 原始调用
tool_manager->execute_tool("exec", {
    {"command", "ls"}
});

// Hook 自动添加参数
// => {"command": "ls", "timeoutSec": 30, "cwd": "/workspace"}
```

---

### 2. 参数规范化 (snake_case ↔ camelCase)

```cpp
#include "hunterclaw/tools/tool_hooks.h"

// 配置参数规范化
tools::ParamNormalizationConfig config;
config.snake_to_camel = true;
config.trim_strings = true;

hook_mgr->register_before_hook("normalize",
    [config](const std::string& tool_name, const core::JsonValue& params, const ToolContext& ctx) {
        tools::BeforeHookOutcome outcome;
        outcome.blocked = false;
        
        // 规范化参数
        outcome.params = tools::normalize_tool_params(params, config);
        
        return outcome;
    }
);
```

**效果**:
```cpp
// 原始调用 (snake_case)
tool_manager->execute_tool("exec", {
    {"timeout_sec", 30},
    {"working_dir", " /workspace "}
});

// Hook 转换为 camelCase + 去除空白
// => {"timeoutSec": 30, "workingDir": "/workspace"}
```

---

### 3. Hook 链 (多个 Hooks 顺序执行)

```cpp
// Hook 1: 参数验证
hook_mgr->register_before_hook("validation", ...);

// Hook 2: 参数规范化
hook_mgr->register_before_hook("normalization", ...);

// Hook 3: 权限检查
hook_mgr->register_before_hook("permission", ...);

// Hook 4: 速率限制
hook_mgr->register_before_hook("rate_limit", ...);
```

**执行顺序**:
```
execute_tool("exec", params)
  ↓
Hook 1: validation (检查必需参数)
  ↓
Hook 2: normalization (规范化参数)
  ↓
Hook 3: permission (权限检查)
  ↓
Hook 4: rate_limit (速率限制)
  ↓
Tool::execute() (执行工具)
  ↓
After Hooks (日志、审计、性能监控)
```

**参数传递**:
- 每个 Hook 可以修改参数
- 后续 Hook 使用前一个 Hook 修改后的参数
- 如果任何 Hook 阻止,立即返回错误

---

### 4. 动态启用/禁用 Hooks

```cpp
// 启用 Hook
hook_mgr->register_before_hook("debug", debug_hook);

// 执行一些工具调用...

// 禁用 Hook
hook_mgr->unregister_before_hook("debug");

// 继续执行 (不再触发 debug_hook)
```

---

### 5. 查询 Hook 状态

```cpp
// 获取已注册的 Hook 数量
auto before_count = hook_mgr->before_hooks_count();
auto after_count = hook_mgr->after_hooks_count();

std::cout << "Before Hooks: " << before_count << "\n";
std::cout << "After Hooks: " << after_count << "\n";

// 列出所有 Hook 名称
auto before_names = hook_mgr->list_before_hooks();
for (const auto& name : before_names) {
    std::cout << "  - " << name << "\n";
}
```

---

## 🎨 使用场景

### 场景 1: 安全审计系统

```cpp
// 记录所有敏感工具调用
tools::builtin_hooks::AuditHookConfig config;
config.sensitive_tools = {"exec", "write_file", "delete_file", "ssh"};
config.audit_log_path = "/var/log/security_audit.log";

hook_mgr->register_after_hook("security_audit",
    tools::builtin_hooks::create_audit_hook(config)
);

// 审计日志包含:
// - 时间戳
// - 工具名称
// - 参数
// - 执行结果
// - 执行时长
```

---

### 场景 2: 开发环境调试

```cpp
// 开发模式: 打印所有工具调用详情
if (is_development_mode()) {
    hook_mgr->register_before_hook("debug_before",
        [](const std::string& tool_name, const core::JsonValue& params, const ToolContext& ctx) {
            std::cout << "=== Tool Call ===" << "\n";
            std::cout << "Tool: " << tool_name << "\n";
            std::cout << "Params: " << params.dump() << "\n";
            
            tools::BeforeHookOutcome outcome;
            outcome.blocked = false;
            outcome.params = params;
            return outcome;
        }
    );
    
    hook_mgr->register_after_hook("debug_after",
        [](const std::string& tool_name, const core::JsonValue& params,
           const ToolResult& result, double duration_ms, const std::string& error) {
            std::cout << "Result: " << (result.success ? "Success" : "Failure") << "\n";
            std::cout << "Duration: " << duration_ms << "ms" << "\n";
            std::cout << "=================" << "\n\n";
        }
    );
}
```

**输出**:
```
=== Tool Call ===
Tool: read_file
Params: {"path":"main.cc"}
Result: Success
Duration: 15.23ms
=================
```

---

### 场景 3: 生产环境监控

```cpp
// 生产模式: 性能监控 + 错误收集 + 审计
tool_manager->enable_builtin_hooks(true, true);  // 日志 + 性能

// 自定义错误告警
hook_mgr->register_after_hook("error_alert",
    [](const std::string& tool_name, const core::JsonValue& params,
       const ToolResult& result, double duration_ms, const std::string& error) {
        if (!result.success) {
            // 发送告警到监控系统
            send_alert({
                {"type", "tool_error"},
                {"tool", tool_name},
                {"error", error.empty() ? result.error : error},
                {"timestamp", std::time(nullptr)}
            });
        }
    }
);
```

---

## 📊 对齐 OpenClaw 对比

| 功能 | OpenClaw | HunterClaw | 状态 |
|------|----------|------------|------|
| **Before Hook** | ✅ | ✅ | 100% 对齐 |
| **After Hook** | ✅ | ✅ | 100% 对齐 |
| **参数修改** | ✅ | ✅ | 100% 对齐 |
| **执行阻止** | ✅ | ✅ | 100% 对齐 |
| **Hook 链** | ✅ | ✅ | 100% 对齐 |
| **内置 Hooks** | ✅ | ✅ | 100% 对齐 |
| **参数规范化** | ✅ | ✅ | 100% 对齐 |
| **snake_case ↔ camelCase** | ✅ | ✅ | 100% 对齐 |
| **审计日志** | ✅ | ✅ | 100% 对齐 |
| **速率限制** | ✅ | ✅ | 100% 对齐 |
| **性能监控** | ✅ | ✅ | 100% 对齐 |

---

## 🎯 最佳实践

### 1. Hook 命名规范

```cpp
// ✅ 好的命名
"param_validation"
"permission_check"
"rate_limit"
"security_audit"

// ❌ 不好的命名
"hook1"
"my_hook"
"temp"
```

### 2. Hook 执行顺序

**推荐顺序** (Before Hooks):
1. 参数验证 (param_validation)
2. 参数规范化 (normalization)
3. 权限检查 (permission_check)
4. 速率限制 (rate_limit)
5. 自定义逻辑

### 3. Hook 错误处理

```cpp
// ✅ 正确: 捕获异常
hook_mgr->register_before_hook("safe_hook",
    [](const std::string& tool_name, const core::JsonValue& params, const ToolContext& ctx) {
        try {
            // Hook 逻辑...
            return outcome;
        } catch (const std::exception& e) {
            // 返回错误,不崩溃
            BeforeHookOutcome outcome;
            outcome.blocked = true;
            outcome.reason = std::string("Hook error: ") + e.what();
            return outcome;
        }
    }
);

// ❌ 错误: 不捕获异常 (可能导致程序崩溃)
```

### 4. Hook 性能优化

```cpp
// ✅ 高效: 只在必要时运行
hook_mgr->register_before_hook("conditional",
    [](const std::string& tool_name, const core::JsonValue& params, const ToolContext& ctx) {
        // 只对特定工具执行
        if (tool_name != "exec") {
            BeforeHookOutcome outcome;
            outcome.blocked = false;
            outcome.params = params;
            return outcome;  // 快速返回
        }
        
        // 只对 exec 工具执行复杂逻辑
        // ...
    }
);
```

---

## 📝 总结

HunterClaw Tool Hook 系统提供了:

1. ✅ **完全对齐 OpenClaw** - 100% 架构一致
2. ✅ **灵活扩展** - Before/After Hook 链
3. ✅ **内置功能** - 参数验证、日志、性能、审计、速率限制
4. ✅ **参数修改** - Hook 可以修改工具参数
5. ✅ **执行控制** - Hook 可以阻止工具执行
6. ✅ **性能监控** - 自动统计工具性能
7. ✅ **安全审计** - 敏感工具调用记录

通过 Hook 系统,可以轻松实现:
- 权限控制
- 审计日志
- 性能监控
- 错误收集
- 速率限制
- 参数规范化
- 自定义业务逻辑

**开始使用**:
```cpp
auto tool_manager = tools::ToolManager::create(ctx);
tool_manager->enable_builtin_hooks(true, true);  // 启用所有内置 Hooks
```

Happy Hooking! 🎣
