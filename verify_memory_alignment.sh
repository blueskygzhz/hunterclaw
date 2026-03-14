#!/bin/bash
# 🎯 HunterClaw Memory 检索系统对齐验证脚本
# 版本: 1.0

set -e

BOLD='\033[1m'
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

echo -e "${BOLD}${BLUE}========================================${NC}"
echo -e "${BOLD}${BLUE}HunterClaw Memory 检索系统对齐验证${NC}"
echo -e "${BOLD}${BLUE}========================================${NC}\n"

PASS=0
FAIL=0
WARN=0

check_pass() {
    echo -e "${GREEN}✓${NC} $1"
    ((PASS++))
}

check_fail() {
    echo -e "${RED}✗${NC} $1"
    ((FAIL++))
}

check_warn() {
    echo -e "${YELLOW}⚠${NC} $1"
    ((WARN++))
}

# ========================================
# 1. Memory Tools 验证
# ========================================
echo -e "${BOLD}[1/8] Memory Tools 验证${NC}"

if [ -f "include/hunterclaw/tools/memory_tools.h" ]; then
    check_pass "memory_tools.h 存在"
else
    check_fail "memory_tools.h 不存在"
fi

if [ -f "src/tools/memory_tools.cc" ]; then
    check_pass "memory_tools.cc 存在"
else
    check_fail "memory_tools.cc 不存在"
fi

if grep -q "MemorySearchTool" include/hunterclaw/tools/memory_tools.h 2>/dev/null; then
    check_pass "MemorySearchTool 定义存在"
else
    check_fail "MemorySearchTool 定义不存在"
fi

if grep -q "MemoryGetTool" include/hunterclaw/tools/memory_tools.h 2>/dev/null; then
    check_pass "MemoryGetTool 定义存在"
else
    check_fail "MemoryGetTool 定义不存在"
fi

echo ""

# ========================================
# 2. SQLite Schema 验证
# ========================================
echo -e "${BOLD}[2/8] SQLite Schema 验证${NC}"

if [ -f "src/memory/schema.sql" ]; then
    check_pass "schema.sql 存在"
    
    # 验证关键表
    if grep -q "CREATE TABLE.*chunks" src/memory/schema.sql; then
        check_pass "chunks 表定义存在"
    else
        check_fail "chunks 表定义不存在"
    fi
    
    # 验证 OpenClaw 对齐字段
    if grep -q "start_line" src/memory/schema.sql; then
        check_pass "start_line 字段存在"
    else
        check_fail "start_line 字段不存在"
    fi
    
    if grep -q "end_line" src/memory/schema.sql; then
        check_pass "end_line 字段存在"
    else
        check_fail "end_line 字段不存在"
    fi
    
    if grep -q "path" src/memory/schema.sql; then
        check_pass "path 字段存在"
    else
        check_fail "path 字段不存在"
    fi
    
    if grep -q "hash" src/memory/schema.sql; then
        check_pass "hash 字段存在"
    else
        check_fail "hash 字段不存在"
    fi
    
    if grep -q "model" src/memory/schema.sql; then
        check_pass "model 字段存在"
    else
        check_fail "model 字段不存在"
    fi
else
    check_fail "schema.sql 不存在"
fi

echo ""

# ========================================
# 3. FTS5 支持验证
# ========================================
echo -e "${BOLD}[3/8] FTS5 全文检索验证${NC}"

if grep -q "chunks_fts.*fts5" src/memory/schema.sql 2>/dev/null; then
    check_pass "FTS5 表定义存在"
else
    check_fail "FTS5 表定义不存在"
fi

if [ -f "src/memory/sqlite_storage_aligned.cc" ]; then
    if grep -q "text_search" src/memory/sqlite_storage_aligned.cc; then
        check_pass "text_search 实现存在"
        
        if grep -q "chunks_fts MATCH" src/memory/sqlite_storage_aligned.cc; then
            check_pass "使用 FTS5 MATCH 查询"
        else
            check_warn "未使用 FTS5 MATCH (可能是 fallback)"
        fi
        
        if grep -q "bm25_score\\|rank" src/memory/sqlite_storage_aligned.cc; then
            check_pass "BM25 评分存在"
        else
            check_warn "BM25 评分缺失"
        fi
    else
        check_fail "text_search 实现不存在"
    fi
fi

echo ""

# ========================================
# 4. Embedding Providers 验证
# ========================================
echo -e "${BOLD}[4/8] Embedding Providers 验证${NC}"

if grep -q "create_openai" include/hunterclaw/memory/memory.h 2>/dev/null; then
    check_pass "OpenAI Provider 支持"
else
    check_warn "OpenAI Provider 可能缺失"
fi

if grep -q "create_ollama" include/hunterclaw/memory/memory.h 2>/dev/null; then
    check_pass "Ollama Provider 支持"
else
    check_warn "Ollama Provider 可能缺失"
fi

if grep -q "create_gemini\\|gemini" include/hunterclaw/memory/memory.h 2>/dev/null; then
    check_pass "Gemini Provider 支持"
else
    check_warn "Gemini Provider 缺失 (非关键)"
fi

if grep -q "create_voyage\\|voyage" include/hunterclaw/memory/memory.h 2>/dev/null; then
    check_warn "Voyage AI Provider 缺失 (非关键)"
else
    check_warn "Voyage AI Provider 缺失"
fi

echo ""

# ========================================
# 5. Hybrid Search 验证
# ========================================
echo -e "${BOLD}[5/8] Hybrid Search 验证${NC}"

if grep -q "hybrid_rank" src/memory/memory_manager.cc 2>/dev/null; then
    check_pass "hybrid_rank 实现存在"
else
    check_fail "hybrid_rank 实现不存在"
fi

if grep -q "vector_weight.*0\\.7" src/memory/memory_manager.cc 2>/dev/null; then
    check_pass "vector_weight = 0.7 (对齐 OpenClaw)"
else
    check_warn "vector_weight 可能不是 0.7"
fi

if grep -q "text_weight.*0\\.3" src/memory/memory_manager.cc 2>/dev/null; then
    check_pass "text_weight = 0.3 (对齐 OpenClaw)"
else
    check_warn "text_weight 可能不是 0.3"
fi

echo ""

# ========================================
# 6. MMR Reranking 验证
# ========================================
echo -e "${BOLD}[6/8] MMR Reranking 验证${NC}"

if grep -q "maximal_marginal_relevance" src/memory/memory_manager.cc 2>/dev/null; then
    check_pass "MMR 实现存在"
else
    check_fail "MMR 实现不存在"
fi

if grep -q "lambda.*0\\.7" src/memory/memory_manager.cc 2>/dev/null; then
    check_pass "MMR lambda = 0.7 (对齐 OpenClaw)"
else
    check_warn "MMR lambda 可能不是 0.7"
fi

echo ""

# ========================================
# 7. Temporal Decay 验证
# ========================================
echo -e "${BOLD}[7/8] Temporal Decay 验证${NC}"

if grep -q "apply_temporal_decay" src/memory/memory_manager.cc 2>/dev/null; then
    check_pass "Temporal Decay 实现存在"
else
    check_warn "Temporal Decay 实现可能缺失"
fi

if grep -q "half_life.*30" include/hunterclaw/memory/memory.h 2>/dev/null; then
    check_pass "Half-life = 30 days (对齐 OpenClaw)"
else
    check_warn "Half-life 可能不是 30 days"
fi

echo ""

# ========================================
# 8. Session Memory 验证
# ========================================
echo -e "${BOLD}[8/8] Session Memory 验证${NC}"

if grep -q "SessionMemoryProvider" include/hunterclaw/memory/provider.h 2>/dev/null; then
    check_pass "SessionMemoryProvider 存在"
else
    check_warn "SessionMemoryProvider 可能缺失"
fi

if grep -q "session_id" include/hunterclaw/memory/provider.h 2>/dev/null; then
    check_pass "session_id 支持"
else
    check_warn "session_id 支持可能缺失"
fi

echo ""

# ========================================
# 总结
# ========================================
echo -e "${BOLD}${BLUE}========================================${NC}"
echo -e "${BOLD}验证结果总结${NC}"
echo -e "${BOLD}${BLUE}========================================${NC}\n"

TOTAL=$((PASS + FAIL + WARN))
PERCENTAGE=$((PASS * 100 / TOTAL))

echo -e "${GREEN}✓ 通过:${NC} $PASS"
echo -e "${RED}✗ 失败:${NC} $FAIL"
echo -e "${YELLOW}⚠ 警告:${NC} $WARN"
echo -e "${BOLD}总计:${NC} $TOTAL"
echo -e "${BOLD}对齐度:${NC} ${PERCENTAGE}%\n"

if [ $FAIL -eq 0 ]; then
    echo -e "${GREEN}${BOLD}🎉 验证通过! Memory 系统基本对齐 OpenClaw!${NC}"
    exit 0
elif [ $FAIL -lt 5 ]; then
    echo -e "${YELLOW}${BOLD}⚠️ 部分对齐,还有 $FAIL 个关键问题需要修复!${NC}"
    exit 1
else
    echo -e "${RED}${BOLD}❌ 对齐度不足,需要立即修复 $FAIL 个问题!${NC}"
    exit 1
fi
