#pragma once

// ============================================================================
// session_actor_queue.h
// Aligned with openclaw: src/acp/control-plane/session-actor-queue.ts
//
// Per-session serialized execution queue (Actor model).
// Operations on the same sessionKey execute serially;
// different sessions execute in parallel.
// ============================================================================

#include <string>
#include <functional>
#include <map>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <atomic>
#include <future>

namespace hunterclaw {
namespace acp {

class SessionActorQueue {
public:
    SessionActorQueue() = default;
    ~SessionActorQueue() = default;

    // Aligned with openclaw: SessionActorQueue.run<T>(actorKey, op)
    // Serializes operations per actorKey.
    template <typename Fn>
    auto run(const std::string& actor_key, Fn&& op) -> decltype(op()) {
        using ResultType = decltype(op());

        auto promise = std::make_shared<std::promise<ResultType>>();
        auto future = promise->get_future();

        std::function<void()> task;
        if constexpr (std::is_void_v<ResultType>) {
            task = [op = std::forward<Fn>(op), promise]() mutable {
                try {
                    op();
                    promise->set_value();
                } catch (...) {
                    promise->set_exception(std::current_exception());
                }
            };
        } else {
            task = [op = std::forward<Fn>(op), promise]() mutable {
                try {
                    promise->set_value(op());
                } catch (...) {
                    promise->set_exception(std::current_exception());
                }
            };
        }

        enqueue(actor_key, std::move(task));
        return future.get();
    }

    // Aligned with openclaw: getTotalPendingCount
    int get_total_pending_count() const {
        std::lock_guard<std::mutex> lock(mutex_);
        int total = 0;
        for (auto& [_, count] : pending_by_session_) {
            total += count;
        }
        return total;
    }

private:
    struct LaneState {
        std::queue<std::function<void()>> tasks;
        bool running = false;
    };

    mutable std::mutex mutex_;
    std::map<std::string, LaneState> lanes_;
    std::map<std::string, int> pending_by_session_;

    void enqueue(const std::string& actor_key, std::function<void()> task) {
        std::unique_lock<std::mutex> lock(mutex_);
        auto& lane = lanes_[actor_key];
        pending_by_session_[actor_key]++;
        lane.tasks.push(std::move(task));

        if (!lane.running) {
            lane.running = true;
            lock.unlock();
            drain_lane(actor_key);
        }
    }

    void drain_lane(const std::string& actor_key) {
        while (true) {
            std::function<void()> task;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                auto it = lanes_.find(actor_key);
                if (it == lanes_.end() || it->second.tasks.empty()) {
                    if (it != lanes_.end()) {
                        it->second.running = false;
                        if (it->second.tasks.empty()) {
                            lanes_.erase(it);
                        }
                    }
                    return;
                }
                task = std::move(it->second.tasks.front());
                it->second.tasks.pop();
            }

            // Execute outside lock
            task();

            {
                std::lock_guard<std::mutex> lock(mutex_);
                auto pit = pending_by_session_.find(actor_key);
                if (pit != pending_by_session_.end()) {
                    pit->second--;
                    if (pit->second <= 0) {
                        pending_by_session_.erase(pit);
                    }
                }
            }
        }
    }
};

} // namespace acp
} // namespace hunterclaw
