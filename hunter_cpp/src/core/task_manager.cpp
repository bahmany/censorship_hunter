#include "core/task_manager.h"
#include "core/utils.h"

#include <algorithm>
#include <stdexcept>

namespace hunter {

// ─── ThreadPool ───

ThreadPool::ThreadPool(size_t num_threads, const std::string& name_prefix)
    : name_prefix_(name_prefix) {
    for (size_t i = 0; i < num_threads; i++) {
        workers_.emplace_back(&ThreadPool::workerLoop, this, (int)i);
    }
}

ThreadPool::~ThreadPool() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
    }
    cv_.notify_all();
    for (auto& w : workers_) {
        if (w.joinable()) w.join();
    }
}

size_t ThreadPool::pendingTasks() const {
    return pending_.load();
}

size_t ThreadPool::activeTasks() const {
    return active_.load();
}

void ThreadPool::resize(size_t new_size) {
    if (new_size < 1) new_size = 1;
    std::lock_guard<std::mutex> lock(mutex_);
    if (stop_) return;
    // Only grow — shrinking is complex; threads will naturally idle
    while (workers_.size() < new_size) {
        int id = static_cast<int>(workers_.size());
        workers_.emplace_back(&ThreadPool::workerLoop, this, id);
    }
}

void ThreadPool::workerLoop(int id) {
    while (true) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] { return stop_ || !tasks_.empty(); });
            if (stop_ && tasks_.empty()) return;
            task = std::move(tasks_.front());
            tasks_.pop();
            pending_.fetch_sub(1);
        }
        active_.fetch_add(1);
        try {
            task();
        } catch (...) {
        }
        active_.fetch_sub(1);
    }
}

// ─── HunterTaskManager ───

HunterTaskManager& HunterTaskManager::instance() {
    static HunterTaskManager inst;
    return inst;
}

HunterTaskManager::HunterTaskManager() {
    hw_ = HardwareSnapshot::detect();
    hw_ts_ = utils::nowTimestamp();
    io_pool_ = std::make_unique<ThreadPool>(hw_.io_pool_size, "hunter-io");
    cpu_pool_ = std::make_unique<ThreadPool>(hw_.cpu_pool_size, "hunter-cpu");
}

HunterTaskManager::~HunterTaskManager() {
    shutdown();
}

HardwareSnapshot HunterTaskManager::getHardware() {
    std::lock_guard<std::mutex> lock(hw_mutex_);
    double now = utils::nowTimestamp();
    if (now - hw_ts_ > HW_TTL) {
        hw_ = HardwareSnapshot::detect();
        hw_ts_ = now;
    }
    return hw_;
}

void HunterTaskManager::maybeResize() {
    auto hw = getHardware();
    if (!io_pool_ || !cpu_pool_) return;

    const size_t current_io = io_pool_->size();
    const size_t target_io = static_cast<size_t>(hw.io_pool_size);
    const size_t current_cpu = cpu_pool_->size();
    const size_t target_cpu = static_cast<size_t>(hw.cpu_pool_size);

    if (target_io != current_io) {
        io_pool_->resize(target_io);
    }
    if (target_cpu != current_cpu) {
        cpu_pool_->resize(target_cpu);
    }
}

HunterTaskManager::Metrics HunterTaskManager::getMetrics() const {
    return Metrics{
        io_pool_ ? (int)io_pool_->size() : 0,
        cpu_pool_ ? (int)cpu_pool_->size() : 0,
        io_pool_ ? (int)io_pool_->pendingTasks() : 0,
        cpu_pool_ ? (int)cpu_pool_->pendingTasks() : 0,
        io_pool_ ? (int)io_pool_->activeTasks() : 0,
        cpu_pool_ ? (int)cpu_pool_->activeTasks() : 0
    };
}

void HunterTaskManager::shutdown() {
    io_pool_.reset();
    cpu_pool_.reset();
}

} // namespace hunter
