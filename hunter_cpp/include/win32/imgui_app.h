#pragma once

#include <windows.h>
#include <d3d9.h>

#include <array>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/config.h"
#include "core/models.h"
#include "orchestrator/orchestrator.h"
#include "security/dpi_evasion.h"

namespace hunter {
namespace win32 {

class ImGuiApp {
public:
    enum class Page { Home, Configs, Censorship, Logs, Advanced };
    enum class ToastKind { Info, Success, Warning, Error };

    ImGuiApp();
    ~ImGuiApp();

    bool Create(HINSTANCE hInstance, int nCmdShow);
    int Run();

private:
    // ── Win32 / D3D9 ──
    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    bool CreateDeviceD3D();
    void CleanupDeviceD3D();
    void ResetDevice();

    // ── DPI / Style / Fonts ──
    void ReloadFonts();
    void UpdateDpiScale(float new_scale);
    float QueryDpiScale() const;
    void SetupStyle();

    // ── Config persistence ──
    void LoadConfig();
    void SyncBuffersFromConfig();
    void SaveConfigToDisk();

    // ── Orchestrator lifecycle ──
    void EnsureOrchestrator();
    void StartHunter();
    void StopHunter();

    // ── Background worker (keeps UI thread free) ──
    struct Snapshot {
        int cycle_count = 0;
        int cached_count = 0;
        int db_total = 0;
        int db_alive = 0;
        int db_tested = 0;
        int db_untested = 0;
        float db_avg_latency = 0.0f;
        HardwareSnapshot hardware{};
        HunterOrchestrator::SpeedProfile speed_profile{};
        security::DpiEvasionOrchestrator::DpiMetrics dpi_metrics{};
        bool edge_bypass_active = false;
        std::string edge_bypass_status;
        std::vector<ConfigHealthRecord> healthy_records;
        std::vector<ConfigHealthRecord> all_records;
        std::vector<HunterOrchestrator::PortSlot> provisioned_ports;
    };

    void StartBackgroundWorker();
    void StopBackgroundWorker();
    void BackgroundWorkerLoop();
    Snapshot BuildSnapshot();
    void JoinThread(std::thread& worker);

    std::thread bg_worker_;
    std::atomic<bool> bg_stop_{false};
    mutable std::mutex snap_mutex_;
    Snapshot snap_;

    // ── Async command queue (commands run off-UI-thread) ──
    struct CmdResult { bool ok; std::string message; };
    void PostCommand(std::function<void()> fn);
    void RunCommandAsync(const std::string& json);
    void DrainCommandResults();

    std::thread cmd_worker_;
    std::atomic<bool> cmd_stop_{false};
    std::mutex cmd_mutex_;
    std::condition_variable cmd_cv_;
    std::deque<std::function<void()>> cmd_queue_;
    std::mutex result_mutex_;
    std::deque<CmdResult> cmd_results_;

    // ── Real-time log consumer ──
    void ConsumeNewLogs();
    size_t log_generation_ = 0;
    std::vector<std::string> ui_logs_;
    static constexpr size_t MAX_UI_LOGS = 2000;
    std::atomic<unsigned long long> command_seq_{0};

    // ── Logging helpers ──
    void AppendLog(const std::string& line);
    void AppendDiscoveryLog(const std::string& line);
    void SetToast(const std::string& text, ToastKind kind, uint64_t duration_ms = 2500);

    // ── Async operations ──
    void RunProbeAsync();
    void RunDiscoveryAsync();
    void RunEdgeBypassAsync();
    void ApplyAdvancedSettings();
    void ImportConfigsFromFile();
    void ExportConfigsToFile();
    void ApplyDiscoveryToEdgeInputs(const security::DpiEvasionOrchestrator::NetworkDiscoveryResult& result);

    // ── JSON helpers ──
    std::string RunRealtimeCommand(const std::string& json);
    bool JsonSuccess(const std::string& json) const;
    std::string JsonMessage(const std::string& json) const;
    std::string ExtractJsonString(const std::string& json, const std::string& key) const;
    std::string JsonEscape(const std::string& value) const;

    // ── File dialogs / buffer helpers ──
    std::string OpenFileDialog(const char* filter);
    std::string SaveFileDialog(const char* filter, const char* def_ext);
    static void CopyBuf(const std::string& value, char* dest, size_t size);
    std::string JoinLines(const std::vector<std::string>& lines) const;
    std::vector<std::string> SplitLines(const char* text) const;
    std::string BuildJsonStringArray(const std::vector<std::string>& values) const;
    std::string JoinLogLines(const std::vector<std::string>& values) const;
    bool CopyTextToClipboard(const std::string& value) const;

    // ── Drawing ──
    void DrawFrame();
    void DrawNavBar();
    void DrawHomePage();
    void DrawConfigsPage();
    void DrawCensorshipPage();
    void DrawLogsPage();
    void DrawAdvancedPage();

    // ── Drawing helpers ──
    void DrawCard(const char* label, const char* value);
    void DrawLogLine(const std::string& line) const;
    void DrawStatusBadge(bool running);

    // ── Win32 / D3D state ──
    HINSTANCE hinstance_ = nullptr;
    HWND hwnd_ = nullptr;
    LPDIRECT3D9 d3d_ = nullptr;
    LPDIRECT3DDEVICE9 d3d_device_ = nullptr;
    D3DPRESENT_PARAMETERS d3dpp_{};
    UINT pending_resize_width_ = 0;
    UINT pending_resize_height_ = 0;
    bool pending_resize_ = false;

    // ── UI state ──
    Page page_ = Page::Home;
    float dpi_scale_ = 1.0f;

    // ── Core state ──
    HunterConfig config_;
    std::string config_path_ = "runtime/hunter_config.json";
    std::unique_ptr<HunterOrchestrator> orchestrator_;
    mutable std::mutex orchestrator_mutex_;
    mutable std::mutex orchestrator_call_mutex_;
    std::thread orchestrator_thread_;
    std::atomic<bool> orchestrator_running_{false};
    std::atomic<bool> probe_in_flight_{false};
    std::atomic<bool> discovery_in_flight_{false};
    std::atomic<bool> edge_bypass_in_flight_{false};
    std::thread probe_thread_;
    std::thread discovery_thread_;
    std::thread edge_bypass_thread_;

    // ── Probe / Discovery results (written by async threads) ──
    mutable std::mutex data_mutex_;
    security::DpiEvasionOrchestrator::DetailedProbeResult probe_result_{};
    security::DpiEvasionOrchestrator::NetworkDiscoveryResult discovery_result_{};
    std::atomic<bool> has_probe_result_{false};
    std::atomic<bool> has_discovery_result_{false};
    std::atomic<bool> discovery_apply_requested_{false};
    std::vector<std::string> discovery_logs_;
    std::vector<std::string> edge_bypass_logs_;

    // ── UI settings ──
    int config_limit_ = 250;
    std::atomic<int> snapshot_config_limit_{250};
    bool show_alive_only_ = true;
    bool advanced_restart_notice_ = false;
    bool telegram_enabled_ = false;
    bool auto_scroll_logs_ = true;
    bool show_only_errors_ = false;
    bool reveal_sensitive_network_data_ = false;
    int max_total_ = 1000;
    int max_workers_ = 12;
    int scan_limit_ = 50;
    int sleep_seconds_ = 300;
    int telegram_limit_ = 50;
    int telegram_timeout_ms_ = 12000;
    int clear_old_hours_ = 168;

    // ── Text buffers for input fields ──
    std::array<char, 512> xray_path_{};
    std::array<char, 512> singbox_path_{};
    std::array<char, 512> mihomo_path_{};
    std::array<char, 512> tor_path_{};
    std::array<char, 512> import_path_{};
    std::array<char, 512> export_path_{};
    std::array<char, 128> edge_target_mac_{};
    std::array<char, 128> edge_exit_ip_{};
    std::array<char, 256> edge_iface_{};
    std::array<char, 256> telegram_api_id_{};
    std::array<char, 256> telegram_api_hash_{};
    std::array<char, 256> telegram_phone_{};
    std::array<char, 8192> telegram_targets_{};
    std::array<char, 16384> github_urls_{};
    std::array<char, 16384> manual_configs_{};

    // ── Toast / feedback ──
    std::string toast_text_;
    uint64_t toast_until_ms_ = 0;
    ToastKind toast_kind_ = ToastKind::Info;
};

} // namespace win32
} // namespace hunter
