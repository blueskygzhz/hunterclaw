// 🎯 对齐 OpenClaw: src/secrets/resolve.ts
// 实现 secret 文件路径安全验证
// 调用 security/scan_paths 的 is_path_inside
// 调用 security/audit 的 check_path_permissions (via inspectPathPermissions)

#include "hunterclaw/secrets/runtime.h"
#include <filesystem>
#include <fstream>
#include <sys/stat.h>
#include <spdlog/spdlog.h>

namespace fs = std::filesystem;

namespace hunterclaw {
namespace secrets {

// Forward declarations from security modules
namespace security {
    bool is_path_inside(const std::string& base_path, const std::string& candidate_path);
}

// ============================================================================
// SafeStat - 对齐 OpenClaw: safeStat (audit-fs.ts)
// ============================================================================

struct SafeStatResult {
    bool ok = false;
    bool is_symlink = false;
    bool is_dir = false;
    int mode = 0;
    std::string error;
};

static SafeStatResult safe_stat(const std::string& path) {
    SafeStatResult result;
    struct stat st;
    if (lstat(path.c_str(), &st) != 0) {
        result.error = "Cannot stat: " + path;
        return result;
    }
    result.ok = true;
    result.is_symlink = S_ISLNK(st.st_mode);
    result.is_dir = S_ISDIR(st.st_mode);
    result.mode = st.st_mode & 0777;
    return result;
}

// ============================================================================
// InspectPathPermissions - 对齐 OpenClaw: inspectPathPermissions (audit-fs.ts)
// ============================================================================

struct PermissionCheck {
    bool ok = false;
    bool world_writable = false;
    bool group_writable = false;
    bool world_readable = false;
    bool group_readable = false;
    std::string error;
};

static PermissionCheck inspect_path_permissions(const std::string& path) {
    PermissionCheck result;
    auto stat_result = safe_stat(path);
    if (!stat_result.ok) {
        result.error = stat_result.error;
        return result;
    }

    int bits = stat_result.mode;
    result.ok = true;
    result.world_writable = (bits & 0002) != 0;
    result.group_writable = (bits & 0020) != 0;
    result.world_readable = (bits & 0004) != 0;
    result.group_readable = (bits & 0040) != 0;
    return result;
}

// ============================================================================
// validate_secret_file_path - 对齐 OpenClaw: resolveEffectiveSecretPath
// 🎯 调用 is_path_inside + inspect_path_permissions
// ============================================================================

struct SecretPathValidation {
    bool ok = false;
    std::string effective_path;
    std::string error;
};

SecretPathValidation validate_secret_file_path(
    const std::string& file_path,
    const std::string& label,
    const std::vector<std::string>& trusted_dirs,
    bool allow_insecure_path,
    bool allow_readable_by_others
) {
    SecretPathValidation result;

    // Resolve absolute path
    std::error_code ec;
    fs::path abs_path = fs::absolute(file_path, ec);
    if (ec) {
        result.error = label + " path resolution failed: " + file_path;
        return result;
    }
    result.effective_path = abs_path.string();

    // 🎯 对齐 OpenClaw: readFileStatOrThrow (line 150)
    auto stat = safe_stat(result.effective_path);
    if (!stat.ok) {
        result.error = label + " is not readable: " + result.effective_path;
        return result;
    }
    if (stat.is_dir) {
        result.error = label + " must be a file: " + result.effective_path;
        return result;
    }
    // 🎯 对齐 OpenClaw: symlink check (line 148-149)
    if (stat.is_symlink) {
        // Resolve symlink target
        auto real = fs::canonical(result.effective_path, ec);
        if (ec) {
            result.error = label + " symlink target must not be a symlink: " + result.effective_path;
            return result;
        }
        auto target_stat = safe_stat(real.string());
        if (target_stat.is_symlink) {
            result.error = label + " symlink target must not be a symlink: " + result.effective_path;
            return result;
        }
    }

    // 🎯 对齐 OpenClaw: trustedDirs check (line 241-246)
    // 调用 security::is_path_inside
    if (!trusted_dirs.empty()) {
        bool in_trusted = false;
        for (const auto& dir : trusted_dirs) {
            fs::path resolved_dir = fs::absolute(dir, ec);
            if (!ec && security::is_path_inside(resolved_dir.string(), result.effective_path)) {
                in_trusted = true;
                break;
            }
        }
        if (!in_trusted) {
            result.error = label + " is outside trustedDirs: " + result.effective_path;
            return result;
        }
    }

    // 🎯 对齐 OpenClaw: allowInsecurePath short-circuit (line 247-248)
    if (allow_insecure_path) {
        result.ok = true;
        return result;
    }

    // 🎯 对齐 OpenClaw: inspectPathPermissions check (line 250-258)
    auto perms = inspect_path_permissions(result.effective_path);
    if (!perms.ok) {
        result.error = label + " permissions could not be verified: " + result.effective_path;
        return result;
    }
    bool writable_by_others = perms.world_writable || perms.group_writable;
    bool readable_by_others = perms.world_readable || perms.group_readable;
    if (writable_by_others || (!allow_readable_by_others && readable_by_others)) {
        result.error = label + " permissions are too open: " + result.effective_path;
        return result;
    }

    result.ok = true;
    return result;
}

}  // namespace secrets
}  // namespace hunterclaw
