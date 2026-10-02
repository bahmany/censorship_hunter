#pragma once

#include <atomic>
#include <condition_variable>
#include <map>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "core/health_score.h"
#include "core/models.h"
#include "core/updater.h"
#include "geo/country_query.h"
#include "core/utils.h"  // for utils::LogRingBuffer

struct GLFWwindow;
struct ImVec4;

namespace hunter {

class HunterOrchestrator;
namespace geo { class CountryService; class CountryTargetState; }

namespace gui {

/**
 * @brief Native ImGui/GLFW frontend for HunterOrchestrator.
 *
 * Drives the orchestrator on a background thread and renders a live
 * table of discovered configs each frame, reading straight from
 * HunterOrchestrator::configDb() (thread-safe on its own).
 */
class HunterGuiApp {
public:
    explicit HunterGuiApp(HunterOrchestrator& orchestrator);
    ~HunterGuiApp();

    HunterGuiApp(const HunterGuiApp&) = delete;
    HunterGuiApp& operator=(const HunterGuiApp&) = delete;

    /** @brief Create the window and run the render loop until closed. @return process exit code */
    int run();

    /** @brief Ask the render loop to exit on its next iteration (safe to call from a signal handler). */
    void requestClose();

    /**
     * @brief Install a poll used to detect a second launch of the app.
     *
     * Called once per frame (throttled); when it returns true the window is
     * un-minimized and focused, which is how a second launch surfaces the
     * instance that is already running instead of starting another one.
     */
    void setShowRequestPoll(std::function<bool()> poll);

    // ─── Android interface (also works for headless/testing) ───
    // These expose the internal rendering and orchestration methods so
    // that non-GLFW frontends (e.g. Android GLES3) can reuse the same UI.
    void startOrchestrator();
    void stopOrchestrator();
    void renderFrame();
    void pollProxyServers();

private:
    void triggerRescan();

    void refreshSnapshotIfDue();
    void snapshotWorker();
    void startSnapshotWorker();
    void stopSnapshotWorker();
    void countryWorker();       // server-country fill + (optional) DoH, off the GUI thread
    void baselineWorker();      // local connectivity overlay (M5), off the GUI thread
    const ParsedConfig& parsedFor(const std::string& uri);
    void rebuildVisibleRowsIfDirty(int sort_col, bool sort_asc);
    void markViewDirty();       // filter/search/view changed: worker re-queries immediately
    void applyExitTarget(const std::string& iso, bool strict);
    void saveConfigNow();

    // ─── Modern UI rendering ───
    void applyModernTheme();
    void renderTabBar();
    void renderDashboardTab();
    void renderConfigsTab();
    void renderSettingsTab();
    void renderAboutTab();
    void renderStatusBar();
    void renderStatusCard(const char* label, const char* value, const ImVec4& color, float width);
    void renderUpdatePanel();

    // Legacy rendering (used within tabs)
    void renderHeader();
    void renderControls();
    void renderFilterBar();
    void renderTable();
    void renderLogPanel();
    void renderQrPopup();
    void renderClearLivePopup();
    void clearAllLive();

    std::string buildClipboardText(bool selected_only) const;
    std::string buildGeminiOkClipboardText() const;
    std::string buildExportText() const;
    void exportToFile(const std::string& content, const char* default_name);
    void showToast(const std::string& msg);

    HunterOrchestrator& orch_;
    GLFWwindow* window_ = nullptr;

    std::thread orchestrator_thread_;
    std::atomic<bool> orchestrator_running_{false};
    std::atomic<bool> transitioning_{false};
    std::atomic<bool> rescan_in_progress_{false};

    // ─── Live log display ───
    // We read directly from utils::LogRingBuffer (the orchestrator's own
    // clean, timestamped log buffer) instead of capturing std::cout. This
    // avoids ANSI dashboard frames polluting the log panel.
    double last_log_refresh_ = -1.0;
    std::vector<std::string> cached_log_lines_;
    size_t log_generation_ = 0;
    static constexpr double kLogRefreshIntervalSeconds = 0.3;
    bool log_auto_scroll_ = true;

    // ─── Config snapshot ───
    //
    // ConfigDatabase::getAliveRecords() and getStats() both walk the entire
    // database (100k+ records) holding its mutex, while ~20 validator threads
    // contend on that same mutex. Doing that from the render thread stalled
    // frames long enough that the window stopped answering _NET_WM_PING, and
    // KWin killed the app as unresponsive. A background thread produces the
    // snapshot instead; the render thread only swaps a pointer.
    /// One table row: the record plus everything the render thread needs, computed by the worker.
    struct UiRow {
        ConfigHealthRecord rec;
        HealthEvaluation ev;
        std::string protocol, address, ps;   // parsed once by the worker
        int port = 0;
    };
    struct UiSnapshot {
        std::vector<UiRow> rows;             // already filtered by the view spec, ranked, capped
        int total = 0;
        int alive_count = 0;
        int tested = 0;
        float avg_latency_ms = 0.0f;
        int stable = 0, healthy = 0, degraded = 0, unstable = 0, dead = 0, unknown = 0;
        int matched = 0;                     // rows matching the view spec before the row cap
        std::map<std::string, int> exit_counts, server_counts;   // across the whole DB
        double scan_ms = 0.0;
        uint64_t spec_version = 0;
    };

    /// What the table shows; written by the render thread, read by the snapshot worker.
    struct ViewSpec {
        bool all_view = false;               // false: live configs only; true: "All configs" (Unknown/Dead too)
        geo::CountryMode mode = geo::CountryMode::All;
        geo::CountryChoice choice;
        std::string query;
        uint64_t version = 1;
    };
    ViewSpec view_spec_;                     // guarded by view_mutex_
    std::mutex view_mutex_;
    std::atomic<bool> view_dirty_{false};

    std::thread snapshot_thread_;
    std::atomic<bool> snapshot_stop_{false};
    std::mutex snapshot_mutex_;
    std::condition_variable snapshot_cv_;
    std::shared_ptr<const UiSnapshot> latest_snapshot_;   // written by the worker
    std::shared_ptr<const UiSnapshot> applied_snapshot_;  // last one the UI took
    static constexpr double kSnapshotIntervalSeconds = 1.0;

    std::vector<UiRow> snapshot_;
    std::map<std::string, int> cached_exit_counts_, cached_server_counts_;
    int cached_matched_ = 0;
    int cached_stable_ = 0, cached_healthy_ = 0, cached_dead_ = 0, cached_unknown_ = 0;
    double cached_scan_ms_ = 0.0;
    int cached_total_ = 0;
    int cached_alive_ = 0;
    int cached_tested_ = 0;
    float cached_avg_latency_ms_ = 0.0f;
    double last_refresh_time_ = -1.0;
    static constexpr double kRefreshIntervalSeconds = 0.5;
    static constexpr int kMaxLiveRows = 500;
    static constexpr int kMaxAllRows = 3000;

    std::vector<const UiRow*> visible_rows_;
    bool rows_dirty_ = true;
    std::string last_filter_applied_;

    std::unordered_map<std::string, ParsedConfig> parse_cache_;
    std::set<std::string> selected_uris_;
    std::string last_clicked_uri_;

    char filter_text_[256] = {0};
    int view_all_ui_ = 0;           // 0 = Live, 1 = All configs
    int country_mode_ui_ = 0;       // geo::CountryMode
    std::string country_choice_ui_; // "" any, "?" unknown, "~" mixed, else ISO
    char target_input_[8] = {0};
    bool target_strict_ui_ = false;
    bool baseline_enabled_ui_ = true;
    bool doh_enabled_ui_ = false;
    bool user_sorted_ = false;
    int sort_column_ = -1;
    bool sort_ascending_ = true;

    // ─── QR code popup ───
    bool show_qr_popup_ = false;
    std::string qr_text_;

    // ─── "Clear all live" confirmation ───
    bool show_clear_live_popup_ = false;

    std::string toast_message_;
    double toast_until_ = 0.0;

    // ─── Second-launch handling ───
    std::function<bool()> show_request_poll_;
    double last_show_poll_ = -1.0;
    static constexpr double kShowPollIntervalSeconds = 0.25;

    // ─── Modern UI state ───
    enum class Tab { Dashboard, Configs, Settings, About };
    Tab active_tab_ = Tab::Dashboard;
    bool theme_applied_ = false;

    // ─── Country service (C2) ───
    std::shared_ptr<geo::CountryService> country_service_;
    std::shared_ptr<geo::CountryTargetState> target_state_;
    std::thread country_thread_;
    std::thread baseline_thread_;
    std::atomic<int> baseline_state_{0};   // 0 idle/unknown, 1 online, 2 offline, 3 indeterminate, 4 disabled
    std::atomic<double> baseline_at_{0.0};
    std::atomic<int> country_filled_{0};

    // ─── Update panel state ───
    core::SelfUpdateManager::UpdateState last_update_state_{core::SelfUpdateManager::UpdateState::Idle};
    float last_update_progress_ = 0.0f;
    double last_update_ui_refresh_ = -1.0;
};

} // namespace gui
} // namespace hunter
