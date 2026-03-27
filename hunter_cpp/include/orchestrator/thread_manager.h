#pragma once

#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <atomic>
#include <thread>
#include <functional>
#include <memory>
#include <condition_variable>

#include "core/models.h"
#include "core/task_manager.h"

namespace hunter {

// Forward declarations
class HunterOrchestrator;

namespace orchestrator {

/**
 * @brief Base class for all managed worker threads
 */
class BaseWorker {
public:
    std::string name;
    int interval_seconds = 1800;

    BaseWorker(const std::string& worker_name, int interval,
               std::atomic<bool>& stop_event);
    virtual ~BaseWorker();

    BaseWorker(const BaseWorker&) = delete;
    BaseWorker& operator=(const BaseWorker&) = delete;

    void start();
    void join(float timeout = 10.0f);
    void requestStop();
    WorkerStatus getStatus() const;
    void setPauseCallback(std::function<bool()> callback);

protected:
    virtual void execute() = 0;

    std::atomic<bool>& stop_;
    WorkerStatus status_;
    mutable std::mutex status_mutex_;
    std::thread thread_;
    std::function<bool()> pause_callback_;

    void runLoop();
    void updateExtra(const std::string& key, const std::string& value);
    HardwareSnapshot getHardware();
    bool sleepInterruptible(std::chrono::milliseconds duration);

private:
    void asyncLoop();
    std::mutex stop_wait_mutex_;
    std::condition_variable stop_wait_cv_;
};

/**
 * @brief Config scanner worker — discovers & validates configs
 */
class ConfigScannerWorker : public BaseWorker {
public:
    ConfigScannerWorker(HunterOrchestrator* orch, std::atomic<bool>& stop);
protected:
    void execute() override;
private:
    HunterOrchestrator* orch_;
};

/**
 * @brief Telegram publisher worker
 */
class TelegramPublisherWorker : public BaseWorker {
public:
    TelegramPublisherWorker(HunterOrchestrator* orch, std::atomic<bool>& stop);
protected:
    void execute() override;
private:
    HunterOrchestrator* orch_;
};

/**
 * @brief Balancer health monitor worker
 */
class BalancerWorker : public BaseWorker {
public:
    BalancerWorker(HunterOrchestrator* orch, std::atomic<bool>& stop);
protected:
    void execute() override;
private:
    HunterOrchestrator* orch_;
};

/**
 * @brief Health monitor worker — watches RAM/CPU
 */
class HealthMonitorWorker : public BaseWorker {
public:
    HealthMonitorWorker(std::atomic<bool>& stop,
                        std::vector<BaseWorker*> all_workers);
protected:
    void execute() override;
private:
    std::vector<BaseWorker*> workers_;
};

/**
 * @brief Aggressive harvester worker
 */
class HarvesterWorker : public BaseWorker {
public:
    HarvesterWorker(HunterOrchestrator* orch, std::atomic<bool>& stop);
protected:
    void execute() override;
private:
    HunterOrchestrator* orch_;
    bool first_run_ = true;
    int harvest_count_ = 0;
};

/**
 * @brief Background GitHub config downloader worker
 */
class GitHubDownloaderWorker : public BaseWorker {
public:
    GitHubDownloaderWorker(HunterOrchestrator* orch, std::atomic<bool>& stop);
protected:
    void execute() override;
private:
    HunterOrchestrator* orch_;
    bool first_run_ = true;
    int download_count_ = 0;
    double last_success_ts_ = 0.0;
    bool pending_connectivity_retry_ = false;
    std::set<std::string> seen_;
    std::string cache_path_;

    std::string cacheFile();
    void loadSeen();
    int appendNew(const std::vector<std::string>& configs);
};

class IranAssetsWorker : public BaseWorker {
public:
    IranAssetsWorker(HunterOrchestrator* orch, std::atomic<bool>& stop);
protected:
    void execute() override;
private:
    HunterOrchestrator* orch_;
    int download_count_ = 0;
};

/**
 * @brief Continuous validator worker
 */
class ValidatorWorker : public BaseWorker {
public:
    ValidatorWorker(HunterOrchestrator* orch, std::atomic<bool>& stop);
protected:
    void execute() override;
private:
    HunterOrchestrator* orch_;
};

/**
 * @brief DPI pressure worker
 */
class DpiPressureWorker : public BaseWorker {
public:
    DpiPressureWorker(HunterOrchestrator* orch, std::atomic<bool>& stop);
protected:
    void execute() override;
private:
    HunterOrchestrator* orch_;
};

/**
 * @brief Import watcher worker — monitors config/import/ folder for manually added configs
 * 
 * Users can drop .txt files containing proxy URIs into config/import/ and this worker
 * will automatically:
 * 1. Scan all .txt files in the folder
 * 2. Extract valid proxy URIs (vmess://, vless://, trojan://, ss://, etc.)
 * 3. Remove duplicates
 * 4. Remove malformed/invalid URIs
 * 5. Add valid configs to ConfigDatabase for testing
 * 6. Move processed files to config/import/processed/
 */
class ImportWatcherWorker : public BaseWorker {
public:
    ImportWatcherWorker(HunterOrchestrator* orch, std::atomic<bool>& stop);
protected:
    void execute() override;
private:
    HunterOrchestrator* orch_;
    int total_imported_ = 0;
    int total_invalid_ = 0;
    int total_duplicate_ = 0;
    std::set<std::string> seen_uris_;

    bool isValidProxyUri(const std::string& uri) const;
    void ensureImportDirs();
};

/**
 * @brief Central thread orchestrator — manages all worker threads
 */
class ThreadManager {
public:
    explicit ThreadManager(HunterOrchestrator* orch);
    ~ThreadManager();

    ThreadManager(const ThreadManager&) = delete;
    ThreadManager& operator=(const ThreadManager&) = delete;

    /**
     * @brief Start all worker threads
     */
    void startAll();

    /**
     * @brief Stop all worker threads gracefully
     */
    void stopAll(float timeout = 15.0f);

    /**
     * @brief Get status of all workers + hardware
     */
    struct Status {
        struct HardwareInfo {
            int cpu_count;
            float cpu_percent;
            float ram_total_gb;
            float ram_used_gb;
            float ram_percent;
            std::string mode;
            int thread_count;
        } hardware;

        std::map<std::string, WorkerStatus> workers;
    };
    Status getStatus() const;

    /**
     * @brief Check if still running
     */
    bool isRunning() const { return !stop_event_.load(); }

private:
    HunterOrchestrator* orch_;
    std::atomic<bool> stop_event_{false};

    std::unique_ptr<ConfigScannerWorker> scanner_;
    std::unique_ptr<TelegramPublisherWorker> telegram_;
    std::unique_ptr<BalancerWorker> balancer_;
    std::unique_ptr<HealthMonitorWorker> health_;
    std::unique_ptr<HarvesterWorker> harvester_;
    std::unique_ptr<GitHubDownloaderWorker> github_downloader_;
    std::unique_ptr<IranAssetsWorker> iran_assets_;
    std::unique_ptr<ValidatorWorker> validator_;
    std::unique_ptr<DpiPressureWorker> dpi_pressure_;
    std::unique_ptr<ImportWatcherWorker> import_watcher_;

    std::vector<BaseWorker*> all_workers_;
};

// Utility functions for GitHub proxy management
std::vector<int> githubProxyPorts(HunterOrchestrator* orch);

} // namespace orchestrator
} // namespace hunter
