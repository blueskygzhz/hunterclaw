// ============================================================================
// interrupt_coordinator.cc
// 见 interrupt_coordinator.h 的设计说明。
// ============================================================================

#include "hunterclaw/tools/interrupt_coordinator.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <sstream>

namespace hunterclaw {
namespace tools {

namespace {

std::string trim_copy(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) ++b;
    while (e > b && (s[e-1] == ' ' || s[e-1] == '\t' || s[e-1] == '\r' || s[e-1] == '\n')) --e;
    return s.substr(b, e - b);
}

std::string to_lower_copy(const std::string& s) {
    std::string o = s;
    std::transform(o.begin(), o.end(), o.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return o;
}

bool all_digits(const std::string& s) {
    if (s.empty()) return false;
    for (unsigned char c : s) if (!std::isdigit(c)) return false;
    return true;
}

}  // namespace

InterruptCoordinator& InterruptCoordinator::instance() {
    // ★ 函数内 static: 线程安全且避开跨 TU 静态初始化顺序问题
    //   （remote_a2a 在 registry 构建时注册 handler，可能早于任何 react 调用）。
    static InterruptCoordinator inst;
    return inst;
}

// ── 恢复函数注册 ────────────────────────────────────────────────────────────

bool InterruptCoordinator::register_resume_handler(const std::string& kind,
                                                  ResumeHandler handler) {
    if (kind.empty() || !handler) return false;
    std::lock_guard<std::mutex> lk(mu_);
    // ⚠ 保留先注册的: 后注册的会让已在等待的中断恢复到错误的子系统上，
    //   而那个错误是静默的（handler 签名一样，只是行为不同）。
    if (handlers_.count(kind)) return false;
    handlers_[kind] = std::move(handler);
    return true;
}

bool InterruptCoordinator::has_resume_handler(const std::string& kind) const {
    std::lock_guard<std::mutex> lk(mu_);
    return handlers_.count(kind) > 0;
}

// ── 等待槽位 ────────────────────────────────────────────────────────────────

InterruptCoordinator::Waiter InterruptCoordinator::begin_wait(
    const std::string& session_key) {
    Waiter w;
    const std::string key = trim_copy(session_key);
    // ⚠ 空 session_key 不登记: 认领是按会话定位的，空 key 会让所有无会话
    //   上下文的中断挤在同一槽位互相顶掉。返回无效 Waiter → 调用方会
    //   fail-closed（不中断、把结果原样交给模型并附上"无法向用户提问"）。
    if (key.empty()) return w;

    std::lock_guard<std::mutex> lk(mu_);
    if (slots_.count(key)) return w;  // 该会话已有中断在等

    auto slot = std::make_shared<Slot>();
    slot->seq = next_seq_++;
    slots_[key] = slot;
    w.session_key_ = key;
    w.seq_ = slot->seq;
    return w;
}

void InterruptCoordinator::Waiter::release() {
    if (!valid()) return;
    auto& c = InterruptCoordinator::instance();
    {
        std::lock_guard<std::mutex> lk(c.mu_);
        auto it = c.slots_.find(session_key_);
        // ⚠ 必须比对 seq 才删: 本槽位可能已被【替补中断】占用（前一个
        //   abort 后立刻起了新的），旧 Waiter 的析构不能删掉新中断的槽位。
        if (it != c.slots_.end() && it->second && it->second->seq == seq_) {
            c.slots_.erase(it);
        }
    }
    session_key_.clear();
    seq_ = 0;
}

std::optional<std::string> InterruptCoordinator::Waiter::wait(
    int64_t timeout_ms, const std::atomic<bool>* aborted) {
    if (!valid()) return std::nullopt;
    auto& c = InterruptCoordinator::instance();

    std::shared_ptr<Slot> slot;
    {
        std::lock_guard<std::mutex> lk(c.mu_);
        auto it = c.slots_.find(session_key_);
        if (it == c.slots_.end() || !it->second || it->second->seq != seq_) {
            return std::nullopt;
        }
        slot = it->second;
    }

    const int64_t effective = timeout_ms > 0 ? timeout_ms : kDefaultInterruptWaitMs;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(effective);

    std::unique_lock<std::mutex> lk(c.mu_);
    while (!slot->answered) {
        // ★ abort 优先于超时判定。顺序颠倒会在"abort 与 deadline 同时到"时
        //   报超时，而用户看到的是自己点了停止。
        if (aborted && aborted->load()) return std::nullopt;
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) break;

        auto slice = std::chrono::milliseconds(kInterruptAbortPollMs);
        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
        if (remaining < slice) slice = remaining;
        slot->cv.wait_for(lk, slice);
    }

    // 再查一次 abort: 循环可能因 answered 或 deadline 退出，
    // 但 abort 也许恰在最后一片里置位。
    if (!slot->answered && aborted && aborted->load()) return std::nullopt;
    if (!slot->answered) return std::nullopt;
    return slot->answer;
}

// ── 答复投递 ────────────────────────────────────────────────────────────────

bool InterruptCoordinator::claim_answer(const std::string& session_key,
                                       const std::string& text) {
    const std::string key = trim_copy(session_key);
    if (key.empty()) return false;

    std::shared_ptr<Slot> slot;
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = slots_.find(key);
        if (it == slots_.end() || !it->second) return false;
        // ★ 幂等闸: 已答复过就不再认领。没有它，用户连发两条消息会让
        //   第二条也被"认领成功"却无人消费 —— 那条消息静默消失
        //   （既没成为答复也没进对话）。
        if (it->second->answered) return false;
        slot = it->second;
        slot->answered = true;
        slot->answer = text;
    }
    // ★ 出锁 notify: 被唤醒者会立刻抢锁，持锁 notify 会让它醒来即阻塞。
    slot->cv.notify_all();
    return true;
}

bool InterruptCoordinator::has_pending(const std::string& session_key) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = slots_.find(trim_copy(session_key));
    return it != slots_.end() && it->second && !it->second->answered;
}

// ── 恢复 ────────────────────────────────────────────────────────────────────

ResumeOutcome InterruptCoordinator::resume(const ToolInterrupt& interrupt,
                                          const std::string& user_text,
                                          const std::atomic<bool>* aborted) const {
    ResumeOutcome out;
    ResumeHandler handler;
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = handlers_.find(interrupt.kind);
        if (it != handlers_.end()) handler = it->second;
    }
    if (!handler) {
        // ⚠ 不抛异常: ReactEngine 要把这条失败写进 tool result 让模型知道
        //   "远端没能恢复"，而不是让整个 run 崩掉。
        out.success = false;
        out.error = "没有为中断类型 '" + interrupt.kind + "' 注册恢复处理器";
        return out;
    }
    try {
        // ⚠ 出锁调用: handler 内部会调 ExecutionEngine::execute 走网络，
        //   持锁会把整个协调器锁死（其它会话的 claim/begin_wait 全部阻塞）。
        return handler(interrupt, user_text, aborted);
    } catch (const std::exception& e) {
        out.success = false;
        out.error = std::string("恢复远端执行时异常: ") + e.what();
        return out;
    } catch (...) {
        out.success = false;
        out.error = "恢复远端执行时未知异常";
        return out;
    }
}

// ── 提示词与选项匹配 ────────────────────────────────────────────────────────

std::string InterruptCoordinator::format_prompt(const ToolInterrupt& interrupt) {
    std::ostringstream ss;
    ss << (interrupt.prompt.empty() ? "需要你补充信息后才能继续" : interrupt.prompt);

    if (!interrupt.token_ids.empty()) {
        // ★ 把待确认项列出来。⚠ 用户看不到自己在确认什么就点同意，
        //   这道确认在安全上等于没有。
        ss << "\n\n待确认项(" << interrupt.token_ids.size() << "):";
        const size_t kMaxShown = 8;
        for (size_t i = 0; i < interrupt.token_ids.size() && i < kMaxShown; ++i) {
            ss << "\n  - " << interrupt.token_ids[i];
        }
        if (interrupt.token_ids.size() > kMaxShown) {
            ss << "\n  ... 其余 " << (interrupt.token_ids.size() - kMaxShown) << " 项";
        }
    }

    if (!interrupt.options.empty()) {
        ss << "\n";
        // ★ 1-based —— 与 match_option 的解析同一基准
        for (size_t i = 0; i < interrupt.options.size(); ++i) {
            ss << "\n" << (i + 1) << ". " << interrupt.options[i];
        }
        ss << "\n\n回复序号或选项文字。";
    } else {
        ss << "\n\n请直接回复你的答案。";
    }
    return ss.str();
}

std::string InterruptCoordinator::match_option(const ToolInterrupt& interrupt,
                                             const std::string& answer) {
    const std::string trimmed = trim_copy(answer);
    if (interrupt.options.empty()) return trimmed;

    // ① 纯数字 → 1-based 选项
    if (all_digits(trimmed)) {
        long long n = 0;
        bool overflow = false;
        for (unsigned char ch : trimmed) {
            n = n * 10 + (ch - '0');
            if (n > 1000000) { overflow = true; break; }
        }
        if (!overflow && n >= 1 && static_cast<size_t>(n) <= interrupt.options.size()) {
            return interrupt.options[static_cast<size_t>(n) - 1];
        }
        // ⚠ 越界时不 return: 选项里可能真有一个就叫 "2026"
    }

    // ② 全等匹配（大小写不敏感），返回【原始大小写】
    const std::string lowered = to_lower_copy(trimmed);
    for (const auto& o : interrupt.options) {
        if (to_lower_copy(trim_copy(o)) == lowered) return o;
    }

    // ③ 匹配不上 → 原文透传。
    //   ★ 这里【不拒绝】自由文本（与 question 通道的 ③ 号规则不同）:
    //     interrupt 的 options 是"建议选项"而非"枚举白名单" ——
    //     远端 input-required 常常既给选项也接受自由描述
    //     （"用 PDF" / "只发给张三"）。是否合法由远端判定。
    return trimmed;
}

void InterruptCoordinator::reset_for_testing() {
    std::lock_guard<std::mutex> lk(mu_);
    slots_.clear();
    handlers_.clear();
    next_seq_ = 1;
}

}  // namespace tools
}  // namespace hunterclaw
