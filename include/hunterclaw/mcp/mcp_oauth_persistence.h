#pragma once

// 🎯 对齐 OpenClaw src/agents/mcp-oauth-store.ts + state/openclaw-state-db.* 的
//   OAuth 凭证持久化宿主层：SQLite `mcp_oauth_stores` 表的读/改写/清除，
//   读改写在 BEGIN IMMEDIATE 写事务 + 进程内互斥下串行化（对齐 lease +
//   runOpenClawStateWriteTransaction 的原子读改写语义）。
//
//   表结构（对齐 openclaw-state-schema.sql）：
//     mcp_oauth_stores(store_key TEXT PK, format_version INTEGER CHECK(=1),
//                      store_json TEXT, updated_at INTEGER) STRICT

#include "hunterclaw/core/result.h"
#include "hunterclaw/mcp/mcp_oauth_store.h"
#include <functional>
#include <optional>
#include <string>

namespace hunterclaw {
namespace mcp {

// 对齐 MCP_OAUTH_STORE_FORMAT_VERSION。
constexpr int MCP_OAUTH_STORE_FORMAT_VERSION = 1;

// 对齐 resolveOpenClawStateSqlitePath: {state_dir}/state/openclaw.sqlite。
std::string resolve_mcp_oauth_state_db_path();

// 对齐 readMcpOAuthStore: 打开可写生命周期读取 canonical 状态；缺行→{}。
core::Result<McpOAuthStore> read_mcp_oauth_store(const std::string& store_key);

// 对齐 readMcpOAuthStoreReadOnly: 只读；db/表不存在→{}（不创建/不修复）。
core::Result<McpOAuthStore> read_mcp_oauth_store_read_only(const std::string& store_key);

// 对齐 updateMcpOAuthStore: 原子读-改-写单行（BEGIN IMMEDIATE + 进程内互斥）。
//   update 接收当前 store，返回下一个 store；写入前经parse 校验（对齐
//   replaceMcpOAuthStore 的 parseMcpOAuthStoreJson）。返回写入后的 store。
core::Result<McpOAuthStore> update_mcp_oauth_store(
    const std::string& store_key,
    const std::function<McpOAuthStore(const McpOAuthStore&)>& update);

// 对齐 clearMcpOAuthStore: 写入 {credentialState:"cleared"}（保留权威行，非删除）。
core::Result<void> clear_mcp_oauth_store(const std::string& store_key);

// 对齐 readMcpOAuthCredentialsStatus: 只读状态投影（db缺失→全false）。
core::Result<McpOAuthCredentialsStatus> read_mcp_oauth_credentials_status(
    const std::string& store_key);

// 测试辅助：使用自定义 db 路径（覆盖默认解析）。传空串恢复默认。
void set_mcp_oauth_state_db_path_for_test(const std::string& path);

} // namespace mcp
} // namespace hunterclaw
