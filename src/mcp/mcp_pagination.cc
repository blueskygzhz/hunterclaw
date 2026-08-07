#include "hunterclaw/mcp/mcp_pagination.h"
#include "hunterclaw/mcp/json_converter.h"

#include <chrono>
#include <set>

// 🎯 严格对齐 OpenClaw src/agents/mcp-pagination.ts。

namespace hunterclaw {
namespace mcp {

namespace {

int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// 对齐 boundedJsonUtf8Bytes 的核心语义：计算 value 序列化后的 UTF-8 字节数，
//   一旦超过 budget 即返回 complete=false（不必精确，只需可靠判定是否超预算）。
struct BoundedBytes {
    bool complete = false;
    int64_t bytes = 0;
};
BoundedBytes bounded_json_utf8_bytes(const core::JsonValue& value, int64_t budget) {
    // dump 后按字节长度衡量；budget < 0 直接判超限。
    if (budget < 0) return BoundedBytes{false, 0};
    std::string s = converters::JsonConverter::to_nlohmann(value).dump();  // 紧凑 UTF-8 序列化
    int64_t n = static_cast<int64_t>(s.size());
    if (n > budget) return BoundedBytes{false, n};
    return BoundedBytes{true, n};
}

} // namespace

core::Result<std::vector<core::JsonValue>> collect_mcp_paginated_items(
    const CollectMcpPaginatedItemsParams& params) {
    using Ret = core::Result<std::vector<core::JsonValue>>;

    // 对齐 clampPositiveTimerTimeoutMs + positiveInteger 校验。
    if (params.timeout_ms <= 0) {
        return Ret::Err(params.label + " requires a positive timeout");
    }
    if (params.max_pages <= 0) {
        return Ret::Err(params.label + " maxPages must be a positive safe integer");
    }
    if (params.max_items <= 0) {
        return Ret::Err(params.label + " maxItems must be a positive safe integer");
    }
    if (params.max_bytes <= 0) {
        return Ret::Err(params.label + " maxBytes must be a positive safe integer");
    }
    if (!params.load_page) {
        return Ret::Err(params.label + " requires a loadPage callback");
    }

    const int64_t timeout_ms = params.timeout_ms;
    const int64_t deadline_at_ms = now_ms() + timeout_ms;
    const std::string timeout_err =
        params.label + " timed out after " + std::to_string(timeout_ms) + "ms";

    // 对齐 assertActive: 到达 deadline 即超时。
    auto expired = [&]() -> bool { return now_ms() >= deadline_at_ms; };

    std::vector<core::JsonValue> items;
    std::set<std::string> seen_cursors;
    int64_t collected_bytes = 0;
    std::optional<std::string> cursor;

    for (int page_number = 0; page_number < params.max_pages; ++page_number) {
        if (expired()) return Ret::Err(timeout_err);

        McpPaginationRequest req;
        req.cursor = cursor;
        req.request_timeout_ms = timeout_ms;
        auto page_res = params.load_page(req);
        if (page_res.is_err()) {
            return Ret::Err(page_res.error);
        }
        if (expired()) return Ret::Err(timeout_err);

        const McpPaginationPage& page = page_res.get();

        // 字节预算：对齐 serializedValue ?? {items,nextCursor}。
        core::JsonValue measured_value;
        if (page.serialized_value.has_value()) {
            measured_value = *page.serialized_value;
        } else {
            core::JsonValue wrap = core::JsonValue::object();
            core::JsonValue arr = core::JsonValue::array();
            for (const auto& it : page.items) arr.push_back(it);
            wrap["items"] = arr;
            if (page.next_cursor.has_value()) wrap["nextCursor"] = *page.next_cursor;
            measured_value = wrap;
        }
        BoundedBytes measured =
            bounded_json_utf8_bytes(measured_value, params.max_bytes - collected_bytes);
        if (!measured.complete || collected_bytes + measured.bytes > params.max_bytes) {
            return Ret::Err(params.label + " exceeded " + std::to_string(params.max_bytes) +
                            " bytes");
        }
        collected_bytes += measured.bytes;

        for (const auto& item : page.items) {
            std::optional<core::JsonValue> mapped;
            if (params.map_item) {
                mapped = params.map_item(item);
            } else {
                mapped = item;
            }
            if (!mapped.has_value()) continue;
            if (static_cast<int>(items.size()) >= params.max_items) {
                return Ret::Err(params.label + " exceeded " + std::to_string(params.max_items) +
                                " " + params.item_label);
            }
            items.push_back(std::move(*mapped));
        }

        if (expired()) return Ret::Err(timeout_err);

        // 终止/续页：对齐 nextCursor 语义与重复游标检测。
        if (!page.next_cursor.has_value()) {
            return Ret::Ok(std::move(items));
        }
        const std::string& next = *page.next_cursor;
        if (seen_cursors.count(next)) {
            return Ret::Err(params.label + " returned a repeated pagination cursor");
        }
        seen_cursors.insert(next);
        cursor = next;
    }

    return Ret::Err(params.label + " exceeded " + std::to_string(params.max_pages) + " pages");
}

} // namespace mcp
} // namespace hunterclaw
