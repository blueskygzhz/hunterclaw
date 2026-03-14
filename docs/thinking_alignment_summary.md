# Thinking 输出机制对齐 OpenClaw - 修改摘要

## 🔧 修改内容

### 1️⃣ **在 `call_llm` 流式回调中更新双缓冲区**

**文件**: `src/react/react_engine.cc:384-416`

```cpp
auto stream_callback = [&](const std::string& delta) {
    if (delta.empty()) return;
    
    // ⭐ 新增: 累积原始内容到 delta_buffer
    streaming_state_.delta_buffer += delta;
    
    auto extraction = extract_reasoning(delta);
    
    if (extraction.has_reasoning && callbacks.on_reasoning_stream) {
        callbacks.on_reasoning_stream(extraction.reasoning);
        accumulated_reasoning += extraction.reasoning;
    }
    
    accumulated_text += extraction.remaining_text;
    current_chunk_buffer += extraction.remaining_text;
    
    // ⭐ 新增: 累积过滤后内容到 block_buffer
    streaming_state_.block_buffer += extraction.remaining_text;
};
```

---

### 2️⃣ **在 `execute_loop` 开始时重置状态**

**文件**: `src/react/react_engine.cc:184-185`

```cpp
ReactResult ReactEngine::execute_loop(...) {
    ReactResult result;
    result.success = true;
    
    // ⭐ 新增: 重置流式状态
    streaming_state_.reset();
    
    // ...
}
```

---

### 3️⃣ **根据 `reasoning_mode` 构造 `final_response`**

**文件**: `src/react/react_engine.cc:318-330`

```cpp
// ⭐ 修改: 根据配置选择 buffer
switch (config_.reasoning_mode) {
    case ReactConfig::ReasoningMode::Off:
        result.final_response = streaming_state_.block_buffer;  // 不含 thinking
        break;
    case ReactConfig::ReasoningMode::On:
    case ReactConfig::ReasoningMode::Stream:
        result.final_response = streaming_state_.delta_buffer;  // 含 thinking
        break;
}
```

---

## 📊 修改前后对比

| 特性 | 修改前 | 修改后 |
|------|--------|--------|
| `delta_buffer` 更新 | ❌ 未实现 | ✅ 每次 delta 累积 |
| `block_buffer` 更新 | ❌ 未实现 | ✅ 累积过滤后内容 |
| `streaming_state_` 初始化 | ❌ 未重置 | ✅ 每次循环重置 |
| `final_response` 内容 | ⚠️ 空字符串 | ✅ 根据 mode 选择 |
| Thinking 输出 | ⚠️ 仅通过回调 | ✅ 回调 + final_response |

---

## 🎯 核心改进

### 问题 1: `final_response` 为空

**原因**: `streaming_state_.delta_buffer` 从未被赋值

**修复**: 在流式回调中累积 `delta`

```cpp
// 修改前
auto stream_callback = [&](const std::string& delta) {
    auto extraction = extract_reasoning(delta);
    accumulated_text += extraction.remaining_text;  // ❌ delta_buffer 未更新
};

// 修改后
auto stream_callback = [&](const std::string& delta) {
    streaming_state_.delta_buffer += delta;  // ✅ 累积原始内容
    auto extraction = extract_reasoning(delta);
    accumulated_text += extraction.remaining_text;
    streaming_state_.block_buffer += extraction.remaining_text;  // ✅ 累积过滤内容
};
```

---

### 问题 2: 无法控制是否显示 thinking

**原因**: `final_response` 固定使用 `delta_buffer`（即使未更新）

**修复**: 根据 `reasoning_mode` 动态选择

```cpp
// 修改前
result.final_response = streaming_state_.delta_buffer;  // ❌ 固定使用（且为空）

// 修改后
switch (config_.reasoning_mode) {
    case ReactConfig::ReasoningMode::Off:
        result.final_response = streaming_state_.block_buffer;  // ✅ 不含 thinking
        break;
    case ReactConfig::ReasoningMode::On:
    case ReactConfig::ReasoningMode::Stream:
        result.final_response = streaming_state_.delta_buffer;  // ✅ 含 thinking
        break;
}
```

---

### 问题 3: 状态未重置

**原因**: `streaming_state_` 跨请求保留旧数据

**修复**: 在 `execute_loop` 开始时调用 `reset()`

```cpp
// 修改前
ReactResult ReactEngine::execute_loop(...) {
    ReactResult result;
    result.success = true;
    // ❌ 未重置状态
    // ...
}

// 修改后
ReactResult ReactEngine::execute_loop(...) {
    ReactResult result;
    result.success = true;
    
    streaming_state_.reset();  // ✅ 清空所有缓冲区
    // ...
}
```

---

## ✅ 完全对齐 OpenClaw

| 功能 | OpenClaw | HunterClaw (修改后) |
|------|----------|---------------------|
| 原始内容累积 | `deltaBuffer += chunk` | `delta_buffer += delta` ✅ |
| 过滤内容累积 | `blockBuffer += filtered` | `block_buffer += remaining` ✅ |
| 推理流回调 | `emitReasoningStream` | `on_reasoning_stream` ✅ |
| 状态重置 | 每次 run 重置 | `streaming_state_.reset()` ✅ |
| 模式控制 | 动态选择 buffer | switch 分支选择 ✅ |

---

## 🚀 使用示例

```cpp
// 1. 配置推理模式
react::ReactConfig config;
config.reasoning_mode = react::ReactConfig::ReasoningMode::Stream;

// 2. 配置回调
react::StreamCallbacks callbacks;
callbacks.on_reasoning_stream = [](const std::string& reasoning) {
    std::cout << "💭 " << reasoning << std::flush;
};

// 3. 运行引擎
auto engine = react::ReactEngine::create(tool_mgr, llm_client, config);
auto result = engine->run(user_input, history, callbacks);

// 4. 获取结果
// - reasoning_mode = Off: result.final_response 不含 <thinking>
// - reasoning_mode = On/Stream: result.final_response 含 <thinking>
std::cout << "\n最终响应: " << result.final_response << std::endl;
```

---

## 📝 修改文件

- `src/react/react_engine.cc` (3 处修改，约 10 行代码)

---

## ⚡ 影响

- ✅ 修复 `final_response` 为空的 bug
- ✅ 支持动态控制是否显示 thinking
- ✅ 完全对齐 OpenClaw 实现
- ✅ 100% 向后兼容
