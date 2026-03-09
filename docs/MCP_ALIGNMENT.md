# HunterClaw MCP 能力对齐报告

## 📋 执行摘要

**OpenClaw 的 MCP 策略**: 通过外部 `mcporter` 桥接实现 MCP 支持,避免内置 MCP 运行时带来的复杂性和安全风险。

**HunterClaw 当前状态**: **完全缺失 MCP 支持** (0% 对齐)

**推荐方案**: 采用与 OpenClaw 一致的 `mcporter` 桥接策略

---

## 🎯 OpenClaw MCP 架构分析

### 1. 核心设计哲学

**OpenClaw 明确表示**:
> "For now, we prefer this bridge model over building first-class MCP runtime into core."

**设计原则**:
1. **解耦原则** - 核心保持轻量,MCP 功能外部化
2. **灵活性优先** - 无需重启即可添加/更改 MCP servers
3. **安全性考虑** - MCP 工具调用通过独立进程隔离
4. **稳定性保护** - MCP 变更不影响核心稳定性

### 2. MCP 集成架构

```
┌─────────────────────────────────────────────────────────────┐
│                     OpenClaw Gateway                         │
│                                                              │
│  ┌──────────────────┐                                       │
│  │  ACP Protocol    │  ← mcpServers[] 参数（被忽略）       │
│  │                  │  ← mcpCapabilities: {http:false}     │
│  └──────────────────┘                                       │
│                                                              │
│  ┌──────────────────────────────────────────┐              │
│  │  Memory/QMD Manager                      │              │
│  │                                           │              │
│  │  ┌──────────────────────────────────┐   │              │
│  │  │ mcporter Integration             │   │              │
│  │  │ • Daemon 管理                    │   │              │
│  │  │ • 工具调用路由                   │   │              │
│  │  │ • 跨集合搜索                     │   │              │
│  │  │ • Windows .cmd shim 解析         │   │              │
│  │  └──────────────────────────────────┘   │              │
│  └──────────────────────────────────────────┘              │
└───────────────────────┬─────────────────────────────────────┘
                        │
                        │ spawn mcporter CLI
                        ▼
            ┌───────────────────────────┐
            │   mcporter (外部工具)      │
            │                           │
            │  • MCP Runtime            │
            │  • Daemon 模式            │
            │  • HTTP/Stdio 传输        │
            │  • 工具调用代理           │
            │  • 配置管理               │
            └─────────────┬─────────────┘
                          │
                          │ MCP Protocol
                          ▼
            ┌───────────────────────────┐
            │   MCP Servers             │
            │                           │
            │  • qmd mcp (QMD 搜索)     │
            │  • 其他 MCP servers       │
            │    (Linear, GitHub, etc.) │
            └───────────────────────────┘
```

### 3. OpenClaw 的 MCP 实现范围

#### ✅ **已实现**

1. **Memory/QMD mcporter 集成**
   - `src/memory/qmd-manager.ts` - 核心实现 (1970行)
   - `src/memory/backend-config.ts` - 配置解析
   - `src/config/types.memory.ts` - 类型定义

2. **mcporter Daemon 管理**
   - 自动启动/停止 daemon
   - 全局单例 (globalThis)
   - 冷启动警告

3. **跨集合搜索**
   - 并发查询多个 collection
   - 按分数合并去重结果

4. **Windows 平台支持**
   - `.cmd` 后缀解析
   - 防止 `spawn ENOENT` 错误

5. **mcporter Skill**
   - `skills/mcporter/SKILL.md`
   - CLI 调用封装

#### ❌ **未实现**

1. **ACP 协议中的 mcpServers 参数**
   - `mcpServers[]` 被忽略 (记录日志但不处理)
   - `mcpCapabilities: { http: false, sse: false }`

2. **通用 MCP Tools 集成**
   - 没有统一的 MCP 工具发现机制
   - 没有动态加载 MCP tools

3. **MCP Resources**
   - 不支持 MCP Resources 订阅
   - 不支持资源更新通知

4. **MCP Prompts**
   - 不支持动态 Prompts 加载
   - 不支持 Prompts 参数化

---

## 🔍 HunterClaw 当前状态

### ✅ **已有基础能力**

1. **Tool 系统**
   - `include/hunterclaw/tools/tool.h` (185行)
   - `src/tools/tool_manager.cc` (5.4 KB)
   - `src/tools/builtin_tools.cc` (16.2 KB)
   - 支持工具注册、执行、LLM 格式转换

2. **插件系统**
   - `include/hunterclaw/plugins/plugin.h`
   - 插件生命周期管理
   - 配置加载

3. **Skill 系统**
   - `include/hunterclaw/skills/manager.h`
   - `src/skills/manager.cc` (192行)
   - Skill 发现和安装

4. **执行环境**
   - `include/hunterclaw/sandbox/docker_sandbox.h`
   - `src/sandbox/docker_sandbox.cc` (363行)
   - 支持命令执行和隔离

### ❌ **缺失的 MCP 能力**

| MCP 功能 | OpenClaw | HunterClaw | 差距 |
|---------|----------|------------|------|
| **mcporter 集成** | ✅ | ❌ | **100%** |
| **MCP Server 管理** | ✅ (Daemon) | ❌ | **100%** |
| **MCP Tools 调用** | ✅ | ❌ | **100%** |
| **MCP 工具发现** | ✅ (mcporter list) | ❌ | **100%** |
| **MCP Resources** | ⚠️ (通过 mcporter) | ❌ | - |
| **MCP Prompts** | ⚠️ (通过 mcporter) | ❌ | - |
| **配置管理** | ✅ | ❌ | **100%** |

**总体对齐度**: **0%**

---

## 🎯 对齐方案设计

### 阶段 1: mcporter 集成基础 (核心)

#### 1.1 MCP Client 抽象

**新增文件**: `include/hunterclaw/mcp/mcporter_client.h`

```cpp
#pragma once

#include "hunterclaw/core/types.h"
#include "hunterclaw/core/result.h"
#include <string>
#include <vector>
#include <memory>

namespace hunterclaw {
namespace mcp {

// mcporter 配置
struct McporterConfig {
    bool enabled = false;
    std::string server_name = "default";  // MCP server 名称
    bool start_daemon = true;             // 自动启动 daemon
    int timeout_ms = 30000;               // 超时时间
};

// mcporter 工具参数
struct McporterCallArgs {
    std::string server_and_tool;          // e.g. "linear.list_issues"
    core::JsonValue args;                 // 工具参数
    std::string output_format = "json";   // json/text/pretty
};

// mcporter 调用结果
struct McporterResult {
    bool success;
    std::string stdout_output;
    std::string stderr_output;
    core::JsonValue structured_content;   // 解析后的 JSON
    std::string error;
};

// mcporter 工具定义
struct McpToolDefinition {
    std::string name;           // 完整名称 "server.tool"
    std::string description;
    std::string server_name;
    std::string tool_name;
    core::JsonValue input_schema;  // JSON Schema
};

/// mcporter Client - 桥接 MCP servers
class McporterClient {
public:
    static std::shared_ptr<McporterClient> create(const McporterConfig& config);
    
    virtual ~McporterClient() = default;
    
    // ========== Daemon 管理 ==========
    
    /// 启动 mcporter daemon
    virtual core::Result<void> start_daemon() = 0;
    
    /// 停止 mcporter daemon
    virtual core::Result<void> stop_daemon() = 0;
    
    /// 检查 daemon 状态
    virtual core::Result<bool> is_daemon_running() = 0;
    
    /// 确保 daemon 已启动 (全局单例,避免重复启动)
    virtual core::Result<void> ensure_daemon_started() = 0;
    
    // ========== 工具发现 ==========
    
    /// 列出所有已配置的 MCP servers
    virtual core::Result<std::vector<std::string>> list_servers() = 0;
    
    /// 列出指定 server 的所有 tools
    virtual core::Result<std::vector<McpToolDefinition>> list_tools(
        const std::string& server_name,
        bool with_schema = true
    ) = 0;
    
    /// 获取所有可用的 MCP tools (跨所有 servers)
    virtual core::Result<std::vector<McpToolDefinition>> discover_all_tools() = 0;
    
    // ========== 工具调用 ==========
    
    /// 调用 MCP 工具
    /// @param server_and_tool 格式: "server.tool" 或 "server:tool"
    /// @param args 工具参数 (JSON)
    virtual core::Result<McporterResult> call_tool(
        const std::string& server_and_tool,
        const core::JsonValue& args
    ) = 0;
    
    /// 批量调用 (并发)
    virtual core::Result<std::vector<McporterResult>> call_tools(
        const std::vector<McporterCallArgs>& calls
    ) = 0;
    
    // ========== 配置管理 ==========
    
    /// 获取 mcporter 配置
    virtual core::Result<core::JsonValue> get_config() = 0;
    
    /// 添加 MCP server 配置
    virtual core::Result<void> add_server(
        const std::string& name,
        const std::string& command,
        const core::JsonValue& options = {}
    ) = 0;
    
    /// 删除 MCP server 配置
    virtual core::Result<void> remove_server(const std::string& name) = 0;
    
    // ========== 实用功能 ==========
    
    /// 验证 mcporter 是否已安装
    static bool is_mcporter_installed();
    
    /// 获取 mcporter 版本
    static core::Result<std::string> get_mcporter_version();
    
private:
    McporterConfig config_;
};

// 工厂函数
std::shared_ptr<McporterClient> create_mcporter_client(const McporterConfig& config);

} // namespace mcp
} // namespace hunterclaw
```

#### 1.2 MCP Tool Adapter

**新增文件**: `include/hunterclaw/mcp/mcp_tool_adapter.h`

```cpp
#pragma once

#include "hunterclaw/tools/tool.h"
#include "hunterclaw/mcp/mcporter_client.h"
#include <memory>

namespace hunterclaw {
namespace mcp {

/// 将 MCP Tool 适配为 HunterClaw Tool
class McpToolAdapter : public tools::Tool {
public:
    McpToolAdapter(
        std::shared_ptr<McporterClient> client,
        const McpToolDefinition& definition
    );
    
    virtual ~McpToolAdapter() = default;
    
    // Tool 接口实现
    std::string name() const override;
    std::string description() const override;
    Schema parameters() const override;
    tools::ToolResult execute(const core::JsonValue& params) override;
    bool requires_approval() const override;
    
private:
    std::shared_ptr<McporterClient> client_;
    McpToolDefinition definition_;
    
    // 将 JSON Schema 转换为 Tool::Schema
    Schema convert_json_schema(const core::JsonValue& json_schema) const;
};

/// MCP Tools 注册器
class McpToolRegistry {
public:
    static std::shared_ptr<McpToolRegistry> create(
        std::shared_ptr<McporterClient> client
    );
    
    /// 从 MCP server 发现并注册所有 tools
    core::Result<int> discover_and_register(
        tools::ToolManager& tool_manager,
        const std::vector<std::string>& server_names = {}  // 空则注册所有
    );
    
    /// 从指定 server 注册 tools
    core::Result<int> register_server_tools(
        tools::ToolManager& tool_manager,
        const std::string& server_name
    );
    
private:
    std::shared_ptr<McporterClient> client_;
};

} // namespace mcp
} // namespace hunterclaw
```

---

### 阶段 2: Tool 系统集成

#### 2.1 扩展 ToolManager

**修改文件**: `include/hunterclaw/tools/tool.h`

```cpp
class ToolManager {
public:
    // ... 现有方法 ...
    
    // ========== MCP 集成 ==========
    
    /// 启用 MCP 工具发现
    void enable_mcp(const mcp::McporterConfig& config);
    
    /// 刷新 MCP 工具列表
    core::Result<int> refresh_mcp_tools();
    
    /// 获取 MCP client (如果已启用)
    std::shared_ptr<mcp::McporterClient> get_mcp_client();
    
private:
    std::shared_ptr<mcp::McporterClient> mcp_client_;
    std::shared_ptr<mcp::McpToolRegistry> mcp_registry_;
};
```

#### 2.2 配置支持

**修改文件**: `include/hunterclaw/core/config.h`

```cpp
struct ToolsConfig {
    bool enable_builtin = true;
    
    // MCP 配置
    struct {
        bool enabled = false;
        std::string server_name = "default";
        bool start_daemon = true;
        int timeout_ms = 30000;
        std::vector<std::string> allowed_servers;  // 白名单
    } mcp;
};
```

---

### 阶段 3: Skill 系统集成

#### 3.1 mcporter Skill

**新增文件**: `skills/mcporter/SKILL.md`

```markdown
---
name: mcporter
description: Use mcporter CLI to list, configure, auth, and call MCP servers/tools directly (HTTP or stdio)
homepage: http://mcporter.dev
metadata:
  hunterclaw:
    emoji: "📦"
    requires:
      bins: ["mcporter"]
    install:
      - id: node
        kind: node
        package: mcporter
        bins: ["mcporter"]
        label: "Install mcporter (node)"
---

# mcporter Skill

## Overview

`mcporter` is a bridge tool that allows HunterClaw to interact with MCP (Model Context Protocol) servers without embedding a full MCP runtime.

## Prerequisites

```bash
# Install mcporter
npm install -g mcporter

# Or via pnpm
pnpm install -g mcporter
```

## Usage

### 1. List MCP Servers and Tools

```bash
mcporter list                    # List all configured servers
mcporter list <server> --schema  # List tools with schema
```

### 2. Call MCP Tools

```bash
# Key-value syntax
mcporter call linear.list_issues team=ENG limit:5

# Function call syntax
mcporter call "linear.create_issue(title: \"Bug\")"

# JSON payload
mcporter call github.search_repos --args '{"query":"hunterclaw","limit":10}'
```

### 3. Configure MCP Servers

```bash
# Add server
mcporter config add qmd --command "qmd mcp" --lifecycle keep-alive

# List config
mcporter config list

# Remove server
mcporter config remove qmd
```

### 4. Daemon Management

```bash
mcporter daemon start    # Start daemon for faster calls
mcporter daemon status
mcporter daemon stop
```

## Integration with HunterClaw

### Enable in Config

```json
{
  "tools": {
    "mcp": {
      "enabled": true,
      "server_name": "default",
      "start_daemon": true,
      "allowed_servers": ["linear", "github", "qmd"]
    }
  }
}
```

### Automatic Tool Discovery

When MCP is enabled, HunterClaw will:
1. Auto-start mcporter daemon
2. Discover all configured MCP servers
3. Register MCP tools as native HunterClaw tools
4. Make them available to AI agents

### Example

```cpp
// C++ API
auto tool_manager = tools::ToolManager::create(context);

// Enable MCP
mcp::McporterConfig mcp_config;
mcp_config.enabled = true;
mcp_config.start_daemon = true;
tool_manager->enable_mcp(mcp_config);

// Auto-discover MCP tools
auto count = tool_manager->refresh_mcp_tools();
// Now "linear.list_issues", "github.search_repos" etc. are available

// Call via ToolManager (seamless)
auto result = tool_manager->execute_tool("linear.list_issues", {
    {"team", "ENG"},
    {"limit", 5}
});
```

## Security Considerations

- mcporter runs as a separate process (isolation)
- Daemon mode reduces cold start latency
- Use `allowed_servers` to whitelist trusted servers
- Sensitive APIs should use `requires_approval = true`

## Troubleshooting

### Windows: `spawn mcporter ENOENT`
**Solution**: HunterClaw auto-resolves to `mcporter.cmd`

### Slow calls
**Solution**: Enable daemon mode (`start_daemon: true`)

### Tool not found
**Solution**: Check config with `mcporter config list`
```

#### 3.2 Skill 自动安装

**修改文件**: `src/skills/manager.cc`

```cpp
core::Result<void> SkillManager::ensure_mcporter() {
    // 检查是否已安装
    if (mcp::McporterClient::is_mcporter_installed()) {
        return core::Result<void>::Ok();
    }
    
    // 尝试自动安装
    log_->info("mcporter not found, attempting to install via npm...");
    
    // 执行安装
    auto install_result = sandbox_->execute_command({
        "npm", "install", "-g", "mcporter"
    });
    
    if (install_result.is_err()) {
        return core::Result<void>::Err(
            "Failed to install mcporter: " + install_result.error
        );
    }
    
    // 验证安装
    if (!mcp::McporterClient::is_mcporter_installed()) {
        return core::Result<void>::Err("mcporter installation failed");
    }
    
    log_->info("mcporter installed successfully");
    return core::Result<void>::Ok();
}
```

---

### 阶段 4: Agent 系统集成

#### 4.1 Agent Tool Context 扩展

**修改文件**: `src/agents/react_executor.cc`

```cpp
void ReactExecutor::initialize_tools() {
    // 创建 ToolManager
    tools::ToolContext context;
    context.workspace_dir = workspace_dir_;
    context.agent_id = agent_id_;
    context.session_key = session_key_;
    
    tool_manager_ = tools::ToolManager::create(context);
    
    // 注册内置工具
    tools::BuiltinTools::register_all(*tool_manager_, context);
    
    // 启用 MCP (如果配置)
    if (config_.tools.mcp.enabled) {
        mcp::McporterConfig mcp_config;
        mcp_config.enabled = true;
        mcp_config.server_name = config_.tools.mcp.server_name;
        mcp_config.start_daemon = config_.tools.mcp.start_daemon;
        mcp_config.timeout_ms = config_.tools.mcp.timeout_ms;
        
        tool_manager_->enable_mcp(mcp_config);
        
        // 异步刷新 MCP 工具
        auto refresh_future = std::async(std::launch::async, [this]() {
            auto result = tool_manager_->refresh_mcp_tools();
            if (result.is_ok()) {
                log_->info("Discovered {} MCP tools", result.value);
            } else {
                log_->warn("Failed to refresh MCP tools: {}", result.error);
            }
        });
    }
}
```

---

## 📊 实现路线图

### 阶段 1: mcporter 集成基础 (1周)

**目标**: 实现 mcporter CLI 封装和基础调用

**交付物**:
- ✅ `include/hunterclaw/mcp/mcporter_client.h` (200行)
- ✅ `src/mcp/mcporter_client.cc` (500行)
- ✅ `include/hunterclaw/mcp/mcp_tool_adapter.h` (100行)
- ✅ `src/mcp/mcp_tool_adapter.cc` (300行)
- ✅ Daemon 管理 (启动/停止/状态检查)
- ✅ 工具发现 (`mcporter list`)
- ✅ 工具调用 (`mcporter call`)
- ✅ Windows `.cmd` shim 解析

**验收标准**:
```cpp
// 1. Daemon 管理
auto client = mcp::create_mcporter_client({
    .enabled = true,
    .start_daemon = true
});
auto started = client->ensure_daemon_started();
assert(started.is_ok());

// 2. 工具发现
auto tools = client->discover_all_tools();
assert(tools.is_ok());
assert(tools.value.size() > 0);

// 3. 工具调用
auto result = client->call_tool("qmd.search", {
    {"query", "test"},
    {"limit", 5}
});
assert(result.is_ok());
assert(result.value.success);
```

---

### 阶段 2: Tool 系统集成 (3天)

**目标**: MCP Tools 无缝集成到 HunterClaw Tool 系统

**交付物**:
- ✅ `McpToolAdapter` 实现 (将 MCP tool 适配为 Tool)
- ✅ `McpToolRegistry` 实现 (批量注册)
- ✅ `ToolManager::enable_mcp()` 方法
- ✅ `ToolManager::refresh_mcp_tools()` 方法
- ✅ 配置支持 (`ToolsConfig::mcp`)

**验收标准**:
```cpp
// 1. 启用 MCP
auto tool_mgr = tools::ToolManager::create(context);
mcp::McporterConfig mcp_config{.enabled = true};
tool_mgr->enable_mcp(mcp_config);

// 2. 刷新工具
auto count = tool_mgr->refresh_mcp_tools();
assert(count.is_ok());

// 3. 调用 MCP tool (透明)
auto result = tool_mgr->execute_tool("linear.list_issues", {
    {"team", "ENG"}
});
assert(result.success);

// 4. 转换为 LLM 格式
auto llm_tools = tool_mgr->to_llm_format();
// 包含 builtin + MCP tools
```

---

### 阶段 3: Skill 和配置 (2天)

**目标**: Skill 封装和配置管理

**交付物**:
- ✅ `skills/mcporter/SKILL.md` (完整文档)
- ✅ `SkillManager::ensure_mcporter()` (自动安装)
- ✅ 配置验证和加载
- ✅ 白名单机制 (`allowed_servers`)

**验收标准**:
```json
// 配置文件
{
  "tools": {
    "mcp": {
      "enabled": true,
      "server_name": "default",
      "start_daemon": true,
      "allowed_servers": ["linear", "github", "qmd"]
    }
  }
}
```

```bash
# Skill 自动安装
hunterclaw skill install mcporter
# => npm install -g mcporter
```

---

### 阶段 4: Agent 集成和测试 (2天)

**目标**: Agent 自动发现和使用 MCP tools

**交付物**:
- ✅ `ReactExecutor::initialize_tools()` 集成 MCP
- ✅ 异步工具刷新 (避免阻塞启动)
- ✅ 单元测试
- ✅ 集成测试

**验收标准**:
```cpp
// Agent 启动时自动加载 MCP tools
auto agent = agents::ReactAgent::create(config);
agent->start();

// AI 可以自动发现和调用 MCP tools
auto response = agent->process_message("@hunterclaw search my Linear issues");
// => 自动调用 linear.list_issues
```

---

## 🎯 对齐验证清单

### 功能对齐

| 功能 | OpenClaw | HunterClaw (目标) | 优先级 |
|------|----------|-------------------|--------|
| mcporter CLI 封装 | ✅ | 🎯 阶段1 | P0 |
| Daemon 管理 | ✅ | 🎯 阶段1 | P0 |
| 工具发现 | ✅ | 🎯 阶段1 | P0 |
| 工具调用 | ✅ | 🎯 阶段1 | P0 |
| Tool 系统集成 | ✅ | 🎯 阶段2 | P0 |
| Skill 封装 | ✅ | 🎯 阶段3 | P0 |
| Agent 集成 | ✅ | 🎯 阶段4 | P0 |
| 跨集合搜索 | ✅ | ⏸️ 可选 | P2 |
| Windows 支持 | ✅ | 🎯 阶段1 | P1 |
| 配置管理 | ✅ | 🎯 阶段3 | P0 |
| Resources 订阅 | ⚠️ | ⏸️ 未计划 | P3 |
| Prompts 加载 | ⚠️ | ⏸️ 未计划 | P3 |

### 架构对齐

| 架构特性 | OpenClaw | HunterClaw (目标) | 备注 |
|---------|----------|-------------------|------|
| 外部桥接 | ✅ mcporter | 🎯 mcporter | 完全一致 |
| 进程隔离 | ✅ spawn | 🎯 spawn | 安全隔离 |
| Daemon 模式 | ✅ | 🎯 | 减少冷启动 |
| 全局单例 | ✅ globalThis | 🎯 static | 防止重复启动 |
| 配置解耦 | ✅ | 🎯 | 无需重启 |

---

## 🚀 使用示例

### 示例 1: 基础 MCP 集成

```cpp
#include "hunterclaw/mcp/mcporter_client.h"
#include "hunterclaw/tools/tool.h"

// 1. 创建 mcporter client
mcp::McporterConfig config;
config.enabled = true;
config.start_daemon = true;

auto client = mcp::create_mcporter_client(config);

// 2. 启动 daemon
auto started = client->ensure_daemon_started();
if (started.is_err()) {
    std::cerr << "Failed to start daemon: " << started.error << std::endl;
    return;
}

// 3. 发现工具
auto tools = client->discover_all_tools();
if (tools.is_ok()) {
    std::cout << "Discovered " << tools.value.size() << " MCP tools:\n";
    for (const auto& tool : tools.value) {
        std::cout << "  - " << tool.name << ": " << tool.description << "\n";
    }
}

// 4. 调用工具
auto result = client->call_tool("linear.list_issues", {
    {"team", "ENG"},
    {"limit", 5}
});

if (result.is_ok() && result.value.success) {
    std::cout << "Result: " << result.value.structured_content.dump() << std::endl;
}
```

### 示例 2: Tool 系统集成

```cpp
#include "hunterclaw/tools/tool.h"
#include "hunterclaw/mcp/mcp_tool_adapter.h"

// 1. 创建 ToolManager
tools::ToolContext context;
context.workspace_dir = "/workspace";
auto tool_mgr = tools::ToolManager::create(context);

// 2. 启用 MCP
mcp::McporterConfig mcp_config{
    .enabled = true,
    .server_name = "default",
    .start_daemon = true
};
tool_mgr->enable_mcp(mcp_config);

// 3. 刷新 MCP 工具
auto count = tool_mgr->refresh_mcp_tools();
std::cout << "Registered " << count.value << " MCP tools\n";

// 4. 调用 MCP tool (透明,和 builtin tools 一样)
auto result = tool_mgr->execute_tool("linear.create_issue", {
    {"title", "New bug"},
    {"description", "Found a bug in the system"},
    {"team", "ENG"}
});

if (result.success) {
    std::cout << "Issue created: " << result.content[0].text << std::endl;
}

// 5. 转换为 LLM 格式 (包含 MCP tools)
auto llm_tools = tool_mgr->to_llm_format();
// AI 现在可以发现和调用 MCP tools
```

### 示例 3: Agent 使用 MCP Tools

```cpp
#include "hunterclaw/agents/react_executor.h"

// 配置 Agent (MCP 自动启用)
agents::AgentConfig config;
config.tools.mcp.enabled = true;
config.tools.mcp.allowed_servers = {"linear", "github", "qmd"};

// 创建 Agent
auto agent = agents::ReactAgent::create(config);

// AI 提示词
std::string prompt = R"(
You now have access to MCP tools:
- linear.list_issues: Search Linear issues
- linear.create_issue: Create new issues
- github.search_repos: Search GitHub repositories
- qmd.search: Search documents in memory

Use these tools to help the user.
)";

// 用户请求
auto response = agent->process_message(
    "Find all open bugs in the ENG team and summarize them"
);

// AI 自动:
// 1. 调用 linear.list_issues(team="ENG", status="open", label="bug")
// 2. 解析结果
// 3. 生成摘要
```

---

## 📈 性能考虑

### Daemon 模式优化

**问题**: 每次调用 `mcporter call` 都会冷启动 MCP server,延迟高

**解决方案**:
```cpp
mcp::McporterConfig config;
config.start_daemon = true;  // 全局 daemon,保持 MCP servers 热启动
```

**性能对比**:
| 模式 | 首次调用 | 后续调用 | 适用场景 |
|------|---------|---------|---------|
| **冷启动** | ~2-5s | ~2-5s | 偶尔使用 |
| **Daemon** | ~100ms | ~50ms | 频繁使用 |

### 异步工具刷新

**问题**: 启动时同步发现 MCP tools 会阻塞 Agent 启动

**解决方案**:
```cpp
// Agent 启动时异步刷新
auto refresh_future = std::async(std::launch::async, [this]() {
    tool_manager_->refresh_mcp_tools();
});

// 不阻塞 Agent 启动,后台完成工具发现
```

### 工具缓存

**问题**: 每次 `list_tools` 都调用 mcporter CLI,开销大

**解决方案**:
```cpp
// 缓存工具列表,定期刷新
class McpToolRegistry {
private:
    std::chrono::seconds cache_ttl_ = 300s;  // 5分钟 TTL
    std::map<std::string, std::vector<McpToolDefinition>> cache_;
    std::chrono::steady_clock::time_point last_refresh_;
};
```

---

## 🔒 安全考虑

### 1. 进程隔离

**OpenClaw 策略**: mcporter 作为独立进程运行,避免 MCP runtime 漏洞影响主进程

**HunterClaw 实现**:
```cpp
// 通过 spawn/popen 调用 mcporter
auto result = subprocess::run({
    "mcporter", "call", "linear.list_issues", "--args", json_args
});
// mcporter 崩溃不会影响主进程
```

### 2. Server 白名单

**配置**:
```json
{
  "tools": {
    "mcp": {
      "allowed_servers": ["linear", "github", "qmd"]
    }
  }
}
```

**实现**:
```cpp
core::Result<int> McpToolRegistry::discover_and_register(
    tools::ToolManager& tool_manager,
    const std::vector<std::string>& allowed_servers
) {
    auto all_servers = client_->list_servers();
    
    // 只注册白名单中的 servers
    for (const auto& server : all_servers.value) {
        if (std::find(allowed_servers.begin(), allowed_servers.end(), server) 
            != allowed_servers.end()) {
            register_server_tools(tool_manager, server);
        }
    }
}
```

### 3. 审批机制

**敏感工具需要审批**:
```cpp
class McpToolAdapter : public tools::Tool {
public:
    bool requires_approval() const override {
        // 根据工具类型判断
        if (definition_.tool_name.find("delete") != std::string::npos ||
            definition_.tool_name.find("create") != std::string::npos) {
            return true;  // 需要用户审批
        }
        return false;
    }
};
```

---

## 📝 总结

### 对齐目标

**HunterClaw MCP 对齐 OpenClaw**: **100% 架构一致性**

### 核心优势

1. ✅ **架构简洁** - 外部桥接,无内置 MCP runtime
2. ✅ **安全隔离** - 进程隔离,白名单机制
3. ✅ **高性能** - Daemon 模式,工具缓存
4. ✅ **易扩展** - 透明集成到 Tool 系统
5. ✅ **低维护** - mcporter 独立更新,不影响核心

### 实施时间

| 阶段 | 时间 | 交付物 |
|------|------|--------|
| **阶段1**: mcporter 基础 | 1周 | mcporter_client.h/cc, mcp_tool_adapter.h/cc |
| **阶段2**: Tool 集成 | 3天 | ToolManager MCP 支持 |
| **阶段3**: Skill 配置 | 2天 | mcporter SKILL.md, 配置支持 |
| **阶段4**: Agent 集成 | 2天 | ReactExecutor 集成, 测试 |
| **总计** | **约 2 周** | **完整 MCP 能力** |

### 验收标准

```cpp
// 1. 自动启动 daemon
auto client = mcp::create_mcporter_client({.enabled = true, .start_daemon = true});
client->ensure_daemon_started();  // ✅

// 2. 发现所有 MCP tools
auto tools = client->discover_all_tools();  // ✅ 返回所有配置的 MCP servers 的 tools

// 3. 透明调用
auto tool_mgr = tools::ToolManager::create(context);
tool_mgr->enable_mcp(mcp_config);
auto result = tool_mgr->execute_tool("linear.list_issues", {});  // ✅ 和 builtin tools 一样

// 4. Agent 自动使用
auto agent = agents::ReactAgent::create(config);
agent->process_message("Find my Linear issues");  // ✅ AI 自动调用 linear.list_issues
```

**完成后,HunterClaw 将拥有与 OpenClaw 完全对齐的 MCP 能力!** 🎉
