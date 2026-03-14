#include "hunterclaw/sandbox/sandbox.h"
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_sinks.h>

namespace hunterclaw::sandbox {

namespace {

// 获取 Sandbox logger
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
    get_logger()->info("SandboxManager destroyed");
}

std::shared_ptr<Sandbox> SandboxManager::get_sandbox_instance(const std::string&) const {
    get_logger()->warn("SandboxManager::get_sandbox_instance not implemented");
    return nullptr;
}

SandboxInfo SandboxManager::get_sandbox(const std::string&) const {
    get_logger()->warn("SandboxManager::get_sandbox not implemented");
    return SandboxInfo{};
}

bool SandboxManager::pause_sandbox(const std::string&) {
    get_logger()->warn("SandboxManager::pause_sandbox not implemented");
    return false;
}

bool SandboxManager::resume_sandbox(const std::string&) {
    get_logger()->warn("SandboxManager::resume_sandbox not implemented");
    return false;
}

} // namespace hunterclaw::sandbox
