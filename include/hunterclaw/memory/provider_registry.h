#pragma once

#include "hunterclaw/memory/types.h"  // MemorySearchManager (openclaw types.ts)
#include "hunterclaw/core/types.h"
#include <string>
#include <map>
#include <memory>
#include <mutex>
#include <functional>

namespace hunterclaw {
namespace memory {

// Align with openclaw: MemorySearchManager is the single interface.
// MemoryProviderRegistry manages MemorySearchManager instances by agent_id.
// This replaces the old CRUD-style MemoryProvider registry.

class MemoryProviderRegistry {
public:
    static MemoryProviderRegistry& instance();

    // Register a MemorySearchManager (e.g., MemoryIndexManager) by agent_id
    core::Result<bool> register_provider(
        const std::string& agent_id,
        std::shared_ptr<MemorySearchManager> manager
    );

    core::Result<bool> unregister_provider(const std::string& agent_id);

    bool has_provider(const std::string& agent_id) const;

    core::Result<std::shared_ptr<MemorySearchManager>> get_provider(
        const std::string& agent_id
    ) const;

    core::Result<std::shared_ptr<MemorySearchManager>> get_default_provider() const;

    void set_default_provider(const std::string& agent_id);

    std::vector<std::string> list_providers() const;

    struct ProviderInfo {
        std::string id;
        std::string backend;
        std::string provider;
        bool is_default;
    };

    std::vector<ProviderInfo> get_all_provider_info() const;

    // Factory: create MemoryIndexManager and register it
    using ManagerFactory = std::function<
        core::Result<std::shared_ptr<MemorySearchManager>>(
            const std::map<std::string, std::string>& config
        )
    >;

    void register_factory(const std::string& type, ManagerFactory factory);

    core::Result<std::string> create_and_register_provider(
        const std::string& type,
        const std::string& agent_id,
        const std::map<std::string, std::string>& config
    );

    void shutdown_all();

private:
    MemoryProviderRegistry() = default;
    ~MemoryProviderRegistry() = default;
    MemoryProviderRegistry(const MemoryProviderRegistry&) = delete;
    MemoryProviderRegistry& operator=(const MemoryProviderRegistry&) = delete;

    mutable std::mutex mutex_;
    std::map<std::string, std::shared_ptr<MemorySearchManager>> providers_;
    std::map<std::string, ManagerFactory> factories_;
    std::string default_provider_id_;
};

} // namespace memory
} // namespace hunterclaw
