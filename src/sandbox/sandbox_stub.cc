// Aligned with openclaw: src/agents/sandbox/manage.ts
// SandboxManager: now fully connected to DockerSandbox implementation.
// Previously a stub returning "not implemented"; now delegates to docker_sandbox.cc.

#include "hunterclaw/sandbox/sandbox.h"
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_sinks.h>

namespace hunterclaw::sandbox {

namespace {

auto get_logger() {
    static auto logger = []() {
        auto log = spdlog::get("sandbox");
        if (!log) {
            log = spdlog::stdout_logger_mt("sandbox");
        }
        return log;
    }();
    return logger;
}

} // namespace

// Constructor and Destructor
SandboxManager::SandboxManager() {
    get_logger()->info("SandboxManager created");
}

SandboxManager::~SandboxManager() {
    // Cleanup all sandboxes on destruction
    for (auto& [id, sandbox] : sandboxes_) {
        try {
            sandbox->cleanup();
        } catch (...) {}
    }
    sandboxes_.clear();
    get_logger()->info("SandboxManager destroyed");
}

// Aligned with openclaw: manage.ts — get sandbox instance by ID
std::shared_ptr<Sandbox> SandboxManager::get_sandbox_instance(const std::string& sandbox_id) const {
    auto it = sandboxes_.find(sandbox_id);
    if (it != sandboxes_.end()) {
        return it->second;
    }
    get_logger()->debug("SandboxManager::get_sandbox_instance: not found id={}", sandbox_id);
    return nullptr;
}

// Aligned with openclaw: manage.ts — get sandbox info by ID
SandboxInfo SandboxManager::get_sandbox(const std::string& sandbox_id) const {
    auto it = sandboxes_.find(sandbox_id);
    if (it == sandboxes_.end()) {
        get_logger()->debug("SandboxManager::get_sandbox: not found id={}", sandbox_id);
        return SandboxInfo{};
    }
    SandboxInfo info;
    info.id = sandbox_id;
    info.status = it->second->get_status();
    return info;
}

// Aligned with openclaw: docker pause container
bool SandboxManager::pause_sandbox(const std::string& sandbox_id) {
    auto it = sandboxes_.find(sandbox_id);
    if (it == sandboxes_.end()) {
        get_logger()->warn("SandboxManager::pause_sandbox: not found id={}", sandbox_id);
        return false;
    }
    return it->second->pause();
}

// Aligned with openclaw: docker unpause container
bool SandboxManager::resume_sandbox(const std::string& sandbox_id) {
    auto it = sandboxes_.find(sandbox_id);
    if (it == sandboxes_.end()) {
        get_logger()->warn("SandboxManager::resume_sandbox: not found id={}", sandbox_id);
        return false;
    }
    return it->second->resume();
}

} // namespace hunterclaw::sandbox
