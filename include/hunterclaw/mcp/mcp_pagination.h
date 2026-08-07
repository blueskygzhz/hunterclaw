#pragma once

// 🎯 严格对齐 OpenClaw src/agents/mcp-pagination.ts:collectMcpPaginatedItems
//   MCP list 操作的有界分页：单一生命周期内受 timeout / maxPages / maxItems /
//   maxBytes 约束，检测重复游标，超限即抛错（以 Result 承载）。
//
//   C++ 无 AbortSignal/Promise：以 wall-clock deadline 近似 timeout，loadPage
//   为同步回调；行为对齐（边界判定、重复游标、字节预算、错误文案）。

#include "hunterclaw/core/result.h"
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace hunterclaw {
namespace mcp {

// 对齐 McpPaginationPage<T>：一页原始 JSON 项+ 可选 nextCursor + 序列化字节。
struct McpPaginationPage {
    std::vector<core::JsonValue> items;
    std::optional<std::string> next_cursor;
    // 用于字节预算的序列化值；缺省时以 {items,nextCursor} 计。
    std::optional<core::JsonValue> serialized_value;
};

// 对齐 McpPaginationRequest：传给 loadPage 的请求参数。
struct McpPaginationRequest {
    std::optional<std::string> cursor;
    int64_t request_timeout_ms = 0;
};

// 对齐 CollectMcpPaginatedItemsParams。
struct CollectMcpPaginatedItemsParams {
    std::string label;        // 用于错误文案（如 "bundle-mcp tools/list"）
    std::string item_label;   // 复数项名（如 "tools"）
    int64_t timeout_ms = 0;   // 须为正
    int max_pages = 0;        // 须为正
    int max_items = 0;        // 须为正
    int64_t max_bytes = 0;    // 须为正
    //加载一页（同步）。失败以Result::Err 返回，整个收集终止。
    std::function<core::Result<McpPaginationPage>(const McpPaginationRequest&)> load_page;
    // 可选项映射：返回 nullopt 表示丢弃该项（对齐 mapItem 返回 undefined）。
    std::function<std::optional<core::JsonValue>(const core::JsonValue&)> map_item;
};

// 对齐 collectMcpPaginatedItems：单一有界生命周期内收集完整 MCP list。
core::Result<std::vector<core::JsonValue>> collect_mcp_paginated_items(
    const CollectMcpPaginatedItemsParams& params);

// ============================================================================
// 默认边界常量（对齐 OpenClaw bundle-mcp 的 list 预算；宿主可覆盖）。
// ============================================================================
constexpr int     MCP_LIST_DEFAULT_MAX_PAGES   = 100;
constexpr int     MCP_LIST_DEFAULT_MAX_ITEMS   = 10000;
constexpr int64_t MCP_LIST_DEFAULT_MAX_BYTES   = 8 * 1024 * 1024;  // 8 MiB
constexpr int64_t MCP_LIST_DEFAULT_TIMEOUT_MS  = 30000;

} // namespace mcp
} // namespace hunterclaw
