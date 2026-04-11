#pragma once

// 严格对齐 OpenClaw auto-reply/media-note.ts (156行)
// 附件 → 文本标注: 构建 [media attached: ...] 注释

#include <optional>
#include <set>
#include <string>
#include <vector>

namespace hunterclaw {
namespace auto_reply {

/// Build a media note string from message context media paths.
/// 对齐 buildInboundMediaNote
std::optional<std::string> build_inbound_media_note(
    const std::vector<std::string>& media_paths,
    const std::vector<std::string>& media_urls = {},
    const std::vector<std::string>& media_types = {},
    const std::optional<std::string>& transcript = std::nullopt,
    const std::set<int>& suppressed_indices = {},
    const std::set<int>& transcribed_audio_indices = {}
);

/// Format a single media attached line.
/// 对齐 formatMediaAttachedLine
std::string format_media_attached_line(
    const std::string& path,
    const std::optional<std::string>& url = std::nullopt,
    const std::optional<std::string>& type = std::nullopt,
    const std::optional<int>& index = std::nullopt,
    const std::optional<int>& total = std::nullopt
);

/// Check if a file path is an audio file.
/// 对齐 isAudioPath
bool is_audio_path(const std::string& path);

}  // namespace auto_reply
}  // namespace hunterclaw
