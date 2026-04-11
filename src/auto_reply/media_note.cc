// 严格对齐 OpenClaw auto-reply/media-note.ts (156行)
#include "hunterclaw/auto_reply/media_note.h"
#include <algorithm>
#include <sstream>

namespace hunterclaw {
namespace auto_reply {

static const std::set<std::string> AUDIO_EXTENSIONS = {
  ".ogg", ".opus", ".mp3", ".m4a", ".wav", ".webm", ".flac", ".aac", ".wma", ".aiff", ".alac", ".oga"
};

bool is_audio_path(const std::string& path) {
  if (path.empty()) return false;
  std::string lower = path;
  std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
  for (const auto& ext : AUDIO_EXTENSIONS) {
    if (lower.size() >= ext.size() && lower.substr(lower.size() - ext.size()) == ext) return true;
  }
  return false;
}

std::string format_media_attached_line(
    const std::string& path,
    const std::optional<std::string>& url,
    const std::optional<std::string>& type,
    const std::optional<int>& index,
    const std::optional<int>& total
) {
  // 对齐 media-note.ts:4-18
  std::string prefix = (index && total)
    ? "[media attached " + std::to_string(*index) + "/" + std::to_string(*total) + ": "
    : "[media attached: ";
  std::string type_part;
  if (type) {
    std::string t = *type;
    auto s = t.find_first_not_of(" \t"); auto e = t.find_last_not_of(" \t");
    if (s != std::string::npos) { t = t.substr(s, e-s+1); type_part = " (" + t + ")"; }
  }
  std::string url_part;
  if (url) {
    std::string u = *url;
    auto s = u.find_first_not_of(" \t");
    if (s != std::string::npos) { u = u.substr(s); url_part = " | " + u; }
  }
  return prefix + path + type_part + url_part + "]";
}

std::optional<std::string> build_inbound_media_note(
    const std::vector<std::string>& media_paths,
    const std::vector<std::string>& media_urls,
    const std::vector<std::string>& media_types,
    const std::optional<std::string>& transcript,
    const std::set<int>& suppressed_indices,
    const std::set<int>& transcribed_audio_indices
) {
  if (media_paths.empty()) return std::nullopt;
  
  bool has_transcript = transcript && !transcript->empty();
  bool can_strip_single = has_transcript && media_paths.size() == 1;
  
  struct Entry { std::string path; std::optional<std::string> type; std::optional<std::string> url; int index; };
  std::vector<Entry> entries;
  
  for (size_t i = 0; i < media_paths.size(); ++i) {
    if (suppressed_indices.count(static_cast<int>(i))) continue;
    
    bool is_audio = is_audio_path(media_paths[i]);
    if (!is_audio && i < media_types.size()) {
      std::string t = media_types[i];
      std::transform(t.begin(), t.end(), t.begin(), ::tolower);
      if (t.find("audio/") == 0) is_audio = true;
    }
    
    if (is_audio) {
      if (transcribed_audio_indices.count(static_cast<int>(i))) continue;
      if (can_strip_single && i == 0) continue;
    }
    
    Entry e;
    e.path = media_paths[i];
    e.index = static_cast<int>(i);
    if (i < media_types.size()) e.type = media_types[i];
    if (i < media_urls.size()) e.url = media_urls[i];
    entries.push_back(e);
  }
  
  if (entries.empty()) return std::nullopt;
  
  if (entries.size() == 1) {
    return format_media_attached_line(entries[0].path, entries[0].url, entries[0].type);
  }
  
  int count = static_cast<int>(entries.size());
  std::ostringstream oss;
  oss << "[media attached: " << count << " files]";
  for (int idx = 0; idx < count; ++idx) {
    oss << "\n" << format_media_attached_line(
      entries[idx].path, entries[idx].url, entries[idx].type, idx + 1, count
    );
  }
  return oss.str();
}

}  // namespace auto_reply
}  // namespace hunterclaw
