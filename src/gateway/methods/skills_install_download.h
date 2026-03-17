// 🎯 对齐 OpenClaw: src/agents/skills-install-download.ts
// Download 安装器头文件

#pragma once

#include <string>
#include <vector>

namespace hunterclaw {
namespace skills {

// 下载安装结果
struct DownloadResult {
    bool ok;
    std::string message;
    int code;
    std::string stdout_output;
    std::string stderr_output;
    std::vector<std::string> warnings;
};

// 🎯 对齐 OpenClaw line 104-238: installDownloadSpec
// 
// 主函数: 下载并可选解压归档文件到目标目录
// 
// 参数:
//   skill_name: 技能名称
//   skill_key: 技能唯一键 (用于生成工具目录)
//   url_raw: 下载 URL
//   target_dir_raw: 目标目录 (相对或绝对路径)
//   archive_type_explicit: 显式指定归档类型 ("tar.gz"/"tar.bz2"/"zip"/"")
//   extract_requested: 是否解压
//   strip_components: 解压时跳过的顶层目录数 (对齐 tar --strip-components)
//   timeout_ms: 超时时间 (毫秒)
//   config_dir: 配置目录 (通常是 ~/.hunterclaw)
// 
// 返回:
//   DownloadResult - 包含成功/失败状态、消息、退出码、输出、警告
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
);

}  // namespace skills
}  // namespace hunterclaw
