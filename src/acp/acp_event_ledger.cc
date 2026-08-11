// ============================================================================
// acp_event_ledger.cc
// 🎯 严格对齐 OpenClaw: src/acp/event-ledger.ts
//
// 使用内存 LRU 双向链表 + HashMap 实现，对齐 OC 的 SQLite 行为:
//   - record_event: 首次返回 true，重复返回 false
//   - 容量满时淘汰最旧条目
//   - 线程安全
// ============================================================================

#include "hunterclaw/acp/acp_event_ledger.h"
#include <list>
#include <unordered_map>
#include <mutex>
#include <memory>
#include <cstddef>

namespace hunterclaw {
namespace acp {

// ── Impl: LRU ordered map (list + unordered_map) ──────────────────────────────

struct AcpEventLedger::Impl {
    size_t max_events;
    std::list<std::string> order;                        // front = oldest, back = newest
    std::unordered_map<std::string, std::list<std::string>::iterator> index;
    mutable std::mutex mu;

    explicit Impl(size_t max) : max_events(max) {
        // 预分配空间
        index.reserve(max);
    }
};

// ── Constructor / Destructor ──────────────────────────────────────────────────

AcpEventLedger::AcpEventLedger(size_t max_events)
    : impl_(std::make_unique<Impl>(max_events)) {}

AcpEventLedger::~AcpEventLedger() = default;

AcpEventLedger::AcpEventLedger(AcpEventLedger&&) noexcept = default;
AcpEventLedger& AcpEventLedger::operator=(AcpEventLedger&&) noexcept = default;

// ── record_event ─────────────────────────────────────────────────────────────
// 🎯 对齐 event-ledger.ts: recordEvent(eventId)
bool AcpEventLedger::record_event(const std::string& event_id) {
    if (event_id.empty()) return false;

    std::lock_guard<std::mutex> lk(impl_->mu);

    // 已存在 → 重复事件，幂等跳过
    if (impl_->index.count(event_id)) {
        return false;
    }

    // LRU 淘汰: 超出容量时移除最旧条目
    while (impl_->order.size() >= impl_->max_events) {
        const std::string& oldest = impl_->order.front();
        impl_->index.erase(oldest);
        impl_->order.pop_front();
    }

    // 记录新事件
    impl_->order.push_back(event_id);
    impl_->index[event_id] = std::prev(impl_->order.end());

    return true;
}

// ── is_event_processed ───────────────────────────────────────────────────────
// 🎯 对齐 event-ledger.ts: isEventProcessed(eventId)
bool AcpEventLedger::is_event_processed(const std::string& event_id) const {
    if (event_id.empty()) return false;
    std::lock_guard<std::mutex> lk(impl_->mu);
    return impl_->index.count(event_id) > 0;
}

// ── size ─────────────────────────────────────────────────────────────────────
size_t AcpEventLedger::size() const {
    std::lock_guard<std::mutex> lk(impl_->mu);
    return impl_->order.size();
}

// ── clear ────────────────────────────────────────────────────────────────────
// 🎯 对齐 event-ledger.ts: clear() — gateway reconnect 时清空
void AcpEventLedger::clear() {
    std::lock_guard<std::mutex> lk(impl_->mu);
    impl_->order.clear();
    impl_->index.clear();
}

} // namespace acp
} // namespace hunterclaw
