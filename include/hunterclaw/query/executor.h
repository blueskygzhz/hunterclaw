#pragma once

#include "hunterclaw/core/result.h"
#include "hunterclaw/core/json.h"
#include "hunterclaw/query/qmd.h"
#include <string>
#include <vector>
#include <memory>

namespace hunterclaw {
namespace query {

// ============================================================================
// QMD Evaluator
// ============================================================================

/// Query evaluation context
struct EvalContext {
    std::string query_text;
    std::map<std::string, std::string> metadata;
    double score = 0.0;
    int64_t timestamp = 0;
};

/// QMD query evaluator
class QMDEvaluator {
public:
    /// Evaluate query against context
    core::Result<bool> evaluate(const QMDNode& node, const EvalContext& ctx);

private:
    bool eval_and(const QMDNode& node, const EvalContext& ctx);
    bool eval_or(const QMDNode& node, const EvalContext& ctx);
    bool eval_not(const QMDNode& node, const EvalContext& ctx);
    bool eval_text(const QMDNode& node, const EvalContext& ctx);
    bool eval_tag(const QMDNode& node, const EvalContext& ctx);
    bool eval_date(const QMDNode& node, const EvalContext& ctx);
    bool eval_score(const QMDNode& node, const EvalContext& ctx);
};

// ============================================================================
// FTS5 Query Generator
// ============================================================================

/// Generate SQLite FTS5 query from QMD AST
class FTS5QueryGenerator {
public:
    /// Convert QMD node to FTS5 MATCH expression
    core::Result<std::string> generate(const QMDNode& node);

private:
    std::string gen_and(const QMDNode& node);
    std::string gen_or(const QMDNode& node);
    std::string gen_not(const QMDNode& node);
    std::string gen_text(const QMDNode& node);
    
    std::string escape_fts5_term(const std::string& term);
};

// ============================================================================
// SQL WHERE Clause Generator
// ============================================================================

/// Generate SQL WHERE clause from QMD AST (for metadata filters)
class SQLWhereGenerator {
public:
    /// Convert QMD node to SQL WHERE expression
    /// Returns: (where_clause, parameters)
    core::Result<std::pair<std::string, std::vector<std::string>>>
    generate(const QMDNode& node);

private:
    std::pair<std::string, std::vector<std::string>> gen_tag(const QMDNode& node);
    std::pair<std::string, std::vector<std::string>> gen_date(const QMDNode& node);
    std::pair<std::string, std::vector<std::string>> gen_score(const QMDNode& node);
    std::pair<std::string, std::vector<std::string>> gen_and(const QMDNode& node);
    std::pair<std::string, std::vector<std::string>> gen_or(const QMDNode& node);
};

// ============================================================================
// Complete QMD Executor
// ============================================================================

/// Execute QMD query against data store
class QMDExecutor {
public:
    QMDExecutor(std::shared_ptr<QMDParser> parser);
    
    /// Parse and execute query (FTS5 mode)
    core::Result<std::vector<EvalContext>> execute_fts5(
        const std::string& query,
        const std::string& table_name = "memories_fts"
    );
    
    /// Parse and evaluate query (in-memory mode)
    core::Result<std::vector<EvalContext>> execute_memory(
        const std::string& query,
        const std::vector<EvalContext>& contexts
    );

private:
    std::shared_ptr<QMDParser> parser_;
    QMDEvaluator evaluator_;
    FTS5QueryGenerator fts5_gen_;
    SQLWhereGenerator where_gen_;
};

} // namespace query
} // namespace hunterclaw
