#pragma once

// ============================================================================
// acp_event_ledger.h
// 🎯 严格对齐 OpenClaw: src/acp/event-ledger.ts
//
// 职责:
//   - 基于 UUID 的 ACP 事件幂等性保证
//   - 内存 LRU 去重（替代 SQLite 方案）
//   - 线程安全
//
// 核心语义:
//   - record_event(uuid) 首次记录返回 true（需处理）、重复返回 false（幂等跳过）
//   - is_event_processed(uuid) 只读检查，不产生副作用
//   - 默认容量 5000，LRU 淘汰
// ============================================================================

#include <string>
#include <cstddef>
#include <memory>

namespace hunterclaw {
namespace acp {

// ── ACP Event Ledger ──────────────────────────────────────────────────────────

class AcpEventLedger {
public:
    // 默认最大容量（对齐 OC event-ledger.ts DEFAULT_MAX_EVENTS）
    static constexpr size_t DEFAULT_MAX_EVENTS = 5000;

    explicit AcpEventLedger(size_t max_events = DEFAULT_MAX_EVENTS);
    ~AcpEventLedger();

    AcpEventLedger(const AcpEventLedger&) = delete;
    AcpEventLedger& operator=(const AcpEventLedger&) = delete;
    AcpEventLedger(AcpEventLedger&&) noexcept;
    AcpEventLedger& operator=(AcpEventLedger&&) noexcept;

    // 🎯 对齐: recordEvent(eventId: string): boolean
    // 首次记录该 id 返回 true（幂等通过，事件需处理），
    // 已存在返回 false（幂等拦截，应跳过）。
    bool record_event(const std::string& event_id);

    // 🎯 对齐: isEventProcessed(eventId: string): boolean
    // 只读检查，不记录。
    bool is_event_processed(const std::string& event_id) const;

    // 当前记录数
    size_t size() const;

    // 清空所有记录（用于 gateway reconnect）
    void clear();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace acp
} // namespace hunterclaw
