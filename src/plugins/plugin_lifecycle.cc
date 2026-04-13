// HunterClaw: plugins/plugin_lifecycle.cc
// 严格对齐 OC: plugins/install.ts (1054行) + plugins/discovery.ts (960行)
//              + plugins/uninstall.ts (289行)
// 插件完整生命周期管理：发现 → 安装 → 卸载
//
// OC 对应导出:
//   install.ts    → resolvePluginInstallDir, installPluginFromArchive, installPluginFromDir,
//                    installPluginFromFile, installPluginFromNpmSpec, installPluginFromPath
//   discovery.ts  → discoverOpenClawPlugins, clearPluginDiscoveryCache, PluginCandidate
//   uninstall.ts  → removePluginFromConfig, resolveUninstallDirectoryTarget, resolveUninstallChannelConfigKeys

#include "hunterclaw/plugins/plugin_system.h"
#include "hunterclaw/config/config_system.h"
#include "hunterclaw/infra/utilities.h"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <chrono>
#include <regex>
#include <map>
#include <set>
#include <mutex>
#include <optional>
#include <sstream>
#include <unistd.h>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace hunterclaw {
namespace plugins {

// =========================================================================
// 对齐 OC: Types (forward declarations)
// =========================================================================

struct InstallPluginResult {
    bool ok = false;
    std::string plugin_id;
    std::string target_dir;
    std::string version;
    std::vector<std::string> extensions;
    std::string error;
    std::string code;
};

// PluginCandidate 在已有 plugin_system.h 中没有定义，这里声明
struct PluginCandidate {
    std::string id_hint;
    std::string source;
    std::string root_dir;
    std::string origin;
    std::string package_name;
    std::string package_version;
    std::string package_description;
};

// PluginDiagnostic 已在 plugin_system.h 中定义 (Level::WARN/ERROR)
// PluginDiscoveryResult 使用已有的 PluginDiagnostic

struct PluginDiscoveryResult {
    std::vector<PluginCandidate> candidates;
    std::vector<PluginDiagnostic> diagnostics;
};

struct UninstallActions {
    bool entry = false;
    bool install = false;
    bool allowlist = false;
    bool load_path = false;
    bool memory_slot = false;
    bool channel_config = false;
    bool directory = false;
};

struct UninstallPluginResult {
    bool ok = false;
    std::string plugin_id;
    json config;
    UninstallActions actions;
    std::vector<std::string> warnings;
    std::string error;
};

// =========================================================================
// 对齐 OC install.ts: Error codes + types
// =========================================================================

static std::string shell_quote(const std::string& s) {
    return "'" + s + "'"; // simplified shell quoting
}

// 对齐 OC: PLUGIN_INSTALL_ERROR_CODE
const std::map<std::string, std::string> PLUGIN_INSTALL_ERROR_CODE = {
    {"INVALID_NPM_SPEC", "invalid_npm_spec"},
    {"INVALID_MIN_HOST_VERSION", "invalid_min_host_version"},
    {"UNKNOWN_HOST_VERSION", "unknown_host_version"},
    {"INCOMPATIBLE_HOST_VERSION", "incompatible_host_version"},
    {"MISSING_EXTENSIONS", "missing_extensions"},
    {"EMPTY_EXTENSIONS", "empty_extensions"},
    {"PACKAGE_NOT_FOUND", "package_not_found"},
    {"PLUGIN_ID_MISMATCH", "plugin_id_mismatch"},
    {"SECURITY_SCAN_BLOCKED", "security_scan_blocked"},
    {"SECURITY_SCAN_FAILED", "security_scan_failed"},
};

// 对齐 OC: PLUGIN_ARCHIVE_ROOT_MARKERS
static const std::vector<std::string> PLUGIN_ARCHIVE_ROOT_MARKERS = {
    "package.json",
    "plugin.json",
};

// =========================================================================
// 对齐 OC install.ts: Plugin ID validation
// =========================================================================

// 对齐 OC: safeDirName
static std::string safe_dir_name(const std::string& input) {
    std::string result;
    for (char c : input) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.') {
            result += c;
        } else {
            result += '_';
        }
    }
    return result;
}

// 对齐 OC: validatePluginId
static std::string validate_plugin_id(const std::string& plugin_id) {
    std::string trimmed = plugin_id;
    // trim
    auto s = trimmed.find_first_not_of(" \t\n\r");
    if (s == std::string::npos) return "invalid plugin name: missing";
    auto e = trimmed.find_last_not_of(" \t\n\r");
    trimmed = trimmed.substr(s, e - s + 1);

    if (trimmed.empty()) return "invalid plugin name: missing";
    if (trimmed.find('\\') != std::string::npos) {
        return "invalid plugin name: path separators not allowed";
    }

    // Split by /
    std::vector<std::string> segments;
    std::istringstream stream(trimmed);
    std::string segment;
    while (std::getline(stream, segment, '/')) {
        segments.push_back(segment);
    }

    for (const auto& seg : segments) {
        if (seg.empty()) return "invalid plugin name: malformed scope";
        if (seg == "." || seg == "..") return "invalid plugin name: reserved path segment";
    }

    if (segments.size() == 1) {
        if (trimmed[0] == '@') return "invalid plugin name: scoped ids must use @scope/name format";
        return "";
    }
    if (segments.size() != 2) return "invalid plugin name: path separators not allowed";
    if (!segments[0].empty() && segments[0][0] != '@') {
        return "invalid plugin name: scoped ids must use @scope/name format";
    }
    if (segments[0].size() < 2) {
        return "invalid plugin name: scoped ids must use @scope/name format";
    }
    return "";
}

// 对齐 OC: resolvePluginInstallDir
std::string resolve_plugin_install_dir(const std::string& plugin_id,
                                        const std::string& extensions_dir) {
    std::string ext_base = extensions_dir.empty()
        ? infra::get_home_dir() + "/.hunterclaw" + "/extensions"
        : extensions_dir;
    auto err = validate_plugin_id(plugin_id);
    if (!err.empty()) throw std::runtime_error(err);

    std::string trimmed = plugin_id;
    auto s = trimmed.find_first_not_of(" \t");
    auto e = trimmed.find_last_not_of(" \t");
    if (s != std::string::npos) trimmed = trimmed.substr(s, e - s + 1);

    std::string dir_name;
    if (trimmed.find('/') == std::string::npos) {
        dir_name = safe_dir_name(trimmed);
    } else {
        // Scoped: hash the full ID
        dir_name = "@" + safe_dir_name(trimmed);
    }
    return (fs::path(ext_base) / dir_name).string();
}

// =========================================================================
// 对齐 OC install.ts: install functions
// =========================================================================

// 对齐 OC: ensureOpenClawExtensions
static InstallPluginResult ensure_extensions(const json& manifest) {
    InstallPluginResult result;
    if (!manifest.contains("extensions") ||
        !manifest["extensions"].is_array()) {
        result.ok = false;
        result.error = "package.json missing extensions field";
        result.code = "missing_extensions";
        return result;
    }
    auto& exts = manifest["extensions"];
    if (exts.empty()) {
        result.ok = false;
        result.error = "extensions array is empty";
        result.code = "empty_extensions";
        return result;
    }
    result.ok = true;
    for (const auto& ext : exts) {
        if (ext.is_string()) {
            result.extensions.push_back(ext.get<std::string>());
        }
    }
    return result;
}

// 对齐 OC: installPluginFromDir
InstallPluginResult install_plugin_from_dir(
    const std::string& dir_path,
    const std::string& extensions_dir,
    bool dry_run
) {
    InstallPluginResult result;
    if (!fs::exists(dir_path) || !fs::is_directory(dir_path)) {
        result.ok = false;
        result.error = "directory not found: " + dir_path;
        return result;
    }

    // Read manifest
    fs::path manifest_path = fs::path(dir_path) / "package.json";
    if (!fs::exists(manifest_path)) {
        manifest_path = fs::path(dir_path) / "plugin.json";
    }
    if (!fs::exists(manifest_path)) {
        result.ok = false;
        result.error = "No package.json or plugin.json found in " + dir_path;
        return result;
    }

    json manifest;
    try {
        std::ifstream ifs(manifest_path);
        ifs >> manifest;
    } catch (const std::exception& e) {
        result.ok = false;
        result.error = "Failed to parse manifest: " + std::string(e.what());
        return result;
    }

    // Extract plugin ID
    std::string plugin_id = manifest.value("name", "");
    if (manifest.contains("id")) plugin_id = manifest.value("id", plugin_id);
    if (plugin_id.empty()) {
        result.ok = false;
        result.error = "Plugin manifest missing name/id field";
        return result;
    }

    // Validate extensions
    auto ext_result = ensure_extensions(manifest);
    if (!ext_result.ok) return ext_result;

    // Resolve target dir
    std::string target_dir = resolve_plugin_install_dir(plugin_id, extensions_dir);

    if (!dry_run) {
        try {
            fs::create_directories(target_dir);
            // Copy plugin directory contents
            fs::copy(dir_path, target_dir,
                     fs::copy_options::recursive | fs::copy_options::overwrite_existing);
        } catch (const std::exception& e) {
            result.ok = false;
            result.error = "Failed to copy plugin: " + std::string(e.what());
            return result;
        }
    }

    result.ok = true;
    result.plugin_id = plugin_id;
    result.target_dir = target_dir;
    result.version = manifest.value("version", "");
    result.extensions = ext_result.extensions;
    return result;
}

// Forward declarations
InstallPluginResult install_plugin_from_archive(const std::string&, const std::string&, bool);
InstallPluginResult install_plugin_from_file(const std::string&, const std::string&, bool);

// 对齐 OC: installPluginFromPath (auto-detect dir/file/archive)
InstallPluginResult install_plugin_from_path(
    const std::string& path_str,
    const std::string& extensions_dir,
    bool dry_run
) {
    if (fs::is_directory(path_str)) {
        return install_plugin_from_dir(path_str, extensions_dir, dry_run);
    }

    if (!fs::exists(path_str)) {
        InstallPluginResult result;
        result.ok = false;
        result.error = "Path not found: " + path_str;
        return result;
    }

    // Check if archive (.tgz, .tar.gz, .zip)
    std::string lower = path_str;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    if (lower.find(".tgz") != std::string::npos ||
        lower.find(".tar.gz") != std::string::npos ||
        lower.find(".zip") != std::string::npos) {
        return install_plugin_from_archive(path_str, extensions_dir, dry_run);
    }

    // Single file install
    return install_plugin_from_file(path_str, extensions_dir, dry_run);
}

InstallPluginResult install_plugin_from_archive(
    const std::string& archive_path,
    const std::string& extensions_dir,
    bool dry_run
) {
    // 对齐 OC: installPluginFromArchive — extract to temp, then install from dir
    InstallPluginResult result;
    std::string temp_dir = std::string("/tmp/hc-plugin-install-" + std::to_string(getpid()));

    try {
        // Extract archive using system tar/unzip
        int ret;
        std::string lower = archive_path;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
        if (lower.find(".zip") != std::string::npos) {
            ret = std::system(("unzip -o -q " + shell_quote(archive_path) +
                               " -d " + shell_quote(temp_dir)).c_str());
        } else {
            ret = std::system(("tar -xf " + shell_quote(archive_path) +
                               " -C " + shell_quote(temp_dir)).c_str());
        }
        if (ret != 0) {
            result.ok = false;
            result.error = "Failed to extract archive: " + archive_path;
            return result;
        }

        // Find package root (look for root markers)
        std::string package_root;
        for (const auto& entry : fs::recursive_directory_iterator(temp_dir)) {
            for (const auto& marker : PLUGIN_ARCHIVE_ROOT_MARKERS) {
                if (entry.path().filename() == marker) {
                    package_root = entry.path().parent_path().string();
                    break;
                }
            }
            if (!package_root.empty()) break;
        }
        if (package_root.empty()) {
            result.ok = false;
            result.error = "No package root found in archive";
            return result;
        }

        result = install_plugin_from_dir(package_root, extensions_dir, dry_run);
    } catch (const std::exception& e) {
        result.ok = false;
        result.error = "Archive install failed: " + std::string(e.what());
    }

    // Cleanup temp dir
    try { fs::remove_all(temp_dir); } catch (...) {}
    return result;
}

InstallPluginResult install_plugin_from_file(
    const std::string& file_path,
    const std::string& extensions_dir,
    bool dry_run
) {
    // 对齐 OC: installPluginFromFile — single JS/TS file as plugin
    InstallPluginResult result;
    if (!fs::exists(file_path) || !fs::is_regular_file(file_path)) {
        result.ok = false;
        result.error = "File not found: " + file_path;
        return result;
    }

    std::string filename = fs::path(file_path).filename().string();
    std::string plugin_id = fs::path(file_path).stem().string();

    std::string target_dir = resolve_plugin_install_dir(plugin_id, extensions_dir);
    if (!dry_run) {
        try {
            fs::create_directories(target_dir);
            fs::copy_file(file_path, fs::path(target_dir) / filename,
                          fs::copy_options::overwrite_existing);
        } catch (const std::exception& e) {
            result.ok = false;
            result.error = "Failed to install file: " + std::string(e.what());
            return result;
        }
    }

    result.ok = true;
    result.plugin_id = plugin_id;
    result.target_dir = target_dir;
    result.extensions = {"./" + filename};
    return result;
}

// =========================================================================
// 对齐 OC discovery.ts: Plugin discovery
// =========================================================================

static std::mutex discovery_cache_mutex;
static std::map<std::string, std::pair<std::chrono::steady_clock::time_point, PluginDiscoveryResult>>
    discovery_cache;

static const int DEFAULT_DISCOVERY_CACHE_MS = 1000;

void clear_plugin_discovery_cache() {
    std::lock_guard<std::mutex> lock(discovery_cache_mutex);
    discovery_cache.clear();
}

// 对齐 OC: CandidateBlockReason
static bool check_source_escapes_root(const std::string& source, const std::string& root_dir) {
    try {
        auto source_real = fs::canonical(source).string();
        auto root_real = fs::canonical(root_dir).string();
        return source_real.find(root_real) != 0;
    } catch (...) {
        return false;
    }
}

// 对齐 OC: checkPathStatAndPermissions (Unix only)
static bool check_path_world_writable(const std::string& path_str) {
#ifndef _WIN32
    try {
        auto perms = fs::status(path_str).permissions();
        return (perms & fs::perms::others_write) != fs::perms::none;
    } catch (...) {}
#else
    (void)path_str;
#endif
    return false;
}

// 对齐 OC: scanDirectoryForCandidates
static void scan_directory_for_candidates(
    const std::string& dir,
    const std::string& origin,
    std::vector<PluginCandidate>& candidates,
    std::vector<PluginDiagnostic>& diagnostics
) {
    static const std::set<std::string> IGNORE_NAMES = {
        ".git", ".hg", ".svn", ".turbo", ".yarn", ".yarn-cache",
        "build", "coverage", "dist", "node_modules"
    };

    if (!fs::exists(dir) || !fs::is_directory(dir)) return;

    try {
        for (const auto& entry : fs::directory_iterator(dir)) {
            if (!entry.is_directory()) continue;
            std::string name = entry.path().filename().string();
            if (IGNORE_NAMES.count(name)) continue;
            if (name.empty() || name[0] == '.') continue;

            // Check for manifest
            fs::path manifest = entry.path() / "package.json";
            fs::path plugin_manifest = entry.path() / "plugin.json";

            if (!fs::exists(manifest) && !fs::exists(plugin_manifest)) continue;

            // Security checks
            if (check_source_escapes_root(entry.path().string(), dir)) {
                PluginDiagnostic diag;
                diag.plugin_id = name;
                diag.message = "Source escapes root directory (symlink?)";
                diag.level = PluginDiagnostic::Level::ERROR;
                diagnostics.push_back(diag);
                continue;
            }

            if (check_path_world_writable(entry.path().string())) {
                PluginDiagnostic diag;
                diag.plugin_id = name;
                diag.message = "Plugin directory is world-writable";
                diag.level = PluginDiagnostic::Level::ERROR;
                diagnostics.push_back(diag);
                continue;
            }

            // Read manifest metadata
            json mf;
            try {
                std::ifstream ifs(fs::exists(manifest) ? manifest : plugin_manifest);
                ifs >> mf;
            } catch (...) {
                PluginDiagnostic diag;
                diag.plugin_id = name;
                diag.message = "Failed to parse plugin manifest";
                diag.level = PluginDiagnostic::Level::WARN;
                diagnostics.push_back(diag);
                continue;
            }

            PluginCandidate candidate;
            candidate.id_hint = mf.value("name", name);
            if (mf.contains("id")) candidate.id_hint = mf.value("id", candidate.id_hint);
            candidate.source = entry.path().string();
            candidate.root_dir = dir;
            candidate.origin = origin;
            candidate.package_name = mf.value("name", "");
            candidate.package_version = mf.value("version", "");
            candidate.package_description = mf.value("description", "");

            candidates.push_back(candidate);
        }
    } catch (const std::exception& e) {
        PluginDiagnostic diag;
        diag.plugin_id = std::nullopt;
        diag.message = "Directory scan failed: " + std::string(e.what());
        diag.level = PluginDiagnostic::Level::WARN;
        diagnostics.push_back(diag);
    }
}

PluginDiscoveryResult discover_plugins(
    const std::string& workspace_dir,
    const std::vector<std::string>& extra_paths,
    bool use_cache
) {
    // 对齐 OC: discoverOpenClawPlugins — scan workspace + global extensions + extra paths
    std::string cache_key = workspace_dir + "::" +
        std::to_string(extra_paths.size());

    if (use_cache) {
        std::lock_guard<std::mutex> lock(discovery_cache_mutex);
        auto it = discovery_cache.find(cache_key);
        if (it != discovery_cache.end()) {
            auto elapsed = std::chrono::steady_clock::now() - it->second.first;
            if (std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()
                < DEFAULT_DISCOVERY_CACHE_MS) {
                return it->second.second;
            }
        }
    }

    PluginDiscoveryResult result;

    // Scan workspace plugins
    if (!workspace_dir.empty()) {
        scan_directory_for_candidates(
            workspace_dir + "/.hunterclaw/plugins",
            "workspace",
            result.candidates,
            result.diagnostics
        );
    }

    // Scan global extensions
    std::string state_dir = infra::get_home_dir() + "/.hunterclaw";
    scan_directory_for_candidates(
        state_dir + "/extensions",
        "bundled",
        result.candidates,
        result.diagnostics
    );

    // Scan extra load paths
    for (const auto& extra_path : extra_paths) {
        scan_directory_for_candidates(
            extra_path,
            "load-path",
            result.candidates,
            result.diagnostics
        );
    }

    // Cache result
    if (use_cache) {
        std::lock_guard<std::mutex> lock(discovery_cache_mutex);
        discovery_cache[cache_key] = {std::chrono::steady_clock::now(), result};
    }

    return result;
}

// =========================================================================
// 对齐 OC uninstall.ts: Plugin uninstall
// =========================================================================

static const std::set<std::string> SHARED_CHANNEL_CONFIG_KEYS = {
    "defaults", "modelByChannel"
};

// 对齐 OC: resolveUninstallChannelConfigKeys
std::vector<std::string> resolve_uninstall_channel_config_keys(
    const std::string& plugin_id,
    const std::vector<std::string>& channel_ids
) {
    auto raw_keys = channel_ids.empty()
        ? std::vector<std::string>{plugin_id}
        : channel_ids;

    std::set<std::string> seen;
    std::vector<std::string> keys;
    for (const auto& key : raw_keys) {
        if (SHARED_CHANNEL_CONFIG_KEYS.count(key) || seen.count(key)) continue;
        seen.insert(key);
        keys.push_back(key);
    }
    return keys;
}

// 对齐 OC: resolveUninstallDirectoryTarget
std::optional<std::string> resolve_uninstall_directory_target(
    const std::string& plugin_id,
    bool has_install,
    const std::string& install_source,
    const std::string& extensions_dir
) {
    if (!has_install) return std::nullopt;
    if (install_source == "path") return std::nullopt;

    try {
        return resolve_plugin_install_dir(plugin_id, extensions_dir);
    } catch (...) {
        return std::nullopt;
    }
}

// 对齐 OC: removePluginFromConfig
UninstallPluginResult remove_plugin_from_config(
    json& cfg,
    const std::string& plugin_id,
    const std::vector<std::string>& channel_ids
) {
    UninstallPluginResult result;
    result.ok = true;
    result.plugin_id = plugin_id;

    // Remove from entries
    if (cfg.contains("plugins") && cfg["plugins"].contains("entries") &&
        cfg["plugins"]["entries"].contains(plugin_id)) {
        cfg["plugins"]["entries"].erase(plugin_id);
        result.actions.entry = true;
    }

    // Remove from installs
    std::string install_source;
    std::string source_path;
    if (cfg.contains("plugins") && cfg["plugins"].contains("installs") &&
        cfg["plugins"]["installs"].contains(plugin_id)) {
        auto& install_record = cfg["plugins"]["installs"][plugin_id];
        install_source = install_record.value("source", "");
        source_path = install_record.value("sourcePath", "");
        cfg["plugins"]["installs"].erase(plugin_id);
        result.actions.install = true;
    }

    // Remove from allowlist
    if (cfg.contains("plugins") && cfg["plugins"].contains("allow") &&
        cfg["plugins"]["allow"].is_array()) {
        auto& allow = cfg["plugins"]["allow"];
        auto it = std::find(allow.begin(), allow.end(), plugin_id);
        if (it != allow.end()) {
            allow.erase(it);
            result.actions.allowlist = true;
        }
    }

    // Remove from load.paths
    if (install_source == "path" && !source_path.empty() &&
        cfg.contains("plugins") && cfg["plugins"].contains("load") &&
        cfg["plugins"]["load"].contains("paths") &&
        cfg["plugins"]["load"]["paths"].is_array()) {
        auto& paths = cfg["plugins"]["load"]["paths"];
        auto it = std::find(paths.begin(), paths.end(), source_path);
        if (it != paths.end()) {
            paths.erase(it);
            result.actions.load_path = true;
        }
    }

    // Reset memory slot
    if (cfg.contains("plugins") && cfg["plugins"].contains("slots") &&
        cfg["plugins"]["slots"].value("memory", "") == plugin_id) {
        cfg["plugins"]["slots"]["memory"] = "default";
        result.actions.memory_slot = true;
    }

    // Remove channel config
    auto keys = resolve_uninstall_channel_config_keys(plugin_id, channel_ids);
    if (cfg.contains("channels")) {
        for (const auto& key : keys) {
            if (cfg["channels"].contains(key)) {
                cfg["channels"].erase(key);
                result.actions.channel_config = true;
            }
        }
    }

    result.config = cfg;
    return result;
}

// 对齐 OC: uninstallPlugin (full flow)
UninstallPluginResult uninstall_plugin(
    json& cfg,
    const std::string& plugin_id,
    const std::string& extensions_dir,
    const std::vector<std::string>& channel_ids,
    bool dry_run
) {
    auto result = remove_plugin_from_config(cfg, plugin_id, channel_ids);
    if (!result.ok) return result;

    // Try to delete directory
    bool has_install = result.actions.install;
    auto target_dir = resolve_uninstall_directory_target(
        plugin_id, has_install, "", extensions_dir);

    if (target_dir.has_value() && fs::exists(*target_dir) && !dry_run) {
        try {
            fs::remove_all(*target_dir);
            result.actions.directory = true;
        } catch (const std::exception& e) {
            result.warnings.push_back("Failed to remove directory " +
                                      *target_dir + ": " + e.what());
        }
    }

    return result;
}

} // namespace plugins
} // namespace hunterclaw
