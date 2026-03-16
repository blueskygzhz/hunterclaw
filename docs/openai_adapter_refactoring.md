# OpenAI Adapter 对齐 OpenClaw 改进方案

## 问题分析

### 当前问题
1. **架构位置不当**: `openai_adapter.cc` 作为独立文件放在 `gateway/` 根目录下显得突兀
2. **职责不清晰**: 当前实现混合了:
   - 数据格式转换 (OpenAI ↔ HunterClaw)
   - HTTP 请求处理 (在 `http_gateway.cc` 中)
   - Session 管理 (Device ID 解析)
3. **与 OpenClaw 不一致**: OpenClaw 将 OpenAI 兼容作为完整的 HTTP 端点实现,而非独立适配器

### OpenClaw 架构分析

**OpenClaw 实现** (`src/gateway/openai-http.ts`):
```typescript
// 完整的 HTTP 端点处理器
export async function handleOpenAiHttpRequest(
  req: IncomingMessage,
  res: ServerResponse,
  opts: OpenAiHttpOptions
): Promise<boolean> {
  // 1. 认证和速率限制
  const handled = await handleGatewayPostJsonEndpoint(req, res, {
    pathname: "/v1/chat/completions",
    auth: opts.auth,
    rateLimiter: opts.rateLimiter,
    ...
  });
  
  // 2. 请求解析和图片处理
  const payload = coerceRequest(handled.body);
  const images = await resolveImagesForRequest(...);
  
  // 3. Session 和 Message Channel 解析
  const { sessionKey, messageChannel } = resolveGatewayRequestContext({
    req, model, user,
    sessionPrefix: "openai",
    defaultMessageChannel: "webchat",
  });
  
  // 4. 调用 Agent 命令
  const result = await agentCommandFromIngress(commandInput, ...);
  
  // 5. 响应格式化
  sendJson(res, 200, { id, object: "chat.completion", ... });
}
```

**特点**:
- 所有功能集成在一个端点处理器中
- 统一的认证、速率限制、会话管理
- 图片处理、流式传输等高级功能
- 与 Gateway 其他功能保持一致

### HunterClaw 当前架构

**当前实现分散在两处**:
1. `gateway/openai_adapter.cc`: 数据格式转换
2. `gateway/http_gateway.cc`: HTTP 请求处理

```cpp
// http_gateway.cc
void HttpGateway::handle_chat_completions(...) {
    // Session 解析 (对齐 OpenClaw ✅)
    std::string session_key;
    if (req.has_header("x-hunterclaw-session-key")) {
        session_key = req.get_header_value("x-hunterclaw-session-key");
    } else {
        std::string device_id = extract_device_id_from_request(req);
        session_key = build_device_session_key("main", device_id);
    }
    
    // 调用 OpenAIAdapter
    auto request = ChatCompletionRequest::from_json(body);
    auto result = openai_adapter_->create_chat_completion(request);
    ...
}

// openai_adapter.cc
Result<ChatCompletionResponse> OpenAIAdapter::create_chat_completion(...) {
    auto llm_messages = convert_messages(request.messages);
    auto llm_result = llm_client_->generate_from_messages(llm_messages);
    return convert_response(llm_result.value, request);
}
```

## 改进方案

### 方案一: 保持当前结构,优化职责分离 (推荐)

**优点**: 最小改动,保持 C++ 风格的清晰分层
**缺点**: 与 OpenClaw 架构不完全一致

#### 改动内容

1. **重命名和重组**:
   ```
   gateway/
   ├── http/                     # 新建 HTTP 相关目录
   │   ├── openai_compat.cc/h   # OpenAI 兼容层 (重命名)
   │   ├── auth.cc/h            # HTTP 认证
   │   └── rate_limit.cc/h      # HTTP 速率限制
   ├── methods/                  # WebSocket RPC methods
   ├── gateway.cc/h             # WebSocket Gateway
   └── http_gateway.cc/h        # HTTP Gateway 主入口
   ```

2. **职责明确**:
   - `openai_compat`: 纯数据格式转换 + 简单的业务逻辑
   - `http_gateway`: HTTP 路由、认证、Session 管理
   - `gateway`: 底层 LLM 调用

3. **对齐 OpenClaw**:
   - ✅ Session Key 解析逻辑 (已完成)
   - ✅ Device ID 支持 (已完成)
   - 🔜 Message Channel 支持
   - 🔜 图片处理 (base64/URL)
   - 🔜 流式传输优化

### 方案二: 完全对齐 OpenClaw,合并为端点处理器

**优点**: 与 OpenClaw 架构完全一致
**缺点**: 需要较大重构,可能丧失 C++ 分层优势

#### 改动内容

1. **合并为单一处理器**:
   ```cpp
   // gateway/http/openai_http.cc
   namespace hunterclaw::gateway::http {
   
   class OpenAiHttpHandler {
   public:
       // 完整的端点处理
       core::Result<bool> handle_request(
           const httplib::Request& req,
           httplib::Response& res,
           const OpenAiHttpOptions& opts
       );
       
   private:
       // 认证
       core::Result<AuthContext> authenticate(...);
       // 速率限制
       core::Result<bool> check_rate_limit(...);
       // Session 解析
       std::string resolve_session_key(...);
       // 图片处理
       std::vector<ImageContent> resolve_images(...);
       // 调用 Agent
       core::Result<AgentResponse> invoke_agent(...);
       // 响应格式化
       void send_completion_response(...);
       void send_stream_chunk(...);
   };
   
   }
   ```

2. **移除独立 Adapter**:
   - 删除 `openai_adapter.cc/h`
   - 将功能整合到 `openai_http.cc`

### 方案三: 混合方案 (平衡)

**优点**: 保持分层同时对齐架构
**缺点**: 需要重新设计接口

#### 改动内容

1. **目录结构**:
   ```
   gateway/
   ├── http/
   │   ├── openai_http.cc/h      # OpenAI HTTP 端点 (主逻辑)
   │   ├── openai_protocol.cc/h  # OpenAI 协议定义和转换
   │   ├── http_auth.cc/h
   │   └── http_utils.cc/h
   ```

2. **接口设计**:
   ```cpp
   // openai_http.h
   class OpenAiHttpEndpoint {
   public:
       void handle_chat_completions(
           const httplib::Request& req,
           httplib::Response& res
       );
       
   private:
       std::unique_ptr<OpenAiProtocolAdapter> protocol_;
       std::shared_ptr<HttpAuthManager> auth_;
       std::shared_ptr<RateLimiter> rate_limiter_;
       std::shared_ptr<llm::LLMClient> llm_client_;
   };
   
   // openai_protocol.h
   class OpenAiProtocolAdapter {
   public:
       // 只负责协议转换
       ChatCompletionRequest parse_request(const json& body);
       json format_response(const AgentResponse& response);
       std::string format_stream_chunk(const std::string& content);
   };
   ```

## 推荐实施步骤

采用 **方案一 (优化职责分离)**:

### 第一阶段: 重组文件结构
1. 创建 `gateway/http/` 目录
2. 移动 `openai_adapter.cc/h` → `gateway/http/openai_compat.cc/h`
3. 更新 BUILD 文件和 include 路径

### 第二阶段: 优化 HTTP Gateway
1. 提取 Session 管理逻辑到独立模块
2. 添加完整的认证和速率限制
3. 改进错误处理和日志

### 第三阶段: 对齐 OpenClaw 功能
1. ✅ Device ID 和 Session Key (已完成)
2. 🔜 Message Channel 支持
3. 🔜 图片处理 (base64 data URI)
4. 🔜 流式传输优化 (SSE chunks)
5. 🔜 Token 估算改进

### 第四阶段: 测试和文档
1. 添加单元测试
2. 添加集成测试
3. 更新 API 文档

## OpenClaw 对齐 Checklist

- [x] **Session Key 解析**: 支持显式 header 和 Device ID 构造
- [x] **Device ID 提取**: 从认证 token 中提取
- [ ] **Message Channel**: 支持 `x-message-channel` header
- [ ] **图片处理**: 支持 `image_url` (data URI 和 URL)
- [ ] **速率限制**: 基于 IP/Device 的速率限制
- [ ] **认证增强**: 支持多种认证方式
- [ ] **流式优化**: 改进 SSE 流式传输
- [ ] **错误对齐**: 使用 OpenAI 标准错误格式
- [ ] **指标收集**: 记录使用量和 Token 消耗

## 代码示例

### 改进后的 openai_compat.h
```cpp
#pragma once

#include "hunterclaw/core/types.h"
#include "hunterclaw/llm/llm_client.h"
#include <nlohmann/json.hpp>

namespace hunterclaw::gateway::http {

// OpenAI 兼容协议定义 (保持现有结构)
struct ChatMessage { ... };
struct ChatCompletionRequest { ... };
struct ChatCompletionResponse { ... };
struct ChatCompletionChunk { ... };

// OpenAI 兼容层 - 专注于协议转换
class OpenAiCompat {
public:
    explicit OpenAiCompat(std::shared_ptr<llm::LLMClient> llm_client);
    
    // 数据转换
    core::Result<ChatCompletionResponse> create_completion(
        const ChatCompletionRequest& request,
        const std::string& session_key = ""
    );
    
    core::Result<bool> create_completion_stream(
        const ChatCompletionRequest& request,
        StreamCallback on_chunk,
        ErrorCallback on_error,
        const std::string& session_key = ""
    );
    
    std::vector<ModelInfo> list_models() const;
    
private:
    std::shared_ptr<llm::LLMClient> llm_client_;
    std::map<std::string, std::pair<std::string, std::string>> model_mapping_;
};

} // namespace hunterclaw::gateway::http
```

### 改进后的 http_gateway.cc
```cpp
void HttpGateway::handle_chat_completions(const httplib::Request& req, httplib::Response& res) {
    // 1. 认证检查
    if (!authenticate_request(req)) {
        send_error(res, 401, "Unauthorized");
        return;
    }
    
    // 2. 速率限制
    if (!check_rate_limit(req)) {
        send_error(res, 429, "Too Many Requests");
        return;
    }
    
    // 3. Session 解析 (对齐 OpenClaw)
    std::string session_key = resolve_session_key(req);
    std::string message_channel = resolve_message_channel(req);
    
    // 4. 请求解析
    auto body = json::parse(req.body);
    auto request = ChatCompletionRequest::from_json(body);
    
    // 5. 调用 OpenAI 兼容层
    if (request.stream.value_or(false)) {
        handle_streaming_completion(req, res, request, session_key, message_channel);
    } else {
        handle_blocking_completion(req, res, request, session_key, message_channel);
    }
}

std::string HttpGateway::resolve_session_key(const httplib::Request& req) {
    // 方式 1: 显式 header
    if (req.has_header("x-hunterclaw-session-key")) {
        return req.get_header_value("x-hunterclaw-session-key");
    }
    if (req.has_header("x-openclaw-session-key")) {
        return req.get_header_value("x-openclaw-session-key");
    }
    
    // 方式 2: Device ID 构造
    std::string device_id = extract_device_id_from_request(req);
    if (!device_id.empty() && is_valid_device_id(device_id)) {
        return build_device_session_key("main", device_id);
    }
    
    // 方式 3: 基于 model/user 的默认 session
    return "openai:default";
}

std::string HttpGateway::resolve_message_channel(const httplib::Request& req) {
    if (req.has_header("x-message-channel")) {
        return req.get_header_value("x-message-channel");
    }
    return "webchat";  // 默认
}
```

## 总结

1. **推荐采用方案一**: 最小化改动,保持清晰分层
2. **核心改动**: 
   - 重命名: `openai_adapter` → `openai_compat`
   - 移动: `gateway/` → `gateway/http/`
   - 优化: Session 管理、认证、速率限制
3. **对齐重点**:
   - ✅ Session Key 和 Device ID (已完成)
   - 🔜 Message Channel
   - 🔜 图片处理
   - 🔜 流式传输优化

这样既能保持 HunterClaw 的 C++ 架构优势,又能与 OpenClaw 的功能保持一致。
