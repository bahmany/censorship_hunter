#pragma once

#include <thread>
#include <mutex>
#include <atomic>
#include <functional>
#include <future>
#include <queue>
#include <vector>
#include <condition_variable>

#include "core/models.h"

namespace hunter {

/**
 * @brief Thread pool for I/O and CPU bound tasks
 */
class ThreadPool {
public:
    explicit ThreadPool(size_t num_threads, const std::string& name_prefix = "pool");
    ~ThreadPool();

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    /**
     * @brief Submit a task to the pool
     */
    template<typename F, typename... Args>
    auto submit(F&& f, Args&&... args) -> std::future<typename std::invoke_result<F, Args...>::type> {
        using ReturnType = typename std::invoke_result<F, Args...>::type;
        auto task = std::make_shared<std::packaged_task<ReturnType()>>(
            std::bind(std::forward<F>(f), std::forward<Args>(args)...)
        );
        auto future = task->get_future();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stop_) throw std::runtime_error("ThreadPool is stopped");
            tasks_.emplace([task]() { (*task)(); });
            pending_.fetch_add(1);
        }
        cv_.notify_one();
        return future;
    }

    size_t size() const { return workers_.size(); }
    size_t pendingTasks() const;
    size_t activeTasks() const;
    void resize(size_t new_size);

private:
    std::vector<std::thread> workers_;
    std::queue<std::function<void()>> tasks_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::atomic<bool> stop_{false};
    std::atomic<size_t> pending_{0};
    std::atomic<size_t> active_{0};
    std::string name_prefix_;

    void workerLoop(int id);
};

/**
 * @brief Unified task manager — singleton per process
 * 
 * Provides shared I/O and CPU thread pools, hardware-aware sizing,
 * memory pressure detection, and a shared fetch lock for coordination.
 */
class HunterTaskManager {
public:
    static HunterTaskManager& instance();

    // Delete copy/move
    HunterTaskManager(const HunterTaskManager&) = delete;
    HunterTaskManager& operator=(const HunterTaskManager&) = delete;

    /**
     * @brief Get cached hardware snapshot (refreshes every 10s)
     */
    HardwareSnapshot getHardware();

    /**
     * @brief Submit I/O-bound task (network, XRay subprocess)
     */
    template<typename F, typename... Args>
    auto submitIO(F&& f, Args&&... args) {
        return io_pool_->submit(std::forward<F>(f), std::forward<Args>(args)...);
    }

    /**
     * @brief Submit CPU-bound task (parsing, obfuscation)
     */
    template<typename F, typename... Args>
    auto submitCPU(F&& f, Args&&... args) {
        return cpu_pool_->submit(std::forward<F>(f), std::forward<Args>(args)...);
    }

    /**
     * @brief Shared fetch lock for coordinating network fetches
     */
    std::timed_mutex& fetchLock() { return fetch_lock_; }

    /**
     * @brief Maybe trigger GC / pool resize based on memory pressure
     */
    void maybeResize();

    /**
     * @brief Get pool metrics
     */
    struct Metrics {
        int io_pool_size;
        int cpu_pool_size;
        int io_pending;
        int cpu_pending;
        int io_active;
        int cpu_active;
    };
    Metrics getMetrics() const;

    /**
     * @brief Shutdown all pools
     */
    void shutdown();

private:
    HunterTaskManager();
    ~HunterTaskManager();

    std::unique_ptr<ThreadPool> io_pool_;
    std::unique_ptr<ThreadPool> cpu_pool_;
    std::timed_mutex fetch_lock_;

    HardwareSnapshot hw_;
    double hw_ts_ = 0.0;
    static constexpr double HW_TTL = 10.0;
    std::mutex hw_mutex_;
};

} // namespace hunter
