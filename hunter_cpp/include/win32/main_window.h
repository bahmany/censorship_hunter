#pragma once

#include <windows.h>
#include <string>
#include <memory>
#include <thread>
#include <atomic>
#include <vector>
#include <mutex>
#include <utility>
#include <unordered_map>

#include "core/config.h"

// Forward declarations
namespace hunter {
class HunterOrchestrator;
}

namespace hunter {
namespace win32 {

class MainWindow {
public:
    MainWindow();
    ~MainWindow();

    bool Create(HINSTANCE hInstance, int nCmdShow);
    void Show() { ShowWindow(hwnd_, SW_SHOW); }
    void Hide() { ShowWindow(hwnd_, SW_HIDE); }

private:
    // Window procedure
    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    
    // Message handlers
    LRESULT HandleCommand(int controlId, int notifyCode);
    LRESULT HandleCtlColor(HWND hwnd, HDC hdc);
    LRESULT HandleNotify(NMHDR* hdr);
    LRESULT HandleDrawItem(const DRAWITEMSTRUCT* dis);
    void HandleSize(int width, int height);
    void HandleDpiChanged(UINT dpi, const RECT* suggested);
    
    // UI creation
    void CreateControls();
    void CreateHeaderPanel();
    void CreateStatusPanel();
    void CreateControlPanel();
    void CreateMetricsPanel();
    void CreateLogPanel();
    
    // Control creation helpers
    HWND CreateLabel(HWND parent, const std::wstring& text);
    HWND CreateButton(HWND parent, const std::wstring& text, int id, DWORD extraStyle = 0);
    
    // Theming
    void ApplyModernTheme();
    void ApplyControlTheme(HWND hwnd, COLORREF color, int font_size, bool bold);

    void RecreateGdiObjects();
    
    // Actions
    void StartHunting();
    void StopHunting();
    void ForceCycle();
    void ClearCache();
    void ExportConfigs();
    void ConfigureTelegram();
    
    // Status updates
    void SetRunning(bool running);
    void UpdateStatus(const std::wstring& phase, int cycle, float memory_pct,
                      int scraped, int tested, int gold, int silver);
    void AddLogEntry(const std::wstring& level, const std::wstring& message);

    void QueueLog(const std::wstring& level, const std::wstring& message);
    void DrainLogs();

    // Window handle
    HWND hwnd_ = nullptr;

    UINT dpi_ = 96;
    HBRUSH bg_brush_ = nullptr;
    HBRUSH panel_brush_ = nullptr;
    HBRUSH header_brush_ = nullptr;
    HFONT font_normal_ = nullptr;
    HFONT font_bold_ = nullptr;
    HFONT font_title_ = nullptr;
    HFONT font_status_ = nullptr;

    std::unordered_map<HWND, COLORREF> text_colors_;
    
    // Header controls
    HWND header_panel_ = nullptr;
    HWND title_label_ = nullptr;
    HWND status_indicator_ = nullptr;
    HWND minimize_to_tray_ = nullptr;
    
    // Status panel
    HWND status_group_ = nullptr;
    HWND cycle_label_ = nullptr;
    HWND cycle_value_ = nullptr;
    HWND phase_label_ = nullptr;
    HWND phase_value_ = nullptr;
    HWND memory_label_ = nullptr;
    HWND memory_value_ = nullptr;
    HWND configs_label_ = nullptr;
    HWND configs_value_ = nullptr;
    HWND uptime_label_ = nullptr;
    HWND uptime_value_ = nullptr;
    
    // Control panel
    HWND control_group_ = nullptr;
    HWND start_button_ = nullptr;
    HWND stop_button_ = nullptr;
    HWND force_cycle_button_ = nullptr;
    HWND clear_cache_button_ = nullptr;
    HWND export_configs_button_ = nullptr;
    HWND ctrl_telegram_label_ = nullptr;
    HWND ctrl_telegram_status_ = nullptr;
    HWND configure_telegram_button_ = nullptr;
    
    // Metrics panel
    HWND metrics_group_ = nullptr;
    HWND workers_label_ = nullptr;
    HWND workers_value_ = nullptr;
    HWND balancer_label_ = nullptr;
    HWND balancer_value_ = nullptr;
    HWND metrics_telegram_label_ = nullptr;
    HWND telegram_value_ = nullptr;
    HWND hw_label_ = nullptr;
    HWND hw_value_ = nullptr;
    HWND mode_label_ = nullptr;
    HWND mode_value_ = nullptr;
    HWND last_publish_label_ = nullptr;
    HWND last_publish_value_ = nullptr;
    
    // Log panel
    HWND log_group_ = nullptr;
    HWND log_list_ = nullptr;
    
    // Core components
    hunter::HunterConfig config_;
    std::unique_ptr<hunter::HunterOrchestrator> orchestrator_;

    std::thread orchestrator_thread_;
    std::atomic<bool> orchestrator_running_{false};

    std::mutex pending_logs_mutex_;
    std::vector<std::pair<std::wstring, std::wstring>> pending_logs_;
};

} // namespace win32
} // namespace hunter
