// Aligned with openclaw: src/agents/skills-clawhub.ts (501 lines)
// ClawHub registry: search, install, update skills from remote registry.
#include "hunterclaw/skills/skill.h"
#include "hunterclaw/core/utils.h"
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <filesystem>
#include <fstream>
#include <regex>
#include <openssl/sha.h>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace hunterclaw {
namespace skills {

namespace {
    auto log() {
        static auto logger = spdlog::get("skills");
        if (!logger) logger = spdlog::stdout_logger_mt("skills");
        return logger;
    }

    // 对齐 OC: VALID_SLUG_PATTERN = /^[a-z0-9](?:[a-z0-9-]*[a-z0-9])?$/i
    static const std::regex VALID_SLUG_RE(R"(^[A-Za-z0-9](?:[A-Za-z0-9-]*[A-Za-z0-9])?$)");
    // 对齐 OC: NON_ASCII_PATTERN = /[^\x00-\x7F]/
    static const std::regex NON_ASCII_RE(R"([^\x00-\x7F])");

    static const std::string DOT_DIR = ".clawhub";
    static const std::string LEGACY_DOT_DIR = ".clawdhub";

    std::string normalize_tracked_slug(const std::string& raw) {
        std::string slug = raw;
        // trim
        size_t s = slug.find_first_not_of(" \t\n\r");
        size_t e = slug.find_last_not_of(" \t\n\r");
        if (s == std::string::npos) throw std::runtime_error("Invalid skill slug: " + raw);
        slug = slug.substr(s, e - s + 1);
        if (slug.empty() || slug.find('/') != std::string::npos ||
            slug.find('\\') != std::string::npos || slug.find("..") != std::string::npos) {
            throw std::runtime_error("Invalid skill slug: " + raw);
        }
        return slug;
    }

    std::string validate_requested_slug(const std::string& raw) {
        auto slug = normalize_tracked_slug(raw);
        if (std::regex_search(slug, NON_ASCII_RE) || !std::regex_match(slug, VALID_SLUG_RE)) {
            throw std::runtime_error("Invalid skill slug: " + raw);
        }
        return slug;
    }

    std::string resolve_clawhub_base_url(const std::string& base_url) {
        if (!base_url.empty()) return base_url;
        const char* env = std::getenv("CLAWHUB_BASE_URL");
        if (env && std::strlen(env) > 0) return env;
        return "https://clawhub.com/api/v1";
    }

    fs::path resolve_skill_install_dir(const std::string& workspace_dir, const std::string& slug) {
        return fs::path(workspace_dir) / "skills" / slug;
    }
}

// ============================================================================
// ClawHubManager::Impl
// ============================================================================

struct ClawHubManager::Impl {
    // HTTP helper (simplified — real impl uses libcurl or ixwebsocket)
    static std::string http_get(const std::string& url) {
        // Use curl CLI as fallback (对齐 OC fetchClawHubSkillDetail)
        std::string cmd = "curl -sS --max-time 30 '" + url + "'";
        FILE* pipe = popen(cmd.c_str(), "r");
        if (!pipe) return "";
        std::string result;
        char buf[4096];
        while (fgets(buf, sizeof(buf), pipe)) result += buf;
        pclose(pipe);
        return result;
    }

    static std::string http_download(const std::string& url, const fs::path& dest) {
        std::string cmd = "curl -sS -L --max-time 120 -o '" + dest.string() + "' '" + url + "'";
        FILE* pipe = popen(cmd.c_str(), "r");
        if (!pipe) return "curl failed";
        std::string out;
        char buf[4096];
        while (fgets(buf, sizeof(buf), pipe)) out += buf;
        int code = pclose(pipe);
        return code == 0 ? "" : "download failed (exit " + std::to_string(code) + ")";
    }
};

// ============================================================================
// ClawHubManager Implementation
// ============================================================================

ClawHubManager::ClawHubManager() : impl_(std::make_unique<Impl>()) {}
ClawHubManager::~ClawHubManager() = default;

// 对齐 OC: searchSkillsFromClawHub (line 211-221)
std::vector<ClawHubSkillSearchResult> ClawHubManager::search(
    const std::string& query, int limit, const std::string& base_url
) const {
    std::string url = resolve_clawhub_base_url(base_url) +
        "/skills/search?q=" + query + "&limit=" + std::to_string(limit);
    std::string body = Impl::http_get(url);
    if (body.empty()) return {};

    std::vector<ClawHubSkillSearchResult> results;
    try {
        auto j = json::parse(body);
        for (const auto& item : j.value("results", json::array())) {
            ClawHubSkillSearchResult r;
            r.score = item.value("score", 0.0);
            r.slug = item.value("slug", "");
            r.display_name = item.value("displayName", "");
            r.summary = item.value("summary", "");
            r.version = item.value("version", "");
            r.updated_at = item.value("updatedAt", 0);
            results.push_back(r);
        }
    } catch (const std::exception& e) {
        log()->warn("ClawHub search parse error: {}", e.what());
    }
    return results;
}

// 对齐 OC: installSkillFromClawHub (line 400-409)
InstallClawHubSkillResult ClawHubManager::install(
    const std::string& workspace_dir, const std::string& slug_raw,
    const std::string& version, const std::string& base_url, bool force
) {
    InstallClawHubSkillResult result;
    try {
        std::string slug = validate_requested_slug(slug_raw);
        result.slug = slug;
        std::string url_base = resolve_clawhub_base_url(base_url);

        // 1. Fetch detail to resolve version
        std::string detail_body = Impl::http_get(url_base + "/skills/" + slug);
        if (detail_body.empty()) {
            result.error = "Skill \"" + slug + "\" not found on ClawHub.";
            return result;
        }
        auto detail_json = json::parse(detail_body);
        std::string resolved_version = version;
        if (resolved_version.empty()) {
            resolved_version = detail_json.value("latestVersion", json::object()).value("version", "");
        }
        if (resolved_version.empty()) {
            result.error = "Skill \"" + slug + "\" has no installable version.";
            return result;
        }

        // 2. Check target dir
        fs::path target = resolve_skill_install_dir(workspace_dir, slug);
        if (!force && fs::exists(target)) {
            result.error = "Skill already exists at " + target.string() + ". Re-run with force/update.";
            return result;
        }

        // 3. Download archive
        log()->info("Downloading {}@{} from ClawHub...", slug, resolved_version);
        fs::path tmp_archive = fs::temp_directory_path() / ("clawhub-" + slug + ".tgz");
        std::string dl_url = url_base + "/skills/" + slug + "/versions/" + resolved_version + "/download";
        std::string dl_err = Impl::http_download(dl_url, tmp_archive);
        if (!dl_err.empty()) {
            result.error = dl_err;
            return result;
        }

        // 4. Extract + install
        std::error_code ec;
        fs::create_directories(target, ec);
        std::string extract_cmd = "tar -xzf '" + tmp_archive.string() + "' -C '" + target.string() + "' --strip-components=1";
        int exit_code = std::system(extract_cmd.c_str());
        fs::remove(tmp_archive, ec);
        if (exit_code != 0) {
            result.error = "Extraction failed (exit " + std::to_string(exit_code) + ")";
            return result;
        }

        // 5. Verify SKILL.md exists
        if (!fs::exists(target / "SKILL.md")) {
            fs::remove_all(target, ec);
            result.error = "Downloaded archive is missing SKILL.md";
            return result;
        }

        // 6. Write origin + lockfile
        ClawHubSkillOrigin origin;
        origin.registry = resolve_clawhub_base_url(base_url);
        origin.slug = slug;
        origin.installed_version = resolved_version;
        origin.installed_at = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        write_origin(target.string(), origin);

        auto lock = read_lockfile(workspace_dir);
        lock.skills[slug] = {resolved_version, origin.installed_at};
        write_lockfile(workspace_dir, lock);

        result.ok = true;
        result.version = resolved_version;
        result.target_dir = target.string();
        log()->info("Installed {}@{} -> {}", slug, resolved_version, target.string());

    } catch (const std::exception& e) {
        result.error = e.what();
    }
    return result;
}

// 对齐 OC: updateSkillsFromClawHub (line 411-463)
std::vector<UpdateClawHubSkillResult> ClawHubManager::update(
    const std::string& workspace_dir, const std::string& slug_raw,
    const std::string& base_url
) {
    std::vector<UpdateClawHubSkillResult> results;
    auto lock = read_lockfile(workspace_dir);

    std::vector<std::string> slugs;
    if (!slug_raw.empty()) {
        slugs.push_back(normalize_tracked_slug(slug_raw));
    } else {
        for (const auto& [s, _] : lock.skills) slugs.push_back(s);
        std::sort(slugs.begin(), slugs.end());
    }

    for (const auto& slug : slugs) {
        auto prev_entry = lock.skills.find(slug);
        std::string prev_version = prev_entry != lock.skills.end() ? prev_entry->second.version : "";

        auto install_result = install(workspace_dir, slug, "", base_url, true);
        UpdateClawHubSkillResult r;
        r.ok = install_result.ok;
        r.slug = slug;
        r.previous_version = prev_version;
        r.version = install_result.version;
        r.changed = install_result.ok && prev_version != install_result.version;
        r.target_dir = install_result.target_dir;
        r.error = install_result.error;
        results.push_back(r);
    }
    return results;
}

// 对齐 OC: readTrackedClawHubSkillSlugs (line 465-468)
std::vector<std::string> ClawHubManager::list_tracked_slugs(const std::string& workspace_dir) const {
    auto lock = read_lockfile(workspace_dir);
    std::vector<std::string> slugs;
    for (const auto& [s, _] : lock.skills) slugs.push_back(s);
    std::sort(slugs.begin(), slugs.end());
    return slugs;
}

// 对齐 OC: computeSkillFingerprint (line 470-500)
/*static*/ std::string ClawHubManager::compute_fingerprint(const std::string& skill_dir) {
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256_CTX sha_ctx;
    SHA256_Init(&sha_ctx);

    std::vector<fs::path> queue{fs::path(skill_dir)};
    while (!queue.empty()) {
        auto current = queue.front();
        queue.erase(queue.begin());

        std::vector<fs::directory_entry> entries;
        for (const auto& entry : fs::directory_iterator(current)) {
            entries.push_back(entry);
        }
        std::sort(entries.begin(), entries.end(),
            [](const auto& a, const auto& b) { return a.path().filename() < b.path().filename(); });

        for (const auto& entry : entries) {
            if (entry.path().filename().string()[0] == '.') continue;
            if (entry.is_directory()) {
                queue.push_back(entry.path());
                continue;
            }
            if (!entry.is_regular_file()) continue;
            std::string rel = fs::relative(entry.path(), skill_dir).string();
            std::replace(rel.begin(), rel.end(), '\\', '/');
            SHA256_Update(&sha_ctx, rel.data(), rel.size());
            SHA256_Update(&sha_ctx, "\n", 1);
            std::ifstream f(entry.path(), std::ios::binary);
            char buf[8192];
            while (f.read(buf, sizeof(buf))) SHA256_Update(&sha_ctx, buf, f.gcount());
            if (f.gcount() > 0) SHA256_Update(&sha_ctx, buf, f.gcount());
            SHA256_Update(&sha_ctx, "\n", 1);
        }
    }

    SHA256_Final(hash, &sha_ctx);
    std::string hex;
    hex.reserve(SHA256_DIGEST_LENGTH * 2);
    for (int i = 0; i < SHA256_DIGEST_LENGTH; i++) {
        char h[3];
        snprintf(h, sizeof(h), "%02x", hash[i]);
        hex += h;
    }
    return hex;
}

// 对齐 OC: readClawHubSkillsLockfile (line 144-167)
/*static*/ ClawHubSkillsLockfile ClawHubManager::read_lockfile(const std::string& workspace_dir) {
    ClawHubSkillsLockfile lock;
    std::vector<fs::path> candidates = {
        fs::path(workspace_dir) / DOT_DIR / "lock.json",
        fs::path(workspace_dir) / LEGACY_DOT_DIR / "lock.json",
    };
    for (const auto& path : candidates) {
        try {
            std::ifstream f(path);
            if (!f.is_open()) continue;
            auto j = json::parse(f);
            if (j.value("version", 0) != 1) continue;
            auto skills = j.value("skills", json::object());
            for (auto& [slug, val] : skills.items()) {
                lock.skills[slug] = {
                    val.value("version", ""),
                    val.value("installedAt", 0)
                };
            }
            return lock;
        } catch (...) {}
    }
    return lock;
}

// 对齐 OC: writeClawHubSkillsLockfile (line 169-176)
/*static*/ void ClawHubManager::write_lockfile(
    const std::string& workspace_dir, const ClawHubSkillsLockfile& lock
) {
    fs::path target = fs::path(workspace_dir) / DOT_DIR / "lock.json";
    std::error_code ec;
    fs::create_directories(target.parent_path(), ec);
    json j;
    j["version"] = 1;
    json skills_j = json::object();
    for (const auto& [slug, entry] : lock.skills) {
        skills_j[slug] = {{"version", entry.version}, {"installedAt", entry.installed_at}};
    }
    j["skills"] = skills_j;
    std::string tmp = target.string() + ".tmp";
    {
        std::ofstream out(tmp);
        out << j.dump(2) << "\n";
    }
    fs::rename(tmp, target, ec);
}

// 对齐 OC: readClawHubSkillOrigin (line 178-200)
/*static*/ std::optional<ClawHubSkillOrigin> ClawHubManager::read_origin(const std::string& skill_dir) {
    std::vector<fs::path> candidates = {
        fs::path(skill_dir) / DOT_DIR / "origin.json",
        fs::path(skill_dir) / LEGACY_DOT_DIR / "origin.json",
    };
    for (const auto& path : candidates) {
        try {
            std::ifstream f(path);
            if (!f.is_open()) continue;
            auto j = json::parse(f);
            if (j.value("version", 0) != 1) continue;
            ClawHubSkillOrigin origin;
            origin.registry = j.value("registry", "");
            origin.slug = j.value("slug", "");
            origin.installed_version = j.value("installedVersion", "");
            origin.installed_at = j.value("installedAt", 0);
            if (!origin.registry.empty() && !origin.slug.empty()) return origin;
        } catch (...) {}
    }
    return std::nullopt;
}

// 对齐 OC: writeClawHubSkillOrigin (line 202-209)
/*static*/ void ClawHubManager::write_origin(
    const std::string& skill_dir, const ClawHubSkillOrigin& origin
) {
    fs::path target = fs::path(skill_dir) / DOT_DIR / "origin.json";
    std::error_code ec;
    fs::create_directories(target.parent_path(), ec);
    json j;
    j["version"] = 1;
    j["registry"] = origin.registry;
    j["slug"] = origin.slug;
    j["installedVersion"] = origin.installed_version;
    j["installedAt"] = origin.installed_at;
    std::string tmp = target.string() + ".tmp";
    {
        std::ofstream out(tmp);
        out << j.dump(2) << "\n";
    }
    fs::rename(tmp, target, ec);
}

}  // namespace skills
}  // namespace hunterclaw
