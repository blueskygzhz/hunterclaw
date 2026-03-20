// DEPRECATED: Integrated MemoryManager (compaction/session/embedding) is removed.
// openclaw alignment: MemoryIndexManager (manager.ts) is the single entry point.
// - Compaction: use CompactionManager directly
// - Session memory: handled internally by MemoryIndexManager (sessionsDirty/sessionDeltas)
// - Embedding: handled internally by MemoryIndexManager (manager-embedding-ops.ts)
// This file is intentionally empty.
