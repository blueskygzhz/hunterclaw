#pragma once
// ============================================================================
// reply_media_paths.h — 严格对齐 OpenClaw reply-media-paths.ts (228行)
//
// Reply 媒体路径规范化器: HTTP 直通、sandbox 路径解析、file:// 拦截、
// workspace-only 策略、volatile media 持久化、路径安全校验
// ============================================================================

#include "hunterclaw/react/reply_payloads.h"
#include <string>
#include <vector>
#include <set>
#include <functional>
#include <optional>
#include <filesystem>

namespace hunterclaw {
namespace react {

// 对齐 reply-media-paths.ts:14-20: 正则常量
bool is_http_url(const std::string& media);
bool is_file_url(const std::string& media);
bool is_windows_drive_path(const std::string& media);
bool has_scheme(const std::string& media);
bool has_file_extension(const std::string& media);

// 对齐 reply-media-paths.ts:22-25: isPathInside
bool is_path_inside(const std::string& root, const std::string& candidate);

// 对齐 reply-media-paths.ts:51-63: isLikelyLocalMediaSource
bool is_likely_local_media_source(const std::string& media);

// 对齐 reply-media-paths.ts:37-49: isAllowedAbsoluteReplyMediaPath
bool is_allowed_absolute_reply_media_path(
    const std::string& candidate,
    const std::string& workspace_dir,
    const std::string& config_dir = "",
    const std::optional<std::string>& sandbox_root = std::nullopt
);

// 对齐 reply-media-paths.ts:69-228: createReplyMediaPathNormalizer
struct ReplyMediaPathNormalizerParams {
    std::string workspace_dir;
    std::string config_dir;
    bool workspace_only = false;
    std::optional<std::string> sandbox_root;
    // 可选: volatile media 持久化回调
    std::function<std::string(const std::string& source_path)> persist_volatile_media;
};

// 创建规范化器 (返回 payload → payload 的转换函数)
std::function<ReplyPayload(const ReplyPayload&)> create_reply_media_path_normalizer(
    const ReplyMediaPathNormalizerParams& params
);

// 对齐 reply-media-paths.ts:125-189: normalizeMediaSource (单条)
std::string normalize_media_source(
    const std::string& raw,
    const std::string& workspace_dir,
    bool workspace_only = false,
    const std::optional<std::string>& sandbox_root = std::nullopt,
    const std::string& config_dir = ""
);

} // namespace react
} // namespace hunterclaw
