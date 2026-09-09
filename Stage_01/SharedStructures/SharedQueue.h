// SharedQueue_v32_Adjusted.h
// Production-safe SharedQueue for persistent pipeline / workload hot-swap.
// C++14 compatible (Jetson Nano toolchain).
//
// Ownership rule:
//   SharedQueue lifetime belongs to the pipeline/ConfigManager.
//   Individual consumers (for example AlgorithmConcrete) must NOT call stop()
//   merely to terminate/restart their own worker thread.
//
// Hot-swap support added without changing existing push/pop signatures:
//   - pop_for(...) : bounded consumer wait, so a worker can terminate without
//                    stopping a shared queue.
//   - drain()      : clears stale phase-boundary items and wakes blocked producers.
//   - isStopped()  : invariant/diagnostic check.
//   - rejectedPushCount() + rate-limited stopped-queue warnings.
//
// Deliberately NO restart()/rearm() API. A stopped queue is a pipeline lifecycle
// state and should be treated as an invariant violation during workload hot-swap.

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <utility>

#include <spdlog/spdlog.h>

// =============================================================================
// BufferPool
// =============================================================================
class BufferPool {
public:
    BufferPool(std::size_t bufferSize, std::size_t poolSize)
        : bufferSize_(bufferSize), poolSize_(poolSize) {
        if (bufferSize_ == 0 || poolSize_ == 0) {
            spdlog::error(
                "[BufferPool] Invalid bufferSize ({}) or poolSize ({}).",
                bufferSize_, poolSize_);
            throw std::invalid_argument("Invalid buffer or pool size");
        }

        for (std::size_t i = 0; i < poolSize_; ++i) {
            freeBuffers_.push(std::make_unique<char[]>(bufferSize_));
        }
    }

    std::unique_ptr<char[]> acquireBuffer() {
        std::lock_guard<std::mutex> lock(mutex_);

        if (freeBuffers_.empty()) {
            spdlog::warn("[BufferPool] Expanding pool.");
            return std::make_unique<char[]>(bufferSize_);
        }

        std::unique_ptr<char[]> buffer = std::move(freeBuffers_.front());
        freeBuffers_.pop();
        return buffer;
    }

    void releaseBuffer(std::unique_ptr<char[]> buffer) {
        if (!buffer) {
            return;
        }

        std::lock_guard<std::mutex> lock(mutex_);
        freeBuffers_.push(std::move(buffer));
    }

private:
    std::size_t bufferSize_;
    std::size_t poolSize_;
    std::queue<std::unique_ptr<char[]> > freeBuffers_;
    std::mutex mutex_;
};

// =============================================================================
// SharedQueue
// =============================================================================
template <typename T>
class SharedQueue {
public:
    /**
     * @param maxSize Maximum number of queued items. 0 means unbounded.
     * @param bufferPool Optional buffer pool retained for compatibility.
     */
    explicit SharedQueue(
        std::size_t maxSize = 100,
        std::shared_ptr<BufferPool> bufferPool = nullptr)
        : maxSize_(maxSize),
          stop_(false),
          rejectedPushes_(0),
          bufferPool_(std::move(bufferPool)) {
        if (maxSize_ == 0) {
            spdlog::warn("[SharedQueue] Unbounded queue; monitor for OOM.");
        }

        spdlog::info("[SharedQueue] Initialized with maxSize: {}.", maxSize_);
    }

    ~SharedQueue() {
        stop();
        spdlog::info("[SharedQueue] Destroyed.");
    }

    SharedQueue(const SharedQueue&) = delete;
    SharedQueue& operator=(const SharedQueue&) = delete;

    // -------------------------------------------------------------------------
    // Producer API -- existing signatures preserved.
    // -------------------------------------------------------------------------

    /**
     * Blocking push.
     * Waits while a bounded queue is full. Returns false if the queue is stopped.
     */
    bool push(const T& item) {
        std::unique_lock<std::mutex> lock(mutex_);

        condNotFull_.wait(lock, [this] {
            return stop_.load(std::memory_order_acquire) ||
                   maxSize_ == 0 ||
                   queue_.size() < maxSize_;
        });

        if (stop_.load(std::memory_order_acquire)) {
            noteRejectedPush_();
            return false;
        }

        queue_.push(item);
        lock.unlock();
        condNotEmpty_.notify_one();
        return true;
    }

    /**
     * Blocking move-push. Returns false if the queue is stopped.
     * Existing callers that ignore the return value remain source-compatible.
     */
    bool push(T&& item) {
        std::unique_lock<std::mutex> lock(mutex_);

        condNotFull_.wait(lock, [this] {
            return stop_.load(std::memory_order_acquire) ||
                   maxSize_ == 0 ||
                   queue_.size() < maxSize_;
        });

        if (stop_.load(std::memory_order_acquire)) {
            noteRejectedPush_();
            return false;
        }

        queue_.push(std::move(item));
        lock.unlock();
        condNotEmpty_.notify_one();
        return true;
    }

    // -------------------------------------------------------------------------
    // Consumer API.
    // -------------------------------------------------------------------------

    /**
     * Blocking pop.
     *
     * If stop() has been called but buffered items remain, those items can still
     * be drained. Returns false only when stopped AND empty.
     */
    bool pop(T& item) {
        std::unique_lock<std::mutex> lock(mutex_);

        condNotEmpty_.wait(lock, [this] {
            return stop_.load(std::memory_order_acquire) || !queue_.empty();
        });

        if (queue_.empty()) {
            // Predicate can reach here only because stop_ is true.
            return false;
        }

        item = std::move(queue_.front());
        queue_.pop();

        lock.unlock();
        if (maxSize_ > 0) {
            condNotFull_.notify_one();
        }

        return true;
    }

    /**
     * Timed pop for hot-swappable consumers.
     *
     * Returns:
     *   true  -> an item was removed.
     *   false -> timeout while queue remained empty, OR queue is stopped+empty.
     *
     * Call isStopped() when the caller needs to distinguish timeout from
     * pipeline shutdown.
     *
     * AlgorithmConcrete should use this instead of pop() so stopAlgorithm()
     * can set its own running_ flag and join the worker without stopping this
     * shared queue.
     */
    template <class Rep, class Period>
    bool pop_for(
        T& item,
        const std::chrono::duration<Rep, Period>& timeout) {
        std::unique_lock<std::mutex> lock(mutex_);

        const bool ready = condNotEmpty_.wait_for(
            lock,
            timeout,
            [this] {
                return stop_.load(std::memory_order_acquire) || !queue_.empty();
            });

        if (!ready) {
            return false;  // Healthy queue timeout.
        }

        if (queue_.empty()) {
            return false;  // Stopped and drained.
        }

        item = std::move(queue_.front());
        queue_.pop();

        lock.unlock();
        if (maxSize_ > 0) {
            condNotFull_.notify_one();
        }

        return true;
    }

    /** Non-blocking pop. */
    bool try_pop(T& item) {
        std::unique_lock<std::mutex> lock(mutex_);

        if (queue_.empty()) {
            return false;
        }

        item = std::move(queue_.front());
        queue_.pop();

        lock.unlock();
        if (maxSize_ > 0) {
            condNotFull_.notify_one();
        }

        return true;
    }

    // -------------------------------------------------------------------------
    // Hot-swap / lifecycle support.
    // -------------------------------------------------------------------------

    /**
     * Remove all currently buffered items without changing queue lifecycle.
     *
     * Intended for a workload phase boundary AFTER the old consumer worker has
     * fully joined. This prevents stale frames/results from crossing into the
     * replacement workload. Blocked producers are awakened after space is freed.
     */
    std::size_t drain() {
        std::size_t removed = 0;

        {
            std::lock_guard<std::mutex> lock(mutex_);
            removed = queue_.size();

            std::queue<T> empty;
            queue_.swap(empty);
        }

        if (removed > 0 && maxSize_ > 0) {
            condNotFull_.notify_all();
        }

        return removed;
    }

    /**
     * Pipeline lifecycle stop.
     *
     * This operation is intentionally one-way for this queue instance.
     * It MUST NOT be called by AlgorithmConcrete during a workload hot-swap.
     * ConfigManager/pipeline shutdown owns this lifecycle transition.
     *
     * Existing buffered items are NOT discarded here.
     */
    void stop() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_.store(true, std::memory_order_release);
        }

        condNotEmpty_.notify_all();
        condNotFull_.notify_all();
    }

    void shutdown() { stop(); }
    void close() { stop(); }

    // -------------------------------------------------------------------------
    // Diagnostics / state snapshots.
    // -------------------------------------------------------------------------

    bool isEmpty() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.empty();
    }

    bool isFull() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return maxSize_ > 0 && queue_.size() >= maxSize_;
    }

    std::size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }

    std::size_t capacity() const {
        return maxSize_;
    }

    bool isStopped() const {
        return stop_.load(std::memory_order_acquire);
    }

    std::uint64_t rejectedPushCount() const {
        return rejectedPushes_.load(std::memory_order_relaxed);
    }

private:
    /**
     * Log stopped-queue push rejection at powers of two: 1,2,4,8,...
     * This preserves evidence of a lifecycle defect without creating the
     * hundreds-of-thousands-line warning flood seen in the failed runtime.
     */
    void noteRejectedPush_() {
        const std::uint64_t n =
            rejectedPushes_.fetch_add(1, std::memory_order_relaxed) + 1ULL;

        if (n == 1ULL || (n & (n - 1ULL)) == 0ULL) {
            spdlog::warn(
                "[SharedQueue] Push to stopped queue ({} rejected so far).",
                n);
        }
    }

private:
    std::queue<T> queue_;
    mutable std::mutex mutex_;
    std::condition_variable condNotEmpty_;
    std::condition_variable condNotFull_;

    const std::size_t maxSize_;
    std::atomic<bool> stop_;
    std::atomic<std::uint64_t> rejectedPushes_;
    std::shared_ptr<BufferPool> bufferPool_;
};