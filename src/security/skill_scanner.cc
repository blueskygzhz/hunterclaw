#include <string>
#include <vector>
#include <regex>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unordered_set>
#include <unordered_map>
#include <algorithm>
#include <mutex>

namespace hunterclaw {
namespace security {

// ============================================================================
// Skill Scanner - 技能/插件代码静态扫描器
// 严格对齐 OpenClaw: src/security/skill-scanner.ts
// ============================================================================

namespace fs = std::filesystem;

// --- Types ---

enum class SkillScanSeverity { Info, Warn, Critical };

struct SkillScanFinding {
    std::string rule_id;
    SkillScanSeverity severity;
    std::string file;
    int line;
    std::string message;
    std::string evidence;
};

struct SkillScanSummary {
    int scanned_files = 0;
    int critical = 0;
    int warn = 0;
    int info = 0;
    std::vector<SkillScanFinding> findings;
};

struct SkillScanOptions {
    std::vector<std::string> include_files;
    int max_files = 500;
    int max_file_bytes = 1024 * 1024;
};

namespace {

// 对齐 OpenClaw: SCANNABLE_EXTENSIONS
const std::unordered_set<std::string> SCANNABLE_EXTENSIONS = {
    ".js", ".ts", ".mjs", ".cjs", ".mts", ".cts", ".jsx", ".tsx",
    // C++ extensions for HunterClaw
    ".cc", ".cpp", ".h", ".hpp"
};

const std::unordered_set<int> STANDARD_PORTS = {80, 443, 8080, 8443, 3000};

// --- Line Rules ---
// 对齐 OpenClaw: LINE_RULES

struct LineRule {
    std::string rule_id;
    SkillScanSeverity severity;
    std::string message;
    std::regex pattern;
    std::optional<std::regex> requires_context;
};

struct SourceRule {
    std::string rule_id;
    SkillScanSeverity severity;
    std::string message;
    std::regex pattern;
    std::optional<std::regex> requires_context;
};

std::vector<LineRule> get_line_rules() {
    return {
        {
            "dangerous-exec",
            SkillScanSeverity::Critical,
            "Shell command execution detected (child_process/system)",
            std::regex(R"(\b(exec|execSync|spawn|spawnSync|execFile|execFileSync|system|popen)\s*\()"),
            std::regex(R"(child_process|cstdlib|popen)")
        },
        {
            "dynamic-code-execution",
            SkillScanSeverity::Critical,
            "Dynamic code execution detected",
            std::regex(R"(\beval\s*\(|new\s+Function\s*\(|dlopen\s*\()"),
            std::nullopt
        },
        {
            "crypto-mining",
            SkillScanSeverity::Critical,
            "Possible crypto-mining reference detected",
            std::regex(R"(stratum\+tcp|stratum\+ssl|coinhive|cryptonight|xmrig)", std::regex::icase),
            std::nullopt
        },
        {
            "suspicious-network",
            SkillScanSeverity::Warn,
            "WebSocket connection to non-standard port",
            std::regex(R"(new\s+WebSocket\s*\(\s*["']wss?:\/\/[^"']*:(\d+))"),
            std::nullopt
        },
    };
}

std::vector<SourceRule> get_source_rules() {
    return {
        {
            "potential-exfiltration",
            SkillScanSeverity::Warn,
            "File read combined with network send - possible data exfiltration",
            std::regex(R"(readFileSync|readFile|ifstream)"),
            std::regex(R"(\bfetch\b|\bpost\b|http\.request|curl_easy)", std::regex::icase)
        },
        {
            "obfuscated-code",
            SkillScanSeverity::Warn,
            "Hex-encoded string sequence detected (possible obfuscation)",
            std::regex(R"((\\x[0-9a-fA-F]{2}){6,})"),
            std::nullopt
        },
        {
            "env-harvesting",
            SkillScanSeverity::Critical,
            "Environment variable access combined with network send - possible credential harvesting",
            std::regex(R"(process\.env|getenv\s*\()"),
            std::regex(R"(\bfetch\b|\bpost\b|http\.request|curl)", std::regex::icase)
        },
    };
}

std::string truncate_evidence(const std::string& evidence, int max_len = 120) {
    if ((int)evidence.size() <= max_len) return evidence;
    return evidence.substr(0, max_len) + "...";
}

std::string trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    size_t end = s.find_last_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    return s.substr(start, end - start + 1);
}

} // anonymous namespace

/**
 * is_scannable - 检查文件是否可扫描
 * 对齐 OpenClaw: isScannable(filePath)
 */
bool is_scannable(const std::string& file_path) {
    auto ext = fs::path(file_path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    return SCANNABLE_EXTENSIONS.count(ext) > 0;
}

/**
 * scan_source - 扫描源代码内容
 * 对齐 OpenClaw: scanSource(source, filePath)
 */
std::vector<SkillScanFinding> scan_source(const std::string& source, const std::string& file_path) {
    std::vector<SkillScanFinding> findings;
    
    // Split lines
    std::vector<std::string> lines;
    std::istringstream stream(source);
    std::string line;
    while (std::getline(stream, line)) {
        lines.push_back(line);
    }
    
    // --- Line rules ---
    auto line_rules = get_line_rules();
    std::unordered_set<std::string> matched_line_rules;
    
    for (auto& rule : line_rules) {
        if (matched_line_rules.count(rule.rule_id)) continue;
        if (rule.requires_context && !std::regex_search(source, *rule.requires_context)) continue;
        
        for (int i = 0; i < (int)lines.size(); i++) {
            std::smatch match;
            if (!std::regex_search(lines[i], match, rule.pattern)) continue;
            
            // Special: suspicious-network port check
            if (rule.rule_id == "suspicious-network" && match.size() > 1) {
                try {
                    int port = std::stoi(match[1].str());
                    if (STANDARD_PORTS.count(port)) continue;
                } catch (...) {}
            }
            
            findings.push_back({
                rule.rule_id, rule.severity, file_path,
                i + 1, rule.message, truncate_evidence(trim(lines[i]))
            });
            matched_line_rules.insert(rule.rule_id);
            break;
        }
    }
    
    // --- Source rules ---
    auto source_rules = get_source_rules();
    std::unordered_set<std::string> matched_source_rules;
    
    for (auto& rule : source_rules) {
        std::string key = rule.rule_id + "::" + rule.message;
        if (matched_source_rules.count(key)) continue;
        if (!std::regex_search(source, rule.pattern)) continue;
        if (rule.requires_context && !std::regex_search(source, *rule.requires_context)) continue;
        
        int match_line = 1;
        std::string match_evidence;
        for (int i = 0; i < (int)lines.size(); i++) {
            if (std::regex_search(lines[i], rule.pattern)) {
                match_line = i + 1;
                match_evidence = trim(lines[i]);
                break;
            }
        }
        if (match_evidence.empty()) {
            match_evidence = source.substr(0, std::min((int)source.size(), 120));
        }
        
        findings.push_back({
            rule.rule_id, rule.severity, file_path,
            match_line, rule.message, truncate_evidence(match_evidence)
        });
        matched_source_rules.insert(key);
    }
    
    return findings;
}

/**
 * scan_directory_with_summary - 扫描目录并返回汇总
 * 对齐 OpenClaw: scanDirectoryWithSummary(dirPath, opts?)
 */
SkillScanSummary scan_directory_with_summary(
    const std::string& dir_path,
    const SkillScanOptions& opts
) {
    SkillScanSummary summary;
    
    if (!fs::exists(dir_path) || !fs::is_directory(dir_path)) {
        return summary;
    }
    
    int file_count = 0;
    std::error_code ec;
    
    for (auto it = fs::recursive_directory_iterator(dir_path, 
            fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator() && file_count < opts.max_files;
         ++it) {
        
        if (ec) { ec.clear(); continue; }
        
        const auto& entry = *it;
        auto filename = entry.path().filename().string();
        
        // Skip hidden dirs and node_modules
        if (filename.starts_with(".") || filename == "node_modules") {
            it.disable_recursion_pending();
            continue;
        }
        
        if (!entry.is_regular_file()) continue;
        if (!is_scannable(entry.path().string())) continue;
        
        auto file_size = entry.file_size(ec);
        if (ec || (int)file_size > opts.max_file_bytes) { ec.clear(); continue; }
        
        // Read file
        std::ifstream file(entry.path());
        if (!file.is_open()) continue;
        
        std::string source((std::istreambuf_iterator<char>(file)),
                          std::istreambuf_iterator<char>());
        
        auto findings = scan_source(source, entry.path().string());
        summary.scanned_files++;
        
        for (const auto& f : findings) {
            summary.findings.push_back(f);
            if (f.severity == SkillScanSeverity::Critical) summary.critical++;
            else if (f.severity == SkillScanSeverity::Warn) summary.warn++;
            else summary.info++;
        }
        
        file_count++;
    }
    
    return summary;
}

}  // namespace security
}  // namespace hunterclaw
