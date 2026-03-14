#pragma once

#include "hunterclaw/core/types.h"
#include <string>
#include <vector>
#include <memory>

namespace hunterclaw {
namespace query {

// ============================================================================
// QMD (Query Matching DSL) - 查询语法
// ============================================================================

/**
 * QMD 查询节点类型
 */
enum class QMDNodeType {
    AND,        // 所有条件必须满足
    OR,         // 任一条件满足
    NOT,        // 取反
    TEXT,       // 文本匹配
    TAG,        // 标签匹配
    DATE,       // 日期范围
    SCORE,      // 分数阈值
};

/**
 * QMD 查询节点
 */
struct QMDNode {
    QMDNodeType type;
    std::string value;
    std::vector<std::shared_ptr<QMDNode>> children;
};

/**
 * QMD 解析器
 */
class QMDParser {
public:
    QMDParser();
    ~QMDParser();
    
    /**
     * 解析 QMD 查询字符串
     * 
     * 语法示例:
     * - "keyword" - 简单文本匹配
     * - "keyword1 AND keyword2" - 逻辑与
     * - "keyword1 OR keyword2" - 逻辑或
     * - "NOT keyword" - 逻辑非
     * - "tag:project" - 标签匹配
     * - "date:2024-01-01..2024-12-31" - 日期范围
     * - "score:>0.8" - 分数阈值
     * - "(keyword1 OR keyword2) AND tag:important" - 组合查询
     */
    core::Result<std::shared_ptr<QMDNode>> parse(const std::string& query);
    
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/**
 * QMD 查询执行器
 */
class QMDExecutor {
public:
    explicit QMDExecutor(std::shared_ptr<QMDNode> root);
    ~QMDExecutor();
    
    /**
     * 评估单个文档是否匹配查询
     */
    bool evaluate(const std::map<std::string, std::string>& document);
    
    /**
     * 生成 FTS5 查询字符串 (用于 SQLite)
     */
    std::string to_fts5_query();
    
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace query
} // namespace hunterclaw
