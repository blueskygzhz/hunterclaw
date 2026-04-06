#pragma once

// ============================================================================
// runtime_registry.h
// Aligned with openclaw: src/acp/runtime/registry.ts
//
// Global ACP runtime backend registry.
// ============================================================================

#include "hunterclaw/acp/runtime_types.h"
#include "hunterclaw/acp/runtime_errors.h"
#include <string>
#include <memory>
#include <map>
#include <mutex>
#include <optional>
#include <algorithm>

namespace hunterclaw {
namespace acp {

// ── AcpRuntimeBackend ────────────────────────────────────────────────────────
// Aligned with openclaw: AcpRuntimeBackend
struct AcpRuntimeBackend {
    std::string id;
    std::shared_ptr<AcpRuntime> runtime;
    std::function<bool()> healthy;  // optional, returns true if healthy
};

// ── Registry ─────────────────────────────────────────────────────────────────
// Aligned with openclaw: registerAcpRuntimeBackend / requireAcpRuntimeBackend / etc.
class AcpRuntimeRegistry {
public:
    static AcpRuntimeRegistry& instance() {
        static AcpRuntimeRegistry inst;
        return inst;
    }

    // Aligned with openclaw: registerAcpRuntimeBackend
    void register_backend(const AcpRuntimeBackend& backend) {
        std::lock_guard<std::mutex> lock(mutex_);
        std::string id = normalize_id(backend.id);
        if (id.empty()) {
            throw std::runtime_error("ACP runtime backend id is required");
        }
        if (!backend.runtime) {
            throw std::runtime_error("ACP runtime backend \"" + id + "\" is missing runtime implementation");
        }
        backends_[id] = backend;
        backends_[id].id = id;
    }

    // Aligned with openclaw: unregisterAcpRuntimeBackend
    void unregister_backend(const std::string& id) {
        std::lock_guard<std::mutex> lock(mutex_);
        backends_.erase(normalize_id(id));
    }

    // Aligned with openclaw: getAcpRuntimeBackend
    std::optional<AcpRuntimeBackend> get_backend(const std::string& id = "") const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::string normalized = normalize_id(id);
        if (!normalized.empty()) {
            auto it = backends_.find(normalized);
            return it != backends_.end() ? std::optional<AcpRuntimeBackend>(it->second) : std::nullopt;
        }
        if (backends_.empty()) return std::nullopt;
        // Return first healthy backend
        for (auto& [_, b] : backends_) {
            if (is_healthy(b)) return b;
        }
        return backends_.begin()->second;
    }

    // Aligned with openclaw: requireAcpRuntimeBackend
    AcpRuntimeBackend require_backend(const std::string& id = "") const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::string normalized = normalize_id(id);
        auto backend = get_backend_unlocked(normalized);
        if (!backend) {
            throw AcpRuntimeError(
                AcpErrorCode::ACP_BACKEND_MISSING,
                "ACP runtime backend is not configured. Install and enable the acpx runtime plugin."
            );
        }
        if (!is_healthy(*backend)) {
            throw AcpRuntimeError(
                AcpErrorCode::ACP_BACKEND_UNAVAILABLE,
                "ACP runtime backend is currently unavailable. Try again in a moment."
            );
        }
        if (!normalized.empty() && backend->id != normalized) {
            throw AcpRuntimeError(
                AcpErrorCode::ACP_BACKEND_MISSING,
                "ACP runtime backend \"" + normalized + "\" is not registered."
            );
        }
        return *backend;
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        backends_.clear();
    }

private:
    AcpRuntimeRegistry() = default;
    mutable std::mutex mutex_;
    std::map<std::string, AcpRuntimeBackend> backends_;

    static std::string normalize_id(const std::string& id) {
        std::string s = id;
        // trim
        size_t start = s.find_first_not_of(" \t\r\n");
        if (start == std::string::npos) return "";
        size_t end = s.find_last_not_of(" \t\r\n");
        s = s.substr(start, end - start + 1);
        // lowercase
        std::transform(s.begin(), s.end(), s.begin(), ::tolower);
        return s;
    }

    static bool is_healthy(const AcpRuntimeBackend& b) {
        if (!b.healthy) return true;
        try { return b.healthy(); } catch (...) { return false; }
    }

    std::optional<AcpRuntimeBackend> get_backend_unlocked(const std::string& normalized) const {
        if (!normalized.empty()) {
            auto it = backends_.find(normalized);
            return it != backends_.end() ? std::optional<AcpRuntimeBackend>(it->second) : std::nullopt;
        }
        if (backends_.empty()) return std::nullopt;
        for (auto& [_, b] : backends_) {
            if (is_healthy(b)) return b;
        }
        return backends_.begin()->second;
    }
};

} // namespace acp
} // namespace hunterclaw
