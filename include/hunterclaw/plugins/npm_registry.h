#pragma once

#include "hunterclaw/core/result.h"
#include "hunterclaw/core/json.h"
#include <string>
#include <vector>
#include <memory>

namespace hunterclaw {
namespace plugins {

// ============================================================================
// npm Package Metadata
// ============================================================================

struct NpmPackageVersion {
    std::string version;
    std::string tarball_url;
    std::string sha_sum;
    std::map<std::string, std::string> dependencies;
};

struct NpmPackageMetadata {
    std::string name;
    std::string description;
    std::string latest_version;
    std::vector<NpmPackageVersion> versions;
    std::vector<std::string> keywords;
    std::string homepage;
    std::string repository_url;
};

// ============================================================================
// npm Registry Client
// ============================================================================

/// npm registry client for package search and download
class NpmRegistryClient {
public:
    explicit NpmRegistryClient(const std::string& registry_url = "https://registry.npmjs.org");
    
    /// Search packages by query
    core::Result<std::vector<NpmPackageMetadata>> search(
        const std::string& query,
        int limit = 20
    );
    
    /// Get package metadata
    core::Result<NpmPackageMetadata> get_package(const std::string& package_name);
    
    /// Get specific version metadata
    core::Result<NpmPackageVersion> get_version(
        const std::string& package_name,
        const std::string& version = "latest"
    );
    
    /// Download package tarball
    core::Result<std::vector<uint8_t>> download_tarball(
        const std::string& tarball_url
    );
    
    /// Extract tarball to directory
    core::Result<bool> extract_tarball(
        const std::vector<uint8_t>& tarball_data,
        const std::string& extract_dir
    );

private:
    std::string registry_url_;
    
    core::Result<core::JsonValue> call_registry(const std::string& endpoint);
};

// ============================================================================
// Plugin Discovery from npm
// ============================================================================

/// Discover HunterClaw plugins from npm registry
class NpmPluginDiscovery {
public:
    explicit NpmPluginDiscovery(std::shared_ptr<NpmRegistryClient> client);
    
    /// Search for HunterClaw plugins
    /// Searches for packages with keywords: "hunterclaw", "hunterclaw-plugin"
    core::Result<std::vector<NpmPackageMetadata>> discover_plugins(
        const std::string& query = "",
        int limit = 20
    );
    
    /// Check if package is a valid HunterClaw plugin
    core::Result<bool> is_hunterclaw_plugin(const std::string& package_name);
    
    /// Get recommended plugins (curated list)
    std::vector<std::string> get_recommended_plugins();

private:
    std::shared_ptr<NpmRegistryClient> client_;
};

} // namespace plugins
} // namespace hunterclaw
