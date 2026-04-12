// ============================================================================
// acp_server.cc
// 🎯 严格对齐 OpenClaw:
//   - src/acp/server.ts (serveAcpGateway 入口)
//   - src/acp/session.ts (AcpSessionStore 内存存储)
//   - src/acp/session-mapper.ts (parseSessionMeta, resolveSessionKey)
// ============================================================================

#include "hunterclaw/acp/acp_server.h"
#include "hunterclaw/acp/acp_translator.h"
#include <algorithm>
#include <chrono>
#include <map>
#include <mutex>
#include <random>

namespace hunterclaw {
namespace acp {

// ── Constants ────────────────────────────────────────────────────────────────
// 🎯 对齐 session.ts DEFAULT_MAX_SESSIONS, DEFAULT_IDLE_TTL_MS

static constexpr int DEFAULT_MAX_SESSIONS = 5000;
static constexpr int64_t DEFAULT_IDLE_TTL_MS = 24 * 60 * 60 * 1000;  // 24 hours

// ── InMemorySessionStore ─────────────────────────────────────────────────────
// 🎯 对齐 session.ts createInMemorySessionStore(options)

class InMemorySessionStore : public AcpSessionStore {
public:
    struct StoreOptions {
        int max_sessions;
        int64_t idle_ttl_ms;
        StoreOptions() : max_sessions(DEFAULT_MAX_SESSIONS), idle_ttl_ms(DEFAULT_IDLE_TTL_MS) {}
    };

    explicit InMemorySessionStore(const StoreOptions& opts = StoreOptions())
        : max_sessions_(std::max(1, opts.max_sessions))
        , idle_ttl_ms_(std::max(int64_t(1000), opts.idle_ttl_ms)) {}

    // 🎯 对齐: createSession(params)
    AcpSession create_session(const std::string& session_key,
                               const std::string& cwd,
                               const std::string& explicit_session_id = "") override {
        std::lock_guard<std::mutex> lock(mu_);
        auto now_ms = now_();

        // Reap idle sessions first
        reap_idle_sessions_(now_ms);

        // Evict if at capacity
        while (static_cast<int>(sessions_.size()) >= max_sessions_) {
            if (!evict_oldest_idle_()) break;
        }

        AcpSession session;
        session.session_id = explicit_session_id.empty()
            ? generate_uuid_() : explicit_session_id;
        session.session_key = session_key;
        session.cwd = cwd;
        session.created_at = now_ms;
        session.last_touched_at = now_ms;

        sessions_[session.session_id] = session;
        return session;
    }

    // 🎯 对齐: hasSession(sessionId)
    bool has_session(const std::string& session_id) const override {
        std::lock_guard<std::mutex> lock(mu_);
        return sessions_.count(session_id) > 0;
    }

    // 🎯 对齐: getSession(sessionId)
    std::optional<AcpSession> get_session(const std::string& session_id) const override {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = sessions_.find(session_id);
        if (it == sessions_.end()) return std::nullopt;
        return it->second;
    }

    // 🎯 对齐: getSessionByRunId(runId)
    std::optional<AcpSession> get_session_by_run_id(const std::string& run_id) const override {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = run_id_to_session_.find(run_id);
        if (it == run_id_to_session_.end()) return std::nullopt;
        auto sit = sessions_.find(it->second);
        if (sit == sessions_.end()) return std::nullopt;
        return sit->second;
    }

    // 🎯 对齐: setActiveRun(sessionId, runId, abortController)
    void set_active_run(const std::string& session_id,
                        const std::string& run_id) override {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = sessions_.find(session_id);
        if (it == sessions_.end()) return;

        // Clear previous run mapping
        if (!it->second.active_run_id.empty()) {
            run_id_to_session_.erase(it->second.active_run_id);
        }

        it->second.active_run_id = run_id;
        it->second.last_touched_at = now_();
        run_id_to_session_[run_id] = session_id;
    }

    // 🎯 对齐: clearActiveRun(sessionId)
    void clear_active_run(const std::string& session_id) override {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = sessions_.find(session_id);
        if (it == sessions_.end()) return;

        if (!it->second.active_run_id.empty()) {
            run_id_to_session_.erase(it->second.active_run_id);
        }
        it->second.active_run_id.clear();
        it->second.last_touched_at = now_();
    }

    // 🎯 对齐: cancelActiveRun(sessionId)
    bool cancel_active_run(const std::string& session_id) override {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = sessions_.find(session_id);
        if (it == sessions_.end()) return false;
        if (it->second.active_run_id.empty()) return false;

        run_id_to_session_.erase(it->second.active_run_id);
        it->second.active_run_id.clear();
        it->second.cancelled = true;
        return true;
    }

private:
    mutable std::mutex mu_;
    int max_sessions_;
    int64_t idle_ttl_ms_;
    std::map<std::string, AcpSession> sessions_;
    std::map<std::string, std::string> run_id_to_session_;  // run_id -> session_id

    // 🎯 对齐 session.ts: reapIdleSessions(nowMs)
    void reap_idle_sessions_(int64_t now_ms) {
        int64_t idle_before = now_ms - idle_ttl_ms_;
        for (auto it = sessions_.begin(); it != sessions_.end();) {
            auto& s = it->second;
            // Skip sessions with active runs
            if (!s.active_run_id.empty()) { ++it; continue; }
            if (s.last_touched_at > idle_before) { ++it; continue; }
            // Clean up run mapping
            if (!s.active_run_id.empty()) {
                run_id_to_session_.erase(s.active_run_id);
            }
            it = sessions_.erase(it);
        }
    }

    // 🎯 对齐 session.ts: evictOldestIdleSession()
    bool evict_oldest_idle_() {
        std::string oldest_id;
        int64_t oldest_time = std::numeric_limits<int64_t>::max();

        for (auto& [id, s] : sessions_) {
            if (!s.active_run_id.empty()) continue;  // Skip active
            if (s.last_touched_at < oldest_time) {
                oldest_time = s.last_touched_at;
                oldest_id = id;
            }
        }

        if (oldest_id.empty()) return false;

        auto it = sessions_.find(oldest_id);
        if (it != sessions_.end()) {
            if (!it->second.active_run_id.empty()) {
                run_id_to_session_.erase(it->second.active_run_id);
            }
            sessions_.erase(it);
        }
        return true;
    }

    int64_t now_() const {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    std::string generate_uuid_() const {
        // Simple UUID v4 generation
        static thread_local std::mt19937_64 rng(std::random_device{}());
        std::uniform_int_distribution<uint64_t> dist;
        uint64_t a = dist(rng), b = dist(rng);
        char buf[40];
        snprintf(buf, sizeof(buf),
            "%08x-%04x-%04x-%04x-%012llx",
            (uint32_t)(a >> 32),
            (uint16_t)(a >> 16),
            (uint16_t)((a & 0x0fff) | 0x4000),
            (uint16_t)((b >> 48 & 0x3fff) | 0x8000),
            (unsigned long long)(b & 0xffffffffffff));
        return buf;
    }
};

// ── serveAcpGateway ──────────────────────────────────────────────────────────
// 🎯 对齐 server.ts serveAcpGateway(opts)
// NOTE: Full implementation requires gateway wiring, which is done at the
// gateway layer (gateway.cc). This provides the ACP-side scaffolding.

void serve_acp_gateway(const AcpServerOptions& opts) {
    auto store = create_default_session_store();
    // The actual gateway connection and event loop is wired in
    // gateway/gateway.cc when ACP mode is enabled.
    // AcpGatewayAgent is instantiated there with the live GatewayClient.
    (void)opts;
    (void)store;
}

// ── AcpSessionStore factory ──────────────────────────────────────────────────
// 🎯 对齐 session.ts defaultAcpSessionStore

std::unique_ptr<AcpSessionStore> create_default_session_store() {
    return std::make_unique<InMemorySessionStore>();
}

std::unique_ptr<AcpSessionStore> create_session_store(int max_sessions, int64_t idle_ttl_ms) {
    InMemorySessionStore::StoreOptions opts;
    opts.max_sessions = max_sessions;
    opts.idle_ttl_ms = idle_ttl_ms;
    return std::make_unique<InMemorySessionStore>(opts);
}

} // namespace acp
} // namespace hunterclaw
