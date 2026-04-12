// ============================================================================
// chat_attachments.cc
// 🎯 严格对齐 OpenClaw: src/gateway/chat-attachments.ts (507 行)
//
// 聊天附件处理：双层策略
// - ≤ 2MB: 内联传递给模型 (ChatImageContent)
// - > 2MB: 卸载到磁盘媒体存储 (OffloadedRef + media://inbound/<id> 标记)
// ============================================================================

#include "hunterclaw/gateway/gateway.h"
#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <optional>
#include <algorithm>
#include <sstream>
#include <filesystem>
#include <fstream>
#include <spdlog/spdlog.h>

using json = nlohmann::json;

namespace hunterclaw {
namespace gateway {

// ── Constants ────────────────────────────────────────────────────────────────
// 🎯 对齐: OFFLOAD_THRESHOLD_BYTES = 2_000_000

static constexpr size_t OFFLOAD_THRESHOLD_BYTES = 2'000'000;

// 🎯 对齐: MIME_TO_EXT mapping
static const std::map<std::string, std::string> MIME_TO_EXT = {
    {"image/jpeg", ".jpg"},
    {"image/jpg", ".jpg"},
    {"image/png", ".png"},
    {"image/webp", ".webp"},
    {"image/gif", ".gif"},
    {"image/heic", ".heic"},
    {"image/heif", ".heif"},
    {"image/bmp", ".bmp"},
    {"image/tiff", ".tiff"},
};

// 🎯 对齐: SUPPORTED_OFFLOAD_MIMES — O(1) lookup set
static const std::set<std::string> SUPPORTED_OFFLOAD_MIMES = {
    "image/jpeg", "image/jpg", "image/png", "image/webp",
    "image/gif", "image/heic", "image/heif",
};

// ── Types ────────────────────────────────────────────────────────────────────

// 🎯 对齐: ChatAttachment
struct ChatAttachment {
    std::string type;
    std::string mime_type;
    std::string file_name;
    std::string content;  // base64 data or raw string
};

// 🎯 对齐: ChatImageContent
struct ChatImageContent {
    std::string data;      // base64
    std::string mime_type;
};

// 🎯 对齐: OffloadedRef — 大附件卸载后的元数据
struct OffloadedRef {
    std::string media_ref;  // "media://inbound/<id>"
    std::string id;
    std::string path;
    std::string mime_type;
    std::string label;
};

// 🎯 对齐: PromptImageOrderEntry
struct PromptImageOrderEntry {
    int index = 0;
    std::string source;  // "inline" | "offloaded"
};

// 🎯 对齐: ParsedMessageWithImages — 核心输出结构
struct ParsedMessageWithImages {
    std::string message;
    std::vector<ChatImageContent> images;
    std::vector<PromptImageOrderEntry> image_order;
    std::vector<OffloadedRef> offloaded_refs;
};

// 🎯 对齐: NormalizedAttachment
struct NormalizedAttachment {
    std::string label;
    std::string mime;
    std::string base64;
};

// ── Helpers ──────────────────────────────────────────────────────────────────

// 🎯 对齐: estimateBase64DecodedBytes
static size_t estimate_base64_decoded_bytes(const std::string& base64) {
    size_t len = base64.size();
    // Remove padding chars
    size_t padding = 0;
    if (len >= 2 && base64[len - 1] == '=') padding++;
    if (len >= 2 && base64[len - 2] == '=') padding++;
    return (len * 3) / 4 - padding;
}

// 🎯 对齐: sniffMimeFromBase64
static std::string sniff_mime_from_base64(const std::string& base64) {
    // Decode first 8 bytes to check magic numbers
    // JPEG: FF D8 FF
    // PNG:  89 50 4E 47
    // GIF:  47 49 46
    // WEBP: 52 49 46 46 ... 57 45 42 50
    if (base64.size() < 8) return "";

    // Check common base64 prefixes for known image types
    if (base64.rfind("/9j/", 0) == 0) return "image/jpeg";
    if (base64.rfind("iVBOR", 0) == 0) return "image/png";
    if (base64.rfind("R0lGO", 0) == 0) return "image/gif";
    if (base64.rfind("UklGR", 0) == 0) return "image/webp";

    return "";
}

// 🎯 对齐: normalizeLowercaseStringOrEmpty
static std::string normalize_lowercase(const std::string& s) {
    std::string result = s;
    std::transform(result.begin(), result.end(), result.begin(), ::tolower);
    // Trim whitespace
    size_t start = result.find_first_not_of(" \t\n\r");
    if (start == std::string::npos) return "";
    size_t end = result.find_last_not_of(" \t\n\r");
    return result.substr(start, end - start + 1);
}

// 🎯 对齐: normalizeAttachment — normalize a single attachment input
static std::optional<NormalizedAttachment> normalize_attachment(
    const ChatAttachment& att) {
    // Content must be a non-empty string (base64 data)
    if (att.content.empty()) return std::nullopt;

    // Resolve MIME type: explicit > sniffed
    std::string mime = normalize_lowercase(att.mime_type);
    if (mime.empty()) {
        mime = normalize_lowercase(att.type);
    }
    if (mime.empty()) {
        mime = sniff_mime_from_base64(att.content);
    }
    if (mime.empty()) return std::nullopt;

    // Resolve label
    std::string label = att.file_name;
    if (label.empty()) {
        auto ext_it = MIME_TO_EXT.find(mime);
        label = "attachment" + (ext_it != MIME_TO_EXT.end() ? ext_it->second : "");
    }

    return NormalizedAttachment{label, mime, att.content};
}

// ── Core: parseMessageWithAttachments ────────────────────────────────────────
// 🎯 对齐: parseMessageWithAttachments(params) — 核心附件处理函数
//
// 双层策略:
//   ≤ OFFLOAD_THRESHOLD_BYTES → images[] (内联传递给模型)
//   > OFFLOAD_THRESHOLD_BYTES → offloadedRefs[] + message 追加 media:// 标记

ParsedMessageWithImages parse_message_with_attachments(
    const std::string& message,
    const std::vector<ChatAttachment>& attachments,
    bool supports_images,
    const std::string& media_store_dir) {

    ParsedMessageWithImages result;
    result.message = message;

    if (attachments.empty()) {
        return result;
    }

    int order_index = 0;
    std::vector<std::string> offload_markers;

    for (const auto& att : attachments) {
        auto normalized = normalize_attachment(att);
        if (!normalized.has_value()) {
            continue;
        }

        size_t decoded_size = estimate_base64_decoded_bytes(normalized->base64);

        // 🎯 对齐: 分流逻辑 — 内联 vs 卸载
        if (decoded_size <= OFFLOAD_THRESHOLD_BYTES && supports_images) {
            // ── Inline path: small attachment → ChatImageContent ──
            ChatImageContent img;
            img.data = normalized->base64;
            img.mime_type = normalized->mime;
            result.images.push_back(std::move(img));

            PromptImageOrderEntry order;
            order.index = order_index++;
            order.source = "inline";
            result.image_order.push_back(order);

        } else if (decoded_size > OFFLOAD_THRESHOLD_BYTES &&
                   SUPPORTED_OFFLOAD_MIMES.count(normalized->mime) > 0) {
            // ── Offload path: large attachment → disk + media:// marker ──

            // Generate media ID
            std::string media_id = core::utils::generate_uuid();
            std::string ext;
            auto ext_it = MIME_TO_EXT.find(normalized->mime);
            if (ext_it != MIME_TO_EXT.end()) {
                ext = ext_it->second;
            }

            // Save to media store
            std::string media_path;
            if (!media_store_dir.empty()) {
                std::filesystem::create_directories(media_store_dir);
                media_path = (std::filesystem::path(media_store_dir) /
                              (media_id + ext)).string();

                // Decode base64 and write to disk
                // NOTE: Full implementation would use proper base64 decoding.
                // For now, write the base64 data directly (placeholder).
                try {
                    std::ofstream out(media_path, std::ios::binary);
                    out << normalized->base64;
                    out.close();
                } catch (const std::exception& e) {
                    auto log = spdlog::get("gateway");
                    if (log) log->warn("[chat_attachments] Failed to save media: {}", e.what());
                    continue;
                }
            }

            std::string media_ref = "media://inbound/" + media_id;

            OffloadedRef ref;
            ref.media_ref = media_ref;
            ref.id = media_id;
            ref.path = media_path;
            ref.mime_type = normalized->mime;
            ref.label = normalized->label;
            result.offloaded_refs.push_back(std::move(ref));

            PromptImageOrderEntry order;
            order.index = order_index++;
            order.source = "offloaded";
            result.image_order.push_back(order);

            // 🎯 对齐: 追加 media:// 标记到消息文本
            offload_markers.push_back(
                "[media attached: " + media_ref + "]");

        } else {
            // Unsupported MIME type or text-only model — skip silently
            // 🎯 对齐: log.warn but don't inject unresolvable markers
        }
    }

    // 🎯 对齐: 将卸载标记追加到消息末尾
    if (!offload_markers.empty()) {
        result.message += "\n";
        for (const auto& marker : offload_markers) {
            result.message += "\n" + marker;
        }
    }

    return result;
}

// ── JSON serialization ──────────────────────────────────────────────────────

json parsed_message_to_json(const ParsedMessageWithImages& parsed) {
    json j;
    j["message"] = parsed.message;

    json images_arr = json::array();
    for (const auto& img : parsed.images) {
        json img_j;
        img_j["type"] = "image";
        img_j["data"] = img.data;
        img_j["mimeType"] = img.mime_type;
        images_arr.push_back(img_j);
    }
    j["images"] = images_arr;

    json order_arr = json::array();
    for (const auto& o : parsed.image_order) {
        json o_j;
        o_j["index"] = o.index;
        o_j["source"] = o.source;
        order_arr.push_back(o_j);
    }
    j["imageOrder"] = order_arr;

    json refs_arr = json::array();
    for (const auto& r : parsed.offloaded_refs) {
        json r_j;
        r_j["mediaRef"] = r.media_ref;
        r_j["id"] = r.id;
        r_j["path"] = r.path;
        r_j["mimeType"] = r.mime_type;
        r_j["label"] = r.label;
        refs_arr.push_back(r_j);
    }
    j["offloadedRefs"] = refs_arr;

    return j;
}

ChatAttachment chat_attachment_from_json(const json& j) {
    ChatAttachment att;
    att.type = j.value("type", "");
    att.mime_type = j.value("mimeType", j.value("mime_type", ""));
    att.file_name = j.value("fileName", j.value("file_name", ""));
    if (j.contains("content") && j["content"].is_string()) {
        att.content = j["content"].get<std::string>();
    }
    return att;
}

}  // namespace gateway
}  // namespace hunterclaw
