# HunterClaw 多媒体工具快速使用指南

## 🚀 快速开始

### 1. 配置 API Keys

```bash
export OPENAI_API_KEY="sk-..."
export ELEVENLABS_API_KEY="..."
export GROQ_API_KEY="gsk_..."
export ANTHROPIC_API_KEY="sk-ant-..."
```

### 2. 注册工具

```cpp
#include "hunterclaw/tools/builtin_tools.h"

// 初始化依赖
auto node_registry = std::make_shared<nodes::NodeRegistry>();
auto http_client = std::make_shared<http::HttpClient>();
auto llm_client = std::make_shared<llm::LlmClient>(/* config */);

// 注册多媒体工具
BuiltinTools::register_media_tools(
    tool_manager, 
    tool_context,
    node_registry,
    http_client, 
    llm_client
);
```

---

## 📷 Camera Tool

### 拍照

```cpp
JsonValue params = JsonValue::object();
params["action"] = "camera_snap";
params["facing"] = "back";        // "front", "back", "both"
params["maxWidth"] = 1600;
params["quality"] = 0.95;

auto result = tool_manager.execute_tool("camera", params);
// result.content[0].data = base64_image
```

### 录视频

```cpp
JsonValue params = JsonValue::object();
params["action"] = "camera_clip";
params["facing"] = "back";
params["durationMs"] = 5000;      // 5秒
params["includeAudio"] = true;

auto result = tool_manager.execute_tool("camera", params);
```

### 获取最新照片

```cpp
JsonValue params = JsonValue::object();
params["action"] = "photos_latest";
params["limit"] = 5;              // 1-20

auto result = tool_manager.execute_tool("camera", params);
// result.content = [image1, image2, ...]
```

---

## 🎵 TTS Tool

### OpenAI TTS

```cpp
JsonValue params = JsonValue::object();
params["text"] = "Hello world";
params["provider"] = "openai";
params["voice"] = "alloy";        // alloy, echo, fable, onyx, nova, shimmer
params["model"] = "tts-1";        // tts-1, tts-1-hd
params["format"] = "mp3";         // mp3, opus, aac, flac
params["speed"] = 1.0;            // 0.25 - 4.0

auto result = tool_manager.execute_tool("tts", params);
std::string audio_base64 = result.details["audio_base64"].as_string();
```

### ElevenLabs TTS (高质量)

```cpp
JsonValue params = JsonValue::object();
params["text"] = "Hello world";
params["provider"] = "elevenlabs";
params["voice"] = "21m00Tcm4TlvDq8ikWAM";  // Rachel voice

auto result = tool_manager.execute_tool("tts", params);
```

---

## 🎤 Audio Transcription

### 转录音频文件

```cpp
JsonValue params = JsonValue::object();
params["audio_path"] = "/path/to/recording.mp3";
params["provider"] = "openai";    // openai, groq
params["language"] = "en";        // en, zh, es, fr, etc.
params["response_format"] = "text";  // text, json, verbose_json

auto result = tool_manager.execute_tool("audio_transcription", params);
std::string transcription = result.details["text"].as_string();
```

### 转录 base64 音频

```cpp
JsonValue params = JsonValue::object();
params["audio_base64"] = "SGVsbG8gV29ybGQ=...";
params["provider"] = "groq";      // Groq 速度更快

auto result = tool_manager.execute_tool("audio_transcription", params);
```

---

## 🖼️ Image Analysis

### 图像描述

```cpp
JsonValue params = JsonValue::object();
params["image_path"] = "/path/to/photo.jpg";
params["prompt"] = "Describe this image in detail";
params["provider"] = "openai";
params["model"] = "gpt-4o";
params["max_tokens"] = 300;

auto result = tool_manager.execute_tool("image_analysis", params);
std::string analysis = result.details["analysis"].as_string();
```

### OCR (文字识别)

```cpp
JsonValue params = JsonValue::object();
params["image_path"] = "/path/to/document.jpg";
params["prompt"] = "Extract all text from this image";
params["provider"] = "anthropic";  // Claude 的 OCR 更准确
params["model"] = "claude-3.5-sonnet";

auto result = tool_manager.execute_tool("image_analysis", params);
```

### 图像 URL

```cpp
JsonValue params = JsonValue::object();
params["image_url"] = "https://example.com/image.jpg";
params["prompt"] = "What objects are in this image?";
params["provider"] = "openai";

auto result = tool_manager.execute_tool("image_analysis", params);
```

### Base64 图像

```cpp
JsonValue params = JsonValue::object();
params["image_base64"] = "iVBORw0KGgoAAAANSUhEUgAA...";
params["prompt"] = "Is this a cat or a dog?";

auto result = tool_manager.execute_tool("image_analysis", params);
```

---

## 🔧 高级用法

### 1. 流式回调

```cpp
auto result = tool_manager.execute_tool(
    "tts", 
    params,
    [](const ToolUpdate& update) {
        // 处理进度更新
        std::cout << "Progress: " << update.progress << "%" << std::endl;
    }
);
```

### 2. 审批系统

```cpp
// Camera Tool 需要审批
auto approval_manager = tool_manager.get_approval_manager();
approval_manager->set_callback([](const std::string& tool_name, const JsonValue& params) {
    // 用户确认
    return ask_user_permission(tool_name, params);
});
```

### 3. 错误处理

```cpp
auto result = tool_manager.execute_tool("camera", params);

if (!result.success) {
    std::cerr << "Error: " << result.error << std::endl;
    return;
}

// 处理结果
for (const auto& content : result.content) {
    if (content.type == ToolResultContent::Type::Image) {
        save_image(content.data, content.mime_type);
    }
}
```

---

## 📋 工具列表

| 工具名 | 功能 | 需要审批 | 依赖 |
|--------|------|----------|------|
| `camera` | 拍照、录视频 | ✅ 是 | NodeRegistry |
| `tts` | 文本转语音 | ❌ 否 | HttpClient |
| `audio_transcription` | 音频转文本 | ❌ 否 | HttpClient |
| `image_analysis` | AI 图像分析 | ❌ 否 | LlmClient |

---

## ⚙️ 配置文件示例

```json
{
  "tools": {
    "camera": {
      "enabled": true,
      "require_approval": true,
      "default_facing": "back",
      "default_quality": 0.95
    },
    "tts": {
      "enabled": true,
      "default_provider": "openai",
      "default_voice": "alloy",
      "cache_enabled": true
    },
    "audio_transcription": {
      "enabled": true,
      "default_provider": "openai",
      "default_language": "en"
    },
    "image_analysis": {
      "enabled": true,
      "default_provider": "openai",
      "default_model": "gpt-4o",
      "max_tokens": 300
    }
  }
}
```

---

## 🐛 常见问题

### Q: Camera Tool 找不到节点?
A: 确保有在线的 iOS/Android/macOS 节点,并且节点支持 `camera` 能力。

### Q: TTS API 返回 401 错误?
A: 检查 `OPENAI_API_KEY` 或 `ELEVENLABS_API_KEY` 环境变量是否正确。

### Q: 音频文件太大无法上传?
A: 使用本地 Whisper 模型,或先压缩音频文件。

### Q: 图像分析不准确?
A: 尝试更详细的 prompt,或切换到 Claude (Anthropic) 模型。

---

## 📚 参考文档

- [OpenAI TTS API](https://platform.openai.com/docs/guides/text-to-speech)
- [OpenAI Whisper API](https://platform.openai.com/docs/guides/speech-to-text)
- [OpenAI Vision API](https://platform.openai.com/docs/guides/vision)
- [ElevenLabs API](https://elevenlabs.io/docs/api-reference)
- [Groq API](https://console.groq.com/docs/quickstart)
- [Claude Vision](https://docs.anthropic.com/claude/docs/vision)

---

**版本**: 1.0  
**最后更新**: 2026-03-13
