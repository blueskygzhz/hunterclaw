// ============================================================================
// question_runtime.cc
// 见 question_broker.h 的对齐说明。
// ============================================================================

#include "hunterclaw/tools/question_broker.h"

#include "hunterclaw/core/logger.h"

#include <spdlog/spdlog.h>

namespace hunterclaw {
namespace tools {

namespace {
std::shared_ptr<spdlog::logger> log() { return core::get_logger("question"); }
}  // namespace

// ── status <-> wire ─────────────────────────────────────────────────────────

const char* question_status_to_wire(QuestionStatus s) {
    switch (s) {
        case QuestionStatus::Pending:   return "pending";
        case QuestionStatus::Answered:  return "answered";
        case QuestionStatus::Cancelled: return "cancelled";
        case QuestionStatus::Expired:   return "expired";
    }
    return "pending";
}

std::optional<QuestionStatus> question_status_from_wire(const std::string& s) {
    if (s == "pending")   return QuestionStatus::Pending;
    if (s == "answered")  return QuestionStatus::Answered;
    if (s == "cancelled") return QuestionStatus::Cancelled;
    if (s == "expired")   return QuestionStatus::Expired;
    return std::nullopt;
}

// ── QuestionRuntime ─────────────────────────────────────────────────────────

QuestionRuntime& QuestionRuntime::instance() {
    // ★ 函数内 static: 线程安全且避开跨 TU 静态初始化顺序问题
    //   （main 在 gateway 就绪后注入，而某些 TU 的静态对象构造更早）。
    static QuestionRuntime inst;
    return inst;
}

bool QuestionRuntime::set_broker(std::shared_ptr<QuestionBroker> broker) {
    std::lock_guard<std::mutex> lk(mu_);
    if (broker_) {
        // ⚠ 见头文件: 重复注入会让先前已登记的提问变成孤儿。
        //   记 error 而非抛异常 —— 不该因此让 daemon 起不来，
        //   但必须留痕（否则表现为"提问莫名超时"）。
        log()->error("question broker already set; ignoring re-registration");
        return false;
    }
    if (!broker) {
        log()->error("refusing to set a null question broker");
        return false;
    }
    broker_ = std::move(broker);
    log()->info("question broker registered");
    return true;
}

std::shared_ptr<QuestionBroker> QuestionRuntime::broker() const {
    std::lock_guard<std::mutex> lk(mu_);
    return broker_;
}

bool QuestionRuntime::has_broker() const {
    std::lock_guard<std::mutex> lk(mu_);
    return static_cast<bool>(broker_);
}

void QuestionRuntime::reset_for_testing() {
    std::lock_guard<std::mutex> lk(mu_);
    broker_.reset();
}

}  // namespace tools
}  // namespace hunterclaw
