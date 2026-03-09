#include "hunterclaw/skills/skill.h"
#include "hunterclaw/core/utils.h"
#include <spdlog/spdlog.h>
#include <regex>
#include <sstream>

namespace hunterclaw {
namespace skills {

namespace {
    auto log() {
        static auto logger = spdlog::get("skills");
        if (!logger) {
            logger = spdlog::stdout_logger_mt("skills");
        }
        return logger;
    }
}

// 危险模式定义 (100% 对齐 OpenClaw 安全策略)
std::vector<SkillScanner::DangerousPattern> SkillScanner::get_patterns() {
    return {
        // ========== Critical 级别 ==========
        
        // 1. 任意文件删除
        {
            .rule_id = "dangerous-rm-rf",
            .severity = "critical",
            .message = "Dangerous recursive delete command detected",
            .pattern = std::regex(R"(rm\s+-rf\s+[/*~])"),
            .requires_context = std::nullopt
        },
        
        // 2. 系统级破坏
        {
            .rule_id = "dangerous-dd",
            .severity = "critical",
            .message = "Dangerous disk write command (dd) detected",
            .pattern = std::regex(R"(dd\s+if=.*/dev/)"),
            .requires_context = std::nullopt
        },
        
        // 3. Fork 炸弹
        {
            .rule_id = "fork-bomb",
            .severity = "critical",
            .message = "Fork bomb pattern detected",
            .pattern = std::regex(R"(:[\(\)]\{.*:\|:&\})"),
            .requires_context = std::nullopt
        },
        
        // 4. 任意代码执行
        {
            .rule_id = "eval-execution",
            .severity = "critical",
            .message = "Arbitrary code execution via eval() detected",
            .pattern = std::regex(R"(eval\s*\(\s*\$)"),
            .requires_context = std::nullopt
        },
        
        // 5. 远程代码下载并执行
        {
            .rule_id = "remote-exec",
            .severity = "critical",
            .message = "Remote code download and execution detected",
            .pattern = std::regex(R"(curl.*\|\s*bash|wget.*\|\s*sh)"),
            .requires_context = std::nullopt
        },
        
        // ========== High 级别 ==========
        
        // 6. 修改 PATH
        {
            .rule_id = "path-modification",
            .severity = "high",
            .message = "PATH modification detected",
            .pattern = std::regex(R"(export\s+PATH=|PATH=)"),
            .requires_context = std::nullopt
        },
        
        // 7. 修改系统配置
        {
            .rule_id = "system-config-modification",
            .severity = "high",
            .message = "System configuration file modification detected",
            .pattern = std::regex(R"(/etc/|/var/|~/.bashrc|~/.zshrc|~/.profile)"),
            .requires_context = std::regex(R"(echo.*>>|sed.*-i|vim|nano)")
        },
        
        // 8. Sudo 提权
        {
            .rule_id = "sudo-usage",
            .severity = "high",
            .message = "Sudo privilege escalation detected",
            .pattern = std::regex(R"(sudo\s+)"),
            .requires_context = std::nullopt
        },
        
        // 9. 修改权限
        {
            .rule_id = "permission-change",
            .severity = "high",
            .message = "File permission modification detected",
            .pattern = std::regex(R"(chmod\s+[0-9]+|chmod\s+\+x)"),
            .requires_context = std::nullopt
        },
        
        // 10. 网络监听
        {
            .rule_id = "network-listen",
            .severity = "high",
            .message = "Network listening detected",
            .pattern = std::regex(R"(nc\s+-l|netcat.*-l|python.*-m.*http\.server)"),
            .requires_context = std::nullopt
        },
        
        // ========== Medium 级别 ==========
        
        // 11. 环境变量读取
        {
            .rule_id = "env-access",
            .severity = "medium",
            .message = "Environment variable access detected",
            .pattern = std::regex(R"(\$\{?[A-Z_]+\}?|process\.env\.)"),
            .requires_context = std::nullopt
        },
        
        // 12. Home 目录访问
        {
            .rule_id = "home-access",
            .severity = "medium",
            .message = "Home directory access detected",
            .pattern = std::regex(R"(~/|~\.|/Users/|/home/)"),
            .requires_context = std::nullopt
        },
        
        // 13. 临时文件创建
        {
            .rule_id = "temp-file-creation",
            .severity = "medium",
            .message = "Temporary file creation detected",
            .pattern = std::regex(R"(/tmp/|mktemp)"),
            .requires_context = std::nullopt
        },
        
        // 14. 外部 HTTP 请求
        {
            .rule_id = "http-request",
            .severity = "medium",
            .message = "External HTTP request detected",
            .pattern = std::regex(R"(curl\s+http|wget\s+http|fetch\s*\()"),
            .requires_context = std::nullopt
        },
        
        // ========== Low 级别 (信息性) ==========
        
        // 15. Git 操作
        {
            .rule_id = "git-operation",
            .severity = "low",
            .message = "Git operation detected",
            .pattern = std::regex(R"(git\s+clone|git\s+push|git\s+pull)"),
            .requires_context = std::nullopt
        },
        
        // 16. Docker 操作
        {
            .rule_id = "docker-operation",
            .severity = "low",
            .message = "Docker operation detected",
            .pattern = std::regex(R"(docker\s+run|docker\s+exec)"),
            .requires_context = std::nullopt
        }
    };
}

core::Result<SkillScanner::ScanResult> SkillScanner::scan(const SkillEntry& entry) {
    ScanResult result;
    
    const auto& skill = entry.skill;
    const std::string& content = skill.content;
    
    log()->debug("Scanning skill: {}", skill.name);
    
    // 按行扫描
    std::istringstream content_stream(content);
    std::string line;
    size_t line_num = 1;
    
    auto patterns = get_patterns();
    
    while (std::getline(content_stream, line)) {
        // 对每个危险模式进行匹配
        for (const auto& pattern : patterns) {
            std::smatch match;
            
            if (std::regex_search(line, match, pattern.pattern)) {
                // 检查是否需要上下文匹配
                bool should_warn = true;
                
                if (pattern.requires_context) {
                    // 需要额外的上下文正则匹配
                    std::smatch context_match;
                    should_warn = std::regex_search(line, context_match, *pattern.requires_context);
                }
                
                if (should_warn) {
                    ScanResult::Warning warning;
                    warning.severity = pattern.severity;
                    warning.file = skill.file_path.string();
                    warning.message = pattern.message + " (" + pattern.rule_id + ")";
                    warning.line = line_num;
                    
                    result.warnings.push_back(warning);
                    
                    log()->warn("Security issue found in '{}' at line {}: {} [{}]",
                               skill.name, line_num, pattern.message, pattern.severity);
                }
            }
        }
        
        line_num++;
    }
    
    // 额外的启发式检查
    
    // 1. 检查是否包含过多的系统命令
    size_t bash_count = 0;
    size_t line_count = line_num - 1;
    
    std::regex bash_regex(R"(```bash|```sh|```shell)");
    std::sregex_iterator iter(content.begin(), content.end(), bash_regex);
    std::sregex_iterator end;
    bash_count = std::distance(iter, end);
    
    if (bash_count > 10 && line_count < 200) {
        ScanResult::Warning warning;
        warning.severity = "medium";
        warning.file = skill.file_path.string();
        warning.message = "Excessive shell commands detected (script-heavy skill)";
        warning.line = 0;
        result.warnings.push_back(warning);
    }
    
    // 2. 检查是否包含 base64 编码 (可能隐藏恶意代码)
    if (content.find("base64") != std::string::npos) {
        ScanResult::Warning warning;
        warning.severity = "medium";
        warning.file = skill.file_path.string();
        warning.message = "Base64 encoding detected (potential obfuscation)";
        warning.line = 0;
        result.warnings.push_back(warning);
    }
    
    // 总结
    if (result.warnings.empty()) {
        log()->info("Skill '{}' passed security scan (no issues)", skill.name);
    } else {
        log()->warn("Skill '{}' has {} security warnings", 
                   skill.name, result.warnings.size());
        
        if (result.has_critical()) {
            log()->error("Skill '{}' contains CRITICAL security issues!", skill.name);
        }
    }
    
    return core::Result<ScanResult>::Ok(result);
}

} // namespace skills
} // namespace hunterclaw
