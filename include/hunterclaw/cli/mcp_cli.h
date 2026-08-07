#pragma once

// 🎯 对齐 OpenClaw src/cli/mcp-cli.ts 的 OAuth 相关子命令：
//   - mcp login <name> [--code <code>]   授权 OAuth MCP server（两步式，无回调 server）
//   - mcp logout <name>                  清除已存OAuth 凭证
//
// 分发入口 dispatch_mcp_cli(argc, argv) 供 main.cc 在 argv[1]=="mcp" 时调用。

namespace hunterclaw {
namespace cli {

// 处理 `hunterclaw mcp <subcommand> ...`。返回进程退出码。
int dispatch_mcp_cli(int argc, char** argv);

} // namespace cli
} // namespace hunterclaw
