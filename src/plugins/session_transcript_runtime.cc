#include "hunterclaw/plugins/session_transcript_runtime.h"

#include "hunterclaw/routing/session_key.h"   // resolve_agent_id_from_session_key
#include "hunterclaw/sessions/session_paths.h" // resolve_session_transcript_path

#include <nlohmann/json.hpp>

#include <cctype>
#include <filesystem>
#include <fstream>

using nlohmann::json;
namespace fs = std::filesystem;

namespace hunterclaw {
namespace plugins {

namespace {

// ============================================================================
// URL 组件编码 (对齐 OpenClaw encodeURIComponent / decodeURIComponent)
// ============================================================================

std::string url_encode_component(const std::string& value) {
  static const char* hex = "0123456789ABCDEF";
  std::string out;
  out.reserve(value.size());
  for (unsigned char c : value) {
    if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out += static_cast<char>(c);
    } else {
      out += '%';
      out += hex[c >> 4];
      out += hex[c & 0x0F];
    }
  }
  return out;
}

std::string url_decode_component(const std::string& value) {
  std::string out;
  out.reserve(value.size());
  for (size_t i = 0; i < value.size(); ++i) {
    if (value[i] == '%' && i + 2 < value.size() &&
        std::isxdigit(static_cast<unsigned char>(value[i + 1])) &&
        std::isxdigit(static_cast<unsigned char>(value[i + 2]))) {
      auto hex_val = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return 0;
      };
      out += static_cast<char>(hex_val(value[i + 1]) * 16 + hex_val(value[i + 2]));
      i += 2;
    } else if (value[i] == '+') {
      out += ' ';
    } else {
      out += value[i];
    }
  }
  return out;
}

/// 对齐 OpenClaw trimToNull: trim 后为空返回 nullopt
std::optional<std::string> trim_to_opt(const std::string& value) {
  size_t begin = 0;
  size_t end = value.size();
  while (begin < end && std::isspace(static_cast<unsigned char>(value[begin]))) ++begin;
  while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1]))) --end;
  if (begin == end) return std::nullopt;
  return value.substr(begin, end - begin);
}

// ============================================================================
// 内部 Runtime Read Target (对齐 OpenClaw resolveSessionTranscriptRuntimeReadTarget)
// 公开的 SessionTranscriptTarget 不含 storePath；读取需要落到具体 JSONL 文件。
// ============================================================================

struct RuntimeReadTarget {
  std::string agent_id;
  std::string session_id;
  std::string session_key;
  std::string store_path;  // JSONL 文件路径
};

core::Result<RuntimeReadTarget> resolve_read_target(
    const SessionTranscriptReadParams& params) {
  const auto session_id = trim_to_opt(params.session_id);
  if (!session_id) {
    return core::Result<RuntimeReadTarget>::Err(
        "SessionTranscriptRuntimeError: sessionId is required");
  }

  std::string agent_id = trim_to_opt(params.agent_id.value_or("")).value_or("");
  if (agent_id.empty()) {
    agent_id = routing::resolve_agent_id_from_session_key(params.session_key);
  }
  agent_id = routing::normalize_agent_id(agent_id);

  std::string session_key = trim_to_opt(params.session_key).value_or("");

  // storePath: 显式参数优先，否则按 JSONL 路径解析 (对齐 resolveStorePath)
  std::string store_path;
  bool has_explicit_store = false;
  if (params.store_path) {
    const auto trimmed = trim_to_opt(*params.store_path);
    if (trimmed) {
      store_path = *trimmed;
      has_explicit_store = true;
    }
  }
  if (!has_explicit_store) {
    sessions::SessionFilePathOptions opts;
    opts.agent_id = agent_id;
    store_path = sessions::resolve_session_transcript_path(*session_id, opts);
  }

  RuntimeReadTarget target;
  target.agent_id = std::move(agent_id);
  target.session_id = *session_id;
  target.session_key = std::move(session_key);
  target.store_path = std::move(store_path);
  return core::Result<RuntimeReadTarget>::Ok(std::move(target));
}

// ============================================================================
// JSONL 读取 (对齐 OpenClaw loadTranscriptEvents 的语义等价实现)
// ============================================================================

core::Result<std::vector<json>> load_transcript_events_json(
    const std::string& transcript_path) {
  std::vector<json> events;

  if (!fs::exists(transcript_path)) {
    return core::Result<std::vector<json>>::Err(
        "SessionTranscriptRuntimeError: transcript not found: " + transcript_path);
  }

  std::ifstream ifs(transcript_path);
  if (!ifs.is_open()) {
    return core::Result<std::vector<json>>::Err(
        "SessionTranscriptRuntimeError: cannot open transcript: " + transcript_path);
  }

  std::string line;
  while (std::getline(ifs, line)) {
    if (line.empty()) continue;
    try {
      events.push_back(json::parse(line));
    } catch (const std::exception&) {
      // 跳过无法解析的行 (对齐 OpenClaw 对损坏事件的容错)
      continue;
    }
  }

  return core::Result<std::vector<json>>::Ok(std::move(events));
}

/// 从 message 的 content 数组提取文本 (对齐 OpenClaw 文本提取)
std::string extract_text(const json& message) {
  std::string text;
  if (!message.contains("content") || !message["content"].is_array()) {
    return text;
  }
  for (const auto& item : message["content"]) {
    if (item.value("type", "") == "text") {
      text += item.value("text", "");
    }
  }
  return text;
}

// ============================================================================
// Transcript Tree (严格对齐 OpenClaw config/sessions/transcript-tree.ts)
//   scanSessionTranscriptTree + selectSessionTranscriptTreePathNodes
//   selectVisibleTranscriptEventEntries
// 处理 leaf control / active-branch / reset-boundary / parentless canonical。
// ============================================================================

/// 对齐 readNonEmptyString: trim 后非空返回该值，否则 nullopt
std::optional<std::string> read_non_empty_string(const json& value) {
  if (!value.is_string()) return std::nullopt;
  const std::string s = value.get<std::string>();
  if (trim_to_opt(s)) return s;
  return std::nullopt;
}

/// 对齐 isCanonicalSessionEntryType
bool is_canonical_session_entry_type(const std::string& type) {
  return type == "message" || type == "thinking_level_change" ||
         type == "model_change" || type == "compaction" || type == "reset" ||
         type == "branch_summary" || type == "custom" || type == "custom_message" ||
         type == "label" || type == "session_info";
}

/// 对齐 isCanonicalSessionTranscriptEntry
bool is_canonical_session_transcript_entry(const json& record) {
  return record.is_object() && record.contains("type") &&
         record["type"].is_string() &&
         is_canonical_session_entry_type(record["type"].get<std::string>());
}

/// 对齐 isSessionTranscriptLeafControl
bool is_session_transcript_leaf_control(const json& record);

/// 对齐 SessionTranscriptTreeEntry
struct TreeEntry {
  std::string id;
  std::optional<std::string> parent_id;  // null -> nullopt (显式 null)
  // leafId: undefined(=no_leaf) / null / value —— 用两级 optional 表达
  bool leaf_defined = false;             // false => undefined
  std::optional<std::string> leaf_id;    // leaf_defined=true 时: nullopt=null, value=id
  std::optional<std::string> append_parent_id;  // null -> nullopt
  bool append_mode_side = false;
};

/// 对齐 parseSessionTranscriptTreeEntry
std::optional<TreeEntry> parse_tree_entry(const json& record) {
  if (!record.is_object() || record.value("type", "") == "session" ||
      !record.contains("parentId")) {
    return std::nullopt;
  }
  auto id = read_non_empty_string(record.value("id", json(nullptr)));
  if (!id) return std::nullopt;

  // parentId: null -> nullopt(显式null); 非空字符串 -> value; 其它(空串等) -> undefined(整体失败)
  std::optional<std::string> parent_id;
  bool parent_is_null = record["parentId"].is_null();
  if (!parent_is_null) {
    parent_id = read_non_empty_string(record["parentId"]);
    if (!parent_id) return std::nullopt;  // parentId===undefined 分支
  }

  const std::string type = record.value("type", "");
  if (type == "leaf") {
    // targetId
    bool target_is_null = record.contains("targetId") && record["targetId"].is_null();
    std::optional<std::string> target_id;
    if (!target_is_null) {
      if (record.contains("targetId")) {
        target_id = read_non_empty_string(record["targetId"]);
        if (!target_id) return std::nullopt;
      } else {
        return std::nullopt;  // targetId===undefined
      }
    }
    // appendParentId: 缺失 -> targetId; null -> null; 否则字符串
    std::optional<std::string> append_parent_id;
    bool append_parent_null = false;
    if (!record.contains("appendParentId")) {
      append_parent_id = target_id;
      append_parent_null = target_is_null;
    } else if (record["appendParentId"].is_null()) {
      append_parent_null = true;
    } else {
      append_parent_id = read_non_empty_string(record["appendParentId"]);
      if (!append_parent_id) return std::nullopt;
    }
    // appendMode: 缺失 -> undefined; "side" -> side; 其它 -> null(整体失败)
    bool append_mode_side = false;
    if (record.contains("appendMode")) {
      if (record["appendMode"].is_string() &&
          record["appendMode"].get<std::string>() == "side") {
        append_mode_side = true;
      } else {
        return std::nullopt;  // appendMode===null 分支
      }
    }

    TreeEntry e;
    e.id = *id;
    e.parent_id = target_is_null ? std::nullopt : target_id;
    e.leaf_defined = true;
    e.leaf_id = target_is_null ? std::nullopt : target_id;
    e.append_parent_id = append_parent_null ? std::nullopt : append_parent_id;
    e.append_mode_side = append_mode_side;
    return e;
  }

  // 非 leaf
  const bool side = record.value("appendMode", "") == "side";
  TreeEntry e;
  e.id = *id;
  e.parent_id = parent_is_null ? std::nullopt : parent_id;
  if (is_canonical_session_transcript_entry(record) && !side) {
    e.leaf_defined = true;
    e.leaf_id = *id;
  }
  e.append_parent_id = *id;
  e.append_mode_side = side;
  return e;
}

bool is_session_transcript_leaf_control(const json& record) {
  return record.is_object() && record.value("type", "") == "leaf" &&
         parse_tree_entry(record).has_value();
}

/// 对齐 parseParentlessCanonicalEntry
std::optional<TreeEntry> parse_parentless_canonical_entry(
    const json& record, const std::optional<std::string>& parent_id) {
  if (!is_canonical_session_transcript_entry(record) || record.contains("parentId")) {
    return std::nullopt;
  }
  auto id = read_non_empty_string(record.value("id", json(nullptr)));
  if (!id) return std::nullopt;
  const bool side = record.value("appendMode", "") == "side";
  TreeEntry e;
  e.id = *id;
  e.parent_id = parent_id;
  if (!side) {
    e.leaf_defined = true;
    e.leaf_id = *id;
  }
  e.append_parent_id = *id;
  e.append_mode_side = side;
  return e;
}

struct TreeNode {
  TreeEntry entry_meta;
  json entry;
  size_t index = 0;
};

struct TranscriptTree {
  std::vector<TreeNode> nodes;
  std::map<std::string, size_t> by_id;  // id -> nodes 索引
  std::optional<std::string> leaf_id;
  bool has_leaf_control = false;
  bool has_explicit_leaf_update = false;
};

/// 对齐 resolveCanonicalParentId: 沿 leaf-control 链归一化父节点
std::optional<std::string> resolve_canonical_parent_id(
    const std::optional<std::string>& parent_id, const TranscriptTree& tree) {
  std::set<std::string> seen;
  std::optional<std::string> current = parent_id;
  while (current.has_value()) {
    if (seen.count(*current)) return current;
    seen.insert(*current);
    auto it = tree.by_id.find(*current);
    if (it == tree.by_id.end()) return current;
    const TreeNode& parent = tree.nodes[it->second];
    if (!is_session_transcript_leaf_control(parent.entry)) return current;
    current = parent.entry_meta.parent_id;
  }
  return std::nullopt;
}

/// 对齐 scanSessionTranscriptTree (简化：不含 invalid-leaf-control 修复分支的全部细节，
/// 但覆盖 leaf/side/reset/parentless/canonical-parent 归一化的主路径)
TranscriptTree scan_transcript_tree(const std::vector<json>& entries) {
  TranscriptTree tree;
  std::optional<std::string> leaf_id;
  std::optional<std::string> append_parent_id;
  bool has_leaf_update = false;
  bool has_explicit_leaf_update = false;
  bool has_leaf_control = false;
  std::optional<std::string> latest_reset_id;
  std::set<std::string> reset_descendant_ids;

  for (size_t index = 0; index < entries.size(); ++index) {
    const json& entry = entries[index];
    auto explicit_entry = parse_tree_entry(entry);

    // reset 后 leaf 控制的重定向 (对齐原逻辑主分支)
    if (latest_reset_id.has_value() && leaf_id.has_value() && explicit_entry &&
        explicit_entry->leaf_defined && is_session_transcript_leaf_control(entry) &&
        (!explicit_entry->leaf_id.has_value() ||
         !reset_descendant_ids.count(*explicit_entry->leaf_id))) {
      explicit_entry->parent_id = leaf_id;
      explicit_entry->leaf_id = leaf_id;
      explicit_entry->append_parent_id = leaf_id;
    }

    std::optional<TreeEntry> tree_entry = explicit_entry;
    if (!tree_entry) {
      tree_entry = parse_parentless_canonical_entry(entry, leaf_id);
    }

    if (tree_entry && is_canonical_session_transcript_entry(entry)) {
      const bool canonical_parent_stale =
          explicit_entry.has_value() && tree_entry->parent_id.has_value() &&
          !tree.by_id.count(*tree_entry->parent_id) && leaf_id.has_value();
      const bool crosses_reset_boundary =
          latest_reset_id.has_value() && !tree_entry->append_mode_side &&
          (!tree_entry->parent_id.has_value() ||
           !reset_descendant_ids.count(*tree_entry->parent_id));

      std::optional<std::string> logical_parent_id;
      if (crosses_reset_boundary) {
        logical_parent_id = leaf_id;
      } else if (!tree_entry->append_mode_side && canonical_parent_stale) {
        logical_parent_id = leaf_id;
      } else if (explicit_entry && !tree_entry->append_mode_side &&
                 tree_entry->parent_id == append_parent_id &&
                 leaf_id != append_parent_id) {
        logical_parent_id = leaf_id;
      } else {
        logical_parent_id = tree_entry->parent_id;
      }

      auto normalized = resolve_canonical_parent_id(logical_parent_id, tree);
      if (normalized != tree_entry->parent_id) {
        tree_entry->parent_id = normalized;
      }
    }

    if (!tree_entry) continue;

    TreeNode node;
    node.entry_meta = *tree_entry;
    node.entry = entry;
    node.index = index;
    tree.nodes.push_back(std::move(node));
    tree.by_id[tree_entry->id] = tree.nodes.size() - 1;

    if (entry.is_object() && entry.value("type", "") == "reset") {
      latest_reset_id = tree_entry->id;
      reset_descendant_ids.clear();
      reset_descendant_ids.insert(tree_entry->id);
    } else if (latest_reset_id.has_value() && tree_entry->parent_id.has_value() &&
               reset_descendant_ids.count(*tree_entry->parent_id)) {
      reset_descendant_ids.insert(tree_entry->id);
    }

    append_parent_id = tree_entry->append_parent_id;
    if (tree_entry->leaf_defined) {
      leaf_id = tree_entry->leaf_id;
      has_leaf_update = true;
      if (explicit_entry) has_explicit_leaf_update = true;
    }
    if (is_session_transcript_leaf_control(entry)) has_leaf_control = true;
  }
  (void)has_leaf_update;

  tree.leaf_id = leaf_id;
  tree.has_leaf_control = has_leaf_control;
  tree.has_explicit_leaf_update = has_explicit_leaf_update;
  return tree;
}

/// 对齐 selectSessionTranscriptTreePathNodes: 从 leaf 沿 parent 链取活动路径
std::vector<const TreeNode*> select_tree_path_nodes(
    const TranscriptTree& tree, const std::optional<std::string>& leaf_id) {
  std::vector<const TreeNode*> path;
  if (!leaf_id.has_value()) return path;

  std::set<std::string> seen;
  std::optional<std::string> current = leaf_id;
  while (current.has_value()) {
    if (seen.count(*current)) return {};
    seen.insert(*current);
    auto it = tree.by_id.find(*current);
    if (it == tree.by_id.end()) break;
    const TreeNode& node = tree.nodes[it->second];
    if (!is_session_transcript_leaf_control(node.entry)) {
      path.insert(path.begin(), &node);
    }
    current = node.entry_meta.parent_id;
  }
  return path;
}

/// 对齐 selectVisibleTranscriptEventEntries: 返回活动可见分支的 events (含 seq)
struct VisibleEventEntry {
  json event;
  int64_t seq = 0;
};

std::vector<VisibleEventEntry> select_visible_event_entries(
    const std::vector<json>& events) {
  const TranscriptTree tree = scan_transcript_tree(events);
  const auto visible = select_tree_path_nodes(tree, tree.leaf_id);
  std::vector<VisibleEventEntry> out;
  if (!visible.empty()) {
    for (const TreeNode* node : visible) {
      out.push_back({node->entry, static_cast<int64_t>(node->index) + 1});
    }
    return out;
  }
  // hasLeafControl 时 leaf 无有效路径 -> 空; 否则回退线性顺序
  if (tree.has_leaf_control) return {};
  int64_t seq = 0;
  for (const auto& event : events) {
    out.push_back({event, ++seq});
  }
  return out;
}

}  // namespace

// ============================================================================
// Memory Hit Key 工具 (对齐 session-transcript-memory-hit.ts)
// ============================================================================

SessionTranscriptMemoryHitKey format_session_transcript_memory_hit_key(
    const SessionTranscriptMemoryHitKeyParams& params) {
  return "transcript:" + url_encode_component(params.agent_id) + ":" +
         url_encode_component(params.session_id);
}

std::optional<SessionTranscriptMemoryHitIdentity> parse_session_transcript_memory_hit_key(
    const std::string& key) {
  // 格式: transcript:{agentId}:{sessionId}
  const std::string prefix = "transcript:";
  if (key.size() < prefix.size() || key.compare(0, prefix.size(), prefix) != 0) {
    return std::nullopt;
  }

  const std::string rest = key.substr(prefix.size());
  const size_t sep = rest.find(':');
  if (sep == std::string::npos) return std::nullopt;

  const std::string agent_id = url_decode_component(rest.substr(0, sep));
  const std::string session_id = url_decode_component(rest.substr(sep + 1));
  if (agent_id.empty() || session_id.empty()) return std::nullopt;

  SessionTranscriptMemoryHitIdentity identity;
  identity.agent_id = agent_id;
  identity.key = key;
  identity.session_id = session_id;
  return identity;
}

std::vector<std::string> resolve_session_transcript_memory_hit_key_to_session_keys(
    const ResolveSessionTranscriptMemoryHitKeyParams& params) {
  const auto parsed = parse_session_transcript_memory_hit_key(params.key);
  if (!parsed) return {};

  std::vector<std::string> session_keys;
  for (const auto& [session_key, session_id] : params.store) {
    if (session_id == parsed->session_id) {
      session_keys.push_back(session_key);
    }
  }
  return session_keys;
}

// ============================================================================
// 目标解析 (对齐 session-transcript-runtime.ts)
// ============================================================================

core::Result<SessionTranscriptIdentity> resolve_session_transcript_identity(
    const SessionTranscriptReadParams& params) {
  SessionTranscriptIdentity identity;
  identity.agent_id = trim_to_opt(params.agent_id.value_or("")).value_or("");
  identity.memory_key = format_session_transcript_memory_hit_key(
      {identity.agent_id, params.session_id});
  identity.session_id = params.session_id;
  identity.session_key = trim_to_opt(params.session_key).value_or("");
  return core::Result<SessionTranscriptIdentity>::Ok(std::move(identity));
}

core::Result<SessionTranscriptTarget> resolve_session_transcript_target(
    const SessionTranscriptReadParams& params) {
  auto target_result = resolve_read_target(params);
  if (!target_result.ok) {
    return core::Result<SessionTranscriptTarget>::Err(target_result.error);
  }
  const auto& target = target_result.value.value();

  SessionTranscriptTarget out;
  out.target_kind = "runtime-session";
  out.agent_id = target.agent_id;
  out.memory_key = format_session_transcript_memory_hit_key(
      {target.agent_id, target.session_id});
  out.session_id = target.session_id;
  out.session_key = target.session_key;
  return core::Result<SessionTranscriptTarget>::Ok(std::move(out));
}

// ============================================================================
// 读取 API (对齐 session-transcript-runtime.ts)
// ============================================================================

core::Result<std::vector<SessionTranscriptEvent>> read_session_transcript_events(
    const SessionTranscriptReadParams& params) {
  auto target_result = resolve_read_target(params);
  if (!target_result.ok) {
    return core::Result<std::vector<SessionTranscriptEvent>>::Err(target_result.error);
  }

  auto events_result = load_transcript_events_json(target_result.value->store_path);
  if (!events_result.ok) {
    return core::Result<std::vector<SessionTranscriptEvent>>::Err(events_result.error);
  }

  std::vector<SessionTranscriptEvent> events;
  events.reserve(events_result.value->size());
  for (const auto& e : *events_result.value) {
    events.emplace_back(e);
  }
  return core::Result<std::vector<SessionTranscriptEvent>>::Ok(std::move(events));
}

core::Result<std::vector<SessionTranscriptMessageEntry>>
read_visible_session_transcript_message_entries(
    const SessionTranscriptReadParams& params) {
  auto target_result = resolve_read_target(params);
  if (!target_result.ok) {
    return core::Result<std::vector<SessionTranscriptMessageEntry>>::Err(
        target_result.error);
  }

  auto events_result = load_transcript_events_json(target_result.value->store_path);
  if (!events_result.ok) {
    return core::Result<std::vector<SessionTranscriptMessageEntry>>::Err(
        events_result.error);
  }

  // 对齐 OpenClaw: 先做 active-branch 可见性投影 (selectVisibleTranscriptEventEntries)，
  // 再筛 role in {user, assistant, tool} (projectVisibleMessageEntry)。
  const auto visible = select_visible_event_entries(*events_result.value);
  std::vector<SessionTranscriptMessageEntry> entries;
  for (const auto& ve : visible) {
    const json& event = ve.event;
    if (event.value("type", "") != "message" || !event.contains("message")) {
      continue;
    }
    const json& message = event["message"];
    const std::string role = message.value("role", "");
    if (role != "user" && role != "assistant" && role != "tool") {
      continue;
    }

    SessionTranscriptMessageEntry entry;
    entry.entry_id = event.value("id", "");
    if (event.contains("parentId") && event["parentId"].is_string()) {
      entry.parent_id = event["parentId"].get<std::string>();
    }
    entry.seq = ve.seq;
    entry.message = core::JsonValue(message);
    entry.role = role;
    if (message.contains("timestamp")) {
      entry.created_at = std::to_string(
          static_cast<int64_t>(message["timestamp"].get<double>()));
    }
    if (message.contains("idempotencyKey") && message["idempotencyKey"].is_string()) {
      entry.idempotency_key = message["idempotencyKey"].get<std::string>();
    }
    entries.push_back(std::move(entry));
  }

  return core::Result<std::vector<SessionTranscriptMessageEntry>>::Ok(
      std::move(entries));
}

core::Result<std::optional<LatestTranscriptAssistantText>>
read_latest_assistant_text_by_identity(
    const SessionTranscriptReadParams& params) {
  auto target_result = resolve_read_target(params);
  if (!target_result.ok) {
    return core::Result<std::optional<LatestTranscriptAssistantText>>::Err(
        target_result.error);
  }

  auto events_result = load_transcript_events_json(target_result.value->store_path);
  if (!events_result.ok) {
    return core::Result<std::optional<LatestTranscriptAssistantText>>::Err(
        events_result.error);
  }

  // 从可见分支末尾向前找最新一条 assistant message (对齐 OpenClaw)
  const auto visible = select_visible_event_entries(*events_result.value);
  for (auto it = visible.rbegin(); it != visible.rend(); ++it) {
    const json& event = it->event;
    if (event.value("type", "") != "message" || !event.contains("message")) {
      continue;
    }
    const json& message = event["message"];
    if (message.value("role", "") != "assistant") {
      continue;
    }

    const std::string text = extract_text(message);
    if (text.empty()) {
      continue;
    }

    LatestTranscriptAssistantText out;
    out.text = text;
    out.entry_id = event.value("id", "");
    if (message.contains("id") && message["id"].is_string()) {
      out.message_id = message["id"].get<std::string>();
    }
    if (message.contains("timestamp")) {
      out.updated_at = std::to_string(
          static_cast<int64_t>(message["timestamp"].get<double>()));
    }
    return core::Result<std::optional<LatestTranscriptAssistantText>>::Ok(
        std::make_optional(std::move(out)));
  }

  return core::Result<std::optional<LatestTranscriptAssistantText>>::Ok(std::nullopt);
}

}  // namespace plugins
}  // namespace hunterclaw
