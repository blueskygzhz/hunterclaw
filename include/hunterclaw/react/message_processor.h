#pragma once

#include "hunterclaw/react/react_engine.h"
#include "hunterclaw/react/loop_detection.h"
#include "hunterclaw/routing/router.h"
#include "hunterclaw/sessions/session_manager.h"
#include "hunterclaw/queue/delivery_queue.h"
#include "hunterclaw/channels/message.h"
#include <memory>
#include <functional>

namespace hunterclaw {
namespace react {

// 对齐 OpenClaw 的消息处理流程
// 参考: openclaw-analysis/src/auto-reply/reply/agent-runner-execution.ts

// 消息处理配置
struct MessageProcessorConfig {
    // ReAct 配置
    ReactConfig react_config;
    
    // 循环检测配置
    LoopDetectionConfig loop_detection_config;
    
    // ACK 反应 (立即确认)
    bool send_ack_reaction = true;
    std::string ack_emoji = "👀";  // 眼睛表情
    
    // Typing 指示器
    bool send_typing_indicator = true;
    
    // Echo 检测 (防止重复响应)
    bool echo_detection = true;
    std::chrono::seconds echo_window{30};
    
    // 历史上下文
    int max_history_messages = 20;
    bool include_group_history = true;  // 群组消息包含历史
    
    // 权限检查
    bool enable_permission_check = true;
    
    // 模型回退
    bool enable_model_fallback = true;
    std::vector<std::string> fallback_models = {
        "gpt-4-turbo-preview",
        "claude-3-5-sonnet-20241022",
        "gemini-pro"
    };
    
    // 自动压缩
    bool enable_auto_compaction = true;
    int compaction_threshold_tokens = 50000;
};

// 消息处理上下文
struct MessageContext {
    // 输入消息
    channels::InboundMessage message;
    
    // 路由信息
    routing::RouteResult route;
    
    // 会话信息
    sessions::SessionEntry session;
    
    // Agent 配置
    std::string agent_id;
    std::string workspace_path;
    
    // 用户信息
    std::string user_id;
    std::string user_name;
    bool is_owner = false;  // 是否为系统所有者
    
    // 权限
    bool authorized = true;
    std::string denial_reason;
};

// 处理结果
struct ProcessResult {
    bool success = false;
    std::string error;
    
    // ReAct 结果
    ReactResult react_result;
    
    // 投递信息
    std::vector<std::string> delivered_message_ids;
    
    // 回退信息
    bool used_fallback = false;
    std::string fallback_model;
    std::vector<std::string> fallback_attempts;
    
    // 统计
    int64_t processing_time_ms = 0;
    int64_t total_tokens = 0;
    bool auto_compaction_triggered = false;
};

// 消息处理器 - 对齐 OpenClaw 的完整流程
class MessageProcessor {
public:
    static std::shared_ptr<MessageProcessor> create(
        std::shared_ptr<routing::MessageRouter> router,
        std::shared_ptr<sessions::SessionManager> session_manager,
        std::shared_ptr<queue::DeliveryQueue> delivery_queue,
        std::shared_ptr<tools::ToolManager> tool_manager,
        std::shared_ptr<llm::LLMClient> llm_client,
        const MessageProcessorConfig& config = MessageProcessorConfig{}
    );
    
    // 处理消息 (主入口)
    ProcessResult process_message(
        const channels::InboundMessage& message
    );
    
    // 异步处理
    std::future<ProcessResult> process_message_async(
        const channels::InboundMessage& message
    );
    
    // 配置
    const MessageProcessorConfig& config() const { return config_; }
    MessageProcessorConfig& mutable_config() { return config_; }
    
private:
    MessageProcessor(
        std::shared_ptr<routing::MessageRouter> router,
        std::shared_ptr<sessions::SessionManager> session_manager,
        std::shared_ptr<queue::DeliveryQueue> delivery_queue,
        std::shared_ptr<tools::ToolManager> tool_manager,
        std::shared_ptr<llm::LLMClient> llm_client,
        const MessageProcessorConfig& config
    );
    
    // 处理流水线
    // 1. 预处理
    MessageContext preprocess_message(const channels::InboundMessage& message);
    
    // 2. 权限检查
    bool check_permissions(MessageContext& context);
    
    // 3. Echo 检测
    bool is_echo(const MessageContext& context);
    
    // 4. 发送 ACK
    void send_ack_reaction(const MessageContext& context);
    
    // 5. 加载会话和历史
    std::vector<LLMMessage> load_history(const MessageContext& context);
    
    // 6. 执行 ReAct (带循环检测)
    ReactResult execute_react_with_loop_detection(
        const MessageContext& context,
        const std::vector<LLMMessage>& history
    );
    
    // 7. 投递响应
    void deliver_response(
        const MessageContext& context,
        const ReactResult& result
    );
    
    // 8. 保存会话
    void save_session(
        const MessageContext& context,
        const ReactResult& result
    );
    
    // 9. 处理错误
    ProcessResult handle_error(
        const MessageContext& context,
        const std::string& error
    );
    
    // 模型回退
    ReactResult execute_with_fallback(
        const MessageContext& context,
        const std::vector<LLMMessage>& history,
        std::vector<std::string>& fallback_attempts
    );
    
    // 自动压缩
    bool try_auto_compaction(MessageContext& context);
    
    std::shared_ptr<routing::MessageRouter> router_;
    std::shared_ptr<sessions::SessionManager> session_manager_;
    std::shared_ptr<queue::DeliveryQueue> delivery_queue_;
    std::shared_ptr<tools::ToolManager> tool_manager_;
    std::shared_ptr<llm::LLMClient> llm_client_;
    MessageProcessorConfig config_;
    
    // Echo 检测缓存
    struct EchoRecord {
        std::string message_id;
        std::string content_hash;
        int64_t timestamp_ms;
    };
    std::vector<EchoRecord> echo_cache_;
};

} // namespace react
} // namespace hunterclaw
