// DEPRECATED: This file is removed.
// openclaw alignment: SessionMemoryManager does not exist in openclaw.
// Session memory is handled internally by MemoryIndexManager via:
//   - sessionsDirty / sessionDeltas / warmSession() (manager.ts)
//   - session-files.ts: buildSessionEntry()
// Use memory_index_manager.h instead.
#error "session_memory.h is deprecated. Session memory is handled by MemoryIndexManager internally."
