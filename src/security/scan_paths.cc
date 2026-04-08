#include <string>
#include <vector>
#include <filesystem>
#include <algorithm>

namespace hunterclaw {
namespace security {

// ============================================================================
// Scan Paths - 路径安全工具（路径穿越检查）
// 严格对齐 OpenClaw: src/security/scan-paths.ts
// ============================================================================

namespace fs = std::filesystem;

/**
 * is_path_inside - 检查 candidate 是否在 base 路径内部
 * 对齐 OpenClaw: isPathInside(basePath, candidatePath)
 */
bool is_path_inside(const std::string& base_path, const std::string& candidate_path) {
    auto base = fs::absolute(base_path).lexically_normal();
    auto candidate = fs::absolute(candidate_path).lexically_normal();
    auto rel = candidate.lexically_relative(base);
    if (rel.empty()) return true; // same path
    std::string rel_str = rel.string();
    if (rel_str == ".") return true;
    if (rel_str.starts_with("..")) return false;
    if (fs::path(rel_str).is_absolute()) return false;
    return true;
}

/**
 * is_path_inside_with_realpath - 包含 realpath 解析的路径内部检查
 * 对齐 OpenClaw: isPathInsideWithRealpath(basePath, candidatePath, opts?)
 */
bool is_path_inside_with_realpath(
    const std::string& base_path,
    const std::string& candidate_path,
    bool require_realpath
) {
    if (!is_path_inside(base_path, candidate_path)) {
        return false;
    }
    
    std::error_code ec;
    auto base_real = fs::canonical(base_path, ec);
    if (ec) return !require_realpath;
    
    auto candidate_real = fs::canonical(candidate_path, ec);
    if (ec) return !require_realpath;
    
    return is_path_inside(base_real.string(), candidate_real.string());
}

/**
 * extension_uses_skipped_scanner_path - 检查扩展入口是否使用了跳过的扫描路径
 * 对齐 OpenClaw: extensionUsesSkippedScannerPath(entry)
 *
 * 检查路径段是否包含 node_modules 或以 "." 开头的隐藏目录
 */
bool extension_uses_skipped_scanner_path(const std::string& entry) {
    std::string segment;
    std::istringstream stream(entry);
    
    // Split by / and backslash
    auto split = [](const std::string& s) -> std::vector<std::string> {
        std::vector<std::string> parts;
        std::string part;
        for (char c : s) {
            if (c == '/' || c == '\\') {
                if (!part.empty()) {
                    parts.push_back(part);
                    part.clear();
                }
            } else {
                part += c;
            }
        }
        if (!part.empty()) parts.push_back(part);
        return parts;
    };
    
    auto segments = split(entry);
    for (const auto& seg : segments) {
        if (seg == "node_modules") return true;
        if (seg.size() > 1 && seg[0] == '.' && seg != "." && seg != "..") return true;
    }
    return false;
}

}  // namespace security
}  // namespace hunterclaw
