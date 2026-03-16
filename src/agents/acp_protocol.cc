#include "hunterclaw/agents/acp_protocol.h"
#include "hunterclaw/core/utils.h"
#include <spdlog/spdlog.h>
#include <chrono>

namespace hunterclaw {
namespace agents {

// SessionAcpMeta implementation
core::Result<SessionAcpMeta> SessionAcpMeta::from_json(const core::JsonValue& json) {
    SessionAcpMeta meta;
    
    // ========== 解析 JSON 到结构体 (对齐 OpenClaw ACP 协议) ==========
    
    try {
        // 1. Labels
        if (json.contains("labels") && json["labels"].is_object()) {
            for (const auto& [key, value] : json["labels"].items()) {
                if (value.is_string()) {
                    meta.labels.labels[key] = value.get<std::string>();
                }
            }
        }
        
        // 2. Scope
        if (json.contains("scope") && json["scope"].is_object()) {
            const auto& scope = json["scope"];
            
            if (scope.contains("isolated_workspace") && scope["isolated_workspace"].is_boolean()) {
                meta.scope.isolated_workspace = scope["isolated_workspace"].get<bool>();
            }
            if (scope.contains("isolated_memory") && scope["isolated_memory"].is_boolean()) {
                meta.scope.isolated_memory = scope["isolated_memory"].get<bool>();
            }
            if (scope.contains("isolated_tools") && scope["isolated_tools"].is_boolean()) {
                meta.scope.isolated_tools = scope["isolated_tools"].get<bool>();
            }
            
            if (scope.contains("workspace_override") && scope["workspace_override"].is_string()) {
                meta.scope.workspace_override = scope["workspace_override"].get<std::string>();
            }
            if (scope.contains("memory_override") && scope["memory_override"].is_string()) {
                meta.scope.memory_override = scope["memory_override"].get<std::string>();
            }
        }
        
        // 3. Lifecycle
        if (json.contains("lifecycle") && json["lifecycle"].is_object()) {
            const auto& lifecycle = json["lifecycle"];
            
            if (lifecycle.contains("on_start") && lifecycle["on_start"].is_string()) {
                meta.lifecycle.on_start = lifecycle["on_start"].get<std::string>();
            }
            if (lifecycle.contains("on_message") && lifecycle["on_message"].is_string()) {
                meta.lifecycle.on_message = lifecycle["on_message"].get<std::string>();
            }
            if (lifecycle.contains("on_end") && lifecycle["on_end"].is_string()) {
                meta.lifecycle.on_end = lifecycle["on_end"].get<std::string>();
            }
        }
        
        // 4. Limits
        if (json.contains("limits") && json["limits"].is_object()) {
            const auto& limits = json["limits"];
            
            if (limits.contains("max_memory_mb") && limits["max_memory_mb"].is_number()) {
                meta.limits.max_memory_mb = limits["max_memory_mb"].get<size_t>();
            }
            if (limits.contains("max_disk_mb") && limits["max_disk_mb"].is_number()) {
                meta.limits.max_disk_mb = limits["max_disk_mb"].get<size_t>();
            }
            if (limits.contains("max_concurrent_runs") && limits["max_concurrent_runs"].is_number()) {
                meta.limits.max_concurrent_runs = limits["max_concurrent_runs"].get<int>();
            }
            if (limits.contains("max_turns_per_run") && limits["max_turns_per_run"].is_number()) {
                meta.limits.max_turns_per_run = limits["max_turns_per_run"].get<int>();
            }
            if (limits.contains("timeout_seconds") && limits["timeout_seconds"].is_number()) {
                meta.limits.timeout_seconds = limits["timeout_seconds"].get<int>();
            }
        }
        
        // 5. Runtime
        if (json.contains("runtime") && json["runtime"].is_object()) {
            const auto& runtime = json["runtime"];
            
            if (runtime.contains("model") && runtime["model"].is_string()) {
                meta.runtime.model = runtime["model"].get<std::string>();
            }
            if (runtime.contains("max_turns") && runtime["max_turns"].is_number()) {
                meta.runtime.max_turns = runtime["max_turns"].get<int>();
            }
            if (runtime.contains("temperature") && runtime["temperature"].is_number()) {
                meta.runtime.temperature = runtime["temperature"].get<double>();
            }
            
            if (runtime.contains("env_vars") && runtime["env_vars"].is_object()) {
                for (const auto& [key, value] : runtime["env_vars"].items()) {
                    if (value.is_string()) {
                        meta.runtime.env_vars[key] = value.get<std::string>();
                    }
                }
            }
            
            if (runtime.contains("enabled_tools") && runtime["enabled_tools"].is_array()) {
                for (const auto& tool : runtime["enabled_tools"]) {
                    if (tool.is_string()) {
                        meta.runtime.enabled_tools.push_back(tool.get<std::string>());
                    }
                }
            }
            
            if (runtime.contains("context") && runtime["context"].is_object()) {
                for (const auto& [key, value] : runtime["context"].items()) {
                    meta.runtime.context[key] = value;  // Store as JsonValue
                }
            }
        }
        
        // 6. Metadata
        if (json.contains("metadata") && json["metadata"].is_object()) {
            for (const auto& [key, value] : json["metadata"].items()) {
                meta.metadata[key] = value;  // Store as JsonValue
            }
        }
        
        return core::Result<SessionAcpMeta>::ok(meta);
        
    } catch (const std::exception& e) {
        return core::Result<SessionAcpMeta>::err(std::string("JSON parse error: ") + e.what());
    }
}

core::JsonValue SessionAcpMeta::to_json() const {
    core::JsonValue json = core::JsonValue::object();
    
    // Labels
    if (!labels.labels.empty()) {
        core::JsonValue labels_json = core::JsonValue::object();
        for (const auto& [key, label] : labels.labels) {
            labels_json[key] = label;
        }
        json["labels"] = labels_json;
    }
    
    // Scope
    {
        core::JsonValue scope_json = core::JsonValue::object();
        scope_json["isolated_workspace"] = scope.isolated_workspace;
        scope_json["isolated_memory"] = scope.isolated_memory;
        scope_json["isolated_tools"] = scope.isolated_tools;
        
        if (scope.workspace_override) {
            scope_json["workspace_override"] = *scope.workspace_override;
        }
        if (scope.memory_override) {
            scope_json["memory_override"] = *scope.memory_override;
        }
        
        json["scope"] = scope_json;
    }
    
    // Lifecycle
    {
        core::JsonValue lifecycle_json = core::JsonValue::object();
        if (lifecycle.on_start) {
            lifecycle_json["on_start"] = *lifecycle.on_start;
        }
        if (lifecycle.on_message) {
            lifecycle_json["on_message"] = *lifecycle.on_message;
        }
        if (lifecycle.on_end) {
            lifecycle_json["on_end"] = *lifecycle.on_end;
        }
        
        if (!lifecycle_json.empty()) {
            json["lifecycle"] = lifecycle_json;
        }
    }
    
    // Limits
    {
        core::JsonValue limits_json = core::JsonValue::object();
        if (limits.max_memory_mb) {
            limits_json["max_memory_mb"] = static_cast<int>(*limits.max_memory_mb);
        }
        if (limits.max_disk_mb) {
            limits_json["max_disk_mb"] = static_cast<int>(*limits.max_disk_mb);
        }
        if (limits.max_concurrent_runs) {
            limits_json["max_concurrent_runs"] = *limits.max_concurrent_runs;
        }
        if (limits.max_turns_per_run) {
            limits_json["max_turns_per_run"] = *limits.max_turns_per_run;
        }
        if (limits.timeout_seconds) {
            limits_json["timeout_seconds"] = *limits.timeout_seconds;
        }
        
        if (!limits_json.empty()) {
            json["limits"] = limits_json;
        }
    }
    
    // Runtime
    {
        core::JsonValue runtime_json = core::JsonValue::object();
        if (runtime.model) {
            runtime_json["model"] = *runtime.model;
        }
        if (runtime.max_turns) {
            runtime_json["max_turns"] = *runtime.max_turns;
        }
        if (runtime.temperature) {
            runtime_json["temperature"] = *runtime.temperature;
        }
        
        if (!runtime.env_vars.empty()) {
            core::JsonValue env_json = core::JsonValue::object();
            for (const auto& [key, value] : runtime.env_vars) {
                env_json[key] = value;
            }
            runtime_json["env_vars"] = env_json;
        }
        
        if (!runtime.enabled_tools.empty()) {
            core::JsonValue tools_json = core::JsonValue::array();
            for (const auto& tool : runtime.enabled_tools) {
                tools_json.push_back(tool);
            }
            runtime_json["enabled_tools"] = tools_json;
        }
        
        if (!runtime.context.empty()) {
            core::JsonValue context_json = core::JsonValue::object();
            for (const auto& [key, value] : runtime.context) {
                context_json[key] = value;
            }
            runtime_json["context"] = context_json;
        }
        
        if (!runtime_json.empty()) {
            json["runtime"] = runtime_json;
        }
    }
    
    // Metadata
    if (!metadata.empty()) {
        core::JsonValue metadata_json = core::JsonValue::object();
        for (const auto& [key, value] : metadata) {
            metadata_json[key] = value;
        }
        json["metadata"] = metadata_json;
    }
    
    return json;
}

void SessionAcpMeta::merge(const SessionAcpMeta& other) {
    // Merge labels
    for (const auto& [key, label] : other.labels.labels) {
        labels.labels[key] = label;
    }
    
    // Merge scope (覆盖)
    scope = other.scope;
    
    // Merge lifecycle (覆盖)
    if (other.lifecycle.on_start) {
        lifecycle.on_start = other.lifecycle.on_start;
    }
    if (other.lifecycle.on_message) {
        lifecycle.on_message = other.lifecycle.on_message;
    }
    if (other.lifecycle.on_end) {
        lifecycle.on_end = other.lifecycle.on_end;
    }
    
    // Merge limits (覆盖)
    if (other.limits.max_memory_mb) {
        limits.max_memory_mb = other.limits.max_memory_mb;
    }
    if (other.limits.max_disk_mb) {
        limits.max_disk_mb = other.limits.max_disk_mb;
    }
    if (other.limits.max_concurrent_runs) {
        limits.max_concurrent_runs = other.limits.max_concurrent_runs;
    }
    if (other.limits.max_turns_per_run) {
        limits.max_turns_per_run = other.limits.max_turns_per_run;
    }
    if (other.limits.timeout_seconds) {
        limits.timeout_seconds = other.limits.timeout_seconds;
    }
    
    // Merge runtime
    if (other.runtime.model) {
        runtime.model = other.runtime.model;
    }
    if (other.runtime.max_turns) {
        runtime.max_turns = other.runtime.max_turns;
    }
    if (other.runtime.temperature) {
        runtime.temperature = other.runtime.temperature;
    }
    
    for (const auto& [key, value] : other.runtime.env_vars) {
        runtime.env_vars[key] = value;
    }
    
    if (!other.runtime.enabled_tools.empty()) {
        runtime.enabled_tools = other.runtime.enabled_tools;
    }
    
    for (const auto& [key, value] : other.runtime.context) {
        runtime.context[key] = value;
    }
    
    // Merge metadata
    for (const auto& [key, value] : other.metadata) {
        metadata[key] = value;
    }
}

// AcpMessage implementation
core::Result<AcpMessage> AcpMessage::from_json(const core::JsonValue& json) {
    AcpMessage msg;
    
    // ========== 解析 JSON 到 AcpMessage (对齐 OpenClaw ACP 协议) ==========
    
    try {
        // 1. 必填字段
        if (!json.contains("message_id") || !json["message_id"].is_string()) {
            return core::Result<AcpMessage>::err("Missing or invalid 'message_id'");
        }
        msg.message_id = json["message_id"].get<std::string>();
        
        if (!json.contains("from_agent_id") || !json["from_agent_id"].is_string()) {
            return core::Result<AcpMessage>::err("Missing or invalid 'from_agent_id'");
        }
        msg.from_agent_id = json["from_agent_id"].get<std::string>();
        
        if (!json.contains("to_agent_id") || !json["to_agent_id"].is_string()) {
            return core::Result<AcpMessage>::err("Missing or invalid 'to_agent_id'");
        }
        msg.to_agent_id = json["to_agent_id"].get<std::string>();
        
        if (!json.contains("content") || !json["content"].is_string()) {
            return core::Result<AcpMessage>::err("Missing or invalid 'content'");
        }
        msg.content = json["content"].get<std::string>();
        
        // 2. 时间戳
        if (json.contains("timestamp_ms") && json["timestamp_ms"].is_number()) {
            msg.timestamp_ms = json["timestamp_ms"].get<int64_t>();
        } else {
            // 默认使用当前时间
            msg.timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()
            ).count();
        }
        
        // 3. Metadata (可选)
        if (json.contains("meta") && json["meta"].is_object()) {
            for (const auto& [key, value] : json["meta"].items()) {
                msg.meta[key] = value;  // Store as JsonValue
            }
        }
        
        return core::Result<AcpMessage>::ok(msg);
        
    } catch (const std::exception& e) {
        return core::Result<AcpMessage>::err(std::string("JSON parse error: ") + e.what());
    }
}

core::JsonValue AcpMessage::to_json() const {
    core::JsonValue json = core::JsonValue::object();
    
    json["message_id"] = message_id;
    json["from_agent_id"] = from_agent_id;
    json["to_agent_id"] = to_agent_id;
    json["content"] = content;
    json["timestamp_ms"] = timestamp_ms;
    
    if (!meta.empty()) {
        core::JsonValue meta_json = core::JsonValue::object();
        for (const auto& [key, value] : meta) {
            meta_json[key] = value;
        }
        json["meta"] = meta_json;
    }
    
    return json;
}

// AcpProtocol implementation
SessionAcpMeta AcpProtocol::create_default_meta() {
    SessionAcpMeta meta;
    
    // 默认作用域 - 完全隔离
    meta.scope.isolated_workspace = true;
    meta.scope.isolated_memory = true;
    meta.scope.isolated_tools = false;  // Tools 通常共享
    
    // 默认限制 - 保守值
    meta.limits.max_memory_mb = 1024;  // 1GB
    meta.limits.max_disk_mb = 2048;    // 2GB
    meta.limits.max_concurrent_runs = 5;
    meta.limits.max_turns_per_run = 50;
    meta.limits.timeout_seconds = 300;  // 5 分钟
    
    return meta;
}

core::Result<void> AcpProtocol::validate_meta(const SessionAcpMeta& meta) {
    // 验证资源限制
    if (meta.limits.max_memory_mb && *meta.limits.max_memory_mb == 0) {
        return core::Result<void>::err("max_memory_mb must be > 0");
    }
    
    if (meta.limits.max_disk_mb && *meta.limits.max_disk_mb == 0) {
        return core::Result<void>::err("max_disk_mb must be > 0");
    }
    
    if (meta.limits.max_concurrent_runs && *meta.limits.max_concurrent_runs <= 0) {
        return core::Result<void>::err("max_concurrent_runs must be > 0");
    }
    
    if (meta.limits.max_turns_per_run && *meta.limits.max_turns_per_run <= 0) {
        return core::Result<void>::err("max_turns_per_run must be > 0");
    }
    
    if (meta.limits.timeout_seconds && *meta.limits.timeout_seconds <= 0) {
        return core::Result<void>::err("timeout_seconds must be > 0");
    }
    
    // 验证作用域覆盖路径
    if (meta.scope.workspace_override) {
        const std::string& path = *meta.scope.workspace_override;
        
        // 1. 检查路径是否为空
        if (path.empty()) {
            return core::Result<void>::err("workspace_override cannot be empty");
        }
        
        // 2. 检查路径是否包含非法字符
        if (path.find("..") != std::string::npos) {
            return core::Result<void>::err("workspace_override contains '..' (path traversal)");
        }
        
        // 3. 检查路径是否为绝对路径 (可选,根据需求)
        // if (!std::filesystem::path(path).is_absolute()) {
        //     return core::Result<void>::err("workspace_override must be absolute path");
        // }
        
        // 4. 检查路径是否存在且可访问 (可选)
        try {
            if (!std::filesystem::exists(path)) {
                // 警告: 路径不存在 (但不阻止,可能稍后创建)
                spdlog::warn("[AcpProtocol] workspace_override path does not exist: {}", path);
            }
        } catch (const std::exception& e) {
            return core::Result<void>::err(std::string("workspace_override path check failed: ") + e.what());
        }
    }
    
    // 验证 memory_override 路径 (同样的检查)
    if (meta.scope.memory_override) {
        const std::string& path = *meta.scope.memory_override;
        
        if (path.empty()) {
            return core::Result<void>::err("memory_override cannot be empty");
        }
        
        if (path.find("..") != std::string::npos) {
            return Error<void>("memory_override contains '..' (path traversal)");
        }
        
        try {
            if (!std::filesystem::exists(path)) {
                auto logger = spdlog::get("acp_protocol");
                if (logger) {
                    logger->warn("[AcpProtocol] memory_override path does not exist: {}", path);
                }
            }
        } catch (const std::exception& e) {
            return Error<void>(std::string("memory_override path check failed: ") + e.what());
        }
    }
    
    return OkVoid();
}

AcpMessage AcpProtocol::create_message(
    const std::string& from_agent,
    const std::string& to_agent,
    const std::string& content,
    const std::map<std::string, core::JsonValue>& meta
) {
    AcpMessage msg;
    
    // 生成消息 ID
    msg.message_id = core::utils::generate_uuid();  // utils::generate_uuid() 不接受参数
    msg.from_agent_id = from_agent;
    msg.to_agent_id = to_agent;
    msg.content = content;
    msg.meta = meta;
    
    // 时间戳
    msg.timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
    
    return msg;
}

std::string AcpProtocol::resolve_key_from_label(
    const AcpLabelMapping& labels,
    const std::string& label
) {
    // 反向查找: label → key
    for (const auto& [key, mapped_label] : labels.labels) {
        if (mapped_label == label) {
            return key;
        }
    }
    
    // 如果没有找到映射,返回 label 本身
    return label;
}

} // namespace agents
} // namespace hunterclaw
