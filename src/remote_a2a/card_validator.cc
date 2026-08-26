// ============================================================================
// remote_a2a/card_validator.cc
// AgentCard RPC URL 的 SSRF 校验实现。
// ============================================================================

#include "hunterclaw/remote_a2a/card_validator.h"

#include "hunterclaw/remote_a2a/url_safety.h"

namespace hunterclaw {
namespace remote_a2a {

bool is_network_source(const std::string& source) {
    return source.rfind("http://", 0) == 0 || source.rfind("https://", 0) == 0;
}

core::Result<bool> validate_card_rpc_targets(const AgentCard& card,
                                             const std::string& source_url,
                                             bool resolve_dns) {
    // ① 只校验网络来源的卡。
    //   本地文件 / 直接传入的卡对象没有经过网络，其目标由调用方负责
    //   （対标 ADK 注释：'A card passed in directly or read from a local file
    //    did not come off the network here, so its target is left to the caller.'）
    if (!is_network_source(source_url)) {
        return core::Result<bool>::Ok(true);
    }

    auto src = parse_url(source_url);
    if (!src) {
        return core::Result<bool>::Err("非法的 card source URL: " + source_url);
    }
    const std::string src_origin = src->origin();

    // ④ 遍历【全部】 RPC URL。
    //   只校验 select_rpc_url() 选中的那一个等于没校验 —— 攻击者可以在别的
    //   interface 里塞恶意端点，等协商逻辑选中它。
    const std::vector<std::string> urls = collect_all_rpc_urls(card);
    if (urls.empty()) {
        return core::Result<bool>::Err("卡未声明任何 RPC URL");
    }

    for (const std::string& u : urls) {
        auto p = parse_url(u);
        if (!p) {
            return core::Result<bool>::Err("卡内 RPC URL 非法（仅支持 http/https）: " + u);
        }

        const bool loopback = is_loopback_host(p->host);

        // ② 必须 https，例外只有 loopback 上的 http（本地开发形态）
        if (p->scheme != "https" && !loopback) {
            return core::Result<bool>::Err(
                "卡内 RPC URL 必须使用 https（仅 loopback 主机允许 http）: " + u);
        }

        // ③ origin 必须与拉卡的 origin 一致
        if (p->origin() != src_origin) {
            return core::Result<bool>::Err(
                "卡内 RPC URL 的 origin 必须与拉卡地址一致（期望 " + src_origin +
                "）: " + u);
        }

        // ⑤ ★ ADK 没有的增强：DNS 解析后的 IP 网段校验
        //   同源检查已经隐含了「与拉卡地址同一主机」，但拉卡地址本身可能
        //   就是攻击者控制的域名并解析到内网（DNS rebinding / 内网域名）。
        if (!loopback && resolve_dns) {
            if (is_metadata_hostname(p->host)) {
                return core::Result<bool>::Err(
                    "卡内 RPC URL 指向云 metadata 主机: " + u);
            }
            auto ip = resolve_first_ip(p->host);
            if (!ip) {
                return core::Result<bool>::Err("无法解析卡内 RPC URL 主机: " + p->host);
            }
            if (is_private_or_reserved(*ip)) {
                return core::Result<bool>::Err(
                    "卡内 RPC URL 解析到私有/保留网段 " + *ip + ": " + u);
            }
        }
    }
    return core::Result<bool>::Ok(true);
}

}  // namespace remote_a2a
}  // namespace hunterclaw
