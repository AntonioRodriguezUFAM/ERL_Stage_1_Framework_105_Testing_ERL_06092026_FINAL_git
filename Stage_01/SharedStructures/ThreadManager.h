
// ThreadManager.h
        /*
        * @brief Initializes the SDL display and creates the window/renderer
        * @return True on success, false otherwise
        */
// ThreadManager.h
//====================================================================================================
// ThreadManager.h          WORKING VERSION !!
//====================================================================================================
// ThreadManager.h
#pragma once

#include <thread>
#include <vector>
#include <unordered_map>
#include <queue>
#include <string>
#include <mutex>
#include <stdexcept>
#include <atomic>
#include <condition_variable>
#include <future>
#include <functional>
#include <spdlog/spdlog.h>

/**
 * @enum Component
 * @brief Identifies the component type for thread categorization.
 */
enum class Component {
    Algorithm,   // For algorithm-related threads
    Camera,      // For camera/vision processing
    Optimizer,   // For optimization tasks
    Network,     // For networking modules
    Lynsyn,    // For Lynsyn-specific tasks
    Custom       // Add more as needed
};

/**
 * @class ThreadManager
 * @brief Manages both "named" threads and a thread-pool for tasks.
 */
class ThreadManager {
public:
    ThreadManager() : stopPool_(false) {
        // Start thread pool with 2 threads by default
        startPool(2);
        }

    // ================== ENUM-BASED INTERFACE ==================
    void addThread(Component component, std::thread&& thread) {
        addThread(componentToString(component), std::move(thread));
    }
    bool joinThreadsFor(Component component) {
        return joinThreadsFor(componentToString(component));
    }

    // ================== STRING-BASED INTERFACE ==================
    void addThread(const std::string& component, std::thread&& thread) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!thread.joinable()) {
            throw std::invalid_argument("Thread must be joinable.");
        }
        threadMap_[component].emplace_back(std::move(thread));
    }
    bool joinThreadsFor(const std::string& component) {
        std::lock_guard<std::mutex> lock(mutex_);
        bool joinedAny = false;
        auto it = threadMap_.find(component);
        if (it != threadMap_.end()) {
            for (auto& t : it->second) {
                if (t.joinable()) {
                    t.join();
                    joinedAny = true;
                }
            }
            threadMap_.erase(it);
        }
        return joinedAny;
    }

    // ================== COMMON FUNCTIONALITY ==================
    void shutdown() {
            // Sequence: Stop pool first to prevent tasks from blocking on named threads,
            // then join named threads to ensure all resources are released.
            // [MOD] Stop pool first to drain tasks, then join named threads to avoid deadlocks if tasks depend on named components.
            stopPool();
            joinAll();
            spdlog::info("ThreadManager shutdown completed");
        }

    // In ThreadManager class

void joinAll() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = threadMap_.begin(); it != threadMap_.end(); ++it) {
        auto& threads = it->second;
        for (auto& thread : threads) {
            if (thread.joinable()) thread.join();
        }
    }
    threadMap_.clear();
}

size_t getThreadCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t total = 0;
    for (auto it = threadMap_.begin(); it != threadMap_.end(); ++it) {
        total += it->second.size();
    }

    std::lock_guard<std::mutex> poolLock(poolMutex_);
    total += workerThreads_.size();
    return total;
}

    // void joinAll() {
    //     std::lock_guard<std::mutex> lock(mutex_);
    //     for (auto& [component, threads] : threadMap_) {
    //         for (auto& t : threads) {
    //             if (t.joinable()) {
    //                 t.join();
    //             }
    //         }
    //     }
    //     threadMap_.clear();
    //     stopPool();
    // }


// // Added method to get total thread count
//     size_t getThreadCount() const {
//         std::lock_guard<std::mutex> lock(mutex_);
//         size_t count = 0;
//         for (const auto& [component, threads] : threadMap_) {
//             count += threads.size();
//         }
//         std::lock_guard<std::mutex> poolLock(poolMutex_);
//         count += workerThreads_.size();
//         return count;
//     }


    bool hasActiveThreads(const std::string& component) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return (threadMap_.find(component) != threadMap_.end());
    }


    // ================== THREAD POOL FUNCTIONALITY ==================
    void startPool(size_t numThreads) {
        {
            std::lock_guard<std::mutex> lock(poolMutex_);
            if (!workerThreads_.empty()) {
                spdlog::error("Thread pool already started with {} threads", workerThreads_.size());
                throw std::runtime_error("Thread pool already started.");
            }
            stopPool_ = false;
            workerThreads_.reserve(numThreads);
        }
        for (size_t i = 0; i < numThreads; ++i) {
            workerThreads_.emplace_back([this]() { this->worker(); });
            spdlog::debug("Started thread pool worker {}", i);
        }
        spdlog::info("Thread pool started with {} threads", numThreads);
    }

    template <typename Func, typename... Args>
    auto submitTask(Func&& f, Args&&... args)
        -> std::future<typename std::result_of<Func(Args...)>::type> {
        using ReturnType = typename std::result_of<Func(Args...)>::type;
        {
            std::lock_guard<std::mutex> lock(poolMutex_);
            if (stopPool_) {
                spdlog::error("Cannot submit task: thread pool is stopped");
                throw std::runtime_error("Thread pool is stopped");
            }
        }
        auto task = std::make_shared<std::packaged_task<ReturnType()>>(
            std::bind(std::forward<Func>(f), std::forward<Args>(args)...)
        );
        std::future<ReturnType> result = task->get_future();
        {
            std::lock_guard<std::mutex> lock(poolMutex_);
            taskQueue_.emplace([task]() { (*task)(); });
        }
        poolCondVar_.notify_one();
        return result;
    }

    void stopPool() {
        {
            std::lock_guard<std::mutex> lock(poolMutex_);
            if (workerThreads_.empty()) {
                return;
            }
            stopPool_ = true;
            // Clear task queue to prevent dangling tasks

            // [MOD] Do not clear taskQueue_ here; allow draining of remaining tasks before join to prevent data loss.
            // std::queue<std::function<void()>> empty;
            // taskQueue_.swap(empty);
            
        }
        poolCondVar_.notify_all();
        for (std::thread& worker : workerThreads_) {
            if (worker.joinable()) {
                worker.join();
                spdlog::debug("Joined thread pool worker");
            }
        }
        workerThreads_.clear();
        spdlog::info("Thread pool stopped");
    }

     ~ThreadManager() {
        try {
            shutdown();
        } catch (const std::exception& e) {
            spdlog::error("Exception in ThreadManager destructor: {}", e.what());
        }
    }
    // Add new method for dynamic resizing
    void resizePool(size_t newSize) {
        // [MOD] Add dynamic pool resizing for scalability (e.g., based on load). Stop current pool, restart with new size.
        stopPool();
        startPool(newSize);
    }

private:
    mutable std::mutex mutex_;
    mutable std::mutex poolMutex_; // Make mutable to allow locking in const methods
    std::unordered_map<std::string, std::vector<std::thread>> threadMap_;

    // Thread pool
    std::vector<std::thread> workerThreads_;
    std::queue<std::function<void()>> taskQueue_;
   // std::mutex poolMutex_;
    std::condition_variable poolCondVar_;
    std::atomic<bool> stopPool_;

    void worker() {
        while (true) {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lock(poolMutex_);
                if (!lock.owns_lock()) {
                    spdlog::error("Failed to acquire lock in worker thread");
                    return;
                    }
                poolCondVar_.wait(lock, [this]() { return stopPool_ || !taskQueue_.empty(); });
                if (stopPool_ && taskQueue_.empty()) {
                    return;
                }
                task = std::move(taskQueue_.front());
                taskQueue_.pop();
            }
            try {
            task();
            }catch (const std::exception& e) {
                spdlog::error("Exception in thread pool task: {}", e.what());
            }
        }
    }

  static std::string componentToString(Component c) {
        switch (c) {
        case Component::Algorithm: return "Algorithm";
        case Component::Camera:    return "Camera";
        case Component::Optimizer: return "Optimizer";
        case Component::Network:   return "Network";
        case Component::Lynsyn:    return "Lynsyn"; // For Lynsyn power monitoring
        case Component::Custom:    return "Custom";
        default:
            spdlog::error("Unknown component enum: {}", static_cast<int>(c));
            throw std::invalid_argument("Unknown component");
        }
    }
};

