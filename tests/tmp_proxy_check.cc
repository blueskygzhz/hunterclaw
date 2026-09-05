// 临时验证：proxy env 键上报 + 摘要文案（严格对齐真源）
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

static int g_fail = 0;
static void check(bool ok, const char* name, const std::string& detail = "") {
    std::printf("%-58s %s\n", name, ok ? "PASS" : "FAIL");
    if (!ok) {
        if (!detail.empty()) std::printf("     %s\n", detail.c_str());
        ++g_fail;
    }
}

// 复刻 agent.cc
std::vector<std::string> resolve_present_proxy_env_keys() {
    static const char* kProxyEnvKeys[] = {
        "HTTP_PROXY", "HTTPS_PROXY", "ALL_PROXY",
        "http_proxy", "https_proxy", "all_proxy",
    };
    std::vector<std::string> present;
    for (const char* key : kProxyEnvKeys) {
        const char* value = std::getenv(key);
        if (value == nullptr) continue;
        std::string v(value);
        if (v.find_first_not_of(" \t\r\n") == std::string::npos) continue;
        present.emplace_back(key);
    }
    return present;
}

// 复刻 acp_spawn.cc
std::string format_proxy_env_summary(const std::vector<std::string>& keys) {
    if (keys.empty()) return "proxy env: none";
    std::string out = "proxy env: ";
    for (size_t i = 0; i < keys.size(); ++i) {
        if (i > 0) out += ", ";
        out += keys[i];
    }
    return out;
}

int main() {
    // 清空全部，确保基线干净
    for (const char* k : {"HTTP_PROXY", "HTTPS_PROXY", "ALL_PROXY",
                          "http_proxy", "https_proxy", "all_proxy"}) {
        unsetenv(k);
    }
    check(resolve_present_proxy_env_keys().empty(), "T1 no proxy env set -> empty list");
    check(format_proxy_env_summary({}) == "proxy env: none",
          "T2 empty -> 'proxy env: none' (aligned, not 'no proxy env')",
          format_proxy_env_summary({}));

    // 设一个
    setenv("HTTPS_PROXY", "http://127.0.0.1:8080", 1);
    {
        auto keys = resolve_present_proxy_env_keys();
        check(keys.size() == 1 && keys[0] == "HTTPS_PROXY",
              "T3 single proxy key detected",
              "n=" + std::to_string(keys.size()));
        check(format_proxy_env_summary(keys) == "proxy env: HTTPS_PROXY",
              "T4 summary reports key name only (no value leak)",
              format_proxy_env_summary(keys));
    }

    // 空白值必须被忽略（对齐 value.trim().length > 0）
    setenv("ALL_PROXY", "   ", 1);
    {
        auto keys = resolve_present_proxy_env_keys();
        bool has_all = false;
        for (const auto& k : keys) {
            if (k == "ALL_PROXY") has_all = true;
        }
        check(!has_all && keys.size() == 1,
              "T5 whitespace-only value ignored",
              "n=" + std::to_string(keys.size()));
    }

    // 顺序必须与真源 ACP_PROXY_ENV_KEYS 一致
    setenv("HTTP_PROXY", "p", 1);
    setenv("ALL_PROXY", "p", 1);
    setenv("http_proxy", "p", 1);
    setenv("https_proxy", "p", 1);
    setenv("all_proxy", "p", 1);
    {
        auto keys = resolve_present_proxy_env_keys();
        std::vector<std::string> want = {"HTTP_PROXY", "HTTPS_PROXY", "ALL_PROXY",
                                         "http_proxy", "https_proxy", "all_proxy"};
        check(keys == want, "T6 key order matches ACP_PROXY_ENV_KEYS",
              "got n=" + std::to_string(keys.size()));
        check(format_proxy_env_summary(keys).rfind("proxy env: ", 0) == 0
                  && format_proxy_env_summary(keys).find(", ") != std::string::npos,
              "T7 multi-key summary joined with ', '");
    }
    // 摘要不得包含代理值
    {
        auto keys = resolve_present_proxy_env_keys();
        const std::string s = format_proxy_env_summary(keys);
        check(s.find("127.0.0.1") == std::string::npos && s.find("http://") == std::string::npos,
              "T8 summary never leaks proxy values", s);
    }

    std::printf("\n%s (%d failure%s)\n", g_fail == 0 ? "ALL PASS" : "FAILED", g_fail,
                g_fail == 1 ? "" : "s");
    return g_fail == 0 ? 0 : 1;
}
