// ============================================================================
// remote_a2a/types.cc
// Remote A2A 协议数据模型实现。纯函数 + JSON 编解码，零 IO、零线程。
// ============================================================================

#include "hunterclaw/remote_a2a/types.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <utility>

namespace hunterclaw {
namespace remote_a2a {

namespace {

std::string trim_copy(std::string_view s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return std::string(s.substr(b, e - b));
}

std::string to_lower_copy(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

/// 安全读字符串字段：非字符串一律返回默认值（远端数据不可信）
std::string jstr(const nlohmann::json& j, const char* key,
                 const std::string& def = std::string()) {
    if (!j.is_object()) return def;
    auto it = j.find(key);
    if (it == j.end() || !it->is_string()) return def;
    return it->get<std::string>();
}

bool jbool(const nlohmann::json& j, const char* key, bool def) {
    if (!j.is_object()) return def;
    auto it = j.find(key);
    if (it == j.end() || !it->is_boolean()) return def;
    return it->get<bool>();
}

const nlohmann::json* jobj(const nlohmann::json& j, const char* key) {
    if (!j.is_object()) return nullptr;
    auto it = j.find(key);
    if (it == j.end() || !it->is_object()) return nullptr;
    return &(*it);
}

const nlohmann::json* jarr(const nlohmann::json& j, const char* key) {
    if (!j.is_object()) return nullptr;
    auto it = j.find(key);
    if (it == j.end() || !it->is_array()) return nullptr;
    return &(*it);
}

/// 兼容 camelCase 与 snake_case 两种键名（不同实现混用）
std::string jstr2(const nlohmann::json& j, const char* camel, const char* snake,
                  const std::string& def = std::string()) {
    std::string v = jstr(j, camel);
    if (!v.empty()) return v;
    v = jstr(j, snake);
    return v.empty() ? def : v;
}

const nlohmann::json* jobj2(const nlohmann::json& j, const char* camel,
                            const char* snake) {
    if (auto* p = jobj(j, camel)) return p;
    return jobj(j, snake);
}

}  // namespace

// ── Part 构造 ───────────────────────────────────────────────────────────────

Part Part::make_text(std::string t, nlohmann::json meta) {
    Part p;
    p.kind = PartKind::Text;
    p.text = std::move(t);
    if (!meta.is_null()) p.metadata = std::move(meta);
    return p;
}

Part Part::make_data(nlohmann::json d, nlohmann::json meta) {
    Part p;
    p.kind = PartKind::Data;
    p.data = std::move(d);
    if (!meta.is_null()) p.metadata = std::move(meta);
    return p;
}

Part Part::make_file(FileRef f, nlohmann::json meta) {
    Part p;
    p.kind = PartKind::File;
    p.file = std::move(f);
    if (!meta.is_null()) p.metadata = std::move(meta);
    return p;
}

// ── Role ────────────────────────────────────────────────────────────────────

const char* role_to_wire(Role r) {
    return r == Role::Agent ? "agent" : "user";
}

std::optional<Role> role_from_wire(std::string_view w) {
    const std::string k = to_lower_copy(trim_copy(w));
    if (k == "user" || k == "role_user") return Role::User;
    if (k == "agent" || k == "role_agent") return Role::Agent;
    return std::nullopt;
}

// ── TaskState ───────────────────────────────────────────────────────────────

namespace {
struct WireEntry {
    TaskState state;
    const char* wire;
};
// 顺序无关（查表用），但必须覆盖全部枚举值
constexpr WireEntry kWireTable[] = {
    {TaskState::Submitted, "submitted"},
    {TaskState::Working, "working"},
    {TaskState::Completed, "completed"},
    {TaskState::Failed, "failed"},
    {TaskState::Canceled, "canceled"},
    {TaskState::InputRequired, "input-required"},
    {TaskState::AuthRequired, "auth-required"},
};
}  // namespace

bool is_terminal(TaskState s) {
    switch (s) {
        case TaskState::Failed:
        case TaskState::AuthRequired:
        case TaskState::InputRequired:
        case TaskState::Canceled:
        case TaskState::Completed:
            return true;
        case TaskState::Working:
        case TaskState::Submitted:
            return false;
    }
    return false;
}

const char* task_state_to_wire(TaskState s) {
    for (const auto& e : kWireTable) {
        if (e.state == s) return e.wire;
    }
    return "working";  // 不可达（枚举已全覆盖）
}

std::optional<TaskState> task_state_from_wire(std::string_view w) {
    std::string k = to_lower_copy(trim_copy(w));
    if (k.empty()) return std::nullopt;

    // 下划线归一化为连字符，兼容 "input_required"
    std::replace(k.begin(), k.end(), '_', '-');

    // 剥离 protobuf 前缀 "task-state-"（原 TASK_STATE_WORKING）
    static const char kProtoPrefix[] = "task-state-";
    if (k.rfind(kProtoPrefix, 0) == 0) {
        k = k.substr(std::strlen(kProtoPrefix));
    }

    for (const auto& e : kWireTable) {
        if (k == e.wire) return e.state;
    }
    // ★ 未知值显式失败。绝不兜底为 Working —— 那会把远端 failed 当成还在跑。
    return std::nullopt;
}

// ── 序列化：Part / Message ──────────────────────────────────────────────────

nlohmann::json to_json(const Part& p) {
    nlohmann::json j = nlohmann::json::object();
    switch (p.kind) {
        case PartKind::Text:
            j["kind"] = "text";
            j["text"] = p.text;
            break;
        case PartKind::Data:
            j["kind"] = "data";
            j["data"] = p.data.is_null() ? nlohmann::json::object() : p.data;
            break;
        case PartKind::File: {
            j["kind"] = "file";
            nlohmann::json f = nlohmann::json::object();
            if (!p.file.uri.empty()) {
                f["uri"] = p.file.uri;
            } else {
                f["bytes"] = p.file.bytes_base64;
            }
            if (!p.file.mime_type.empty()) f["mimeType"] = p.file.mime_type;
            if (!p.file.name.empty()) f["name"] = p.file.name;
            j["file"] = std::move(f);
            break;
        }
    }
    if (!p.metadata.is_null() && !p.metadata.empty()) j["metadata"] = p.metadata;
    return j;
}

nlohmann::json to_json(const Message& m) {
    nlohmann::json j = nlohmann::json::object();
    j["messageId"] = m.message_id;
    j["role"] = role_to_wire(m.role);
    nlohmann::json parts = nlohmann::json::array();
    for (const auto& p : m.parts) parts.push_back(to_json(p));
    j["parts"] = std::move(parts);
    if (!m.task_id.empty()) j["taskId"] = m.task_id;
    if (!m.context_id.empty()) j["contextId"] = m.context_id;
    if (!m.metadata.is_null() && !m.metadata.empty()) j["metadata"] = m.metadata;
    return j;
}

// ── 反序列化 ────────────────────────────────────────────────────────────────

Part part_from_json(const nlohmann::json& j) {
    Part p;
    if (!j.is_object()) return p;

    const std::string kind = to_lower_copy(jstr(j, "kind"));

    // 优先按 kind 判定；kind 缺失时按字段存在性推断（部分实现不发 kind）
    if (kind == "text" || (kind.empty() && j.contains("text"))) {
        p.kind = PartKind::Text;
        p.text = jstr(j, "text");
    } else if (kind == "data" || (kind.empty() && j.contains("data"))) {
        p.kind = PartKind::Data;
        auto it = j.find("data");
        if (it != j.end()) p.data = *it;
    } else if (kind == "file" || (kind.empty() && j.contains("file"))) {
        p.kind = PartKind::File;
        if (const auto* f = jobj(j, "file")) {
            p.file.uri = jstr(*f, "uri");
            p.file.bytes_base64 = jstr(*f, "bytes");
            p.file.mime_type = jstr2(*f, "mimeType", "mime_type");
            p.file.name = jstr(*f, "name");
        }
    } else {
        // 未知 kind → 当作空 Text，不抛异常（远端数据不可信）
        p.kind = PartKind::Text;
    }

    auto mit = j.find("metadata");
    if (mit != j.end() && mit->is_object()) p.metadata = *mit;
    return p;
}

Message message_from_json(const nlohmann::json& j) {
    Message m;
    if (!j.is_object()) return m;

    m.message_id = jstr2(j, "messageId", "message_id");
    if (auto r = role_from_wire(jstr(j, "role"))) m.role = *r;
    m.task_id = jstr2(j, "taskId", "task_id");
    m.context_id = jstr2(j, "contextId", "context_id");

    if (const auto* parts = jarr(j, "parts")) {
        for (const auto& p : *parts) m.parts.push_back(part_from_json(p));
    }
    auto mit = j.find("metadata");
    if (mit != j.end() && mit->is_object()) m.metadata = *mit;
    return m;
}

Artifact artifact_from_json(const nlohmann::json& j) {
    Artifact a;
    if (!j.is_object()) return a;
    a.artifact_id = jstr2(j, "artifactId", "artifact_id");
    a.name = jstr(j, "name");
    if (const auto* parts = jarr(j, "parts")) {
        for (const auto& p : *parts) a.parts.push_back(part_from_json(p));
    }
    return a;
}

Task task_from_json(const nlohmann::json& j) {
    Task t;
    if (!j.is_object()) return t;

    t.id = jstr(j, "id");
    t.context_id = jstr2(j, "contextId", "context_id");

    if (const auto* st = jobj(j, "status")) {
        if (auto s = task_state_from_wire(jstr(*st, "state"))) {
            t.status.state = *s;
        }
        // ★ 保留原串，不规范化 —— ProgressFingerprint 依赖它的逐字节稳定性
        t.status.timestamp = jstr(*st, "timestamp");
        if (const auto* msg = jobj(*st, "message")) {
            Message m = message_from_json(*msg);
            // 1.x protobuf 的 status.message 恒存在但可能全空，归一化为无
            if (!m.parts.empty() || !m.message_id.empty()) {
                t.status.message = std::move(m);
            }
        }
    }

    if (const auto* hist = jarr(j, "history")) {
        for (const auto& h : *hist) t.history.push_back(message_from_json(h));
    }
    if (const auto* arts = jarr(j, "artifacts")) {
        for (const auto& a : *arts) t.artifacts.push_back(artifact_from_json(a));
    }
    auto mit = j.find("metadata");
    if (mit != j.end() && mit->is_object()) t.metadata = *mit;
    return t;
}

// ── 流式事件解析 ────────────────────────────────────────────────────────────

ParsedStreamEvent parse_stream_event(const nlohmann::json& j) {
    ParsedStreamEvent e;
    if (!j.is_object()) return e;

    const bool has_status = jobj(j, "status") != nullptr;
    const bool has_artifact = jobj(j, "artifact") != nullptr;
    const bool has_artifacts_arr = jarr(j, "artifacts") != nullptr;
    const bool has_parts = jarr(j, "parts") != nullptr;

    // 判定顺序：完整 Task > ArtifactUpdate > StatusUpdate > Message
    // 完整 Task 的特征：有 id + status，且带 artifacts/history 数组
    if (has_status && j.contains("id") && (has_artifacts_arr || j.contains("history"))) {
        e.kind = ParsedStreamEvent::Kind::Task;
    } else if (has_artifact) {
        e.kind = ParsedStreamEvent::Kind::ArtifactUpdate;
    } else if (has_status) {
        e.kind = ParsedStreamEvent::Kind::StatusUpdate;
    } else if (has_parts && j.contains("role")) {
        e.kind = ParsedStreamEvent::Kind::Message;
    }

    e.task_id = jstr2(j, "taskId", "task_id");
    if (e.task_id.empty()) e.task_id = jstr(j, "id");
    e.context_id = jstr2(j, "contextId", "context_id");
    // 1.x 移除了 final 字段，缺失即 false
    e.final = jbool(j, "final", false);

    switch (e.kind) {
        case ParsedStreamEvent::Kind::Task: {
            Task t = task_from_json(j);
            e.state = t.status.state;
            e.status_timestamp = t.status.timestamp;
            e.has_status_message = t.status.message.has_value();
            if (t.status.message) {
                e.text = concat_text_parts(t.status.message->parts);
                e.function_calls = extract_function_calls(t.status.message->parts);
            }
            // 完整 Task 的 artifacts 计入指纹
            for (const auto& a : t.artifacts) e.artifact_part_count += a.parts.size();
            if (t.status.state == TaskState::Failed && e.text.empty()) {
                e.error_text = "远端任务失败（未提供原因）";
            } else if (t.status.state == TaskState::Failed) {
                e.error_text = e.text;
            }
            break;
        }
        case ParsedStreamEvent::Kind::StatusUpdate: {
            const auto* st = jobj(j, "status");
            if (st) {
                if (auto s = task_state_from_wire(jstr(*st, "state"))) e.state = *s;
                e.status_timestamp = jstr(*st, "timestamp");
                if (const auto* msg = jobj(*st, "message")) {
                    Message m = message_from_json(*msg);
                    if (!m.parts.empty()) {
                        e.has_status_message = true;
                        e.text = concat_text_parts(m.parts);
                        e.function_calls = extract_function_calls(m.parts);
                    }
                }
            }
            if (e.state == TaskState::Failed || e.state == TaskState::Canceled) {
                e.error_text = e.text.empty()
                                   ? std::string("远端任务") +
                                         (e.state == TaskState::Failed ? "失败" : "被取消")
                                   : e.text;
            }
            break;
        }
        case ParsedStreamEvent::Kind::ArtifactUpdate: {
            const auto* a = jobj(j, "artifact");
            if (a) {
                Artifact art = artifact_from_json(*a);
                e.artifact_id = art.artifact_id;
                e.artifact_part_count = art.parts.size();
                // ★ 只取本次增量的 parts。
                //   ADK 注释（remote_a2a_agent.py:1327-1331）：若转换归一化后的
                //   累积 Task，之前的分片会被重复发出，内容成倍重复。
                e.text = concat_text_parts(art.parts);
            }
            e.last_chunk = jbool(j, "lastChunk", jbool(j, "last_chunk", true));
            // artifact 事件本身不改变 task state，保持 Working
            e.state = TaskState::Working;
            break;
        }
        case ParsedStreamEvent::Kind::Message: {
            Message m = message_from_json(j);
            e.text = concat_text_parts(m.parts);
            e.function_calls = extract_function_calls(m.parts);
            // 独立 Message 视为最终回复
            e.state = TaskState::Completed;
            break;
        }
        case ParsedStreamEvent::Kind::Unknown:
            break;
    }
    return e;
}

// ── 辅助 ────────────────────────────────────────────────────────────────────

std::string concat_text_parts(const std::vector<Part>& parts) {
    std::string out;
    for (const auto& p : parts) {
        if (p.kind != PartKind::Text || p.text.empty()) continue;
        if (!out.empty()) out += "\n";
        out += p.text;
    }
    return out;
}

std::vector<nlohmann::json> extract_function_calls(const std::vector<Part>& parts) {
    std::vector<nlohmann::json> out;
    const std::string type_key = meta_key("type");
    for (const auto& p : parts) {
        if (p.kind != PartKind::Data) continue;
        if (!p.metadata.is_object()) continue;
        auto it = p.metadata.find(type_key);
        if (it == p.metadata.end() || !it->is_string()) continue;
        if (it->get<std::string>() != TYPE_FUNCTION_CALL) continue;
        out.push_back(p.data.is_null() ? nlohmann::json::object() : p.data);
    }
    return out;
}

std::string extract_result_text(const Task& t) {
    std::string out;
    for (const auto& a : t.artifacts) {
        std::string s = concat_text_parts(a.parts);
        if (s.empty()) continue;
        if (!out.empty()) out += "\n";
        out += s;
    }
    // artifacts 为空时回落到 status.message
    if (out.empty() && t.status.message) {
        out = concat_text_parts(t.status.message->parts);
    }
    return out;
}

std::optional<nlohmann::json> parse_sse_data_line(std::string_view line) {
    // 去掉行尾 \r（CRLF）
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
        line.remove_suffix(1);
    }
    if (line.empty()) return std::nullopt;         // 事件分隔空行
    if (line.front() == ':') return std::nullopt;  // 注释/心跳行

    static const char kPrefix[] = "data:";
    if (line.rfind(kPrefix, 0) != 0) return std::nullopt;  // event:/id:/retry: 忽略

    std::string_view payload = line.substr(std::strlen(kPrefix));
    // SSE 规范：data: 后的单个前导空格要去掉
    if (!payload.empty() && payload.front() == ' ') payload.remove_prefix(1);

    const std::string body = trim_copy(payload);
    if (body.empty() || body == "[DONE]") return std::nullopt;

    try {
        return nlohmann::json::parse(body);
    } catch (const std::exception&) {
        return std::nullopt;  // 非法 JSON 静默跳过（远端数据不可信）
    }
}

}  // namespace remote_a2a
}  // namespace hunterclaw
