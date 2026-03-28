#pragma once

#include <string>
#include <vector>
#include <set>
#include <optional>

namespace hunterclaw {
namespace security {

// ============================================================================
// Security Path Canonicalization - 安全路径规范化
// ============================================================================

/**
 * SecurityPathCanonicalization - 路径规范化结果
 *
 * 🎯 对齐 OpenClaw: SecurityPathCanonicalization
 * {
 *   canonicalPath: string;
 *   candidates: string[];
 *   decodePasses: number;
 *   decodePassLimitReached: boolean;
 *   malformedEncoding: boolean;
 *   rawNormalizedPath: string;
 * }
 */
struct SecurityPathCanonicalization {
    std::string canonical_path;          // 最终规范化路径
    std::vector<std::string> candidates; // 所有候选路径 (含中间解码结果)
    int decode_passes = 0;               // URL 解码次数
    bool decode_pass_limit_reached = false; // 是否达到解码上限
    bool malformed_encoding = false;     // 是否存在格式错误的编码
    std::string raw_normalized_path;     // 原始规范化路径 (未解码)
};

// 🎯 对齐 OpenClaw: const MAX_PATH_DECODE_PASSES = 32
constexpr int MAX_PATH_DECODE_PASSES = 32;

// ============================================================================
// Core Path Functions - 核心路径函数
// ============================================================================

/**
 * normalize_path_separators - 规范化路径分隔符
 *
 * 🎯 对齐 OpenClaw: normalizePathSeparators(pathname)
 * - 折叠连续的 "/" 为单个 "/"
 * - 移除末尾的 "/"（除非路径长度 <= 1）
 */
std::string normalize_path_separators(const std::string& pathname);

/**
 * normalize_path_for_security - 安全路径规范化（小写 + 解析 dot segments）
 *
 * 🎯 对齐 OpenClaw: normalizePathForSecurity(pathname)
 * - 解析 . 和 .. 段
 * - 转小写
 * - 折叠连续 "/"
 */
std::string normalize_path_for_security(const std::string& pathname);

/**
 * build_canonical_path_candidates - 构建规范化路径候选列表
 *
 * 🎯 对齐 OpenClaw: buildCanonicalPathCandidates(pathname, maxDecodePasses)
 * - 对路径进行多轮 URL 解码
 * - 每轮解码后规范化并去重
 * - 返回所有候选路径
 */
struct CanonicalPathCandidatesResult {
    std::vector<std::string> candidates;
    int decode_passes = 0;
    bool decode_pass_limit_reached = false;
    bool malformed_encoding = false;
};

CanonicalPathCandidatesResult build_canonical_path_candidates(
    const std::string& pathname,
    int max_decode_passes = MAX_PATH_DECODE_PASSES
);

/**
 * canonicalize_path_variant - 返回最终规范化路径
 *
 * 🎯 对齐 OpenClaw: canonicalizePathVariant(pathname)
 * - 返回 candidates 中最后一个元素（最深度解码的路径）
 */
std::string canonicalize_path_variant(const std::string& pathname);

/**
 * canonicalize_path_for_security - 完整的安全路径规范化
 *
 * 🎯 对齐 OpenClaw: canonicalizePathForSecurity(pathname)
 * - 返回完整的 SecurityPathCanonicalization 结果
 */
SecurityPathCanonicalization canonicalize_path_for_security(const std::string& pathname);

/**
 * has_security_path_canonicalization_anomaly - 检测路径规范化异常
 *
 * 🎯 对齐 OpenClaw: hasSecurityPathCanonicalizationAnomaly(pathname)
 * - 返回 true 如果存在格式错误的编码或达到解码上限
 */
bool has_security_path_canonicalization_anomaly(const std::string& pathname);

// ============================================================================
// Prefix Protection - 前缀保护检查
// ============================================================================

/**
 * is_path_protected_by_prefixes - 检查路径是否受保护前缀保护
 *
 * 🎯 对齐 OpenClaw: isPathProtectedByPrefixes(pathname, prefixes)
 * - 对所有候选路径检查是否匹配任意保护前缀
 * - Fail closed: 解码上限达到时返回 true（保守策略）
 * - 格式错误编码时: 对 rawNormalizedPath 进行前缀匹配
 */
bool is_path_protected_by_prefixes(
    const std::string& pathname,
    const std::vector<std::string>& prefixes
);

/**
 * PROTECTED_PLUGIN_ROUTE_PREFIXES - 受保护的插件路由前缀
 *
 * 🎯 对齐 OpenClaw: PROTECTED_PLUGIN_ROUTE_PREFIXES = ["/api/channels"] as const
 */
inline const std::vector<std::string> PROTECTED_PLUGIN_ROUTE_PREFIXES = {
    "/api/channels"
};

/**
 * is_protected_plugin_route_path - 检查路径是否为受保护的插件路由
 *
 * 🎯 对齐 OpenClaw: isProtectedPluginRoutePath(pathname)
 */
bool is_protected_plugin_route_path(const std::string& pathname);

// ============================================================================
// URL Decode Utilities - URL 解码工具
// ============================================================================

/**
 * url_decode_once - 单次 URL 解码
 *
 * @param input 输入字符串
 * @param ok 输出: 是否解码成功（false 表示格式错误）
 * @return 解码后的字符串
 */
std::string url_decode_once(const std::string& input, bool& ok);

}  // namespace security
}  // namespace hunterclaw
