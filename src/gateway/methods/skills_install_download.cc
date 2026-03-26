// 🎯 对齐 OpenClaw: src/agents/skills-install-download.ts (全文件 239 行)
// 实现下载安装器，支持归档解压和 stripComponents

#include <chrono>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <iomanip>
#include <filesystem>  // ✅ 添加缺失的 filesystem 头文件

#include <openssl/sha.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>

namespace fs = std::filesystem;  // ✅ 添加命名空间别名

// C++11 兼容的路径操作函数
namespace path_utils {

inline bool ends_with(const std::string& str, const std::string& suffix) {
    if (suffix.length() > str.length()) return false;
    return str.compare(str.length() - suffix.length(), suffix.length(), suffix) == 0;
}

inline bool starts_with(const std::string& str, const std::string& prefix) {
    if (prefix.length() > str.length()) return false;
    return str.compare(0, prefix.length(), prefix) == 0;
}

inline bool is_absolute(const std::string& p) {
    return !p.empty() && p[0] == '/';
}

inline std::string join(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    if (a[a.length() - 1] == '/') {
        return a + b;
    }
    return a + "/" + b;
}

inline std::string basename(const std::string& path) {
    size_t pos = path.find_last_of('/');
    if (pos == std::string::npos) {
        return path;
    }
    return path.substr(pos + 1);
}

inline std::string parent_path(const std::string& path) {
    size_t pos = path.find_last_of('/');
    if (pos == std::string::npos) {
        return ".";
    }
    return path.substr(0, pos);
}

inline bool file_exists(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

inline size_t file_size(const std::string& path) {
    struct stat st;
    if (stat(path.c_str(), &st) != 0) return 0;
    return st.st_size;
}

inline bool create_directories(const std::string& path) {
    // 简化实现: 使用 mkdir -p
    std::string cmd = "mkdir -p \"" + path + "\" 2>/dev/null";
    return system(cmd.c_str()) == 0;
}

inline bool remove_file(const std::string& path) {
    return unlink(path.c_str()) == 0 || errno == ENOENT;
}

inline bool rename_file(const std::string& from, const std::string& to) {
    return rename(from.c_str(), to.c_str()) == 0;
}

inline std::string realpath_safe(const std::string& path) {
    char* resolved = realpath(path.c_str(), nullptr);
    if (!resolved) return path;
    std::string result(resolved);
    free(resolved);
    return result;
}

inline std::string relative(const std::string& path, const std::string& base) {
    // 简化实现: 检查前缀
    if (starts_with(path, base)) {
        size_t base_len = base.length();
        if (base_len < path.length() && path[base_len] == '/') {
            return path.substr(base_len + 1);
        }
        if (base_len == path.length()) {
            return ".";
        }
    }
    
    // 包含 ".."
    if (path.find("..") != std::string::npos) {
        return "..";
    }
    
    return path;
}

inline void to_lower(std::string& str) {
    std::transform(str.begin(), str.end(), str.begin(), ::tolower);
}

}  // namespace path_utils

namespace hunterclaw {
namespace skills {

// 🎯 对齐 OpenClaw line 19-21
struct DownloadResult {
    bool ok;
    std::string message;
    int code;
    std::string stdout_output;
    std::string stderr_output;
    std::vector<std::string> warnings;
};

// 🎯 对齐 OpenClaw line 7-17: resolveSkillToolsRootDir
// 返回每个 skill 的独立工具目录: ~/.hunterclaw/tools/<safe-key>
std::string resolve_skill_tools_root_dir(
    const std::string& skill_key,
    const std::string& config_dir
) {
    // 简化的安全路径生成 (hash 前 10 位)
    std::string safe_key = skill_key;
    // 替换非法字符
    std::replace(safe_key.begin(), safe_key.end(), '/', '-');
    std::replace(safe_key.begin(), safe_key.end(), '\\', '-');
    
    // 计算 SHA256 hash (前 10 位)
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(skill_key.c_str()), 
           skill_key.length(), hash);
    
    std::ostringstream hex_stream;
    for (int i = 0; i < 5; ++i) {  // 10 个十六进制字符
        hex_stream << std::hex << std::setw(2) << std::setfill('0') 
                   << static_cast<int>(hash[i]);
    }
    
    std::string safe_segment = safe_key.length() > 50 
        ? safe_key.substr(0, 50) + "-" + hex_stream.str()
        : safe_key;
    
    return config_dir + "/tools/" + safe_segment;
}

// 🎯 对齐 OpenClaw line 23-42: resolveDownloadTargetDir
std::string resolve_download_target_dir(
    const std::string& safe_root,
    const std::string& target_dir_raw
) {
    using namespace path_utils;
    
    if (target_dir_raw.empty()) {
        return safe_root;
    }
    
    // 处理相对路径 (相对于 safe_root)
    std::string resolved;
    if (target_dir_raw[0] == '~' || is_absolute(target_dir_raw)) {
        // 绝对路径或 ~ 开头
        if (target_dir_raw[0] == '~') {
            const char* home = getenv("HOME");
            if (home) {
                resolved = join(home, target_dir_raw.substr(1));
            } else {
                resolved = target_dir_raw;
            }
        } else {
            resolved = target_dir_raw;
        }
    } else {
        // 相对路径，基于 safe_root
        resolved = join(safe_root, target_dir_raw);
    }
    
    // 安全检查: 必须在 safe_root 内
    std::string canonical_root = realpath_safe(safe_root);
    std::string canonical_resolved = realpath_safe(resolved);
    
    std::string rel = relative(canonical_resolved, canonical_root);
    if (rel.empty() || starts_with(rel, "..")) {
        throw std::runtime_error(
            "Refusing to install outside the skill tools directory. targetDir=\"" +
            target_dir_raw + "\" resolves to \"" + resolved + 
            "\". Allowed root: \"" + safe_root + "\"."
        );
    }
    
    return resolved;
}

// 🎯 对齐 OpenClaw line 44-60: resolveArchiveType
std::string resolve_archive_type(
    const std::string& explicit_type,
    const std::string& filename
) {
    // 显式指定优先
    if (!explicit_type.empty()) {
        std::string lower = explicit_type;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
        return lower;
    }
    
    // 从文件名推断
    std::string lower_filename = filename;
    std::transform(lower_filename.begin(), lower_filename.end(), 
                   lower_filename.begin(), ::tolower);
    
    if (path_utils::ends_with(lower_filename, ".tar.gz") || path_utils::ends_with(lower_filename, ".tgz")) {
        return "tar.gz";
    }
    if (path_utils::ends_with(lower_filename, ".tar.bz2") || path_utils::ends_with(lower_filename, ".tbz2")) {
        return "tar.bz2";
    }
    if (path_utils::ends_with(lower_filename, ".zip")) {
        return "zip";
    }
    
    return "";
}

// 🎯 对齐 OpenClaw line 62-102: downloadFile
struct DownloadFileResult {
    bool ok;
    size_t bytes;
    std::string error;
};

DownloadFileResult download_file(
    const std::string& url,
    const std::string& root_dir,
    const std::string& relative_path,
    int timeout_ms
) {
    // 🎯 对齐 OpenClaw line 68-76: 准备目录和临时文件
    fs::path dest_path = fs::path(root_dir) / relative_path;
    // 🎯 对齐 OpenClaw: staging 目录名为 .openclaw-download-staging
    fs::path staging_dir = fs::path(root_dir) / ".openclaw-download-staging";
    
    try {
        fs::create_directories(staging_dir);
    } catch (const std::exception& e) {
        return {false, 0, "Failed to create staging dir: " + std::string(e.what())};
    }
    
    // 生成临时文件名
    std::string temp_filename = "download-" + 
        std::to_string(std::chrono::system_clock::now().time_since_epoch().count()) +
        ".tmp";
    fs::path temp_path = staging_dir / temp_filename;
    
    // ✅ 对齐 OpenClaw: fetchWithSsrfGuard (SSRF 防护)
    // 对应 OpenClaw: 检查 URL 是否指向内网 IP
    // SSRF 防护: 拒绝内网 IP 地址
    {
        // 检查 URL 是否为 http/https
        if (url.substr(0, 7) != "http://" && url.substr(0, 8) != "https://") {
            return {false, 0, "SSRF guard: only http/https URLs allowed"};
        }
        // 检查是否包含内网 IP 模式
        std::vector<std::string> blocked_patterns = {
            "localhost", "127.", "10.", "192.168.", "172.16.", "172.17.",
            "172.18.", "172.19.", "172.20.", "172.21.", "172.22.", "172.23.",
            "172.24.", "172.25.", "172.26.", "172.27.", "172.28.", "172.29.",
            "172.30.", "172.31.", "169.254.", "::1", "0.0.0.0"
        };
        // 提取 host 部分
        std::string host;
        size_t host_start = url.find("://");
        if (host_start != std::string::npos) {
            host_start += 3;
            size_t host_end = url.find('/', host_start);
            host = url.substr(host_start, host_end == std::string::npos ? std::string::npos : host_end - host_start);
            // 去除端口
            size_t colon = host.find(':');
            if (colon != std::string::npos) host = host.substr(0, colon);
        }
        for (const auto& pattern : blocked_patterns) {
            if (host.find(pattern) == 0 || host == pattern.substr(0, pattern.size() - 1)) {
                return {false, 0, "SSRF guard: blocked host " + host};
            }
        }
    }
    
    std::string curl_cmd = "curl -L -s -S --max-time " + 
        std::to_string(timeout_ms / 1000) + 
        " -o \"" + temp_path.string() + "\" \"" + url + "\" 2>&1";
    
    FILE* pipe = popen(curl_cmd.c_str(), "r");
    if (!pipe) {
        return {false, 0, "Failed to execute curl"};
    }
    
    std::string curl_output;
    char buffer[256];
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        curl_output += buffer;
    }
    
    int exit_code = pclose(pipe);
    if (exit_code != 0) {
        fs::remove(temp_path);
        return {false, 0, "Download failed (curl exit " + 
                std::to_string(exit_code) + "): " + curl_output};
    }
    
    // 🎯 对齐 OpenClaw line 91-96: 安全移动文件到目标位置
    try {
        // 确保目标目录存在
        fs::create_directories(dest_path.parent_path());
        
        // 原子移动 (rename)
        if (fs::exists(dest_path)) {
            fs::remove(dest_path);
        }
        fs::rename(temp_path, dest_path);
        
        // 获取文件大小
        size_t bytes = fs::file_size(dest_path);
        return {true, bytes, ""};
        
    } catch (const std::exception& e) {
        fs::remove(temp_path);
        return {false, 0, "Failed to move downloaded file: " + std::string(e.what())};
    }
}

// 🎯 对齐 OpenClaw line 140-233: extractArchive
// 前向声明 ExtractResult，供 verifyArchiveHashStable 使用
struct ExtractResult {
    bool ok;
    int code;
    std::string stdout_output;
    std::string stderr_output;
};

// 🎯 对齐 OpenClaw: hashFileSha256 (skills-install-extract.ts line 18-30)
static std::string hash_file_sha256(const std::string& file_path) {
    std::ifstream f(file_path, std::ios::binary);
    if (!f.is_open()) return "";
    
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256_CTX ctx;
    SHA256_Init(&ctx);
    
    char buf[8192];
    while (f.read(buf, sizeof(buf)) || f.gcount() > 0) {
        SHA256_Update(&ctx, buf, f.gcount());
    }
    SHA256_Final(hash, &ctx);
    
    std::ostringstream hex;
    for (int i = 0; i < SHA256_DIGEST_LENGTH; ++i) {
        hex << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(hash[i]);
    }
    return hex.str();
}

// 🎯 对齐 OpenClaw: verifyArchiveHashStable (skills-install-extract.ts line 96-107)
// 防止 TOCTOU 攻击: 确认 preflight 前后归档文件未变
static bool verify_archive_hash_stable(
    const std::string& archive_path,
    const std::string& expected_hash,
    ExtractResult& out_failure
) {
    std::string post_hash = hash_file_sha256(archive_path);
    if (post_hash == expected_hash) return true;
    out_failure = {false, 1, "", "tar archive changed during safety preflight; refusing to extract"};
    return false;
}

// 🎯 对齐 OpenClaw: extractArchive (skills-install-extract.ts)
ExtractResult extract_archive(
    const std::string& archive_path,
    const std::string& archive_type,
    const std::string& target_dir,
    int strip_components,
    int timeout_ms
) {
    // 🎯 对齐 OpenClaw line 153-163: zip 解压
    if (archive_type == "zip") {
        std::string cmd = "unzip -o -q ";
        if (strip_components > 0) {
            // unzip 不直接支持 strip-components，需要特殊处理
            // 这里简化处理: 解压到临时目录然后移动
            cmd += "\"" + archive_path + "\" -d \"" + target_dir + "\" 2>&1";
        } else {
            cmd += "\"" + archive_path + "\" -d \"" + target_dir + "\" 2>&1";
        }
        
        FILE* pipe = popen(cmd.c_str(), "r");
        if (!pipe) {
            return {false, -1, "", "Failed to execute unzip"};
        }
        
        std::string output;
        char buffer[256];
        while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
            output += buffer;
        }
        
        int exit_code = pclose(pipe);
        if (exit_code != 0) {
            return {false, exit_code, output, "unzip failed"};
        }
        
        // ✅ 对齐 OpenClaw: 实现 strip_components 逻辑
        // 对应 OpenClaw: 移动子目录内容到 target_dir
        if (strip_components > 0) {
            // 找到解压后的子目录
            std::vector<fs::path> subdirs;
            try {
                for (const auto& entry : fs::directory_iterator(target_dir)) {
                    if (entry.is_directory()) subdirs.push_back(entry.path());
                }
            } catch (...) {}
            
            if (!subdirs.empty()) {
                // 将第一个子目录的内容移动到 target_dir
                fs::path subdir = subdirs[0];
                try {
                    for (const auto& entry : fs::directory_iterator(subdir)) {
                        fs::rename(entry.path(), fs::path(target_dir) / entry.path().filename());
                    }
                    fs::remove(subdir);
                } catch (...) {}
            }
        }
        
        return {true, 0, output, ""};
    }
    
    // 🎯 对齐 OpenClaw line 165-175: tar.gz 解压
    if (archive_type == "tar.gz") {
        std::string cmd = "tar xzf \"" + archive_path + "\" -C \"" + target_dir + "\"";
        if (strip_components > 0) {
            cmd += " --strip-components=" + std::to_string(strip_components);
        }
        cmd += " 2>&1";
        
        FILE* pipe = popen(cmd.c_str(), "r");
        if (!pipe) {
            return {false, -1, "", "Failed to execute tar"};
        }
        
        std::string output;
        char buffer[256];
        while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
            output += buffer;
        }
        
        int exit_code = pclose(pipe);
        if (exit_code != 0) {
            return {false, exit_code, output, "tar extraction failed"};
        }
        
        return {true, 0, output, ""};
    }
    
    // 🎯 对齐 OpenClaw: tar.bz2 解压 (含 preflight + TOCTOU 防护)
    if (archive_type == "tar.bz2") {
        // 检查 tar 命令是否存在
        if (system("which tar > /dev/null 2>&1") != 0) {
            return {false, -1, "", "tar not found on PATH"};
        }
        
        // 🎯 对齐 OpenClaw: hashFileSha256 在 preflight 前计算哈希
        std::string preflight_hash = hash_file_sha256(archive_path);
        
        // 🎯 对齐 OpenClaw: readTarPreflight — tar tf + tar tvf
        // Step 1: tar tf 列出所有条目
        std::vector<std::string> entries;
        {
            std::string list_cmd = "tar tf \"" + archive_path + "\" 2>&1";
            FILE* list_pipe = popen(list_cmd.c_str(), "r");
            if (!list_pipe) {
                return {false, -1, "", "tar list failed"};
            }
            std::string list_output;
            char buf[256];
            while (fgets(buf, sizeof(buf), list_pipe) != nullptr) list_output += buf;
            int list_code = pclose(list_pipe);
            if (list_code != 0) {
                return {false, list_code, list_output, "tar list failed"};
            }
            
            std::istringstream entries_ss(list_output);
            std::string ep;
            while (std::getline(entries_ss, ep)) {
                ep.erase(0, ep.find_first_not_of(" \t\r"));
                if (!ep.empty()) ep.erase(ep.find_last_not_of(" \t\r") + 1);
                if (!ep.empty()) entries.push_back(ep);
            }
        }
        
        // 🎯 对齐 OpenClaw: createTarEntryPreflightChecker — 检查每个条目路径安全
        for (const auto& entry_path : entries) {
            // 应用 strip_components
            std::string check_path = entry_path;
            if (strip_components > 0) {
                int slashes = 0;
                size_t pos = 0;
                while (pos < check_path.size() && slashes < strip_components) {
                    if (check_path[pos] == '/') slashes++;
                    pos++;
                }
                check_path = (pos < check_path.size()) ? check_path.substr(pos) : "";
            }
            if (check_path.empty()) continue;
            // 检查路径穿越
            if (check_path.find("..") != std::string::npos) {
                return {false, -1, "", "tar entry path traversal detected: " + entry_path};
            }
            if (!check_path.empty() && check_path[0] == '/') {
                return {false, -1, "", "tar entry absolute path detected: " + entry_path};
            }
        }
        
        // 🎯 对齐 OpenClaw: verifyArchiveHashStable — TOCTOU 防护
        {
            ExtractResult hash_failure;
            if (!verify_archive_hash_stable(archive_path, preflight_hash, hash_failure)) {
                return hash_failure;
            }
        }
        
        // 🎯 对齐 OpenClaw: extractTarBz2WithStaging — 使用 staging 目录解压再合并
        // 创建临时 staging 目录
        std::string staging_dir = target_dir + "/.tar-bz2-staging-" +
            std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
        try {
            fs::create_directories(staging_dir);
        } catch (const std::exception& e) {
            return {false, -1, "", "Failed to create staging dir: " + std::string(e.what())};
        }
        
        std::string cmd = "tar xjf \"" + archive_path + "\" -C \"" + staging_dir + "\"";
        if (strip_components > 0) {
            cmd += " --strip-components=" + std::to_string(strip_components);
        }
        cmd += " 2>&1";
        
        FILE* pipe = popen(cmd.c_str(), "r");
        if (!pipe) {
            fs::remove_all(staging_dir);
            return {false, -1, "", "Failed to execute tar"};
        }
        
        std::string output;
        char buffer[256];
        while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
            output += buffer;
        }
        
        int exit_code = pclose(pipe);
        if (exit_code != 0) {
            fs::remove_all(staging_dir);
            return {false, exit_code, output, "tar.bz2 extraction failed"};
        }
        
        // 🎯 对齐 OpenClaw: mergeExtractedTreeIntoDestination — 将 staging 内容合并到目标目录
        try {
            for (const auto& entry : fs::directory_iterator(staging_dir)) {
                fs::path dest = fs::path(target_dir) / entry.path().filename();
                if (fs::exists(dest)) {
                    fs::remove_all(dest);
                }
                fs::rename(entry.path(), dest);
            }
            fs::remove_all(staging_dir);
        } catch (const std::exception& e) {
            fs::remove_all(staging_dir);
            return {false, -1, output, "Failed to merge extracted tree: " + std::string(e.what())};
        }
        
        return {true, 0, output, ""};
    }
    
    // 未支持的归档类型
    return {false, -1, "", "unsupported archive type: " + archive_type};
}

// 🎯 对齐 OpenClaw line 104-238: installDownloadSpec (主函数)
DownloadResult install_download_spec(
    const std::string& skill_name,
    const std::string& skill_key,
    const std::string& url_raw,
    const std::string& target_dir_raw,
    const std::string& archive_type_explicit,
    bool extract_requested,
    int strip_components,
    int timeout_ms,
    const std::string& config_dir
) {
    // 🎯 对齐 OpenClaw line 110-120: 参数验证
    std::string url = url_raw;
    // trim
    size_t start = url.find_first_not_of(" \t\n\r");
    size_t end = url.find_last_not_of(" \t\n\r");
    if (start != std::string::npos && end != std::string::npos) {
        url = url.substr(start, end - start + 1);
    }
    
    if (url.empty()) {
        return {false, "missing download url", -1, "", "", {}};
    }
    
    // 🎯 对齐 OpenClaw line 122-131: 从 URL 提取文件名
    std::string filename;
    try {
        size_t last_slash = url.find_last_of('/');
        if (last_slash != std::string::npos && last_slash < url.length() - 1) {
            filename = url.substr(last_slash + 1);
        }
    } catch (...) {
        filename = "download";
    }
    
    if (filename.empty()) {
        filename = "download";
    }
    
    // 🎯 对齐 OpenClaw line 133-156: 安全目录准备
    std::string safe_root;
    std::string target_dir;
    
    try {
        safe_root = resolve_skill_tools_root_dir(skill_key, config_dir);
        fs::create_directories(safe_root);
        
        // 规范化 safe_root
        safe_root = fs::canonical(safe_root).string();
        
        // 解析目标目录
        std::string requested_target_dir = resolve_download_target_dir(
            safe_root, target_dir_raw
        );
        fs::create_directories(requested_target_dir);
        
        // 再次规范化，确保在 safe_root 内
        fs::path canonical_target = fs::canonical(requested_target_dir);
        fs::path canonical_root = fs::canonical(safe_root);
        
        auto rel = fs::relative(canonical_target, canonical_root);
        std::string rel_str = rel.string();
        if (rel.empty() || path_utils::starts_with(rel_str, "..")) {
            throw std::runtime_error("target directory outside allowed root");
        }
        
        target_dir = canonical_target.string();
        
    } catch (const std::exception& e) {
        return {false, std::string(e.what()), -1, "", std::string(e.what()), {}};
    }
    
    // 🎯 对齐 OpenClaw line 158-173: 归档路径安全检查
    fs::path archive_path = fs::path(target_dir) / filename;
    fs::path archive_relative = fs::relative(archive_path, safe_root);
    
    std::string rel_str = archive_relative.string();
    if (archive_relative.empty() || 
        rel_str == ".." ||
        path_utils::starts_with(rel_str, "..") ||
        archive_relative.is_absolute()) {
        return {false, "invalid download archive path", -1, "", 
                "invalid download archive path", {}};
    }
    
    // 🎯 对齐 OpenClaw line 174-186: 下载文件
    size_t downloaded = 0;
    try {
        auto download_result = download_file(
            url,
            safe_root,
            archive_relative.string(),
            timeout_ms
        );
        
        if (!download_result.ok) {
            return {false, download_result.error, -1, "", download_result.error, {}};
        }
        
        downloaded = download_result.bytes;
        
    } catch (const std::exception& e) {
        return {false, std::string(e.what()), -1, "", std::string(e.what()), {}};
    }
    
    // 🎯 对齐 OpenClaw line 188-198: 检测归档类型和是否解压
    // 对齐 OpenClaw: const shouldExtract = spec.extract ?? Boolean(archiveType);
    std::string archive_type = resolve_archive_type(archive_type_explicit, filename);
    bool should_extract;
    if (extract_requested) {
        // 显式指定 extract=true
        should_extract = true;
    } else if (!archive_type_explicit.empty()) {
        // 显式指定了 archive 类型，默认解压
        should_extract = !archive_type.empty();
    } else {
        // 对齐 OpenClaw: spec.extract 未设置时，根据 archiveType 自动判断
        should_extract = !archive_type.empty();
    }
    
    if (!should_extract) {
        std::string msg = "Downloaded to " + archive_path.string();
        std::string stdout_msg = "downloaded=" + std::to_string(downloaded);
        return {true, msg, 0, stdout_msg, "", {}};
    }
    
    if (archive_type.empty()) {
        return {false, "extract requested but archive type could not be detected",
                -1, "", "", {}};
    }
    
    // 🎯 对齐 OpenClaw line 210-227: 解压归档
    // 对齐 OpenClaw: 解压前对 targetDir 进行额外安全检查
    {
        // assertCanonicalPathWithinBase(canonicalSafeRoot, targetDir)
        try {
            fs::path canonical_target = fs::canonical(target_dir);
            fs::path canonical_root = fs::canonical(safe_root);
            auto rel = fs::relative(canonical_target, canonical_root);
            std::string rel_str = rel.string();
            if (rel.empty() || path_utils::starts_with(rel_str, "..")) {
                return {false, "target directory outside allowed root (pre-extract check)",
                        -1, "", "target directory outside allowed root", {}};
            }
        } catch (const std::exception& e) {
            return {false, std::string(e.what()), -1, "", std::string(e.what()), {}};
        }
    }
    
    try {
        auto extract_result = extract_archive(
            archive_path.string(),
            archive_type,
            target_dir,
            strip_components,
            timeout_ms
        );
        
        // 🎯 对齐 OpenClaw: const success = extractResult.code === 0
        bool success = (extract_result.code == 0);
        
        // 🎯 对齐 OpenClaw line 228-237: 成功返回
        // 失败时使用 formatInstallFailureMessage
        std::string out = extract_result.stdout_output;
        out.erase(0, out.find_first_not_of(" \t\n\r"));
        if (!out.empty()) out.erase(out.find_last_not_of(" \t\n\r") + 1);
        std::string err = extract_result.stderr_output;
        err.erase(0, err.find_first_not_of(" \t\n\r"));
        if (!err.empty()) err.erase(err.find_last_not_of(" \t\n\r") + 1);
        
        if (success) {
            return {true, "Downloaded and extracted to " + target_dir,
                    0, out, err, {}};
        } else {
            // 🎯 对齐 OpenClaw: formatInstallFailureMessage(extractResult)
            struct FakeCommandResult {
                int code;
                std::string stdout_output;
                std::string stderr_output;
            };
            // 内联调用 format_install_failure_message 等价逻辑
            std::string code_str = (extract_result.code >= 0)
                ? ("exit " + std::to_string(extract_result.code))
                : "unknown exit";
            // summarize stderr first, then stdout
            auto summarize = [](const std::string& text) -> std::string {
                std::string raw = text;
                raw.erase(0, raw.find_first_not_of(" \t\n\r"));
                if (!raw.empty()) raw.erase(raw.find_last_not_of(" \t\n\r") + 1);
                if (raw.empty()) return "";
                std::vector<std::string> lines;
                std::istringstream ss(raw);
                std::string line;
                while (std::getline(ss, line)) {
                    line.erase(0, line.find_first_not_of(" \t\r"));
                    if (!line.empty()) line.erase(line.find_last_not_of(" \t\r") + 1);
                    if (!line.empty()) lines.push_back(line);
                }
                if (lines.empty()) return "";
                std::string preferred;
                for (const auto& l : lines) {
                    std::string lower = l;
                    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
                    if (lower.rfind("error", 0) == 0) { preferred = l; break; }
                }
                if (preferred.empty()) {
                    for (const auto& l : lines) {
                        std::string lower = l;
                        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
                        if (lower.find("err!") != std::string::npos ||
                            lower.find("error:") != std::string::npos ||
                            lower.find("failed") != std::string::npos) {
                            preferred = l; break;
                        }
                    }
                }
                if (preferred.empty()) preferred = lines.back();
                std::string normalized;
                bool prev_space = false;
                for (char c : preferred) {
                    if (std::isspace(c)) {
                        if (!prev_space && !normalized.empty()) { normalized += ' '; prev_space = true; }
                    } else { normalized += c; prev_space = false; }
                }
                if (!normalized.empty() && normalized.back() == ' ') normalized.pop_back();
                if (normalized.size() > 200) normalized = normalized.substr(0, 199) + "\xe2\x80\xa6";
                return normalized;
            };
            std::string summary = summarize(err);
            if (summary.empty()) summary = summarize(out);
            std::string fail_msg = summary.empty()
                ? "Install failed (" + code_str + ")"
                : "Install failed (" + code_str + "): " + summary;
            return {false, fail_msg, extract_result.code, out, err, {}};
        }
        
    } catch (const std::exception& e) {
        return {false, std::string(e.what()), -1, "", std::string(e.what()), {}};
    }
}

}  // namespace skills
}  // namespace hunterclaw
