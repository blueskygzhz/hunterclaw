#include "hunterclaw/tools/slack_tool.h"
#include <iostream>
#include <sstream>
#include <regex>
#include <cstdlib>
#include <curl/curl.h>
#include <fstream>

namespace hunterclaw {
namespace tools {

// ==================== Impl 结构体 ====================
struct SlackTool::Impl {
  std::string bot_token;
  std::string api_base = "https://slack.com/api";
  std::unordered_map<std::string, std::string> channel_type_cache;
  
  // 从环境变量加载 Token
  bool load_token() {
    const char* token = std::getenv("SLACK_BOT_TOKEN");
    if (!token) {
      return false;
    }
    bot_token = token;
    return true;
  }
  
  // libcurl 回调
  static size_t write_callback(void* contents, size_t size, size_t nmemb, std::string* s) {
    size_t total = size * nmemb;
    s->append(static_cast<char*>(contents), total);
    return total;
  }
};

// ==================== 构造与析构 ====================
SlackTool::SlackTool() : impl_(std::make_unique<Impl>()) {
  if (!impl_->load_token()) {
    std::cerr << "[SlackTool] Warning: SLACK_BOT_TOKEN not set" << std::endl;
  }
}

SlackTool::~SlackTool() = default;

// ==================== 主执行入口 ====================
nlohmann::json SlackTool::execute(const std::string& action,
                                   const nlohmann::json& params) {
  try {
    if (action == "slack_send") {
      return slack_send(params);
    } else if (action == "slack_upload") {
      return slack_upload(params);
    } else if (action == "slack_channel") {
      return slack_channel(params);
    } else {
      return {
        {"status", "error"},
        {"error", "Unknown action: " + action}
      };
    }
  } catch (const std::exception& e) {
    return {
      {"status", "error"},
      {"error", std::string("Slack action failed: ") + e.what()}
    };
  }
}

// ==================== slack_send ====================
nlohmann::json SlackTool::slack_send(const nlohmann::json& params) {
  // 验证必需参数
  if (!params.contains("to") || !params.contains("message")) {
    return {
      {"status", "error"},
      {"error", "Missing required params: to, message"}
    };
  }
  
  std::string to = params["to"];
  std::string message = params["message"];
  
  // 解析目标
  auto target = parse_target(to);
  
  // 构建请求参数
  nlohmann::json api_params = {
    {"channel", target.id},
    {"text", truncate_text(message)}
  };
  
  // 可选参数: thread_ts
  if (params.contains("thread_ts")) {
    api_params["thread_ts"] = params["thread_ts"].get<std::string>();
  }
  
  // 可选参数: blocks
  if (params.contains("blocks")) {
    auto blocks = format_blocks(params["blocks"]);
    if (validate_blocks(blocks)) {
      api_params["blocks"] = blocks;
      // 添加 fallback text
      api_params["text"] = build_blocks_fallback(blocks);
    }
  }
  
  // 可选参数: identity (username, icon_url, icon_emoji)
  if (params.contains("identity")) {
    auto identity = parse_identity(params["identity"]);
    if (identity.username) api_params["username"] = *identity.username;
    if (identity.icon_url) api_params["icon_url"] = *identity.icon_url;
    if (identity.icon_emoji) api_params["icon_emoji"] = *identity.icon_emoji;
  }
  
  // 发送请求
  auto response = send_request("chat.postMessage", api_params);
  
  if (!response.contains("ok") || !response["ok"].get<bool>()) {
    // 检查是否缺少 chat:write.customize scope
    if (response.contains("error") && response["error"] == "missing_scope") {
      // 重试不带自定义身份
      api_params.erase("username");
      api_params.erase("icon_url");
      api_params.erase("icon_emoji");
      response = send_request("chat.postMessage", api_params);
    }
    
    if (!response.contains("ok") || !response["ok"].get<bool>()) {
      return {
        {"status", "error"},
        {"error", response.value("error", "Unknown error")},
        {"response", response}
      };
    }
  }
  
  return {
    {"status", "success"},
    {"message_id", response["ts"].get<std::string>()},
    {"channel_id", response["channel"].get<std::string>()}
  };
}

// ==================== slack_upload ====================
nlohmann::json SlackTool::slack_upload(const nlohmann::json& params) {
  // 验证必需参数
  if (!params.contains("channels")) {
    return {
      {"status", "error"},
      {"error", "Missing required param: channels"}
    };
  }
  
  // 至少需要一个: file_path, file_url, content
  if (!params.contains("file_path") && !params.contains("file_url") && !params.contains("content")) {
    return {
      {"status", "error"},
      {"error", "Must provide one of: file_path, file_url, content"}
    };
  }
  
  std::string channels = params["channels"];
  
  // 构建请求参数
  nlohmann::json api_params = {
    {"channels", channels}
  };
  
  // 可选参数
  if (params.contains("title")) api_params["title"] = params["title"];
  if (params.contains("filename")) api_params["filename"] = params["filename"];
  if (params.contains("initial_comment")) api_params["initial_comment"] = params["initial_comment"];
  if (params.contains("thread_ts")) api_params["thread_ts"] = params["thread_ts"];
  
  // 文件来源
  if (params.contains("file_url")) {
    std::string file_url = params["file_url"];
    
    // SSRF 检查 (对齐 OpenClaw SLACK_UPLOAD_SSRF_POLICY)
    SsrfPolicy policy;
    policy.allowed_hostnames = {"*.slack.com", "*.slack-edge.com", "*.slack-files.com"};
    policy.allow_rfc2544_benchmark_range = true;
    
    if (!check_ssrf_policy(file_url, policy)) {
      return {
        {"status", "error"},
        {"error", "File URL blocked by SSRF policy"}
      };
    }
    
    api_params["file"] = file_url;
  } else if (params.contains("content")) {
    api_params["content"] = params["content"];
  } else if (params.contains("file_path")) {
    // 本地文件: 需要使用 multipart/form-data
    // 简化: 这里返回提示 (实际需要 libcurl multipart upload)
    return {
      {"status", "error"},
      {"error", "Local file upload not implemented (use file_url or content)"},
      {"hint", "Implement multipart/form-data upload with libcurl"}
    };
  }
  
  // 发送请求
  auto response = send_request("files.upload", api_params);
  
  if (!response.contains("ok") || !response["ok"].get<bool>()) {
    return {
      {"status", "error"},
      {"error", response.value("error", "Unknown error")},
      {"response", response}
    };
  }
  
  auto file = response["file"];
  return {
    {"status", "success"},
    {"file_id", file["id"].get<std::string>()},
    {"url", file.value("url_private", "")},
    {"permalink", file.value("permalink", "")}
  };
}

// ==================== slack_channel ====================
nlohmann::json SlackTool::slack_channel(const nlohmann::json& params) {
  // 验证必需参数
  if (!params.contains("operation")) {
    return {
      {"status", "error"},
      {"error", "Missing required param: operation"}
    };
  }
  
  std::string operation = params["operation"];
  
  if (operation == "list") {
    // 列出频道
    nlohmann::json api_params;
    if (params.contains("types")) {
      api_params["types"] = params["types"];  // "public_channel,private_channel"
    }
    
    auto response = send_request("conversations.list", api_params);
    
    if (!response.contains("ok") || !response["ok"].get<bool>()) {
      return {
        {"status", "error"},
        {"error", response.value("error", "Unknown error")}
      };
    }
    
    return {
      {"status", "success"},
      {"operation", "list"},
      {"channels", response["channels"]}
    };
    
  } else if (operation == "info") {
    // 获取频道信息
    if (!params.contains("channel_id")) {
      return {
        {"status", "error"},
        {"error", "Missing required param: channel_id"}
      };
    }
    
    nlohmann::json api_params = {
      {"channel", params["channel_id"]}
    };
    
    auto response = send_request("conversations.info", api_params);
    
    if (!response.contains("ok") || !response["ok"].get<bool>()) {
      return {
        {"status", "error"},
        {"error", response.value("error", "Unknown error")}
      };
    }
    
    return {
      {"status", "success"},
      {"operation", "info"},
      {"channel", response["channel"]}
    };
    
  } else if (operation == "create") {
    // 创建频道
    if (!params.contains("name")) {
      return {
        {"status", "error"},
        {"error", "Missing required param: name"}
      };
    }
    
    nlohmann::json api_params = {
      {"name", params["name"]}
    };
    
    if (params.contains("is_private")) {
      api_params["is_private"] = params["is_private"].get<bool>();
    }
    
    auto response = send_request("conversations.create", api_params);
    
    if (!response.contains("ok") || !response["ok"].get<bool>()) {
      return {
        {"status", "error"},
        {"error", response.value("error", "Unknown error")}
      };
    }
    
    return {
      {"status", "success"},
      {"operation", "created"},
      {"channel", response["channel"]}
    };
    
  } else if (operation == "archive" || operation == "unarchive") {
    // 归档/取消归档频道
    if (!params.contains("channel_id")) {
      return {
        {"status", "error"},
        {"error", "Missing required param: channel_id"}
      };
    }
    
    nlohmann::json api_params = {
      {"channel", params["channel_id"]}
    };
    
    std::string method = (operation == "archive") ? "conversations.archive" : "conversations.unarchive";
    auto response = send_request(method, api_params);
    
    if (!response.contains("ok") || !response["ok"].get<bool>()) {
      return {
        {"status", "error"},
        {"error", response.value("error", "Unknown error")}
      };
    }
    
    return {
      {"status", "success"},
      {"operation", operation},
      {"channel_id", params["channel_id"]}
    };
    
  } else if (operation == "join" || operation == "leave") {
    // 加入/离开频道
    if (!params.contains("channel_id")) {
      return {
        {"status", "error"},
        {"error", "Missing required param: channel_id"}
      };
    }
    
    nlohmann::json api_params = {
      {"channel", params["channel_id"]}
    };
    
    std::string method = (operation == "join") ? "conversations.join" : "conversations.leave";
    auto response = send_request(method, api_params);
    
    if (!response.contains("ok") || !response["ok"].get<bool>()) {
      return {
        {"status", "error"},
        {"error", response.value("error", "Unknown error")}
      };
    }
    
    return {
      {"status", "success"},
      {"operation", operation},
      {"channel_id", params["channel_id"]}
    };
    
  } else if (operation == "set_topic" || operation == "set_purpose") {
    // 设置主题/目的
    if (!params.contains("channel_id")) {
      return {
        {"status", "error"},
        {"error", "Missing required param: channel_id"}
      };
    }
    
    std::string key = (operation == "set_topic") ? "topic" : "purpose";
    if (!params.contains(key)) {
      return {
        {"status", "error"},
        {"error", "Missing required param: " + key}
      };
    }
    
    nlohmann::json api_params = {
      {"channel", params["channel_id"]},
      {key, params[key]}
    };
    
    std::string method = "conversations." + operation.substr(4);  // "set_topic" -> "setTopic"
    auto response = send_request(method, api_params);
    
    if (!response.contains("ok") || !response["ok"].get<bool>()) {
      return {
        {"status", "error"},
        {"error", response.value("error", "Unknown error")}
      };
    }
    
    return {
      {"status", "success"},
      {"operation", operation},
      {"channel_id", params["channel_id"]}
    };
    
  } else {
    return {
      {"status", "error"},
      {"error", "Unknown operation: " + operation}
    };
  }
}

// ==================== 辅助方法实现 ====================

bool SlackTool::validate_token(const std::string& token) {
  return !token.empty() && (token.find("xoxb-") == 0 || token.find("xoxp-") == 0);
}

SlackTool::SlackTarget SlackTool::parse_target(const std::string& to) {
  SlackTarget target;
  
  // Slack ID 格式: C = channel, D = DM, G = group
  if (to[0] == 'C' || to[0] == 'G') {
    target.type = "channel";
    target.id = to;
  } else if (to[0] == 'D') {
    target.type = "user";
    target.id = to;
  } else {
    target.type = "channel";
    target.id = to;
  }
  
  return target;
}

std::string SlackTool::build_api_url(const std::string& method) {
  return impl_->api_base + "/" + method;
}

nlohmann::json SlackTool::send_request(const std::string& method,
                                        const nlohmann::json& params) {
  CURL* curl = curl_easy_init();
  if (!curl) {
    throw std::runtime_error("Failed to initialize CURL");
  }
  
  std::string url = build_api_url(method);
  std::string response_string;
  
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, Impl::write_callback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_string);
  curl_easy_setopt(curl, CURLOPT_POST, 1L);
  
  // 设置 Authorization Header
  struct curl_slist* headers = nullptr;
  std::string auth_header = "Authorization: Bearer " + impl_->bot_token;
  headers = curl_slist_append(headers, auth_header.c_str());
  headers = curl_slist_append(headers, "Content-Type: application/json");
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  
  // 设置 POST body
  std::string body_str = params.dump();
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body_str.c_str());
  
  // 执行请求
  CURLcode res = curl_easy_perform(curl);
  
  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);
  
  if (res != CURLE_OK) {
    throw std::runtime_error(std::string("CURL request failed: ") + curl_easy_strerror(res));
  }
  
  // 解析响应
  return nlohmann::json::parse(response_string);
}

nlohmann::json SlackTool::format_blocks(const nlohmann::json& blocks) {
  // 验证并格式化 Block Kit 块
  // OpenClaw 对齐: src/slack/blocks-input.ts (validateSlackBlocksArray)
  
  if (!blocks.is_array()) {
    return nlohmann::json::array();
  }
  
  // 简化: 直接返回 blocks (实际应该验证结构)
  return blocks;
}

bool SlackTool::validate_blocks(const nlohmann::json& blocks) {
  if (!blocks.is_array()) return false;
  
  for (const auto& block : blocks) {
    if (!block.contains("type")) return false;
  }
  
  return true;
}

std::string SlackTool::build_blocks_fallback(const nlohmann::json& blocks) {
  // OpenClaw 对齐: src/slack/blocks-fallback.ts (buildSlackBlocksFallbackText)
  
  std::string fallback;
  for (const auto& block : blocks) {
    if (block.contains("text") && block["text"].contains("text")) {
      fallback += block["text"]["text"].get<std::string>() + "\n";
    }
  }
  return fallback;
}

std::string SlackTool::markdown_to_mrkdwn(const std::string& markdown) {
  // 简化: Slack mrkdwn 与 Markdown 类似但有差异
  // OpenClaw 对齐: src/slack/format.ts (markdownToSlackMrkdwnChunks)
  
  std::string mrkdwn = markdown;
  // 替换链接格式: [text](url) -> <url|text>
  // 简化实现
  return mrkdwn;
}

std::string SlackTool::truncate_text(const std::string& text) {
  if (text.length() <= SLACK_TEXT_LIMIT) {
    return text;
  }
  return text.substr(0, SLACK_TEXT_LIMIT);
}

bool SlackTool::check_ssrf_policy(const std::string& url, const SsrfPolicy& policy) {
  // 简化 SSRF 检查
  // OpenClaw 对齐: src/infra/net/fetch-guard.ts
  
  // 检查 allowed_hostnames
  for (const auto& pattern : policy.allowed_hostnames) {
    if (url.find(pattern.substr(2)) != std::string::npos) {  // "*.slack.com" -> "slack.com"
      return true;
    }
  }
  
  return false;
}

SlackTool::SlackIdentity SlackTool::parse_identity(const nlohmann::json& identity) {
  SlackIdentity result;
  
  if (identity.contains("username")) {
    result.username = identity["username"].get<std::string>();
  }
  if (identity.contains("icon_url")) {
    result.icon_url = identity["icon_url"].get<std::string>();
  }
  if (identity.contains("icon_emoji")) {
    result.icon_emoji = identity["icon_emoji"].get<std::string>();
  }
  
  return result;
}

bool SlackTool::has_customize_scope() {
  // 简化: 实际应该调用 auth.test 检查 scopes
  return true;
}

std::string SlackTool::get_channel_type(const std::string& channel_id) {
  // OpenClaw 对齐: SLACK_CHANNEL_TYPE_CACHE
  
  auto it = impl_->channel_type_cache.find(channel_id);
  if (it != impl_->channel_type_cache.end()) {
    return it->second;
  }
  
  // 缓存未命中: 调用 conversations.info
  nlohmann::json params = {{"channel", channel_id}};
  auto response = send_request("conversations.info", params);
  
  if (response.contains("ok") && response["ok"].get<bool>()) {
    std::string type = response["channel"].value("is_channel", false) ? "channel" : "group";
    impl_->channel_type_cache[channel_id] = type;
    return type;
  }
  
  return "unknown";
}

} // namespace tools
} // namespace hunterclaw
