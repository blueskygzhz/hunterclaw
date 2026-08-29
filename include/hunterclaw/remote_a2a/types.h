#pragma once

// ============================================================================
// remote_a2a/types.h
//
// Remote A2A —— Google Agent2Agent 协议客户端数据模型（跨进程）
//
// ⚠ 与 tools/a2a_context.h + tools/sessions_send_a2a.h 是【两套不同的东西】：
//   ┌────────────┬──────────────────────────┬────────────────────────────┐
//   │            │ tools/ 下的 a2a          │ remote_a2a/（本目录）       │
//   ├────────────┼──────────────────────────┼────────────────────────────┤
//   │ 语义       │ 同进程 session↔session   │ 跨进程 Agent2Agent 协议     │
//   │ 对端寻址   │ session key              │ URL                        │
//   │ 传输       │ GatewayClient RPC        │ HTTP + JSON-RPC / SSE      │
//   │ 能力发现   │ 无                       │ AgentCard (.well-known)    │
//   │ 挂起       │ support tokens           │ input-required             │
//   │ 对端信任   │ 可信（同进程）            │ ★ 不可信（任意网络服务）    │
//   └────────────┴──────────────────────────┴────────────────────────────┘
//
// 本目录代码【不复用】tools/ 下的 a2a_context / sessions_send_a2a 任何逻辑。
//
// ── 序列化方向不对称（刻意设计）──────────────────────────────────────────
// 客户端只【发】Message，只【读】Task。故：
//   · 有 to_json(Part) / to_json(Message)
//   · 无 to_json(Task) —— 客户端从不发 Task，防止有人误把本模块当服务端用
//
// ── 并发原语约束（项目级）────────────────────────────────────────────────
// ✅ std::thread / mutex / condition_variable / atomic
// ❌ std::future / promise / async / packaged_task
// ============================================================================

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace hunterclaw {
namespace remote_a2a {

// ── metadata 命名空间 ────────────────────────────────────────────────────────
// Part 级 metadata 用 "hc_" 前缀（対标 ADK 的 "adk_"），
// session 级 metadata 用 "a2a:" 前缀（対标 ADK 的 A2A_METADATA_PREFIX）。
constexpr const char* META_PREFIX = "hc_";
constexpr const char* SESSION_META_PREFIX = "a2a:";

inline std::string meta_key(std::string_view k) {
    return std::string(META_PREFIX) + std::string(k);
}
inline std::string session_meta_key(std::string_view k) {
    return std::string(SESSION_META_PREFIX) + std::string(k);
}

// hc_type 的取值
constexpr const char* TYPE_FUNCTION_CALL = "function_call";
constexpr const char* TYPE_FUNCTION_RESPONSE = "function_response";
constexpr const char* TYPE_TOOL_RESULT = "tool_result";

// ── Part ────────────────────────────────────────────────────────────────────

enum class PartKind { Text, File, Data };

struct FileRef {
    std::string uri;           // 与 bytes_base64 二选一
    std::string bytes_base64;
    std::string mime_type;
    std::string name;
};

struct Part {
    PartKind kind = PartKind::Text;
    std::string text;               // kind == Text
    FileRef file;                   // kind == File
    nlohmann::json data;            // kind == Data
    /// hc_type / is_user_input 等
    nlohmann::json metadata;

    static Part make_text(std::string t, nlohmann::json meta = nullptr);
    static Part make_data(nlohmann::json d, nlohmann::json meta = nullptr);
    static Part make_file(FileRef f, nlohmann::json meta = nullptr);
};

// ── Message ─────────────────────────────────────────────────────────────────

enum class Role { User, Agent };

const char* role_to_wire(Role);
std::optional<Role> role_from_wire(std::string_view);

struct Message {
    std::string message_id;
    Role role = Role::User;
    std::vector<Part> parts;
    std::string task_id;
    std::string context_id;
    nlohmann::json metadata;
};

// ── Task ────────────────────────────────────────────────────────────────────

/// ⚠ 枚举顺序即归并优先级（Failed 最高）。
///   task_tracking.h 的 classify() 依赖这个顺序，改顺序会破坏优先级判定。
enum class TaskState {
    Failed = 0,
    AuthRequired,
    InputRequired,
    Canceled,
    Completed,
    Working,
    Submitted,
};

/// 终态集合：Failed / AuthRequired / InputRequired / Canceled / Completed。
/// ⚠ InputRequired / AuthRequired 也是终态 —— 它们是「挂起」，本轮结束，
///   等外部回填后由新的 message 续接同一个 task。
bool is_terminal(TaskState s);

/// 挂起态集合：InputRequired / AuthRequired。
///
/// ★ 「挂起」是终态的【子集】：本轮结束了，但任务没结束 ——
///   等外部回填后由新的 message 续接同一个 task。
///
/// ⚠ 抽成公共函数而非各处内联判断: 这个判定原先散落在
///   ExecutionResult::is_suspended()、ingest_task 的 suspended 局部变量
///   等多处。分散的后果是漏一处就静默错 ——
///   PendingTaskStore::update_terminal 当初就漏了, 导致
///   reason=Completed 把 InputRequired 抹成 Completed。
bool is_suspended_state(TaskState s);

struct TaskStatus {
    TaskState state = TaskState::Submitted;
    std::optional<Message> message;
    /// ISO8601 原串。★ 用于 ProgressFingerprint，不要规范化。
    std::string timestamp;
};

struct Artifact {
    std::string artifact_id;
    std::string name;
    std::vector<Part> parts;
};

struct Task {
    std::string id;
    std::string context_id;
    TaskStatus status;
    std::vector<Message> history;
    std::vector<Artifact> artifacts;
    nlohmann::json metadata;
};

// ── 序列化（方向不对称，见文件头说明）──────────────────────────────────────

nlohmann::json to_json(const Part&);
nlohmann::json to_json(const Message&);

Part part_from_json(const nlohmann::json&);
Message message_from_json(const nlohmann::json&);
Task task_from_json(const nlohmann::json&);
Artifact artifact_from_json(const nlohmann::json&);

// ★ 刻意不提供 to_json(const Task&)

/// wire 格式用连字符："input-required" / "auth-required"
const char* task_state_to_wire(TaskState);

/// ⚠ 返回 optional —— 未知 state 必须【显式失败】，绝不能静默兜底为 Working。
///   兜底为 Working 的后果：远端 failed 被当成还在跑，任务永久挂起。
/// 兼容三种写法：
///   · 连字符  "input-required"（A2A 规范）
///   · 下划线  "input_required"（部分实现）
///   · proto   "TASK_STATE_INPUT_REQUIRED"（a2a-sdk 1.x protobuf）
std::optional<TaskState> task_state_from_wire(std::string_view);

// ── 流式事件（只反序列化）────────────────────────────────────────────────────

struct ParsedStreamEvent {
    enum class Kind { Task, StatusUpdate, ArtifactUpdate, Message, Unknown };

    Kind kind = Kind::Unknown;
    std::string task_id;
    std::string context_id;
    TaskState state = TaskState::Working;

    /// ⚠ 本次增量文本，不是累积值。
    ///   ADK 注释（remote_a2a_agent.py:1327-1331）：若取归一化后的累积 Task，
    ///   之前的分片会被重复发出，导致内容成倍重复。
    std::string text;

    bool last_chunk = true;
    bool final = false;
    std::vector<nlohmann::json> function_calls;
    std::string error_text;

    // ProgressFingerprint 用
    std::string status_timestamp;
    std::string artifact_id;
    size_t artifact_part_count = 0;
    bool has_status_message = false;
};

ParsedStreamEvent parse_stream_event(const nlohmann::json&);

// ── 辅助 ────────────────────────────────────────────────────────────────────

/// 拼接所有 Text part 的文本（用 "\n" 分隔非空项）
std::string concat_text_parts(const std::vector<Part>&);

/// 提取 hc_type == function_call 的 DataPart
std::vector<nlohmann::json> extract_function_calls(const std::vector<Part>&);

/// 从 Task 的 artifacts 提取全部文本（终态取结果用）
std::string extract_result_text(const Task&);

/// 从 "data: {...}" 行提取 JSON。
/// 返回 nullopt 表示：注释行（": ..."）/ 空行 / 非 data 字段 / JSON 非法。
std::optional<nlohmann::json> parse_sse_data_line(std::string_view line);

}  // namespace remote_a2a
}  // namespace hunterclaw
