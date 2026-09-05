// 临时验证：relay 严格对齐后的核心逻辑
// 复刻 acp_spawn.cc 中的纯函数与 append_visible_progress / item 快照算法进行断言。
#include <algorithm>
#include <cstdio>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

using json = nlohmann::json;

static int g_fail = 0;
static void check(bool ok, const char* name, const std::string& detail = "") {
    std::printf("%-62s %s\n", name, ok ? "PASS" : "FAIL");
    if (!ok) {
        if (!detail.empty()) std::printf("     %s\n", detail.c_str());
        ++g_fail;
    }
}

// ─── 复刻被测逻辑（与 acp_spawn.cc 保持一致）────────────────────────────────
const std::map<std::string, bool>& acp_tag_visibility_defaults() {
    static const std::map<std::string, bool> kDefaults = {
        {"agent_message_chunk", true},       {"tool_call", false},
        {"tool_call_update", false},         {"usage_update", false},
        {"available_commands_update", false},{"current_mode_update", false},
        {"config_option_update", false},     {"session_info_update", false},
        {"plan", false},                     {"agent_thought_chunk", false},
    };
    return kDefaults;
}
bool is_acp_session_update_tag(const std::string& tag) {
    return acp_tag_visibility_defaults().count(tag) > 0;
}
struct RelayProjectionSettings {
    std::map<std::string, bool> tag_visibility;
};
RelayProjectionSettings resolve_relay_projection_settings(const std::optional<json>& cfg) {
    RelayProjectionSettings s;
    if (!cfg.has_value() || !cfg->is_object()) return s;
    const json acp = cfg->value("acp", json::object());
    if (!acp.is_object()) return s;
    const json projection = acp.value("projection", json::object());
    if (!projection.is_object()) return s;
    const json tv = projection.value("tagVisibility", json::object());
    if (!tv.is_object()) return s;
    for (auto it = tv.begin(); it != tv.end(); ++it) {
        if (it.value().is_boolean()) s.tag_visibility[it.key()] = it.value().get<bool>();
    }
    return s;
}
bool is_acp_tag_visible(const RelayProjectionSettings& s, const std::string& tag) {
    if (tag.empty()) return true;
    if (!is_acp_session_update_tag(tag)) return true;
    auto ov = s.tag_visibility.find(tag);
    if (ov != s.tag_visibility.end()) return ov->second;
    const auto& d = acp_tag_visibility_defaults();
    auto di = d.find(tag);
    return di != d.end() ? di->second : false;
}
bool should_relay_acp_status_progress(const std::string& et, const std::string& tag,
                                     const std::string& text,
                                     const RelayProjectionSettings& s) {
    if (et != "status" || text.empty()) return false;
    return is_acp_tag_visible(s, tag);
}
bool resolve_parent_progress_commentary(const std::optional<json>& cfg) {
    if (!cfg.has_value() || !cfg->is_object()) return true;
    const json st = cfg->value("streaming", json::object());
    if (st.is_object()) {
        const auto it = st.find("progressCommentary");
        if (it != st.end() && it->is_boolean()) return it->get<bool>();
    }
    return true;
}
std::string slice_tail_utf8_safe(const std::string& t, size_t max_bytes) {
    if (t.size() <= max_bytes) return t;
    size_t start = t.size() - max_bytes;
    while (start < t.size() && (static_cast<unsigned char>(t[start]) & 0xC0) == 0x80) ++start;
    return t.substr(start);
}
std::string format_proxy_env_summary(const std::vector<std::string>& keys) {
    if (keys.empty()) return "no proxy env";
    std::string out = "proxy env: ";
    for (size_t i = 0; i < keys.size(); ++i) {
        if (i > 0) out += ", ";
        out += keys[i];
    }
    return out;
}

// append_visible_progress + item 快照的行为模型
static constexpr size_t SNIPPET_MAX = 220;
static constexpr size_t BUFFER_MAX = 4000;
struct Harness {
    std::string pending_text;
    std::string pending_kind;
    std::vector<std::string> flushed;
    std::map<std::string, std::string> item_text_by_id;
    int64_t first_visible_output_at = 0;
    bool stall_notified = false;
    int resumed_emits = 0;

    void flush_pending() {
        if (pending_text.empty()) {
            pending_kind.clear();
            return;
        }
        flushed.push_back(pending_text);
        pending_text.clear();
        pending_kind.clear();
    }
    void append_visible_progress(const std::string& delta, const std::string& kind) {
        if (stall_notified) {
            stall_notified = false;
            ++resumed_emits;
        }
        if (first_visible_output_at == 0) first_visible_output_at = 1;
        if (!pending_text.empty() && !pending_kind.empty() && pending_kind != kind) {
            flush_pending();
        }
        pending_kind = kind;
        pending_text += delta;
        if (pending_text.size() > BUFFER_MAX) {
            pending_text = slice_tail_utf8_safe(pending_text, BUFFER_MAX);
        }
        if (pending_text.size() >= SNIPPET_MAX || delta.find("\n\n") != std::string::npos) {
            flush_pending();
        }
    }
    void append_item_progress_snapshot(const std::string& id, const std::string& text) {
        std::string previous;
        auto it = item_text_by_id.find(id);
        if (it != item_text_by_id.end()) previous = it->second;
        if (text == previous) return;
        const std::string kind = "item:" + id;
        const bool is_prefix = !previous.empty() && text.rfind(previous, 0) == 0;
        if (!previous.empty() && !is_prefix) {
            if (pending_kind == kind && !pending_text.empty()) pending_text.clear();
        }
        item_text_by_id[id] = text;
        const std::string delta = is_prefix ? text.substr(previous.size()) : text;
        if (delta.empty()) return;
        append_visible_progress(delta, kind);
    }
};

int main() {
    // ── tag 可见性（对齐 ACP_TAG_VISIBILITY_DEFAULTS）──────────────────────
    {
        RelayProjectionSettings empty;
        check(is_acp_tag_visible(empty, ""), "T01 no tag -> visible");
        check(is_acp_tag_visible(empty, "agent_message_chunk"),
              "T02 agent_message_chunk -> visible (default true)");
        check(!is_acp_tag_visible(empty, "agent_thought_chunk"),
              "T03 agent_thought_chunk -> hidden (default false)");
        check(!is_acp_tag_visible(empty, "tool_call"), "T04 tool_call -> hidden");
        check(!is_acp_tag_visible(empty, "plan"), "T05 plan -> hidden");
        check(is_acp_tag_visible(empty, "some_unknown_kind"),
              "T06 non-session-update tag -> visible");
    }
    // ── tagVisibility 覆盖 ────────────────────────────────────────────────
    {
        json cfg = {{"acp", {{"projection", {{"tagVisibility", {{"agent_thought_chunk", true},
                                                                {"agent_message_chunk", false}}}}}}}};
        auto s = resolve_relay_projection_settings(cfg);
        check(is_acp_tag_visible(s, "agent_thought_chunk"),
              "T07 override true beats default false");
        check(!is_acp_tag_visible(s, "agent_message_chunk"),
              "T08 override false beats default true");
        check(!is_acp_tag_visible(s, "tool_call"),
              "T09 unlisted tag still uses default");
    }
    // ── should_relay_acp_status_progress ──────────────────────────────────
    {
        RelayProjectionSettings s;
        check(should_relay_acp_status_progress("status", "", "working", s),
              "T10 status + text + no tag -> relay");
        check(!should_relay_acp_status_progress("text_delta", "", "x", s),
              "T11 non-status eventType -> no relay");
        check(!should_relay_acp_status_progress("status", "", "", s),
              "T12 empty text -> no relay");
        check(!should_relay_acp_status_progress("status", "agent_thought_chunk", "t", s),
              "T13 hidden tag -> no relay");
        check(should_relay_acp_status_progress("status", "agent_message_chunk", "t", s),
              "T14 visible tag -> relay");
    }
    // ── progressCommentary 默认与覆盖 ─────────────────────────────────────
    {
        check(resolve_parent_progress_commentary(std::nullopt),
              "T15 no cfg -> commentary defaults true");
        check(resolve_parent_progress_commentary(json{{"streaming", json::object()}}),
              "T16 cfg without flag -> true");
        check(!resolve_parent_progress_commentary(
                  json{{"streaming", {{"progressCommentary", false}}}}),
              "T17 explicit false honored");
    }
    // ── kind 隔离：不同来源不得拼接 ───────────────────────────────────────
    {
        Harness h;
        h.append_visible_progress("aaa", "assistant:answer");
        h.append_visible_progress("bbb", "acp:status");
        bool ok = h.flushed.size() == 1 && h.flushed[0] == "aaa" && h.pending_text == "bbb";
        check(ok, "T18 kind switch flushes previous kind first",
              "flushed=" + std::to_string(h.flushed.size())
                  + " pending=" + h.pending_text);
    }
    {
        Harness h;
        h.append_visible_progress("aa", "assistant:answer");
        h.append_visible_progress("bb", "assistant:answer");
        check(h.flushed.empty() && h.pending_text == "aabb",
              "T19 same kind accumulates without flush", "pending=" + h.pending_text);
    }
    // ── 段落分隔立即 flush ────────────────────────────────────────────────
    {
        Harness h;
        h.append_visible_progress("Reading file\n\n", "acp:status");
        check(h.flushed.size() == 1 && h.pending_text.empty(),
              "T20 delta with blank line flushes immediately",
              "flushed=" + std::to_string(h.flushed.size()));
    }
    // ── snippet 阈值触发 flush ────────────────────────────────────────────
    {
        Harness h;
        h.append_visible_progress(std::string(SNIPPET_MAX, 'x'), "assistant:answer");
        check(h.flushed.size() == 1 && h.pending_text.empty(),
              "T21 reaching snippet threshold flushes");
    }
    // ── 缓冲上限 UTF-8 安全截断 ───────────────────────────────────────────
    {
        std::string wide;
        while (wide.size() < BUFFER_MAX + 30) wide += "中";  // 3 bytes each
        std::string cut = slice_tail_utf8_safe(wide, BUFFER_MAX);
        bool valid = cut.size() <= BUFFER_MAX && (cut.size() % 3 == 0);
        check(valid, "T22 buffer tail slice keeps UTF-8 boundary",
              "size=" + std::to_string(cut.size()));
    }
    // ── item 快照：前缀扩展只投增量 ───────────────────────────────────────
    {
        Harness h;
        h.append_item_progress_snapshot("i1", "Analyzing");
        h.append_item_progress_snapshot("i1", "Analyzing login flow");
        check(h.pending_text == "Analyzing login flow",
              "T23 item prefix update relays only the delta",
              "pending=" + h.pending_text);
    }
    // ── item 快照：相同文本丢弃 ───────────────────────────────────────────
    {
        Harness h;
        h.append_item_progress_snapshot("i1", "Same");
        const size_t before = h.pending_text.size();
        h.append_item_progress_snapshot("i1", "Same");
        check(h.pending_text.size() == before,
              "T24 identical item snapshot is dropped");
    }
    // ── item 快照：被重写时丢弃同 kind 未 flush 内容 ──────────────────────
    {
        Harness h;
        h.append_item_progress_snapshot("i1", "Old text");
        h.append_item_progress_snapshot("i1", "Totally new");
        check(h.pending_text == "Totally new",
              "T25 rewritten item snapshot replaces pending, no duplication",
              "pending=" + h.pending_text);
    }
    // ── stall 恢复只通知一次 ──────────────────────────────────────────────
    {
        Harness h;
        h.stall_notified = true;
        h.append_visible_progress("a", "assistant:answer");
        h.append_visible_progress("b", "assistant:answer");
        check(h.resumed_emits == 1 && !h.stall_notified,
              "T26 resume notice emitted exactly once",
              "resumed=" + std::to_string(h.resumed_emits));
    }
    // ── firstVisibleOutputAt 只记一次，且仅由可见输出触发 ─────────────────
    {
        Harness h;
        check(h.first_visible_output_at == 0,
              "T27 no visible output before any progress");
        h.append_visible_progress("x", "acp:status");
        check(h.first_visible_output_at == 1,
              "T28 first visible output recorded on relay");
    }
    // ── proxy env 摘要 ────────────────────────────────────────────────────
    {
        check(format_proxy_env_summary({}) == "no proxy env",
              "T29 empty proxy keys -> 'no proxy env'");
        check(format_proxy_env_summary({"HTTPS_PROXY", "ALL_PROXY"})
                  == "proxy env: HTTPS_PROXY, ALL_PROXY",
              "T30 proxy keys joined");
    }

    std::printf("\n%s (%d failure%s)\n", g_fail == 0 ? "ALL PASS" : "FAILED", g_fail,
                g_fail == 1 ? "" : "s");
    return g_fail == 0 ? 0 : 1;
}
