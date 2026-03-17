#include "hunterclaw/tools/browser_tool.h"
#include "hunterclaw/core/utils.h"
#include "hunterclaw/infra/http_client.h"
#include <spdlog/spdlog.h>
#include <regex>
#include <fstream>

namespace hunterclaw {
namespace tools {

namespace {
    auto log() {
        auto logger = spdlog::get("browser");
        if (!logger) {
            logger = spdlog::stdout_logger_mt("browser_tool");
        }
        return logger;
    }
    
    // OpenClaw 常量对齐
    const int DEFAULT_BROWSER_TIMEOUT_MS = 20000;
    const int BROWSER_PROXY_TIMEOUT_SLACK_MS = 5000;
    const int DEFAULT_AI_SNAPSHOT_MAX_CHARS = 100000;
    const char* DEFAULT_UPLOAD_DIR = "~/.openclaw/uploads";
}

// ===== Constructor =====

BrowserTool::BrowserTool(const ToolContext& context) 
    : context_(context) {}

std::string BrowserTool::name() const {
    return "browser";
}

std::string BrowserTool::description() const {
    std::stringstream desc;
    desc << "Control the browser via OpenClaw's browser control server "
         << "(status/start/stop/profiles/tabs/open/snapshot/screenshot/actions). "
         << "\n\n**Profiles**: "
         << "\n- profile=\"chrome\": Chrome extension relay takeover (existing Chrome tabs) "
         << "\n- profile=\"openclaw\": Isolated openclaw-managed browser "
         << "\n\n**Chrome Extension**: "
         << "If user mentions Chrome extension/Browser Relay/toolbar button/\"attach tab\", "
         << "ALWAYS use profile=\"chrome\" (do not ask). "
         << "Needs attached tab: user must click OpenClaw Browser Relay toolbar icon (badge ON). "
         << "\n\n**Refs & Stability**: "
         << "When using refs from snapshot (e.g. e12), keep same tab: pass targetId into subsequent actions. "
         << "For stable, self-resolving refs, use snapshot with refs=\"aria\" (Playwright aria-ref ids). "
         << "Default refs=\"role\" are role+name-based. "
         << "\n\n**UI Automation**: "
         << "Use snapshot+act for automation. Avoid act:wait by default; use only when no reliable UI state. "
         << "\n\n**Target Selection**: "
         << "- target=\"sandbox\": Sandbox browser (requires sandbox.browser.enabled) "
         << "- target=\"host\": Host browser (requires browser.enabled=true) "
         << "- target=\"node\": Node-hosted browser proxy (auto-routes to connected node) "
         << "\n\n**Node Routing**: "
         << "When node browser proxy available, tool may auto-route. Pin node with node=<id|name>. ";
    
    return desc.str();
}

Schema BrowserTool::parameters() const {
    Schema schema;
    schema.type = "object";
    schema.description = "Browser tool parameters (action-specific)";
    
    // action (required)
    auto action_prop = core::JsonValue::object({
        {"type", "string"},
        {"description", "Browser action to perform"},
        {"enum", core::JsonValue::array({
            "status", "start", "stop", "profiles", "tabs", "open", "focus", "close",
            "snapshot", "screenshot", "navigate", "console", "pdf", "upload", "dialog", "act"
        })}
    });
    schema.properties["action"] = action_prop;
    
    // target (optional)
    auto target_prop = core::JsonValue::object({
        {"type", "string"},
        {"description", "Browser location: sandbox, host, or node"},
        {"enum", core::JsonValue::array({"sandbox", "host", "node"})}
    });
    schema.properties["target"] = target_prop;
    
    // profile (optional)
    auto profile_prop = core::JsonValue::object({
        {"type", "string"},
        {"description", "Browser profile: chrome (extension relay) or openclaw (managed)"}
    });
    schema.properties["profile"] = profile_prop;
    
    // node (optional)
    auto node_prop = core::JsonValue::object({
        {"type", "string"},
        {"description", "Node ID or name (for target=node)"}
    });
    schema.properties["node"] = node_prop;
    
    // targetUrl (for open/navigate)
    auto url_prop = core::JsonValue::object({
        {"type", "string"},
        {"description", "Target URL to open or navigate to"}
    });
    schema.properties["targetUrl"] = url_prop;
    schema.properties["url"] = url_prop;  // Alias
    
    // targetId (for tab-specific actions)
    auto target_id_prop = core::JsonValue::object({
        {"type", "string"},
        {"description", "Browser tab target ID"}
    });
    schema.properties["targetId"] = target_id_prop;
    
    // Snapshot 参数
    auto format_prop = core::JsonValue::object({
        {"type", "string"},
        {"enum", core::JsonValue::array({"aria", "ai"})}
    });
    schema.properties["snapshotFormat"] = format_prop;
    
    auto refs_prop = core::JsonValue::object({
        {"type", "string"},
        {"enum", core::JsonValue::array({"role", "aria"})}
    });
    schema.properties["refs"] = refs_prop;
    
    auto mode_prop = core::JsonValue::object({
        {"type", "string"},
        {"enum", core::JsonValue::array({"efficient"})}
    });
    schema.properties["mode"] = mode_prop;
    
    schema.properties["maxChars"] = core::JsonValue::object({{"type", "integer"}});
    schema.properties["labels"] = core::JsonValue::object({{"type", "boolean"}});
    schema.properties["interactive"] = core::JsonValue::object({{"type", "boolean"}});
    schema.properties["compact"] = core::JsonValue::object({{"type", "boolean"}});
    schema.properties["depth"] = core::JsonValue::object({{"type", "integer"}});
    schema.properties["selector"] = core::JsonValue::object({{"type", "string"}});
    schema.properties["frame"] = core::JsonValue::object({{"type", "string"}});
    
    // Screenshot 参数
    schema.properties["fullPage"] = core::JsonValue::object({{"type", "boolean"}});
    schema.properties["ref"] = core::JsonValue::object({{"type", "string"}});
    schema.properties["element"] = core::JsonValue::object({{"type", "string"}});
    schema.properties["type"] = core::JsonValue::object({
        {"type", "string"},
        {"enum", core::JsonValue::array({"png", "jpeg"})}
    });
    
    // Console 参数
    schema.properties["level"] = core::JsonValue::object({{"type", "string"}});
    
    // Upload 参数
    schema.properties["paths"] = core::JsonValue::object({
        {"type", "array"},
        {"items", core::JsonValue::object({{"type", "string"}})}
    });
    schema.properties["inputRef"] = core::JsonValue::object({{"type", "string"}});
    
    // Dialog 参数
    schema.properties["accept"] = core::JsonValue::object({{"type", "boolean"}});
    schema.properties["promptText"] = core::JsonValue::object({{"type", "string"}});
    schema.properties["timeoutMs"] = core::JsonValue::object({{"type", "integer"}});
    
    // Act 参数 (11 种 kind)
    schema.properties["kind"] = core::JsonValue::object({
        {"type", "string"},
        {"enum", core::JsonValue::array({
            "click", "type", "press", "hover", "drag", "select", 
            "fill", "resize", "wait", "evaluate", "close"
        })}
    });
    
    // Act 子参数 (扁平化,避免 anyOf)
    schema.properties["text"] = core::JsonValue::object({{"type", "string"}});
    schema.properties["key"] = core::JsonValue::object({{"type", "string"}});
    schema.properties["submit"] = core::JsonValue::object({{"type", "boolean"}});
    schema.properties["slowly"] = core::JsonValue::object({{"type", "boolean"}});
    schema.properties["doubleClick"] = core::JsonValue::object({{"type", "boolean"}});
    schema.properties["button"] = core::JsonValue::object({{"type", "string"}});
    schema.properties["modifiers"] = core::JsonValue::object({
        {"type", "array"},
        {"items", core::JsonValue::object({{"type", "string"}})}
    });
    schema.properties["startRef"] = core::JsonValue::object({{"type", "string"}});
    schema.properties["endRef"] = core::JsonValue::object({{"type", "string"}});
    schema.properties["values"] = core::JsonValue::object({
        {"type", "array"},
        {"items", core::JsonValue::object({{"type", "string"}})}
    });
    schema.properties["fields"] = core::JsonValue::object({
        {"type", "array"},
        {"items", core::JsonValue::object({}, true)}  // additionalProperties: true
    });
    schema.properties["width"] = core::JsonValue::object({{"type", "integer"}});
    schema.properties["height"] = core::JsonValue::object({{"type", "integer"}});
    schema.properties["timeMs"] = core::JsonValue::object({{"type", "integer"}});
    schema.properties["textGone"] = core::JsonValue::object({{"type", "string"}});
    schema.properties["loadState"] = core::JsonValue::object({{"type", "string"}});
    schema.properties["fn"] = core::JsonValue::object({{"type", "string"}});
    schema.properties["delayMs"] = core::JsonValue::object({{"type", "integer"}});
    
    // request (嵌套 act 请求,推荐)
    schema.properties["request"] = core::JsonValue::object({{"type", "object"}});
    
    schema.required = {"action"};
    
    return schema;
}

ToolResult BrowserTool::execute(const core::JsonValue& params) {
    ToolParams reader(params);
    std::string action = reader.read_string("action", true);
    std::string profile = reader.read_string("profile", false, "openclaw");
    std::string target = reader.read_string("target", false, "");
    std::string requested_node = reader.read_string("node", false, "");
    
    // 特殊逻辑: profile="chrome" 默认使用 host
    if (profile == "chrome" && target.empty() && requested_node.empty()) {
        target = "host";
    }
    
    log()->info("[BrowserTool] Action: {}, Profile: {}, Target: {}", action, profile, target);
    
    // 解析 Node 目标
    auto node_target = resolve_browser_node_target(requested_node, target, context_.sandbox_bridge_url);
    
    // 解析 Base URL (sandbox vs host)
    std::optional<std::string> base_url;
    if (!node_target.has_value()) {
        base_url = resolve_browser_base_url(target, context_.sandbox_bridge_url);
    }
    
    // 路由到对应的 Action
    try {
        if (action == "status") {
            return action_status(base_url, node_target, profile);
        } else if (action == "start") {
            return action_start(base_url, node_target, profile);
        } else if (action == "stop") {
            return action_stop(base_url, node_target, profile);
        } else if (action == "profiles") {
            return action_profiles(base_url, node_target);
        } else if (action == "tabs") {
            return action_tabs(base_url, node_target, profile);
        } else if (action == "open") {
            std::string url = reader.read_string("targetUrl", false, 
                                reader.read_string("url", true));
            return action_open(base_url, node_target, profile, url);
        } else if (action == "focus") {
            std::string target_id = reader.read_string("targetId", true);
            return action_focus(base_url, node_target, profile, target_id);
        } else if (action == "close") {
            std::string target_id = reader.read_string("targetId", false, "");
            return action_close(base_url, node_target, profile, target_id);
        } else if (action == "navigate") {
            std::string url = reader.read_string("targetUrl", false, 
                                reader.read_string("url", true));
            std::string target_id = reader.read_string("targetId", false, "");
            return action_navigate(base_url, node_target, profile, url, target_id);
        } else if (action == "snapshot") {
            return action_snapshot(params, base_url, node_target, profile);
        } else if (action == "screenshot") {
            return action_screenshot(params, base_url, node_target, profile);
        } else if (action == "console") {
            return action_console(params, base_url, node_target, profile);
        } else if (action == "pdf") {
            return action_pdf(params, base_url, node_target, profile);
        } else if (action == "act") {
            return action_act(params, base_url, node_target, profile);
        } else if (action == "upload") {
            return action_upload(params, base_url, node_target, profile);
        } else if (action == "dialog") {
            return action_dialog(params, base_url, node_target, profile);
        } else {
            return tool_result::error("Unknown browser action: " + action);
        }
    } catch (const std::exception& e) {
        log()->error("[BrowserTool] Action '{}' failed: {}", action, e.what());
        return tool_result::error(std::string("Browser action failed: ") + e.what());
    }
}

bool BrowserTool::requires_approval() const {
    return false;  // Browser 操作一般无需审批 (观察为主)
}

// ===== 连接管理 =====

std::optional<std::string> BrowserTool::resolve_browser_base_url(
    const std::string& target,
    const std::string& sandbox_bridge_url
) const {
    std::string normalized_target = target.empty() ? 
        (sandbox_bridge_url.empty() ? "host" : "sandbox") : target;
    
    if (normalized_target == "sandbox") {
        if (sandbox_bridge_url.empty()) {
            throw std::runtime_error(
                "Sandbox browser unavailable. Enable agents.defaults.sandbox.browser.enabled "
                "or use target=\"host\"");
        }
        // 去除尾部斜杠
        std::string url = sandbox_bridge_url;
        if (url.back() == '/') {
            url.pop_back();
        }
        return url;
    }
    
    if (normalized_target == "host") {
        // TODO: 读取配置 browser.enabled
        // 暂时返回默认 localhost:8765
        return "http://localhost:8765";
    }
    
    return std::nullopt;  // target="node"
}

std::optional<BrowserTool::BrowserNodeTarget> BrowserTool::resolve_browser_node_target(
    const std::string& requested_node,
    const std::string& target,
    const std::string& sandbox_bridge_url
) const {
    // TODO: 实现完整的 Node 路由逻辑
    // 1. 检查 gateway.nodes.browser.mode (auto, manual, off)
    // 2. 调用 listNodes() 获取支持 browser 的节点
    // 3. 根据策略选择节点 (requested_node, policy.node, selectDefaultNode)
    
    if (target == "node" || !requested_node.empty()) {
        // 暂时返回 mock 节点
        log()->warn("[BrowserTool] Node routing not yet implemented, falling back to host");
        return std::nullopt;
    }
    
    return std::nullopt;
}

// ===== Browser 状态管理 =====

ToolResult BrowserTool::action_status(
    const std::optional<std::string>& base_url,
    const std::optional<BrowserNodeTarget>& node_target,
    const std::string& profile
) {
    log()->info("[BrowserTool] GET /status (profile={})", profile);
    
    if (node_target.has_value()) {
        // Node 代理请求
        auto result = call_node_browser_proxy(
            node_target->node_id, "GET", "/", 
            core::JsonValue::object({{"profile", profile}}),
            core::JsonValue::object(), DEFAULT_BROWSER_TIMEOUT_MS);
        return tool_result::json(result);
    }
    
    if (!base_url.has_value()) {
        return tool_result::error("Browser base URL not configured");
    }
    
    std::string url = base_url.value() + "/?profile=" + profile;
    auto result = http_get(url);
    
    return tool_result::json(result);
}

ToolResult BrowserTool::action_start(
    const std::optional<std::string>& base_url,
    const std::optional<BrowserNodeTarget>& node_target,
    const std::string& profile
) {
    log()->info("[BrowserTool] POST /start (profile={})", profile);
    
    if (node_target.has_value()) {
        call_node_browser_proxy(
            node_target->node_id, "POST", "/start",
            core::JsonValue::object({{"profile", profile}}),
            core::JsonValue::object(), DEFAULT_BROWSER_TIMEOUT_MS);
        
        // 返回状态
        auto result = call_node_browser_proxy(
            node_target->node_id, "GET", "/",
            core::JsonValue::object({{"profile", profile}}),
            core::JsonValue::object(), DEFAULT_BROWSER_TIMEOUT_MS);
        return tool_result::json(result);
    }
    
    if (!base_url.has_value()) {
        return tool_result::error("Browser base URL not configured");
    }
    
    std::string url = base_url.value() + "/start?profile=" + profile;
    http_post(url, core::JsonValue::object());
    
    // 返回状态
    std::string status_url = base_url.value() + "/?profile=" + profile;
    auto result = http_get(status_url);
    
    return tool_result::json(result);
}

ToolResult BrowserTool::action_stop(
    const std::optional<std::string>& base_url,
    const std::optional<BrowserNodeTarget>& node_target,
    const std::string& profile
) {
    log()->info("[BrowserTool] POST /stop (profile={})", profile);
    
    if (node_target.has_value()) {
        call_node_browser_proxy(
            node_target->node_id, "POST", "/stop",
            core::JsonValue::object({{"profile", profile}}),
            core::JsonValue::object(), DEFAULT_BROWSER_TIMEOUT_MS);
        
        auto result = call_node_browser_proxy(
            node_target->node_id, "GET", "/",
            core::JsonValue::object({{"profile", profile}}),
            core::JsonValue::object(), DEFAULT_BROWSER_TIMEOUT_MS);
        return tool_result::json(result);
    }
    
    if (!base_url.has_value()) {
        return tool_result::error("Browser base URL not configured");
    }
    
    std::string url = base_url.value() + "/stop?profile=" + profile;
    http_post(url, core::JsonValue::object());
    
    std::string status_url = base_url.value() + "/?profile=" + profile;
    auto result = http_get(status_url);
    
    return tool_result::json(result);
}

ToolResult BrowserTool::action_profiles(
    const std::optional<std::string>& base_url,
    const std::optional<BrowserNodeTarget>& node_target
) {
    log()->info("[BrowserTool] GET /profiles");
    
    if (node_target.has_value()) {
        auto result = call_node_browser_proxy(
            node_target->node_id, "GET", "/profiles",
            core::JsonValue::object(), core::JsonValue::object(), 
            DEFAULT_BROWSER_TIMEOUT_MS);
        return tool_result::json(result);
    }
    
    if (!base_url.has_value()) {
        return tool_result::error("Browser base URL not configured");
    }
    
    std::string url = base_url.value() + "/profiles";
    auto result = http_get(url);
    
    return tool_result::json(result);
}

// ===== HTTP 请求辅助 (完整实现) =====

core::JsonValue BrowserTool::http_get(const std::string& url) const {
    log()->info("[BrowserTool] HTTP GET: {}", url);
    
    auto client = infra::HttpClient::create();
    auto result = client->get(url, DEFAULT_BROWSER_TIMEOUT_MS);
    
    if (result.is_err()) {
        log()->error("[BrowserTool] HTTP GET failed: {}", result.error());
        return core::JsonValue::object({
            {"ok", false},
            {"error", result.error()}
        });
    }
    
    auto& response = result.value();
    if (!response.ok) {
        log()->warn("[BrowserTool] HTTP GET returned {}", response.status_code);
        return core::JsonValue::object({
            {"ok", false},
            {"status", response.status_code},
            {"body", response.body}
        });
    }
    
    // 解析 JSON 响应
    auto json_result = response.json();
    if (json_result.is_err()) {
        log()->error("[BrowserTool] Failed to parse JSON: {}", json_result.error());
        return core::JsonValue::object({
            {"ok", false},
            {"error", "Invalid JSON response"},
            {"body", response.body}
        });
    }
    
    return json_result.value();
}

core::JsonValue BrowserTool::http_post(const std::string& url, const core::JsonValue& body) const {
    log()->info("[BrowserTool] HTTP POST: {}", url);
    
    auto client = infra::HttpClient::create();
    auto result = client->post(url, body, DEFAULT_BROWSER_TIMEOUT_MS);
    
    if (result.is_err()) {
        log()->error("[BrowserTool] HTTP POST failed: {}", result.error());
        return core::JsonValue::object({
            {"ok", false},
            {"error", result.error()}
        });
    }
    
    auto& response = result.value();
    if (!response.ok) {
        log()->warn("[BrowserTool] HTTP POST returned {}", response.status_code);
        return core::JsonValue::object({
            {"ok", false},
            {"status", response.status_code},
            {"body", response.body}
        });
    }
    
    // 解析 JSON 响应
    auto json_result = response.json();
    if (json_result.is_err()) {
        // POST 可能没有返回 JSON body
        return core::JsonValue::object({
            {"ok", true},
            {"status", response.status_code}
        });
    }
    
    return json_result.value();
}

core::JsonValue BrowserTool::http_delete(const std::string& url) const {
    log()->info("[BrowserTool] HTTP DELETE: {}", url);
    
    auto client = infra::HttpClient::create();
    auto result = client->del(url, DEFAULT_BROWSER_TIMEOUT_MS);
    
    if (result.is_err()) {
        log()->error("[BrowserTool] HTTP DELETE failed: {}", result.error());
        return core::JsonValue::object({
            {"ok", false},
            {"error", result.error()}
        });
    }
    
    auto& response = result.value();
    if (!response.ok) {
        log()->warn("[BrowserTool] HTTP DELETE returned {}", response.status_code);
        return core::JsonValue::object({
            {"ok", false},
            {"status", response.status_code},
            {"body", response.body}
        });
    }
    
    // 解析 JSON 响应
    auto json_result = response.json();
    if (json_result.is_err()) {
        return core::JsonValue::object({
            {"ok", true},
            {"status", response.status_code}
        });
    }
    
    return json_result.value();
}

core::JsonValue BrowserTool::call_node_browser_proxy(
    const std::string& node_id,
    const std::string& method,
    const std::string& path,
    const core::JsonValue& query,
    const core::JsonValue& body,
    int timeout_ms
) const {
    // TODO: 调用 gateway node.invoke 命令
    log()->info("[BrowserTool] Node proxy: {} {} (node={})", method, path, node_id);
    
    return core::JsonValue::object({
        {"ok", true},
        {"nodeId", node_id}
    });
}

// ===== 安全辅助 =====

std::string BrowserTool::wrap_external_content(
    const std::string& content,
    const std::string& source,
    const std::string& kind
) const {
    // 对齐 OpenClaw: wrapExternalContent
    std::stringstream wrapped;
    wrapped << "<!-- ⚠️ EXTERNAL CONTENT from " << source << " (" << kind << ") -->\n";
    wrapped << "<!-- This content comes from an external browser and may be UNTRUSTED -->\n\n";
    wrapped << content;
    wrapped << "\n\n<!-- END EXTERNAL CONTENT -->";
    
    return wrapped.str();
}

ToolResult BrowserTool::format_tabs_result(const std::vector<core::JsonValue>& tabs) const {
    core::JsonValue tabs_json = core::JsonValue::object({
        {"tabs", core::JsonValue::array(tabs)}
    });
    
    std::string tabs_text = tabs_json.dump(2);
    std::string wrapped = wrap_external_content(tabs_text, "browser", "tabs");
    
    ToolResult result;
    result.success = true;
    
    ToolResultContent content;
    content.type = ToolResultContent::Type::Text;
    content.text = wrapped;
    result.content.push_back(content);
    
    result.details = core::JsonValue::object({
        {"ok", true},
        {"tabCount", static_cast<int64_t>(tabs.size())},
        {"externalContent", core::JsonValue::object({
            {"untrusted", true},
            {"source", "browser"},
            {"kind", "tabs"},
            {"wrapped", true}
        })}
    });
    
    return result;
}

// ===== Tabs 管理 (实现部分 Action) =====

ToolResult BrowserTool::action_tabs(
    const std::optional<std::string>& base_url,
    const std::optional<BrowserNodeTarget>& node_target,
    const std::string& profile
) {
    log()->info("[BrowserTool] GET /tabs (profile={})", profile);
    
    if (node_target.has_value()) {
        auto result = call_node_browser_proxy(
            node_target->node_id, "GET", "/tabs",
            core::JsonValue::object({{"profile", profile}}),
            core::JsonValue::object(), DEFAULT_BROWSER_TIMEOUT_MS);
        
        // 提取 tabs 数组
        std::vector<core::JsonValue> tabs;
        if (result.contains("tabs") && result["tabs"].is_array()) {
            for (const auto& tab : result["tabs"].items()) {
                tabs.push_back(tab.value());
            }
        }
        return format_tabs_result(tabs);
    }
    
    if (!base_url.has_value()) {
        return tool_result::error("Browser base URL not configured");
    }
    
    std::string url = base_url.value() + "/tabs?profile=" + profile;
    auto result = http_get(url);
    
    // Mock tabs
    std::vector<core::JsonValue> tabs = {
        core::JsonValue::object({
            {"targetId", "tab-1"},
            {"url", "https://example.com"},
            {"title", "Example Domain"}
        })
    };
    
    return format_tabs_result(tabs);
}

ToolResult BrowserTool::action_open(
    const std::optional<std::string>& base_url,
    const std::optional<BrowserNodeTarget>& node_target,
    const std::string& profile,
    const std::string& target_url
) {
    log()->info("[BrowserTool] POST /tabs/open (profile={}, url={})", profile, target_url);
    
    if (node_target.has_value()) {
        auto result = call_node_browser_proxy(
            node_target->node_id, "POST", "/tabs/open",
            core::JsonValue::object({{"profile", profile}}),
            core::JsonValue::object({{"url", target_url}}),
            DEFAULT_BROWSER_TIMEOUT_MS);
        return tool_result::json(result);
    }
    
    if (!base_url.has_value()) {
        return tool_result::error("Browser base URL not configured");
    }
    
    std::string url = base_url.value() + "/tabs/open?profile=" + profile;
    auto body = core::JsonValue::object({{"url", target_url}});
    auto result = http_post(url, body);
    
    // Mock 响应
    result["targetId"] = "tab-new";
    result["url"] = target_url;
    
    return tool_result::json(result);
}

// ... (其他 Action 实现省略,结构相同)

// ===== Factory =====

std::shared_ptr<Tool> BrowserToolFactory::create(const ToolContext& context) {
    return std::make_shared<BrowserTool>(context);
}

} // namespace tools
} // namespace hunterclaw
