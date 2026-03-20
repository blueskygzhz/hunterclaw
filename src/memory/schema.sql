-- 🎯 100% 对齐 OpenClaw Memory Schema
-- OpenClaw: src/memory/memory-schema.ts

-- ========================================
-- Meta Table (对齐 OpenClaw)
-- ========================================
CREATE TABLE IF NOT EXISTS meta (
    key TEXT PRIMARY KEY,
    value TEXT NOT NULL
);

-- ========================================
-- Files Table (对齐 OpenClaw)
-- ========================================
CREATE TABLE IF NOT EXISTS files (
    path TEXT PRIMARY KEY,
    source TEXT NOT NULL DEFAULT 'memory',  -- 'memory' | 'sessions'
    hash TEXT NOT NULL,
    mtime INTEGER NOT NULL,
    size INTEGER NOT NULL
);

-- ========================================
-- Chunks Table (对齐 OpenClaw: 主表名为 chunks)
-- embedding 存储为 TEXT (JSON array)，对齐 openclaw
-- ========================================
CREATE TABLE IF NOT EXISTS chunks (
    id TEXT PRIMARY KEY,
    path TEXT NOT NULL,
    source TEXT NOT NULL DEFAULT 'memory',  -- 'memory' | 'sessions'
    start_line INTEGER NOT NULL,
    end_line INTEGER NOT NULL,
    hash TEXT NOT NULL,
    model TEXT NOT NULL,
    text TEXT NOT NULL,
    embedding TEXT NOT NULL,  -- JSON array (对齐 openclaw)
    updated_at INTEGER NOT NULL
);

CREATE INDEX IF NOT EXISTS idx_chunks_path ON chunks(path);
CREATE INDEX IF NOT EXISTS idx_chunks_source ON chunks(source);

-- ========================================
-- Embedding Cache Table (对齐 OpenClaw)
-- ========================================
CREATE TABLE IF NOT EXISTS embedding_cache (
    provider TEXT NOT NULL,
    model TEXT NOT NULL,
    provider_key TEXT NOT NULL,
    hash TEXT NOT NULL,
    embedding TEXT NOT NULL,  -- JSON array
    dims INTEGER,
    updated_at INTEGER NOT NULL,
    PRIMARY KEY (provider, model, provider_key, hash)
);

CREATE INDEX IF NOT EXISTS idx_embedding_cache_updated_at ON embedding_cache(updated_at);

-- ========================================
-- FTS5 Full-text Search Table (对齐 OpenClaw)
-- ========================================
CREATE VIRTUAL TABLE IF NOT EXISTS chunks_fts USING fts5(
    text,
    id UNINDEXED,
    path UNINDEXED,
    source UNINDEXED,
    model UNINDEXED,
    start_line UNINDEXED,
    end_line UNINDEXED
);

-- ========================================
-- sqlite-vec Virtual Table (可选，需要 sqlite-vec 扩展)
-- 用于 ANN 向量搜索，镜像 chunks 表的 id 和 embedding
-- ========================================
-- CREATE VIRTUAL TABLE IF NOT EXISTS chunks_vec USING vec0(
--     id TEXT PRIMARY KEY,
--     embedding FLOAT[1536]
-- );

-- ========================================
-- Migration: ensureColumn (对齐 openclaw:ensureColumn)
-- 通过 PRAGMA table_info() 检查列是否存在，再 ALTER TABLE
-- 实际迁移逻辑在 sqlite_storage.cc:ensure_schema() 中实现
-- ========================================
