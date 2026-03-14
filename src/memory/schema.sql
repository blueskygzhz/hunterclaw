-- 🎯 100% 对齐 OpenClaw Memory Schema
-- OpenClaw: src/memory/memory-schema.ts

-- ========================================
-- Meta Table
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
-- Chunks Table (对齐 OpenClaw)
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
    embedding TEXT NOT NULL,  -- JSON array or BLOB
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
    embedding TEXT NOT NULL,
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
-- Migration: 添加缺失字段 (兼容旧版本)
-- ========================================

-- 检查并添加 source 字段
-- SQLite 不支持 IF NOT EXISTS for columns, 需要捕获错误
-- 实际实现应该用 PRAGMA table_info() 检查

-- ALTER TABLE files ADD COLUMN source TEXT NOT NULL DEFAULT 'memory';
-- ALTER TABLE chunks ADD COLUMN source TEXT NOT NULL DEFAULT 'memory';
-- ALTER TABLE chunks ADD COLUMN start_line INTEGER NOT NULL DEFAULT 1;
-- ALTER TABLE chunks ADD COLUMN end_line INTEGER NOT NULL DEFAULT 1;
-- ALTER TABLE chunks ADD COLUMN hash TEXT NOT NULL DEFAULT '';
-- ALTER TABLE chunks ADD COLUMN model TEXT NOT NULL DEFAULT 'unknown';
