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

**Design Philosophy**: External bridge model over first-class MCP runtime (aligned with OpenClaw).

## Prerequisites

```bash
# Install mcporter
npm install -g mcporter

# Or via pnpm
pnpm install -g mcporter

# Or via yarn
yarn global add mcporter
```

## Usage

### 1. List MCP Servers and Tools

```bash
# List all configured servers
mcporter list

# List tools from specific server with schema
mcporter list <server> --schema

# Example
mcporter list linear --schema
```

### 2. Call MCP Tools

```bash
# Key-value syntax
mcporter call linear.list_issues team=ENG limit:5

# Function call syntax
mcporter call "linear.create_issue(title: \"Bug Fix\")"

# JSON payload
mcporter call github.search_repos --args '{"query":"hunterclaw","limit":10}'

# HTTP MCP server
mcporter call https://api.example.com/mcp.fetch url:https://example.com

# Stdio MCP server
mcporter call --stdio "bun run ./server.ts" scrape url=https://example.com
```

### 3. Configure MCP Servers

```bash
# Add server with keep-alive lifecycle
mcporter config add qmd --command "qmd mcp" --lifecycle keep-alive

# Add server with per-request lifecycle
mcporter config add github --command "github-mcp" --lifecycle per-request

# List config
mcporter config list

# Get specific config
mcporter config get linear

# Remove server
mcporter config remove qmd

# Import config file
mcporter config import ./mcporter.json
```

### 4. Daemon Management

```bash
# Start daemon (for faster calls)
mcporter daemon start

# Check daemon status
mcporter daemon status

# Stop daemon
mcporter daemon stop

# Restart daemon
mcporter daemon restart
```

### 5. Authentication

```bash
# Authenticate with server
mcporter auth linear

# Reset authentication
mcporter auth linear --reset
```

## Integration with HunterClaw

### Enable in Config

Add to your HunterClaw configuration:

```json
{
  "tools": {
    "mcp": {
      "enabled": true,
      "server_name": "default",
      "start_daemon": true,
      "timeout_ms": 30000,
      "allowed_servers": ["linear", "github", "qmd"]
    }
  }
}
```

### Automatic Tool Discovery

When MCP is enabled, HunterClaw will:

1. ✅ Auto-start mcporter daemon
2. ✅ Discover all configured MCP servers
3. ✅ Register MCP tools as native HunterClaw tools
4. ✅ Make them available to AI agents

### Example C++ API

```cpp
#include "hunterclaw/tools/tool.h"
#include "hunterclaw/mcp/mcporter_client.h"

// Create ToolManager
tools::ToolContext context;
context.workspace_dir = "/workspace";
auto tool_mgr = tools::ToolManager::create(context);

// Enable MCP
mcp::McporterConfig mcp_config;
mcp_config.enabled = true;
mcp_config.start_daemon = true;
mcp_config.allowed_servers = {"linear", "github", "qmd"};
tool_mgr->enable_mcp(mcp_config);

// Auto-discover MCP tools
auto count = tool_mgr->refresh_mcp_tools();
std::cout << "Discovered " << count.value << " MCP tools\n";

// Call via ToolManager (seamless, like builtin tools)
auto result = tool_mgr->execute_tool("linear.list_issues", {
    {"team", "ENG"},
    {"limit", 5}
});

if (result.success) {
    std::cout << "Result: " << result.content[0].text << std::endl;
}
```

### Agent Usage

```cpp
#include "hunterclaw/agents/react_executor.h"

// Configure Agent (MCP auto-enabled)
agents::AgentConfig config;
config.tools.mcp.enabled = true;
config.tools.mcp.allowed_servers = {"linear", "github", "qmd"};

// Create Agent
auto agent = agents::ReactAgent::create(config);

// AI automatically discovers and uses MCP tools
auto response = agent->process_message(
    "Find all open bugs in the ENG team and summarize them"
);

// AI will:
// 1. Discover linear.list_issues tool
// 2. Call linear.list_issues(team="ENG", status="open", label="bug")
// 3. Parse results
// 4. Generate summary
```

## Configuration Examples

### Example 1: Linear Integration

```bash
# Install Linear MCP server
npm install -g @linear/mcp

# Configure
mcporter config add linear --command "linear-mcp" --lifecycle keep-alive

# Authenticate
mcporter auth linear
# => Opens browser for OAuth

# Test
mcporter call linear.list_issues team=ENG limit:5
```

### Example 2: GitHub Integration

```bash
# Install GitHub MCP server
npm install -g @github/mcp

# Configure
mcporter config add github --command "github-mcp" --lifecycle keep-alive

# Authenticate
export GITHUB_TOKEN=ghp_your_token_here

# Test
mcporter call github.search_repos query:hunterclaw limit:10
```

### Example 3: QMD Memory Search

```bash
# Install QMD
npm install -g qmd

# Configure
mcporter config add qmd --command "qmd mcp" --lifecycle keep-alive

# Initialize QMD
qmd init

# Test
mcporter call qmd.search query:"project updates" limit:5
```

## HunterClaw Configuration

### Full Configuration

```json
{
  "tools": {
    "enable_builtin": true,
    "mcp": {
      "enabled": true,
      "server_name": "default",
      "start_daemon": true,
      "timeout_ms": 30000,
      "allowed_servers": ["linear", "github", "qmd"]
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

### Environment Variables

```bash
# mcporter config directory (optional)
export MCPORTER_CONFIG_DIR=~/.config/mcporter

# Enable MCP debug logs
export MCPORTER_DEBUG=1

# HunterClaw MCP settings
export HUNTERCLAW_MCP_ENABLED=true
export HUNTERCLAW_MCP_START_DAEMON=true
```

## Security Considerations

### 1. Process Isolation

✅ mcporter runs as a separate process
✅ MCP server crashes don't affect HunterClaw
✅ Reduced attack surface

### 2. Server Whitelist

```json
{
  "tools": {
    "mcp": {
      "allowed_servers": ["linear", "github", "qmd"]
    }
  }
}
```

Only whitelisted servers are loaded and their tools registered.

### 3. Tool Approval

Sensitive operations automatically require approval:

```cpp
// Tools with these keywords require approval:
// - delete, remove, destroy, drop
// - create, insert, update, modify
// - execute, run, exec, eval
// - write, commit, push, deploy

auto tool = tool_mgr->get_tool("linear.delete_issue");
assert(tool->requires_approval() == true);
```

### 4. Timeout Protection

```json
{
  "tools": {
    "mcp": {
      "timeout_ms": 30000  // 30 seconds max
    }
  }
}
```

All MCP calls have timeout protection.

## Performance Optimization

### Daemon Mode

**Problem**: Cold start for each call (~2-5s)

**Solution**: Enable daemon mode

```json
{
  "tools": {
    "mcp": {
      "start_daemon": true
    }
  }
}
```

**Performance Impact**:
| Mode | First Call | Subsequent | Use Case |
|------|-----------|------------|----------|
| **Cold Start** | 2-5s | 2-5s | Rare usage |
| **Daemon** | 100ms | 50ms | Frequent usage |

### Tool Caching

HunterClaw caches MCP tool definitions for 5 minutes:

```cpp
// Cache TTL configuration
mcp::McpToolRegistry registry(client);
// cache_ttl_ = 300s (5 minutes)
```

### Async Refresh

Agent startup doesn't block on MCP tool discovery:

```cpp
// Async refresh in background
std::async(std::launch::async, [this]() {
    tool_manager_->refresh_mcp_tools();
});
// Agent continues initialization
```

## Troubleshooting

### Issue: `spawn mcporter ENOENT` (Windows)

**Cause**: Windows requires `.cmd` suffix for npm-installed tools

**Solution**: HunterClaw auto-resolves `mcporter` → `mcporter.cmd`

```cpp
// Automatic resolution
std::string McporterClient::resolve_mcporter_command() {
#ifdef _WIN32
    return "mcporter.cmd";
#else
    return "mcporter";
#endif
}
```

### Issue: Slow calls

**Cause**: Cold start without daemon

**Solution**: Enable daemon mode

```bash
mcporter daemon start
```

Or in config:

```json
{"tools": {"mcp": {"start_daemon": true}}}
```

### Issue: Tool not found

**Cause**: Server not configured in mcporter

**Solution**: Check config

```bash
mcporter config list

# If missing, add it
mcporter config add <server> --command "<command>"
```

### Issue: Authentication failed

**Cause**: Missing credentials

**Solution**: Authenticate

```bash
mcporter auth <server>

# Or set environment variable
export LINEAR_API_KEY=lin_api_...
export GITHUB_TOKEN=ghp_...
```

### Issue: Daemon not starting

**Cause**: Port conflict or permission issue

**Solution**: Check logs and restart

```bash
mcporter daemon status
mcporter daemon stop
mcporter daemon start
```

## Advanced Usage

### Custom MCP Server

```typescript
// server.ts - Custom MCP server
import { MCPServer } from '@modelcontextprotocol/server';

const server = new MCPServer();

server.addTool({
  name: 'custom_tool',
  description: 'My custom tool',
  inputSchema: {
    type: 'object',
    properties: {
      param1: { type: 'string' },
      param2: { type: 'number' }
    },
    required: ['param1']
  },
  handler: async (args) => {
    // Tool implementation
    return { result: 'success' };
  }
});

server.listen();
```

```bash
# Register custom server
mcporter config add myserver \
  --command "node server.ts" \
  --lifecycle keep-alive

# Use in HunterClaw
mcporter call myserver.custom_tool param1:hello param2:42
```

### Batch Calls

```cpp
// Batch call multiple MCP tools
mcp::McporterCallArgs call1{
    .server_and_tool = "linear.list_issues",
    .args = {{"team", "ENG"}}
};

mcp::McporterCallArgs call2{
    .server_and_tool = "github.search_repos",
    .args = {{"query", "hunterclaw"}}
};

auto results = client->call_tools({call1, call2});
// Concurrent execution
```

## Resources

- **mcporter Homepage**: http://mcporter.dev
- **MCP Specification**: https://modelcontextprotocol.io
- **HunterClaw Docs**: `/docs/MCP_ALIGNMENT.md`
- **OpenClaw Reference**: VISION.md (mcporter integration section)

## Changelog

### v1.0.0 (2026-03-09)

- ✅ Initial mcporter integration
- ✅ Daemon management
- ✅ Tool discovery and registration
- ✅ Tool execution
- ✅ Whitelist security
- ✅ Windows support
- ✅ Performance optimization (caching, async)
- ✅ 100% aligned with OpenClaw architecture
