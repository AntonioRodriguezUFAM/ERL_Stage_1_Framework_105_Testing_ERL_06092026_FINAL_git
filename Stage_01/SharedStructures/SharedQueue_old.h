// SharedQueue.h

//**
// Review of SharedQueue.h
// Overview
// SharedQueue<T> is a thread-safe, bounded queue for producer-consumer patterns, using mutexes and condition variables. It supports stopping/graceful shutdown and optional buffer pooling (via BufferPool). The template allows flexibility (e.g., T = std::shared_ptr<ZeroCopyFrameData> for frame queues).
// BufferPool: A separate class for pre-allocating/reusing buffers (char arrays). It expands dynamically if exhausted.
// Key features:

// Bounded/Unbounded: maxSize enforces capacity; unbounded if 0 (but code asserts >0, so always bounded).
// Operations: Blocking push/pop, non-blocking try_pop, isEmpty/isFull, stop/close.
// Shutdown: stop() sets flag, clears queue, notifies allunblocks waiters.
// Buffer Management: Optional BufferPool integration (though not used in visible methods; likely for T-specific alloc).

// *// @file SharedQueue.h
//  * @brief Thread-safe bounded queue for producer-consumer scenarios with optional buffer pooling.
//  * 
//  * This class provides a thread-safe queue implementation to support producer-consumer scenarios.
//  * It allows producers to push items into the queue and consumers to pop items from it.
//  * The queue can be bounded (with a maximum size) or unbounded, and it supports blocking and non-blocking operations.
//  * Additionally, it includes a mechanism to stop the queue and unblock all waiting threads.
//  * 
//  * Optionally, a BufferPool can be provided to manage memory for the items in the queue.
//  * 
//  * @tparam T The type of items stored in the queue. Typically, this would be std::shared_ptr<ZeroCopyFrameData>.
//  * 
//  * @note This implementation uses C++11 features such as std::mutex, std::condition_variable, and std::atomic.
//  * 
//  * @author Antonio Souto
//  * @date 2024-06-10
//  */
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <queue>
#include <utility>

#include <spdlog/spdlog.h>

// Thread-safe bounded queue used by the persistent camera/algorithm/display
// pipeline. Queue lifetime belongs to ConfigManager/the pipeline, not to an
// individual AlgorithmConcrete instance.
//
// Hot-swap invariant:
//   AlgorithmConcrete may stop/join its own worker, but must NEVER call stop()
//   or restart() on a SharedQueue during a workload transition.
//
// C++11/14 compatible.
template <typename T>
class SharedQueue {
public:
    explicit SharedQueue(std::size_t maxSize = 0)
        : maxSize_(maxSize), stop_(false), rejectedPushCount_(0) {}

    SharedQueue(const SharedQueue&) = delete;
    SharedQueue& operator=(const SharedQueue&) = delete;

    bool push(const T& item) {
        return pushImpl_(item);
    }

    bool push(T&& item) {
        return pushImpl_(std::move(item));
    }

    // Blocking pop retained for existing consumers. It returns false only when
    // the queue has been explicitly stopped and no buffered item remains.
    bool pop(T& item) {
        std::unique_lock<std::mutex> lock(mutex_);
        condNotEmpty_.wait(lock, [this] {
            return stop_ || !queue_.empty();
        });

        if (queue_.empty()) {
            return false;
        }

        item = std::move(queue_.front());
        queue_.pop();
        lock.unlock();
        condNotFull_.notify_one();
        return true;
    }

    // Bounded pop used by hot-swappable workers. False means either timeout or
    // stopped+drained; callers can query isStopped() when the distinction matters.
    template <class Rep, class Period>
    bool pop_for(T& item, const std::chrono::duration<Rep, Period>& timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!condNotEmpty_.wait_for(lock, timeout, [this] {
                return stop_ || !queue_.empty();
            })) {
            return false;
        }

        if (queue_.empty()) {
            return false;
        }

        item = std::move(queue_.front());
        queue_.pop();
        lock.unlock();
        condNotFull_.notify_one();
        return true;
    }

    bool try_pop(T& item) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.empty()) return false;
        item = std::move(queue_.front());
        queue_.pop();
        condNotFull_.notify_one();
        return true;
    }

    // Drop all currently buffered items. Used only at a workload boundary after
    // the old algorithm worker has fully joined, to prevent previous-workload
    // backlog from contaminating the next phase.
    std::size_t drain() {
        std::unique_lock<std::mutex> lock(mutex_);
        const std::size_t n = queue_.size();
        std::queue<T> emptyQueue;
        queue_.swap(emptyQueue);
        lock.unlock();
        condNotFull_.notify_all();
        return n;
    }

    // Process/pipeline lifecycle stop. This is intentionally one-way during
    // normal operation. Hot-swap code must assert !isStopped() rather than
    // silently re-arming a stopped pipeline.
    void stop() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_ = true;
        }
        condNotEmpty_.notify_all();
        condNotFull_.notify_all();
    }

    // Manual lifecycle recovery hook. Production workload switching does NOT
    // call this automatically because a stopped queue may indicate global
    // shutdown or a fatal producer condition.
    void restart() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!stop_) return;
            stop_ = false;
            rejectedPushCount_ = 0;
        }
        condNotEmpty_.notify_all();
        condNotFull_.notify_all();
        spdlog::warn("[SharedQueue] restart(): stop flag cleared manually");
    }

    bool isStopped() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return stop_;
    }

    bool stopped() const { return isStopped(); }

    bool empty() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.empty();
    }

    std::size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }

    std::size_t getSize() const { return size(); }

    std::size_t capacity() const {
        return maxSize_;
    }

private:
    template <typename U>
    bool pushImpl_(U&& item) {
        std::unique_lock<std::mutex> lock(mutex_);

        if (maxSize_ > 0) {
            condNotFull_.wait(lock, [this] {
                return stop_ || queue_.size() < maxSize_;
            });
        }

        if (stop_) {
            ++rejectedPushCount_;
            const std::uint64_t n = rejectedPushCount_;
            // 1,2,4,8,... gives useful diagnostics without creating a power/
            // thermal/logging perturbation during a failed experiment.
            if ((n & (n - 1ULL)) == 0ULL) {
                spdlog::warn(
                    "[SharedQueue] Push to stopped queue ({} rejected so far)", n);
            }
            return false;
        }

        queue_.push(std::forward<U>(item));
        lock.unlock();
        condNotEmpty_.notify_one();
        return true;
    }

    mutable std::mutex mutex_;
    std::condition_variable condNotEmpty_;
    std::condition_variable condNotFull_;
    std::queue<T> queue_;
    std::size_t maxSize_;
    bool stop_;
    std::uint64_t rejectedPushCount_;
};
