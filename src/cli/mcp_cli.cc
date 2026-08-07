// 🎯 对齐 OpenClaw src/cli/mcp-cli.ts 的 mcp login / mcp logout 子命令。
//   校验顺序、错误文案、两步式提示均严格对齐 openclaw。

#include "hunterclaw/cli/mcp_cli.h"
#include "hunterclaw/mcp/mcp_config.h"
#include "hunterclaw/mcp/mcp_oauth.h"
#include "hunterclaw/mcp/mcp_oauth_authorization.h"

#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace hunterclaw {
namespace cli {

namespace {
using json = nlohmann::json;

void print_line(const std::string& s) { std::cout << s << std::endl; }
int fail(const std::string& s) {
    std::cerr << "\033[31mError:\033[0m " << s << std::endl;
    return 1;
}

// 从原始 server json 读取字符串字段（对齐 openclaw server.url / server.oauth.*）。
std::optional<std::string> str_field(const json& obj, const char* key) {
    if (obj.is_object() && obj.contains(key) && obj[key].is_string()) {
        std::string v = obj[key].get<std::string>();
        if (!v.empty()) return v;
    }
    return std::nullopt;
}

// 是否 auth:"oauth"（或 oauth:true）。
bool is_oauth_server(const json& server) {
    if (server.is_object()) {
        if (server.contains("auth") && server["auth"].is_string() &&
            server["auth"].get<std::string>() == "oauth")
            return true;
        if (server.contains("oauth") && server["oauth"].is_boolean() &&
            server["oauth"].get<bool>())
            return true;
        // server.oauth 为对象时也视为启用（对齐 McpOAuthConfig 存在即启用）。
        if (server.contains("oauth") && server["oauth"].is_object()) return true;
    }
    return false;
}

// 从 server.oauth 对象抽取登录配置字段。
void fill_oauth_config(const json& server, mcp::McpOAuthLoginConfig& cfg) {
    if (!server.is_object() || !server.contains("oauth") || !server["oauth"].is_object()) return;
    const json& o = server["oauth"];
    cfg.scope = str_field(o, "scope");
    cfg.redirect_url = str_field(o, "redirectUrl");
    cfg.client_id = str_field(o, "clientId");
    cfg.authorization_server_url = str_field(o, "authorizationServerUrl");
    cfg.registration_endpoint = str_field(o, "registrationEndpoint");
    cfg.client_name = str_field(o, "clientName");
}

// mcp login <name> [--code <code>]（对齐 mcp-cli.ts login action）。
int cmd_login(const std::vector<std::string>& args) {
    std::string name;
    std::optional<std::string> code;
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--code" && i + 1 < args.size()) {
            code = args[++i];
        } else if (name.empty() && args[i].rfind("-", 0) != 0) {
            name = args[i];
        }
    }
    if (name.empty()) return fail("Usage: hunterclaw mcp login <name> [--code <code>]");

    auto loaded = mcp::list_configured_mcp_servers();
    if (!loaded.ok) return fail(loaded.error);
    auto it = loaded.mcp_servers.find(name);
    if (it == loaded.mcp_servers.end()) {
        return fail("No MCP server named \"" + name + "\" in " + loaded.path +
                    ". Run `hunterclaw mcp list` to see configured servers.");
    }
    const json& server = it->second;
    if (!is_oauth_server(server)) {
        return fail("MCP server \"" + name + "\" is not configured with auth: \"oauth\".");
    }
    auto url = str_field(server, "url");
    if (!url.has_value()) {
        return fail("MCP server \"" + name + "\" needs a URL for OAuth login.");
    }

    mcp::McpOAuthLoginConfig cfg;
    cfg.server_name = name;
    cfg.server_url = *url;
    fill_oauth_config(server, cfg);

    auto result = mcp::run_mcp_oauth_login(
        cfg, code, [&name](const std::string& authorization_url) {
            print_line("Open this URL to authorize \"" + name + "\":");
            print_line(authorization_url);
            print_line("After approval, run `hunterclaw mcp login " + name + " --code <code>`.");
        });
    if (result.is_err()) return fail(result.error);
    if (result.get() == mcp::McpOAuthLoginResult::Authorized) {
        print_line("MCP OAuth credentials saved for \"" + name + "\".");
    }
    return 0;
}

// mcp logout <name>（对齐 mcp-cli.ts logout action）。
int cmd_logout(const std::vector<std::string>& args) {
    std::string name;
    for (const auto& a : args) {
        if (a.rfind("-", 0) != 0) { name = a; break; }
    }
    if (name.empty()) return fail("Usage: hunterclaw mcp logout <name>");

    auto loaded = mcp::list_configured_mcp_servers();
    if (!loaded.ok) return fail(loaded.error);
    auto it = loaded.mcp_servers.find(name);
    if (it == loaded.mcp_servers.end()) {
        return fail("No MCP server named \"" + name + "\" in " + loaded.path +
                    ". Run `hunterclaw mcp list` to see configured servers.");
    }
    auto url = str_field(it->second, "url");
    if (!url.has_value()) {
        return fail("MCP server \"" + name + "\" needs a URL for OAuth logout.");
    }
    auto cleared = mcp::clear_mcp_oauth_credentials(name, *url);
    if (cleared.is_err()) return fail(cleared.error);
    print_line("MCP OAuth credentials cleared for \"" + name + "\".");
    return 0;
}

} // namespace

int dispatch_mcp_cli(int argc, char** argv) {
    // argv: [prog, "mcp", <subcommand>, ...]
    if (argc < 3) {
        std::cerr << "Usage: hunterclaw mcp <login|logout> <name> [options]" << std::endl;
        return 1;
    }
    std::string sub = argv[2];
    std::vector<std::string> rest;
    for (int i = 3; i < argc; ++i) rest.push_back(argv[i]);

    if (sub == "login") return cmd_login(rest);
    if (sub == "logout") return cmd_logout(rest);

    std::cerr << "Unknown mcp subcommand: " << sub << std::endl;
    std::cerr << "Available: login, logout" << std::endl;
    return 1;
}

} // namespace cli
} // namespace hunterclaw
