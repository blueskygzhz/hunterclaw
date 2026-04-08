// 🎯 严格对齐 OpenClaw: src/agents/sandbox/registry.ts (188 lines)
//                        + prune.ts (113 lines) + manage.ts (107 lines)
// JSON 文件持久化的容器注册表 + 自动清理 + CLI 管理

#include <string>
#include <vector>
#include <optional>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <chrono>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

namespace hunterclaw {
namespace sandbox {

using json = nlohmann::json;
namespace fs = std::filesystem;

// ============================================================================
// Registry types - 对齐 OpenClaw SandboxRegistryEntry
// ============================================================================

struct RegistryEntry {
    std::string container_name;
    std::string session_key;
    int64_t created_at_ms = 0;
    int64_t last_used_at_ms = 0;
    std::string image;
    std::optional<std::string> config_hash;
};

static std::mutex registry_mutex;

static std::string get_registry_path() {
    const char* home = getenv("HOME");
    return std::string(home ? home : "/tmp") + "/.hunterclaw/state/sandbox/containers.json";
}

// ============================================================================
// Read/Write - 对齐 OpenClaw readRegistryFromFile / writeRegistryFile
// ============================================================================

static std::vector<RegistryEntry> read_registry_entries(const std::string& path) {
    std::vector<RegistryEntry> entries;
    try {
        if (!fs::exists(path)) return entries;
        std::ifstream f(path);
        json j = json::parse(f);
        if (!j.contains("entries") || !j["entries"].is_array()) return entries;
        for (const auto& e : j["entries"]) {
            RegistryEntry entry;
            entry.container_name = e.value("containerName", "");
            entry.session_key = e.value("sessionKey", "");
            entry.created_at_ms = e.value("createdAtMs", (int64_t)0);
            entry.last_used_at_ms = e.value("lastUsedAtMs", (int64_t)0);
            entry.image = e.value("image", "");
            if (e.contains("configHash") && e["configHash"].is_string())
                entry.config_hash = e["configHash"].get<std::string>();
            entries.push_back(entry);
        }
    } catch (...) {}
    return entries;
}

static void write_registry_entries(const std::string& path,
                                    const std::vector<RegistryEntry>& entries) {
    try {
        fs::create_directories(fs::path(path).parent_path());
        json j;
        j["entries"] = json::array();
        for (const auto& e : entries) {
            json entry;
            entry["containerName"] = e.container_name;
            entry["sessionKey"] = e.session_key;
            entry["createdAtMs"] = e.created_at_ms;
            entry["lastUsedAtMs"] = e.last_used_at_ms;
            entry["image"] = e.image;
            if (e.config_hash) entry["configHash"] = *e.config_hash;
            j["entries"].push_back(entry);
        }
        std::ofstream f(path);
        f << j.dump(2) << "\n";
    } catch (...) {}
}

// 🎯 对齐 OpenClaw: updateRegistry (upsert)
void update_registry(const RegistryEntry& entry) {
    std::lock_guard<std::mutex> lock(registry_mutex);
    std::string path = get_registry_path();
    auto entries = read_registry_entries(path);
    // Upsert
    bool found = false;
    for (auto& e : entries) {
        if (e.container_name == entry.container_name) {
            e.last_used_at_ms = entry.last_used_at_ms;
            if (entry.config_hash) e.config_hash = entry.config_hash;
            found = true;
            break;
        }
    }
    if (!found) entries.push_back(entry);
    write_registry_entries(path, entries);
}

// 🎯 对齐 OpenClaw: removeRegistryEntry
void remove_registry_entry(const std::string& container_name) {
    std::lock_guard<std::mutex> lock(registry_mutex);
    std::string path = get_registry_path();
    auto entries = read_registry_entries(path);
    entries.erase(std::remove_if(entries.begin(), entries.end(),
        [&](const RegistryEntry& e) { return e.container_name == container_name; }),
        entries.end());
    write_registry_entries(path, entries);
}

// ============================================================================
// Prune - 对齐 OpenClaw prune.ts
// ============================================================================

static int64_t last_prune_at_ms = 0;

// 🎯 对齐 OpenClaw: maybePruneSandboxes
void maybe_prune_sandboxes(int idle_hours, int max_age_days) {
    auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    // 🎯 对齐 OpenClaw: 5 minute throttle
    if (now - last_prune_at_ms < 5 * 60 * 1000) return;
    last_prune_at_ms = now;

    if (idle_hours == 0 && max_age_days == 0) return;

    std::string path = get_registry_path();
    auto entries = read_registry_entries(path);
    std::vector<std::string> to_remove;

    for (const auto& entry : entries) {
        int64_t idle_ms = now - entry.last_used_at_ms;
        int64_t age_ms = now - entry.created_at_ms;
        bool should_prune =
            (idle_hours > 0 && idle_ms > (int64_t)idle_hours * 3600 * 1000) ||
            (max_age_days > 0 && age_ms > (int64_t)max_age_days * 24 * 3600 * 1000);
        if (should_prune) {
            to_remove.push_back(entry.container_name);
        }
    }

    for (const auto& name : to_remove) {
        // docker rm -f
        std::string cmd = "docker rm -f " + name + " 2>/dev/null";
        system(cmd.c_str());
        remove_registry_entry(name);
        spdlog::info("[sandbox/prune] Removed expired container: {}", name);
    }
}

// ============================================================================
// path_utils - 对齐 OpenClaw path-utils.ts (16 lines)
// ============================================================================

std::string normalize_container_path(const std::string& value) {
    fs::path p(value);
    auto normalized = p.lexically_normal().string();
    return normalized == "." ? "/" : normalized;
}

bool is_path_inside_container_root(const std::string& root, const std::string& target) {
    auto nr = normalize_container_path(root);
    auto nt = normalize_container_path(target);
    if (nr == "/") return true;
    return nt == nr || (nt.rfind(nr + "/", 0) == 0);
}

}  // namespace sandbox
}  // namespace hunterclaw
