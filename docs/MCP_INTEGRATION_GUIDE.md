# HunterClaw MCP 集成指南

## 🎯 快速开始

### 1. 安装 mcporter

```bash
npm install -g mcporter
```

验证安装:

```bash
mcporter --version
```

### 2. 配置 MCP Server

```bash
# 添加 Linear
mcporter config add linear --command "linear-mcp" --lifecycle keep-alive

# 添加 GitHub
mcporter config add github --command "github-mcp" --lifecycle keep-alive

# 添加 QMD (内存搜索)
mcporter config add qmd --command "qmd mcp" --lifecycle keep-alive

# 查看配置
mcporter config list
```

### 3. 启用 HunterClaw MCP

编辑配置文件 `~/.hunterclaw/config.json`:

```json
{
  "tools": {
    "mcp": {
      "enabled": true,
      "start_daemon": true,
      "allowed_servers": ["linear", "github", "qmd"]
    }
  }
}
```

### 4. 启动并验证

```bash
# 启动 HunterClaw
hunterclaw start

# 检查 MCP 状态
hunterclaw tools list | grep -E "linear|github|qmd"

# 应该看到:
# - linear.list_issues
# - linear.create_issue
# - github.search_repos
# - qmd.search
# - qmd.vector_search
```

---

## 📋 完整配置参考

### HunterClaw 配置

```json
{
  "tools": {
    "enable_builtin": true,
    "mcp": {
      // 是否启用 MCP 集成
      "enabled": true,
      
      // MCP server 名称 (用于默认调用)
      "server_name": "default",
      
      // 是否自动启动 mcporter daemon
      "start_daemon": true,
      
      // 工具调用超时 (毫秒)
      "timeout_ms": 30000,
      
      // 允许的 MCP servers 白名单 (空=全部)
      "allowed_servers": [
        "linear",
        "github",
        "qmd"
      ]
    }
  },
  
  "agents": {
    "default": {
      "name": "HunterClaw",
      "model": "claude-3.5-sonnet",
      "tools": {
        "mcp_enabled": true
      }
    }
  }
}
```

### mcporter 配置

位置: `~/.config/mcporter/config.json`

```json
{
  "servers": {
    "linear": {
      "command": "linear-mcp",
      "lifecycle": "keep-alive",
      "env": {
        "LINEAR_API_KEY": "${LINEAR_API_KEY}"
      }
    },
    "github": {
      "command": "github-mcp",
      "lifecycle": "keep-alive",
      "env": {
        "GITHUB_TOKEN": "${GITHUB_TOKEN}"
      }
    },
    "qmd": {
      "command": "qmd mcp",
      "lifecycle": "keep-alive",
      "args": ["--index-path", "~/.hunterclaw/memory"]
    }
  }
}
```

---

## 🔧 使用示例

### 示例 1: Linear Issue 管理

```cpp
#include "hunterclaw/agents/react_executor.h"

auto agent = agents::ReactAgent::create(config);

// AI 自动调用 linear.list_issues
auto response = agent->process_message(
    "List all open bugs in the ENG team"
);

// AI 执行:
// 1. 发现 linear.list_issues 工具
// 2. 调用: linear.list_issues(team="ENG", status="open", label="bug")
// 3. 解析结果并生成摘要
```

### 示例 2: GitHub 代码搜索

```cpp
auto response = agent->process_message(
    "Search for HunterClaw repositories on GitHub"
);

// AI 执行:
// 1. 调用 github.search_repos(query="hunterclaw", limit=10)
// 2. 展示仓库列表和统计
```

### 示例 3: QMD 记忆搜索

```cpp
auto response = agent->process_message(
    "What did we discuss about MCP integration?"
);

// AI 执行:
// 1. 调用 qmd.search(query="MCP integration", limit=5)
// 2. 基于历史上下文回答
```

### 示例 4: 手动调用 MCP Tool

```cpp
#include "hunterclaw/tools/tool.h"

auto tool_mgr = tools::ToolManager::create(context);
tool_mgr->enable_mcp(mcp_config);

// 直接调用
auto result = tool_mgr->execute_tool("linear.create_issue", {
    {"title", "Bug: MCP integration"},
    {"description", "Found a bug in the MCP integration"},
    {"team", "ENG"},
    {"priority", "high"}
});

if (result.success) {
    std::cout << "Issue created: " << result.content[0].text << std::endl;
}
```

---

## 🚀 性能优化

### 1. 启用 Daemon 模式

**配置**:
```json
{"tools": {"mcp": {"start_daemon": true}}}
```

**性能提升**:
- 首次调用: 2-5s → 100ms
- 后续调用: 2-5s → 50ms

### 2. 工具缓存

HunterClaw 自动缓存 MCP 工具定义 (TTL: 5分钟):

```cpp
// 无需手动管理,自动刷新
tool_mgr->refresh_mcp_tools();  // 强制刷新
```

### 3. 异步工具发现

Agent 启动时异步发现 MCP tools,不阻塞:

```cpp
// ReactExecutor::initialize_tools() 自动实现
std::async(std::launch::async, [this]() {
    tool_manager_->refresh_mcp_tools();
});
```

---

## 🔒 安全最佳实践

### 1. 使用白名单

```json
{
  "tools": {
    "mcp": {
      "allowed_servers": ["linear", "github", "qmd"]
    }
  }
}
```

只加载可信的 MCP servers。

### 2. 敏感操作审批

HunterClaw 自动识别敏感操作:

- `delete`, `remove`, `destroy` → 需要审批
- `create`, `insert`, `update` → 需要审批
- `execute`, `run`, `eval` → 需要审批

### 3. 超时保护

```json
{"tools": {"mcp": {"timeout_ms": 30000}}}
```

防止 MCP 调用hang。

### 4. 环境变量隔离

```bash
# 使用环境变量存储敏感信息
export LINEAR_API_KEY=lin_api_...
export GITHUB_TOKEN=ghp_...

# mcporter 自动读取
mcporter config add linear --command "linear-mcp" --lifecycle keep-alive
```

---

## 🐛 故障排除

### 问题 1: mcporter 未安装

**错误**:
```
mcporter not installed. Please install: npm install -g mcporter
```

**解决**:
```bash
npm install -g mcporter
mcporter --version
```

### 问题 2: Daemon 未启动

**错误**:
```
Failed to start mcporter daemon
```

**解决**:
```bash
# 检查状态
mcporter daemon status

# 手动启动
mcporter daemon start

# 查看日志
journalctl -u mcporter  # Linux
```

### 问题 3: 工具未发现

**错误**:
```
Tool not found: linear.list_issues
```

**解决**:
```bash
# 检查 mcporter 配置
mcporter config list

# 检查 server 是否在白名单
cat ~/.hunterclaw/config.json | grep allowed_servers

# 手动刷新工具
hunterclaw tools refresh
```

### 问题 4: Windows ENOENT 错误

**错误**:
```
spawn mcporter ENOENT
```

**解决**:
HunterClaw 自动解析 `mcporter.cmd`,无需手动处理。

如果仍有问题:
```bash
# 检查 mcporter.cmd 是否存在
where mcporter.cmd

# 确保 npm bin 在 PATH
echo %PATH%
```

---

## 📊 监控和调试

### 启用调试日志

```bash
# 环境变量
export MCPORTER_DEBUG=1
export HUNTERCLAW_LOG_LEVEL=debug

# 启动 HunterClaw
hunterclaw start
```

### 查看 MCP 调用日志

```bash
# HunterClaw 日志
tail -f ~/.hunterclaw/logs/hunterclaw.log | grep mcp

# mcporter 日志
mcporter daemon status
```

### 性能监控

```cpp
// 在代码中添加计时
auto start = std::chrono::steady_clock::now();
auto result = tool_mgr->execute_tool("linear.list_issues", {});
auto elapsed = std::chrono::steady_clock::now() - start;

log()->info("MCP call took {} ms", 
           std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count());
```

---

## 🎓 进阶主题

### 自定义 MCP Server

创建自己的 MCP server:

```typescript
// my-mcp-server.ts
import { MCPServer } from '@modelcontextprotocol/server';

const server = new MCPServer();

server.addTool({
  name: 'custom_search',
  description: 'Search my custom database',
  inputSchema: {
    type: 'object',
    properties: {
      query: { type: 'string' },
      limit: { type: 'number', default: 10 }
    },
    required: ['query']
  },
  handler: async (args) => {
    // 实现搜索逻辑
    const results = await searchDatabase(args.query, args.limit);
    return { results };
  }
});

server.listen();
```

注册到 mcporter:

```bash
mcporter config add myserver \
  --command "node my-mcp-server.ts" \
  --lifecycle keep-alive

# 在 HunterClaw 中使用
hunterclaw tools refresh
# 现在可以使用 myserver.custom_search
```

### 批量调用

```cpp
#include "hunterclaw/mcp/mcporter_client.h"

auto client = mcp::create_mcporter_client(config);

// 准备批量调用
std::vector<mcp::McporterCallArgs> calls = {
    {
        .server_and_tool = "linear.list_issues",
        .args = {{"team", "ENG"}}
    },
    {
        .server_and_tool = "github.search_repos",
        .args = {{"query", "hunterclaw"}}
    },
    {
        .server_and_tool = "qmd.search",
        .args = {{"query", "MCP"}}
    }
};

// 并发执行
auto results = client->call_tools(calls);

for (const auto& result : results.value) {
    if (result.success) {
        std::cout << result.structured_content.dump(2) << std::endl;
    }
}
```

---

## 📚 参考资源

- **HunterClaw MCP 对齐报告**: `/docs/MCP_ALIGNMENT.md`
- **mcporter Skill**: `/skills/mcporter/SKILL.md`
- **OpenClaw 参考**: `openclaw-analysis/VISION.md`
- **MCP 规范**: https://modelcontextprotocol.io

---

## 🎉 总结

通过 mcporter 桥接,HunterClaw 实现了:

✅ **100% 对齐 OpenClaw MCP 架构**  
✅ **零代码侵入** - 外部进程隔离  
✅ **高性能** - Daemon 模式 + 缓存  
✅ **安全** - 白名单 + 审批机制  
✅ **易扩展** - 透明集成到 Tool 系统

开始使用 MCP 吧! 🚀
