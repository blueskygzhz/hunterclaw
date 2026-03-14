#include "hunterclaw/tools/discord_tool.h"
#include <iostream>
#include <sstream>
#include <regex>
#include <cstdlib>
#include <curl/curl.h>

namespace hunterclaw {
namespace tools {

// ==================== Impl 结构体 ====================
struct DiscordTool::Impl {
  std::string bot_token;
  std::string api_base = "https://discord.com/api/v10";
  
  // 从环境变量加载 Token
  bool load_token() {
    const char* token = std::getenv("DISCORD_BOT_TOKEN");
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
DiscordTool::DiscordTool() : impl_(std::make_unique<Impl>()) {
  if (!impl_->load_token()) {
    std::cerr << "[DiscordTool] Warning: DISCORD_BOT_TOKEN not set" << std::endl;
  }
}

DiscordTool::~DiscordTool() = default;

// ==================== 主执行入口 ====================
nlohmann::json DiscordTool::execute(const std::string& action,
                                     const nlohmann::json& params) {
  try {
    if (action == "discord_send") {
      return discord_send(params);
    } else if (action == "discord_react") {
      return discord_react(params);
    } else if (action == "discord_thread") {
      return discord_thread(params);
    } else if (action == "discord_presence") {
      return discord_presence(params);
    } else {
      return {
        {"status", "error"},
        {"error", "Unknown action: " + action}
      };
    }
  } catch (const std::exception& e) {
    return {
      {"status", "error"},
      {"error", std::string("Discord action failed: ") + e.what()}
    };
  }
}

// ==================== discord_send ====================
nlohmann::json DiscordTool::discord_send(const nlohmann::json& params) {
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
  
  // 构建请求 Body
  nlohmann::json body = {
    {"content", message}
  };
  
  // 可选参数: reply_to
  if (params.contains("reply_to")) {
    body["message_reference"] = {
      {"message_id", params["reply_to"].get<std::string>()}
    };
  }
  
  // 可选参数: silent
  if (params.contains("silent") && params["silent"].get<bool>()) {
    body["flags"] = 4096;  // SUPPRESS_NOTIFICATIONS
  }
  
  // 可选参数: embeds
  if (params.contains("embeds")) {
    body["embeds"] = format_embeds(params["embeds"]);
  }
  
  // 可选参数: components
  if (params.contains("components")) {
    body["components"] = format_components(params["components"]);
  }
  
  // 发送请求
  std::string endpoint = "/channels/" + target.id + "/messages";
  auto response = send_request("POST", endpoint, body);
  
  if (!response.contains("id")) {
    return {
      {"status", "error"},
      {"error", "Failed to send message"},
      {"response", response}
    };
  }
  
  return {
    {"status", "success"},
    {"message_id", response["id"].get<std::string>()},
    {"channel_id", response["channel_id"].get<std::string>()}
  };
}

// ==================== discord_react ====================
nlohmann::json DiscordTool::discord_react(const nlohmann::json& params) {
  // 验证必需参数
  if (!params.contains("channel_id") || !params.contains("message_id") || !params.contains("emoji")) {
    return {
      {"status", "error"},
      {"error", "Missing required params: channel_id, message_id, emoji"}
    };
  }
  
  std::string channel_id = params["channel_id"];
  std::string message_id = params["message_id"];
  std::string emoji_str = params["emoji"];
  bool remove = params.value("remove", false);
  
  // 解析 Emoji
  auto emoji = parse_emoji(emoji_str);
  
  // 构建 Emoji URL 编码
  std::string emoji_encoded;
  if (emoji.type == "unicode") {
    // Unicode Emoji: URL encode
    emoji_encoded = emoji.value;  // 简化: 实际应该 URL encode
  } else {
    // Custom Emoji: name:id
    emoji_encoded = emoji.name.value() + ":" + emoji.id.value();
  }
  
  // 构建 endpoint
  std::string endpoint = "/channels/" + channel_id + "/messages/" + message_id + "/reactions/" + emoji_encoded + "/@me";
  
  // 发送请求
  std::string method = remove ? "DELETE" : "PUT";
  auto response = send_request(method, endpoint);
  
  return {
    {"status", "success"},
    {"action", remove ? "removed" : "added"},
    {"channel_id", channel_id},
    {"message_id", message_id},
    {"emoji", emoji_str}
  };
}

// ==================== discord_thread ====================
nlohmann::json DiscordTool::discord_thread(const nlohmann::json& params) {
  // 验证必需参数
  if (!params.contains("operation") || !params.contains("channel_id")) {
    return {
      {"status", "error"},
      {"error", "Missing required params: operation, channel_id"}
    };
  }
  
  std::string operation = params["operation"];
  std::string channel_id = params["channel_id"];
  
  if (operation == "create") {
    // 创建线程
    if (!params.contains("name")) {
      return {
        {"status", "error"},
        {"error", "Missing required param: name (for create operation)"}
      };
    }
    
    std::string name = params["name"];
    std::string truncated_name = truncate_thread_name(name);
    
    nlohmann::json body = {
      {"name", truncated_name}
    };
    
    // 可选: auto_archive_duration
    if (params.contains("auto_archive_duration")) {
      body["auto_archive_duration"] = params["auto_archive_duration"].get<int>();
    }
    
    // 可选: 从消息创建线程
    std::string endpoint;
    if (params.contains("message_id")) {
      endpoint = "/channels/" + channel_id + "/messages/" + params["message_id"].get<std::string>() + "/threads";
    } else {
      endpoint = "/channels/" + channel_id + "/threads";
    }
    
    auto response = send_request("POST", endpoint, body);
    
    if (!response.contains("id")) {
      return {
        {"status", "error"},
        {"error", "Failed to create thread"},
        {"response", response}
      };
    }
    
    return {
      {"status", "success"},
      {"operation", "created"},
      {"thread_id", response["id"].get<std::string>()},
      {"name", truncated_name}
    };
    
  } else if (operation == "archive" || operation == "unarchive") {
    // 归档/取消归档线程
    if (!params.contains("thread_id")) {
      return {
        {"status", "error"},
        {"error", "Missing required param: thread_id"}
      };
    }
    
    std::string thread_id = params["thread_id"];
    nlohmann::json body = {
      {"archived", operation == "archive"}
    };
    
    std::string endpoint = "/channels/" + thread_id;
    auto response = send_request("PATCH", endpoint, body);
    
    return {
      {"status", "success"},
      {"operation", operation},
      {"thread_id", thread_id}
    };
    
  } else if (operation == "join" || operation == "leave") {
    // 加入/离开线程
    if (!params.contains("thread_id")) {
      return {
        {"status", "error"},
        {"error", "Missing required param: thread_id"}
      };
    }
    
    std::string thread_id = params["thread_id"];
    std::string endpoint = "/channels/" + thread_id + "/thread-members/@me";
    std::string method = (operation == "join") ? "PUT" : "DELETE";
    
    auto response = send_request(method, endpoint);
    
    return {
      {"status", "success"},
      {"operation", operation},
      {"thread_id", thread_id}
    };
    
  } else {
    return {
      {"status", "error"},
      {"error", "Unknown operation: " + operation}
    };
  }
}

// ==================== discord_presence ====================
nlohmann::json DiscordTool::discord_presence(const nlohmann::json& params) {
  // 注意: Presence 更新需要通过 Gateway WebSocket,不是 REST API
  // 这里提供一个简化实现,实际应该集成 Gateway
  
  std::string status = params.value("status", "online");
  
  nlohmann::json presence = {
    {"status", status}
  };
  
  if (params.contains("activity_type") && params.contains("activity_name")) {
    std::string activity_type = params["activity_type"];
    std::string activity_name = params["activity_name"];
    
    nlohmann::json activity = {
      {"name", activity_name},
      {"type", activity_type_to_int(activity_type)}
    };
    
    if (params.contains("url")) {
      activity["url"] = params["url"].get<std::string>();
    }
    
    presence["activity"] = activity;
  }
  
  // 注意: 实际实现需要通过 WebSocket Gateway 发送 Presence Update
  // 这里返回构建的 presence 对象作为示例
  
  return {
    {"status", "success"},
    {"presence", presence},
    {"note", "Presence update requires Gateway WebSocket (not implemented in this demo)"}
  };
}

// ==================== 辅助方法实现 ====================

bool DiscordTool::validate_token(const std::string& token) {
  return !token.empty() && token.find("Bot ") != std::string::npos;
}

DiscordTool::DiscordTarget DiscordTool::parse_target(const std::string& to) {
  DiscordTarget target;
  
  // 格式: channel:ID, user:ID, guild:ID#channel
  if (to.find("channel:") == 0) {
    target.type = "channel";
    target.id = to.substr(8);
  } else if (to.find("user:") == 0) {
    target.type = "user";
    target.id = to.substr(5);
  } else if (to.find("guild:") == 0) {
    target.type = "guild";
    size_t hash_pos = to.find('#');
    if (hash_pos != std::string::npos) {
      target.id = to.substr(6, hash_pos - 6);
      target.channel_name = to.substr(hash_pos + 1);
    } else {
      target.id = to.substr(6);
    }
  } else {
    // 默认作为 channel ID
    target.type = "channel";
    target.id = to;
  }
  
  return target;
}

std::string DiscordTool::build_api_url(const std::string& endpoint) {
  return impl_->api_base + endpoint;
}

nlohmann::json DiscordTool::send_request(const std::string& method,
                                          const std::string& endpoint,
                                          const nlohmann::json& body) {
  CURL* curl = curl_easy_init();
  if (!curl) {
    throw std::runtime_error("Failed to initialize CURL");
  }
  
  std::string url = build_api_url(endpoint);
  std::string response_string;
  
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, Impl::write_callback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_string);
  
  // 设置 Authorization Header
  struct curl_slist* headers = nullptr;
  std::string auth_header = "Authorization: Bot " + impl_->bot_token;
  headers = curl_slist_append(headers, auth_header.c_str());
  headers = curl_slist_append(headers, "Content-Type: application/json");
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  
  // 设置 HTTP 方法
  if (method == "POST") {
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    std::string body_str = body.dump();
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body_str.c_str());
  } else if (method == "PUT") {
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
    if (!body.empty()) {
      std::string body_str = body.dump();
      curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body_str.c_str());
    }
  } else if (method == "PATCH") {
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PATCH");
    std::string body_str = body.dump();
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body_str.c_str());
  } else if (method == "DELETE") {
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");
  }
  
  // 执行请求
  CURLcode res = curl_easy_perform(curl);
  
  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);
  
  if (res != CURLE_OK) {
    throw std::runtime_error(std::string("CURL request failed: ") + curl_easy_strerror(res));
  }
  
  // 解析响应
  if (response_string.empty()) {
    return nlohmann::json::object();
  }
  
  return nlohmann::json::parse(response_string);
}

nlohmann::json DiscordTool::format_embeds(const nlohmann::json& embeds) {
  // 验证并格式化 embeds 数组
  // OpenClaw 对齐: src/discord/send.shared.ts (resolveDiscordSendEmbeds)
  
  if (!embeds.is_array()) {
    return nlohmann::json::array();
  }
  
  nlohmann::json formatted = nlohmann::json::array();
  for (const auto& embed : embeds) {
    nlohmann::json formatted_embed;
    
    if (embed.contains("title")) formatted_embed["title"] = embed["title"];
    if (embed.contains("description")) formatted_embed["description"] = embed["description"];
    if (embed.contains("url")) formatted_embed["url"] = embed["url"];
    if (embed.contains("color")) formatted_embed["color"] = embed["color"];
    if (embed.contains("timestamp")) formatted_embed["timestamp"] = embed["timestamp"];
    
    // Footer, Image, Thumbnail, Author, Fields...
    if (embed.contains("footer")) formatted_embed["footer"] = embed["footer"];
    if (embed.contains("image")) formatted_embed["image"] = embed["image"];
    if (embed.contains("thumbnail")) formatted_embed["thumbnail"] = embed["thumbnail"];
    if (embed.contains("author")) formatted_embed["author"] = embed["author"];
    if (embed.contains("fields")) formatted_embed["fields"] = embed["fields"];
    
    formatted.push_back(formatted_embed);
  }
  
  return formatted;
}

nlohmann::json DiscordTool::format_components(const nlohmann::json& components) {
  // 验证并格式化 components 数组
  // OpenClaw 对齐: src/discord/send.shared.ts (resolveDiscordSendComponents)
  
  if (!components.is_array()) {
    return nlohmann::json::array();
  }
  
  // 简化: 直接返回 components (实际应该验证结构)
  return components;
}

DiscordTool::EmojiInfo DiscordTool::parse_emoji(const std::string& emoji) {
  EmojiInfo info;
  
  // 格式: custom:name:id 或 Unicode
  if (emoji.find("custom:") == 0) {
    info.type = "custom";
    size_t first_colon = emoji.find(':', 7);
    if (first_colon != std::string::npos) {
      info.name = emoji.substr(7, first_colon - 7);
      info.id = emoji.substr(first_colon + 1);
    }
  } else {
    info.type = "unicode";
    info.value = emoji;
  }
  
  return info;
}

std::string DiscordTool::truncate_thread_name(const std::string& name) {
  if (name.length() <= DISCORD_THREAD_NAME_LIMIT) {
    return name;
  }
  return name.substr(0, DISCORD_THREAD_NAME_LIMIT);
}

std::optional<DiscordTool::WebhookInfo> DiscordTool::resolve_webhook(const std::string& thread_id) {
  // 简化: 实际应该从 ThreadBindingManager 查询
  // OpenClaw 对齐: src/discord/monitor/thread-bindings.ts
  return std::nullopt;
}

int DiscordTool::activity_type_to_int(const std::string& type) {
  // Discord Activity Types:
  // 0 = Playing, 1 = Streaming, 2 = Listening, 3 = Watching, 5 = Competing
  if (type == "playing") return 0;
  if (type == "streaming") return 1;
  if (type == "listening") return 2;
  if (type == "watching") return 3;
  if (type == "competing") return 5;
  return 0;  // default
}

} // namespace tools
} // namespace hunterclaw
