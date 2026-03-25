// Aligned with openclaw: src/plugin-sdk/keyed-async-queue.ts
#pragma once

#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <string>

namespace hunterclaw {
namespace channels {

// ============================================================================
// KeyedAsyncQueue
// Aligned with OpenClaw: KeyedAsyncQueue (plugin-sdk/keyed-async-queue.ts)
//
// Serializes async tasks per key: tasks with the same key run sequentially,
// tasks with different keys run concurrently.
// Each task is a std::function<void()> that runs on the caller's thread pool.
// ============================================================================

class KeyedAsyncQueue {
public:
    KeyedAsyncQueue() = default;
    ~KeyedAsyncQueue() = default;

    // Non-copyable
    KeyedAsyncQueue(const KeyedAsyncQueue&) = delete;
    KeyedAsyncQueue& operator=(const KeyedAsyncQueue&) = delete;

    // Aligned with openclaw: enqueue(key, task)
    // Enqueues a task under the given key. Tasks with the same key are
    // serialized (each waits for the previous to complete). Returns a future
    // that resolves when the task completes.
    std::shared_future<void> enqueue(const std::string& key,
                                     std::function<void()> task);

private:
    std::mutex mutex_;
    // Aligned with openclaw: tails: Map<string, Promise<void>>
    std::map<std::string, std::shared_future<void>> tails_;
};

} // namespace channels
} // namespace hunterclaw
