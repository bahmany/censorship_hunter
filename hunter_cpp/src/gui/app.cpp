#include "gui/app.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>
#include <string>

#ifndef _WIN32
#include <unistd.h>  // readlink() for /proc/self/exe
#endif

#ifndef __ANDROID__
#include <GLFW/glfw3.h>
#include "backends/imgui_impl_glfw.h"
#include "backends/imgui_impl_opengl2.h"
#endif

#ifdef _WIN32
#include <windows.h>
#include "core/gl_fallback_embed.h"
#endif

#include "imgui.h"
#ifdef IMGUI_ENABLE_TEST_ENGINE
#include "imgui_internal.h"
#endif
#include "gui/update_panel.h"

#include "core/constants.h"
#include "core/utils.h"
#include "geo/country_database.h"
#include "geo/country_service.h"
#include "gui/qr_renderer.h"
#include "network/connectivity_baseline.h"
#include "network/continuous_validator.h"
#include "network/country_provider.h"
#include "network/traffic_probe.h"
#include "network/uri_parser.h"
#include "orchestrator/orchestrator.h"
#include "network/proxy_tester.h"
#include "network/sys_proxy.h"
#include "core/client_export.h"
#include "gui/ui_fonts.h"

// ─── stb_image for runtime icon loading (Linux/macOS) ───
// On Windows the icon is embedded via resources.rc and GLFW picks it up
// automatically. On Linux/macOS there is no resource system, so we load
// the PNG at runtime and pass it to glfwSetWindowIcon().
#ifndef _WIN32
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include "stb_image.h"
#endif

// ─── Design System – centralised via UI::Theme ───
#include "ui/theme.hpp"

namespace hunter {
namespace gui {

using Spacing = UI::Theme::Spacing;
using Colors   = UI::Theme::Colors;
constexpr float kNarrowThreshold = UI::Theme::NarrowThreshold;
constexpr float kUiFontSizePx = 16.0f;    // body text, design px
constexpr float kStatFontSizePx = 24.0f;  // stat-card numbers, design px
constexpr float kTitleFontSizePx = 20.0f; // top-bar title, design px

// Aliases for readability (optional – can be removed later)
constexpr float kSpaceS = Spacing::Small;
constexpr float kSpaceM = Spacing::Medium;
constexpr float kSpaceL = Spacing::Large;
constexpr float kRadius = UI::Theme::CornerRadius;

// Colour aliases – keep old names for minimal code changes
const ImVec4 kAccentBlue    = Colors::AccentBlue;
const ImVec4 kSuccessGreen  = Colors::SuccessGreen;
const ImVec4 kWarnAmber     = Colors::WarnAmber;
const ImVec4 kDangerRed     = Colors::DangerRed;
const ImVec4 kTextPrimary   = Colors::TextPrimary;
const ImVec4 kTextSecondary = Colors::TextSecondary;
const ImVec4 kTextDisabled  = Colors::TextDisabled;
const ImVec4 kBgPrimary     = Colors::BgPrimary;
const ImVec4 kBgSecondary   = Colors::BgSecondary;
const ImVec4 kBgTertiary    = Colors::BgTertiary;
const ImVec4 kBorderLight   = Colors::BorderLight;
const ImVec4 kBorderStrong  = Colors::BorderStrong;



// Load the application icon from hunter_icon.png and set it as the GLFW
// window icon. On Linux this makes the icon appear in the taskbar, window
// decoration, and alt-tab preview. The PNG is copied next to the binary
// at build time (see CMakeLists.txt).
//
// We provide multiple sizes (16, 32, 48, 64, 128, 256) so the window
// manager can pick the best fit for each context.
#ifndef _WIN32  // Windows: icon comes from resources.rc; stb_image is not included there.
void setWindowIcon(GLFWwindow* window) {
    // Search for the icon next to the executable, then in common locations.
    std::vector<std::string> search_paths = {
        "hunter_icon.png",                          // CWD
        "bin/hunter_icon.png",                      // project bin/
        "../hunter_icon.png",                       // build/ dir
        "../../hunter_cpp/hunter_icon.png",         // deeper build subdir
    };

    // Also check next to the executable via /proc/self/exe (Linux).
#ifdef __linux__
    char exe_path[4096];
    ssize_t len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
    if (len > 0) {
        exe_path[len] = '\0';
        std::string exe_dir = std::string(exe_path);
        auto last_slash = exe_dir.find_last_of('/');
        if (last_slash != std::string::npos) {
            exe_dir = exe_dir.substr(0, last_slash);
            search_paths.insert(search_paths.begin(), exe_dir + "/hunter_icon.png");
        }
    }
#endif

    std::string icon_path;
    for (const auto& p : search_paths) {
        if (utils::fileExists(p)) {
            icon_path = p;
            break;
        }
    }
    if (icon_path.empty()) return;

    // Load the PNG file into memory.
    std::ifstream file(icon_path, std::ios::binary | std::ios::ate);
    if (!file) return;
    size_t file_size = file.tellg();
    file.seekg(0);
    std::vector<unsigned char> file_data(file_size);
    file.read(reinterpret_cast<char*>(file_data.data()), file_size);
    if (!file) return;

    // Decode the PNG into RGBA pixels.
    int w, h, channels;
    unsigned char* pixels = stbi_load_from_memory(
        file_data.data(), (int)file_size, &w, &h, &channels, 4);
    if (!pixels) return;

    // Provide GLFW with several sizes from the single 256x256 image.
    // GLFW will downscale as needed; providing the full-res image is enough.
    GLFWimage images[1];
    images[0].width = w;
    images[0].height = h;
    images[0].pixels = pixels;
    glfwSetWindowIcon(window, 1, images);

    stbi_image_free(pixels);
}
#endif  // !_WIN32

// Portable time function - uses glfwGetTime() on desktop, chrono on Android
#ifdef __ANDROID__
inline double portableGetTime() {
    auto now = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch());
    return ms.count() / 1000.0;
}
#define glfwGetTime portableGetTime
#endif

// ─── Health presentation (Stage 4): real health state / stability / score replace gold/silver ───
const char* healthLabel(HealthState st) {
    switch (st) {
        case HealthState::Healthy: return "Healthy";
        case HealthState::Degraded: return "Degraded";
        case HealthState::Unstable: return "Unstable";
        case HealthState::Dead: return "Dead";
        case HealthState::Testing: return "Testing";
        default: return "Unknown";
    }
}

ImVec4 healthColor(HealthState st) {
     switch (st) {
         case HealthState::Healthy: return kSuccessGreen;
         case HealthState::Degraded: return kWarnAmber;
         case HealthState::Unstable: return ImVec4(0.95f, 0.55f, 0.20f, 1.0f); // Keep original for now
         case HealthState::Dead: return kDangerRed;
         case HealthState::Testing: return ImVec4(0.45f, 0.65f, 0.95f, 1.0f); // Keep original for now
         default: return kTextDisabled;
     }
 }

// Stable / Provisional (healthy but not yet certified) / Unstable / Dead / Unrated.
const char* stabilityLabel(const ConfigHealthRecord& r, const HealthEvaluation& ev) {
    if (ev.stable) return "Stable";
    if (r.ev.state == HealthState::Healthy) return "Provisional";
    switch (ev.stability) {
        case Stability::Dead: return "Dead";
        case Stability::Unstable: return "Unstable";
        case Stability::Stable: return "Stable";
        default: return "Unrated";
    }
}

ImVec4 stabilityColor(const char* label) {
     if (std::strcmp(label, "Stable") == 0) return kSuccessGreen;
     if (std::strcmp(label, "Provisional") == 0) return ImVec4(0.55f, 0.80f, 0.90f, 1.0f); // Keep original for now
     if (std::strcmp(label, "Unstable") == 0) return ImVec4(0.95f, 0.55f, 0.20f, 1.0f); // Keep original for now
     if (std::strcmp(label, "Dead") == 0) return kDangerRed;
     return kTextDisabled;
 }

int healthSortOrder(HealthState st) {
    switch (st) {
        case HealthState::Healthy: return 0;
        case HealthState::Degraded: return 1;
        case HealthState::Testing: return 2;
        case HealthState::Unknown: return 3;
        case HealthState::Unstable: return 4;
        default: return 5;
    }
}

// Cheap, allocation-free case-insensitive substring test (needle already lower-case).
bool icontains(const std::string& hay, const std::string& needle) {
    if (needle.empty()) return true;
    if (hay.size() < needle.size()) return false;
    for (size_t i = 0; i + needle.size() <= hay.size(); i++) {
        size_t j = 0;
        while (j < needle.size() && std::tolower((unsigned char)hay[i + j]) == (unsigned char)needle[j]) j++;
        if (j == needle.size()) return true;
    }
    return false;
}

// Exit/server country cell text and tooltip with provenance + freshness (D4).
std::string countryCell(const std::string& iso) {
    if (iso.empty()) return "Unknown";
    if (iso == geo::kMixed) return "Mixed";
    std::string n = geo::countryName(iso);
    return n.empty() ? iso : iso + " " + n;
}

const char* sourceDescription(const std::string& src) {
    if (src == geo::kSrcTrace) return "Cloudflare trace through the tunnel (measured)";
    if (src == geo::kSrcTraceFallback) return "one.one.one.one trace through the tunnel (measured)";
    if (src == geo::kSrcOfflineExit) return "exit IP via ipify + offline DB (lower confidence)";
    if (src == geo::kSrcOfflineIp) return "server IP via offline DB-IP database (hint)";
    if (src == geo::kSrcDohOffline) return "domain resolved by verified DoH + offline DB (hint)";
    if (src == geo::kSrcNone) return "could not be determined";
    return src.empty() ? "not determined yet" : src.c_str();
}

std::string formatAge(double now, double ts) {
    if (ts <= 0.0) return "-";
    double d = now - ts;
    if (d < 0) d = 0;
    if (d < 60) return std::to_string(static_cast<int>(d)) + "s ago";
    if (d < 3600) return std::to_string(static_cast<int>(d / 60)) + "m ago";
    if (d < 86400) return std::to_string(static_cast<int>(d / 3600)) + "h ago";
    return std::to_string(static_cast<int>(d / 86400)) + "d ago";
}

// v2rayN (2dust/v2rayN v7+) clipboard import accepts only these schemes;
// anything else is skipped silently.
bool isV2rayNScheme(const std::string& scheme) {
    return scheme == "vmess" || scheme == "ss" || scheme == "socks" || scheme == "socks5" ||
           scheme == "socks4" || scheme == "trojan" || scheme == "vless" ||
           scheme == "hysteria2" || scheme == "hy2" || scheme == "hysteria2+realm" ||
           scheme == "hysteria2+realm+http" || scheme == "tuic" || scheme == "wireguard" ||
           scheme == "anytls" || scheme == "naive+https" || scheme == "naive+quic";
}

std::string uriScheme(const std::string& uri) {
    auto pos = uri.find("://");
    if (pos == std::string::npos) return "";
    std::string s = uri.substr(0, pos);
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

// Normalize one URI for v2rayN import; returns "" when it must be dropped.
std::string normalizeForV2rayN(const std::string& uri) {
    std::string scheme = uriScheme(uri);
    if (!isV2rayNScheme(scheme)) return "";
    std::string out = uri;
    if (scheme == "hy2") out = "hysteria2://" + uri.substr(6);
    // Non-vmess links need a '#remark' fragment or v2rayN shows a blank row.
    // vmess bodies are base64 payloads — leave them untouched.
    if (scheme != "vmess" && out.find('#') == std::string::npos) out += "#Hunter";
    return out;
}

struct Top20Candidate {
    std::string uri, exit_country, server_country;
    RankKey rank;
};

Top20Candidate top20CandidateFor(const ConfigHealthRecord& r, const HealthThresholds& th, double now) {
    Top20Candidate c;
    c.uri = r.uri;
    c.exit_country = r.exit_country;
    c.server_country = r.server_country;
    HealthEvaluation hev = evaluateHealth(r.ev, now, th);
    c.rank.tier = hev.tier;
    c.rank.score = hev.score;
    c.rank.last_full_success = r.ev.last_full_success;
    c.rank.key = r.endpoint_key;
    return c;
}

// Ranked order, US-exit (server-country fallback) first: up to 5 US, then the
// best remaining regardless of country, up to 20 total. Dedups by URI.
void selectTop20ForV2rayN(std::vector<Top20Candidate> cands,
                          std::vector<std::string>& out_uris, int& out_us) {
    out_uris.clear();
    out_us = 0;
    std::sort(cands.begin(), cands.end(), [](const Top20Candidate& a, const Top20Candidate& b) {
        return rankedBefore(a.rank, b.rank);
    });
    std::set<std::string> seen;
    auto isUS = [](const Top20Candidate& c) {
        if (c.exit_country == "US") return true;
        if (c.exit_country.empty() || c.exit_country == "Mixed" || c.exit_country == "?")
            return c.server_country == "US";
        return false;
    };
    for (const auto& c : cands) {
        if (out_us >= 5 || out_uris.size() >= 20) break;
        if (!isUS(c)) continue;
        if (!seen.insert(c.uri).second) continue;
        std::string n = normalizeForV2rayN(c.uri);
        if (n.empty()) continue;
        out_uris.push_back(n);
        out_us++;
    }
    for (const auto& c : cands) {
        if (out_uris.size() >= 20) break;
        if (!seen.insert(c.uri).second) continue;
        std::string n = normalizeForV2rayN(c.uri);
        if (n.empty()) continue;
        out_uris.push_back(n);
    }
}

HunterGuiApp::HunterGuiApp(HunterOrchestrator& orchestrator) : orch_(orchestrator) {
    // Country service: offline DB-IP provider (registered explicitly, no work done at creation).
    network::registerBuiltinCountryProviders();
    auto provider = network::CountryProviderRegistry::instance().create("offline");
    HunterConfig& cfg = orch_.config();
    geo::CountryServiceOptions copt;
    copt.doh_enabled = cfg.countryDohEnabled();
    copt.doh_max_ttl_s = std::min(3600.0, (double)std::max(60, cfg.dohMaxTtlSeconds()));
    copt.exit_fresh_s = (double)std::max(60, cfg.exitCountryFreshSeconds());
    auto transport = network::makeCurlTransport();
    country_service_ = std::make_shared<geo::CountryService>(
        provider, geo::makeCloudflareDohResolver(transport), transport, nullptr, copt);
    target_state_ = std::make_shared<geo::CountryTargetState>();
    doh_enabled_ui_ = cfg.countryDohEnabled();
    baseline_enabled_ui_ = cfg.directBaselineEnabled();
    if (!baseline_enabled_ui_) network::ConnectivityBaseline::shared()->setEnabled(false);
    std::string tgt = cfg.exitCountryTarget();
    std::snprintf(target_input_, sizeof(target_input_), "%s", tgt.c_str());
    target_strict_ui_ = cfg.exitCountryStrict();
    applyExitTarget(tgt, target_strict_ui_);

    // Country-targeted discovery: reorder the validator's candidate pool (D4).
    if (auto* db = orch_.configDb()) {
        auto state = target_state_;
        ClockFn clock = systemClock();
        db->setBatchPrioritizer([state, clock](std::vector<ConfigHealthRecord>& pool, int batch) {
            geo::prioritizeForTarget(pool, batch, state->get(), clock());
        });
    }

    // Test/smoke hook: HUNTER_GUI_VIEW=all, HUNTER_GUI_QUERY="exit:DE", HUNTER_GUI_MODE=exit|server.
    if (const char* v = std::getenv("HUNTER_GUI_VIEW")) { if (std::string(v) == "all") { view_all_ui_ = 1; view_spec_.all_view = true; } }
    if (const char* q = std::getenv("HUNTER_GUI_QUERY")) {
        std::snprintf(filter_text_, sizeof(filter_text_), "%s", q);
        view_spec_.query = filter_text_;
    }
    if (const char* m = std::getenv("HUNTER_GUI_MODE")) {
        std::string ms = m;
        country_mode_ui_ = ms == "exit" ? 1 : ms == "server" ? 2 : 0;
        view_spec_.mode = (geo::CountryMode)country_mode_ui_;
    }

    // Runs for the whole life of the app, not just while the orchestrator is
    // running: with the orchestrator stopped the database is still there and
    // the table must keep reflecting it.
    startSnapshotWorker();
    country_thread_ = std::thread([this]() { countryWorker(); });
    baseline_thread_ = std::thread([this]() { baselineWorker(); });
}

HunterGuiApp::~HunterGuiApp() {
    // Background tasks capture `this`; let them finish before members die.
    // Stop the orchestrator first so long-running tasks (tests, rescans) end.
    stopOrchestratorBlocking();
    if (!waitForBackgroundTasks(std::chrono::seconds(20))) {
        // A task (e.g. a long connectivity test) still references `this`.
        // Destroying members under it would be a use-after-free; the
        // orchestrator has already persisted its state, so end the process
        // without running destructors instead.
        std::fprintf(stderr, "[gui] %d background task(s) still running at exit; exiting now\n",
                     bg_inflight_.load());
        std::fflush(nullptr);
        std::_Exit(0);
    }
    stopSnapshotWorker();   // also stops the country + baseline workers
    if (auto* db = orch_.configDb()) db->setBatchPrioritizer(nullptr);
    stopOrchestratorBlocking();
}

void HunterGuiApp::markViewDirty() {
    {
        std::lock_guard<std::mutex> lk(view_mutex_);
        view_spec_.version++;
    }
    view_dirty_ = true;
    snapshot_cv_.notify_all();
}

void HunterGuiApp::applyExitTarget(const std::string& iso, bool strict) {
    HunterConfig& cfg = orch_.config();
    cfg.setExitCountryTarget(iso);
    cfg.setExitCountryStrict(strict);
    geo::CountryTarget t;
    t.iso = cfg.exitCountryTarget();
    t.strict = strict && !t.iso.empty();
    t.exploration = cfg.countryExplorationRatio();
    t.exit_fresh_s = (double)std::max(60, cfg.exitCountryFreshSeconds());
    target_state_->set(t);
}

void HunterGuiApp::saveConfigNow() {
    std::error_code ec;
    std::filesystem::create_directories("runtime", ec);
    orch_.config().saveToFile("runtime/hunter_config.json");
}

void HunterGuiApp::startOrchestrator() {
    if (shutdown_in_progress_.load()) {
        showToast("Still stopping - try again in a moment");
        return;
    }
    std::lock_guard<std::mutex> lifecycle(lifecycle_mu_);
    if (orchestrator_running_.load()) return;
    // A previous run's thread has finished (running == false) but must be
    // joined before the std::thread is reassigned, or std::terminate fires.
    if (orchestrator_thread_.joinable()) orchestrator_thread_.join();
    transitioning_ = true;
    orchestrator_running_ = true;
    orchestrator_thread_ = std::thread([this]() {
        try {
            orch_.start();
        } catch (const std::exception& e) {
            std::fprintf(stderr, "[gui] orchestrator error: %s\n", e.what());
            showToast(std::string("Hunter stopped: ") + e.what());
        } catch (...) {
            std::fprintf(stderr, "[gui] orchestrator error: unknown exception\n");
            showToast("Hunter stopped after an unexpected error");
        }
        orchestrator_running_ = false;
    });
    transitioning_ = false;
}

void HunterGuiApp::stopOrchestrator() {
    // UI paths: return immediately and start shutdown in a detached thread
    requestShutdown();
}

void HunterGuiApp::requestShutdown() {
    if (shutdown_in_progress_.exchange(true)) return; // idempotent
    runBackground([this]() {
        stopOrchestratorBlocking();
        shutdown_in_progress_.store(false);
    });
}

void HunterGuiApp::stopOrchestratorBlocking() {
    // Serialized with startOrchestrator(): the Stop button (background task)
    // and the destructor may both get here; two threads joining/detaching the
    // same std::thread throws std::system_error -> std::terminate.
    std::lock_guard<std::mutex> lifecycle(lifecycle_mu_);
    if (!orchestrator_running_.load()) {
        if (orchestrator_thread_.joinable()) orchestrator_thread_.join();
        return;
    }
    transitioning_ = true;
    orch_.stop();
    // orch_.start() returns once stop() has been requested (worker joins are
    // bounded). Always join: a detached thread would keep using orch_ and
    // `this` after they are destroyed.
    if (orchestrator_thread_.joinable()) orchestrator_thread_.join();
    transitioning_ = false;
}

void HunterGuiApp::runBackground(std::function<void()> fn) {
    bg_inflight_.fetch_add(1);
    std::thread([this, fn = std::move(fn)]() {
        try {
            fn();
        } catch (const std::exception& e) {
            std::fprintf(stderr, "[gui] background task failed: %s\n", e.what());
            showToast(std::string("Action failed: ") + e.what());
        } catch (...) {
            std::fprintf(stderr, "[gui] background task failed: unknown exception\n");
            showToast("Action failed (unexpected error)");
        }
        bg_inflight_.fetch_sub(1);
    }).detach();
}

bool HunterGuiApp::waitForBackgroundTasks(std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (bg_inflight_.load() > 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return bg_inflight_.load() == 0;
}

void HunterGuiApp::requestClose() {
    requestShutdown();
    #ifndef __ANDROID__
        if (window_) glfwSetWindowShouldClose(window_, GLFW_TRUE);
    #endif
}

void HunterGuiApp::setShowRequestPoll(std::function<bool()> poll) {
    show_request_poll_ = std::move(poll);
}

void HunterGuiApp::triggerRescan() {
    if (rescan_in_progress_.exchange(true)) return;
    runBackground([this]() {
        try {
            orch_.runCycle();
        } catch (...) {}
        rescan_in_progress_ = false;
    });
}

const ParsedConfig& HunterGuiApp::parsedFor(const std::string& uri) {
    auto it = parse_cache_.find(uri);
    if (it != parse_cache_.end()) return it->second;
    ParsedConfig parsed;
    if (auto opt = network::UriParser::parse(uri)) {
        parsed = *opt;
    } else {
        parsed.uri = uri;
        parsed.protocol = "?";
        parsed.address = uri.substr(0, std::min<size_t>(uri.size(), 40));
    }
    auto [inserted, _] = parse_cache_.emplace(uri, std::move(parsed));
    return inserted->second;
}

void HunterGuiApp::startSnapshotWorker() {
    if (snapshot_thread_.joinable()) return;
    snapshot_stop_ = false;
    snapshot_thread_ = std::thread([this]() { snapshotWorker(); });
}

void HunterGuiApp::stopSnapshotWorker() {
    snapshot_stop_ = true;
    snapshot_cv_.notify_all();
    if (snapshot_thread_.joinable()) snapshot_thread_.join();
    if (country_thread_.joinable()) country_thread_.join();
    if (baseline_thread_.joinable()) baseline_thread_.join();
}

// Fills server-IP countries (offline DB; verified DoH for domains only if enabled) in the
// background. Never touches the GUI thread; the DB is only locked for short, bounded passes.
void HunterGuiApp::countryWorker() {
    while (!snapshot_stop_.load()) {
        auto* db = orch_.configDb();
        if (db && country_service_) {
            try {
                country_service_->setDohEnabled(doh_enabled_ui_);
                country_filled_ += country_service_->refreshServerCountries(*db, 400);
            } catch (...) {}
        }
        std::unique_lock<std::mutex> lock(snapshot_mutex_);
        snapshot_cv_.wait_for(lock, std::chrono::seconds(3), [this]() { return snapshot_stop_.load(); });
    }
}

// Local connectivity overlay (M5): direct baseline, only while the orchestrator runs.
void HunterGuiApp::baselineWorker() {
    while (!snapshot_stop_.load()) {
        auto base = network::ConnectivityBaseline::shared();
        if (!baseline_enabled_ui_) {
            baseline_state_ = 4;
        } else if (orchestrator_running_.load()) {
            try {
                auto snap = base->current(30.0);
                baseline_at_ = snap.at;
                switch (snap.state) {
                    case network::BaselineState::Online: baseline_state_ = 1; break;
                    case network::BaselineState::Offline: baseline_state_ = 2; break;
                    default: baseline_state_ = 3; break;
                }
            } catch (...) {}
        } else {
            baseline_state_ = 0;
        }
        std::unique_lock<std::mutex> lock(snapshot_mutex_);
        snapshot_cv_.wait_for(lock, std::chrono::seconds(10), [this]() { return snapshot_stop_.load(); });
    }
}

void HunterGuiApp::snapshotWorker() {
    // Per-key cache of the searchable text of base64-wrapped URIs (vmess) whose address is not
    // visible in the raw URI; filled outside the DB lock.
    std::unordered_map<std::string, std::string> meta_cache;
    std::unordered_map<std::string, std::tuple<std::string, std::string, std::string, int>> parsed_cache;
    while (!snapshot_stop_.load()) {
        view_dirty_ = false;
        ViewSpec spec;
        {
            std::lock_guard<std::mutex> lk(view_mutex_);
            spec = view_spec_;
        }
        auto* db = orch_.configDb();
        double next_wait = kSnapshotIntervalSeconds;
        if (db) {
            auto snap = std::make_shared<UiSnapshot>();
            snap->spec_version = spec.version;
            try {
                const auto t0 = std::chrono::steady_clock::now();
                const HealthThresholds th = db->thresholds();
                const double now = systemClock()();
                const auto terms = geo::parseSearchQuery(spec.query);

                // Phase 1 (DB lock held, cheap per-record work only): count + select + cheap rank key.
                struct Item { int cls; double lfs; float lat; std::string key; };
                std::vector<Item> items;
                std::vector<Top20Candidate> top_cands;   // every alive record, for the v2rayN top-20
                std::vector<std::pair<std::string, std::string>> need_meta;   // key, uri
                double lat_sum = 0.0; int lat_n = 0;
                db->forEachRecord([&](const ConfigHealthRecord& r) {
                    snap->total++;
                    if (r.total_tests > 0) snap->tested++;
                    if (r.alive) { snap->alive_count++; if (r.latency_ms > 0) { lat_sum += r.latency_ms; lat_n++; } }
                    switch (r.ev.state) {
                        case HealthState::Healthy: snap->healthy++; break;
                        case HealthState::Degraded: snap->degraded++; break;
                        case HealthState::Unstable: snap->unstable++; break;
                        case HealthState::Dead: snap->dead++; break;
                        default: snap->unknown++; break;
                    }
                    if (!r.exit_country.empty()) snap->exit_counts[r.exit_country]++;
                    if (!r.server_country.empty()) snap->server_counts[r.server_country]++;
                    if (r.alive) top_cands.push_back(top20CandidateFor(r, th, now));

                    if (!spec.all_view && !r.alive) return;
                    if (!geo::passesCountryFilter(spec.mode, spec.choice, r.exit_country, r.server_country)) return;
                    if (!terms.empty()) {
                        auto mc = meta_cache.find(r.endpoint_key);
                        const std::string empty;
                        const std::string& cached = mc != meta_cache.end() ? mc->second : empty;
                        auto text_has = [&](const std::string& t) {
                            return icontains(r.uri, t) || icontains(r.tag, t) || icontains(cached, t) ||
                                   (t == "gemini-ok" && r.gemini_status == 1) ||
                                   (t == "gemini-blocked" && r.gemini_status == 0);
                        };
                        if (!geo::matchesSearchQuery(terms, text_has, spec.mode, r.exit_country, r.server_country)) {
                            // vmess URIs hide the address in base64: parse once (outside the lock) and retry.
                            if (mc == meta_cache.end() && need_meta.size() < 2000 && r.uri.compare(0, 8, "vmess://") == 0)
                                need_meta.emplace_back(r.endpoint_key, r.uri);
                            return;
                        }
                    }
                    Item it;
                    it.cls = healthSortOrder(r.ev.state);
                    it.lfs = r.ev.last_full_success;
                    it.lat = r.latency_ms > 0 ? r.latency_ms : 1e9f;
                    it.key = r.endpoint_key;
                    items.push_back(std::move(it));
                });
                snap->avg_latency_ms = lat_n ? (float)(lat_sum / lat_n) : 0.0f;
                snap->matched = (int)items.size();

                // Phase 1b (lock released): parse the few base64-wrapped URIs we could not search.
                for (auto& kv : need_meta) {
                    std::string hay;
                    if (auto pc = network::UriParser::parse(kv.second)) hay = pc->protocol + " " + pc->address + " " + pc->ps;
                    meta_cache[kv.first] = hay;
                }
                if (!need_meta.empty()) view_dirty_ = true;   // re-run immediately with the new metadata

                // Phase 2: rank, cap.
                const size_t cap = spec.all_view ? (size_t)kMaxAllRows : (size_t)kMaxLiveRows;
                auto cmp = [](const Item& a, const Item& b) {
                    if (a.cls != b.cls) return a.cls < b.cls;
                    if (a.lfs != b.lfs) return a.lfs > b.lfs;
                    if (a.lat != b.lat) return a.lat < b.lat;
                    return a.key < b.key;
                };
                if (items.size() > cap) {
                    std::partial_sort(items.begin(), items.begin() + cap, items.end(), cmp);
                    items.resize(cap);
                } else {
                    std::sort(items.begin(), items.end(), cmp);
                }

                // Phase 3: fetch the visible rows (short individual locks) and evaluate with the shared evaluator.
                snap->rows.reserve(items.size());
                for (const auto& it : items) {
                    if (snapshot_stop_.load()) break;
                    UiRow row;
                    if (!db->getRecord(it.key, &row.rec)) continue;
                    row.ev = evaluateHealth(row.rec.ev, now, th);
                    auto pc = parsed_cache.find(row.rec.endpoint_key);
                    if (pc == parsed_cache.end()) {
                        std::tuple<std::string, std::string, std::string, int> t;
                        if (auto parsed = network::UriParser::parse(row.rec.uri)) {
                            t = {parsed->protocol, parsed->address, parsed->ps, parsed->port};
                        } else {
                            t = {"?", row.rec.uri.substr(0, std::min<size_t>(row.rec.uri.size(), 40)), "", 0};
                        }
                        if (parsed_cache.size() > 20000) parsed_cache.clear();
                        pc = parsed_cache.emplace(row.rec.endpoint_key, std::move(t)).first;
                    }
                    row.protocol = std::get<0>(pc->second);
                    row.address = std::get<1>(pc->second);
                    row.ps = displaySafeText(std::get<2>(pc->second));
                    row.port = std::get<3>(pc->second);
                    snap->rows.push_back(std::move(row));
                }
                // Default order = the shared ranking used by DB/failover (tier, score, last success, key).
                std::stable_sort(snap->rows.begin(), snap->rows.end(), [](const UiRow& a, const UiRow& b) {
                    RankKey ka, kb;
                    ka.tier = a.ev.tier; ka.score = a.ev.score; ka.last_full_success = a.rec.ev.last_full_success; ka.key = a.rec.endpoint_key;
                    kb.tier = b.ev.tier; kb.score = b.ev.score; kb.last_full_success = b.rec.ev.last_full_success; kb.key = b.rec.endpoint_key;
                    return rankedBefore(ka, kb);
                });
                selectTop20ForV2rayN(std::move(top_cands), snap->top20_uris, snap->top20_us);
                for (const auto& r : snap->rows) if (r.ev.stable) snap->stable++;
                snap->scan_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
                // Adaptive cadence: a slow scan (huge DB) backs off instead of contending with validators.
                next_wait = std::max(kSnapshotIntervalSeconds, snap->scan_ms / 1000.0 * 8.0);

                std::lock_guard<std::mutex> lock(snapshot_mutex_);
                latest_snapshot_ = std::move(snap);
            } catch (...) {
                // A failed scan just means the UI keeps the previous snapshot.
            }
        }

        std::unique_lock<std::mutex> lock(snapshot_mutex_);
        snapshot_cv_.wait_for(lock, std::chrono::milliseconds((int)(next_wait * 1000)),
            [this]() { return snapshot_stop_.load() || view_dirty_.load(); });
    }
}

void HunterGuiApp::refreshSnapshotIfDue() {
    // Cheap: take whatever the worker last published. Never touches the
    // database mutex, so a contended or slow scan can no longer stall a frame.
    std::shared_ptr<const UiSnapshot> latest;
    {
        std::lock_guard<std::mutex> lock(snapshot_mutex_);
        latest = latest_snapshot_;
    }

    if (latest && latest != applied_snapshot_) {
        applied_snapshot_ = latest;
        snapshot_ = latest->rows;
        top_v2rayn_uris_ = latest->top20_uris;
        top_v2rayn_us_count_ = latest->top20_us;
        cached_total_ = latest->total;
        cached_alive_ = latest->alive_count;
        cached_tested_ = latest->tested;
        cached_avg_latency_ms_ = latest->avg_latency_ms;
        cached_matched_ = latest->matched;
        cached_stable_ = latest->stable;
        cached_healthy_ = latest->healthy;
        cached_dead_ = latest->dead;
        cached_unknown_ = latest->unknown;
        cached_scan_ms_ = latest->scan_ms;
        cached_exit_counts_ = latest->exit_counts;
        cached_server_counts_ = latest->server_counts;
        rows_dirty_ = true;
    }

    // Poll running proxy servers — detect crashed processes and update status.
    // Throttled, since it walks the child processes.
    double now = glfwGetTime();
    if (last_refresh_time_ < 0.0 || now - last_refresh_time_ >= kRefreshIntervalSeconds) {
        last_refresh_time_ = now;
        orch_.proxyServerManager().poll();
    }
}

void HunterGuiApp::pollProxyServers() {
    orch_.proxyServerManager().poll();
}

void HunterGuiApp::rebuildVisibleRowsIfDirty(int sort_col, bool sort_asc) {
    if (!rows_dirty_ && sort_col == sort_column_ && sort_asc == sort_ascending_) return;

    // Filtering/search/country selection happen in the snapshot worker (whole DB, off the render
    // thread); here we only apply the column sort to the published rows.
    visible_rows_.clear();
    visible_rows_.reserve(snapshot_.size());
    for (const auto& r : snapshot_) visible_rows_.push_back(&r);

    if (sort_col >= 0) {
        std::stable_sort(visible_rows_.begin(), visible_rows_.end(),
                  [&](const UiRow* a, const UiRow* b) {
            int c = 0;   // <0: a before b (ascending)
            auto cmpd = [&](double x, double y) { return x < y ? -1 : (x > y ? 1 : 0); };
            switch (sort_col) {
                case 0: c = healthSortOrder(a->rec.ev.state) - healthSortOrder(b->rec.ev.state); break;
                case 1: c = std::strcmp(stabilityLabel(a->rec, a->ev), stabilityLabel(b->rec, b->ev)); break;
                case 2: c = cmpd(a->ev.score, b->ev.score); break;
                case 3: c = cmpd(a->ev.has_p90 ? a->ev.p90_ms : 1e18, b->ev.has_p90 ? b->ev.p90_ms : 1e18); break;
                case 4: c = cmpd(a->rec.latency_ms, b->rec.latency_ms); break;
                case 5: c = countryCell(a->rec.exit_country).compare(countryCell(b->rec.exit_country)); break;
                case 6: c = countryCell(a->rec.server_country).compare(countryCell(b->rec.server_country)); break;
                case 7: c = a->protocol.compare(b->protocol); break;
                case 8: c = a->address.compare(b->address); break;
                case 9: c = a->rec.engine_used.compare(b->rec.engine_used); break;
                case 10: c = cmpd(a->rec.last_tested, b->rec.last_tested); break;
                case 11: c = a->rec.tag.compare(b->rec.tag); break;
                case 12: c = a->rec.gemini_status - b->rec.gemini_status; break;
                case 13: {
                    auto& psm = orch_.proxyServerManager();
                    c = (int)psm.getStatus(a->rec.uri) - (int)psm.getStatus(b->rec.uri);
                    break;
                }
            }
            if (c == 0) return a->rec.endpoint_key < b->rec.endpoint_key;   // stable, key tie-break
            return sort_asc ? c < 0 : c > 0;
        });
    }

    sort_column_ = sort_col;
    sort_ascending_ = sort_asc;
    rows_dirty_ = false;
}

std::string HunterGuiApp::buildClipboardText(bool selected_only) const {
    std::vector<std::string> uris;
    for (const auto& row : snapshot_) {
        const auto& r = row.rec;
        if (!r.alive) continue;
        if (selected_only && selected_uris_.find(r.uri) == selected_uris_.end()) continue;
        uris.push_back(r.uri);
    }
    return client_export::buildShareText(uris);
}

std::string HunterGuiApp::buildGeminiOkClipboardText() const {
    std::vector<std::string> uris;
    for (const auto& row : snapshot_) {
        const auto& r = row.rec;
        if (!r.alive) continue;
        if (r.gemini_status != 1) continue;  // only Gemini-accessible
        uris.push_back(r.uri);
    }
    return client_export::buildShareText(uris);
}

std::string HunterGuiApp::buildExportText() const {
    // Plain URI list, no comment header: v2rayN/v2rayNG/NekoBox reject '#' lines.
    std::vector<std::string> uris;
    for (const auto& row : snapshot_) {
        if (!row.rec.alive) continue;
        uris.push_back(row.rec.uri);
    }
    return client_export::buildShareText(uris);
}

std::string HunterGuiApp::buildV2rayNTop20Text(int& us_count) const {
    if (!top_v2rayn_uris_.empty()) {
        us_count = top_v2rayn_us_count_;
        return client_export::buildShareText(top_v2rayn_uris_);
    }
    // Fallback: the worker hasn't published yet — walk the DB directly.
    us_count = 0;
    auto* db = orch_.configDb();
    if (!db) return "";
    std::vector<Top20Candidate> cands;
    const HealthThresholds th = db->thresholds();
    const double now = systemClock()();
    db->forEachRecord([&](const ConfigHealthRecord& r) {
        if (!r.alive) return;
        cands.push_back(top20CandidateFor(r, th, now));
    });
    std::vector<std::string> uris;
    selectTop20ForV2rayN(std::move(cands), uris, us_count);
    return client_export::buildShareText(uris);
}

std::string HunterGuiApp::buildFoundClipboardText() const {
    std::vector<std::string> uris;
    for (const auto& e : orch_.foundHistory().snapshot()) uris.push_back(e.uri);
    return client_export::buildShareText(uris);
}

void HunterGuiApp::writeTop20Files(const std::string& text) {
    std::error_code ec;
    std::filesystem::create_directories("runtime", ec);
    {
        std::ofstream f("runtime/v2rayn_top20.txt", std::ios::trunc);
        if (f) f << text;
    }
    {
        std::ofstream f("runtime/v2rayn_top20_b64.txt", std::ios::trunc);
        if (f) f << utils::base64Encode(text);
    }
}

void HunterGuiApp::copyTop20ForV2rayN() {
    int us = 0;
    std::string text = buildV2rayNTop20Text(us);
    if (text.empty()) {
        showToast("No live configs yet");
        return;
    }
    int n = std::count(text.begin(), text.end(), '\n');
    ImGui::SetClipboardText(text.c_str());
    writeTop20Files(text);
    showToast("Copied " + std::to_string(n) + " configs for v2rayN (" + std::to_string(us) + " US)");
}

void HunterGuiApp::exportFoundHistory() {
    auto entries = orch_.foundHistory().snapshot();
    if (entries.empty()) {
        showToast("No found configs yet");
        return;
    }
    std::string text;
    for (const auto& e : entries) {
        text += e.uri;
        text += '\n';
    }
    std::error_code ec;
    std::filesystem::create_directories("runtime", ec);
    std::ofstream f("runtime/found_history.txt", std::ios::trunc);
    if (!f) {
        showToast("Export failed: cannot write runtime/found_history.txt");
        return;
    }
    f << text;
    showToast("Exported " + std::to_string(entries.size()) + " found configs to runtime/found_history.txt");
}

void HunterGuiApp::showToast(const std::string& msg) {
    std::lock_guard<std::mutex> lock(toast_mu_);
    toast_message_ = msg;
    toast_until_ = glfwGetTime() + 3.0;
}

void HunterGuiApp::exportToFile(const std::string& content, const char* default_name) {
    // Use a simple timestamped filename in the runtime/ directory.
    std::time_t now = std::time(nullptr);
    char timebuf[32];
    std::strftime(timebuf, sizeof(timebuf), "%Y%m%d_%H%M%S", std::localtime(&now));

    std::string filename = "runtime/";
    filename += default_name;
    filename += "_";
    filename += timebuf;
    filename += ".txt";

    std::ofstream f(filename);
    if (!f) {
        showToast("Export failed: cannot write " + filename);
        return;
    }
    f << content;
    f.close();
    showToast("Exported to " + filename);
}

void HunterGuiApp::renderQrPopup() {
    // OpenPopup only needs to fire on the frame the button was pressed, but
    // BeginPopupModal must be reached on EVERY frame the popup is open —
    // ImGui closes a popup whose Begin is not called. Returning early here
    // when the flag was already consumed is what made the QR flash for a
    // single frame and disappear.
    if (show_qr_popup_) {
        ImGui::OpenPopup("QR Code");
        show_qr_popup_ = false; // consume the flag
    }

    ImGui::SetNextWindowSize(scaledSize(380, 420), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("QR Code", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        if (qr_text_.empty()) {
            ImGui::TextDisabled("No config selected.");
        } else {
            ImGui::TextDisabled("Scan with v2rayNG / v2rayN / etc.");
            ImGui::Spacing();

            // Render the QR code centered in the popup.
            float qr_size = recommendedQrSize(qr_text_, 280.0f);
            float popup_width = ImGui::GetContentRegionAvail().x;
            float x_offset = (popup_width - qr_size) * 0.5f;
            if (x_offset < 0) x_offset = 0;

            ImVec2 origin = ImGui::GetCursorScreenPos();
            origin.x += x_offset;
            float drawn = qr_size;
            if (renderQrCode(qr_text_, origin, qr_size, &drawn)) {
                // Reserve the size actually drawn, not the requested one.
                ImGui::Dummy(ImVec2(popup_width, drawn + 4));
             } else {
                 ImGui::TextColored(kDangerRed, "URI too long to encode as a QR code.");
             }

            ImGui::Spacing();
            ImGui::TextWrapped("URI: %s", qr_text_.substr(0, 200).c_str());
            if (qr_text_.size() > 200) ImGui::TextDisabled("... (%zu bytes)", qr_text_.size());
        }

        ImGui::Spacing();
        if (ImGui::Button("Copy URI", ImVec2(120, 0))) {
            ImGui::SetClipboardText(qr_text_.c_str());
            showToast("URI copied to clipboard");
        }
        ImGui::SameLine();
        if (ImGui::Button("Close", ImVec2(120, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void HunterGuiApp::clearAllLive() {
    auto* db = orch_.configDb();
    if (!db) return;

    int removed = db->clearAlive();

    // The live cache is re-merged into the DB on every startup
    // (HunterOrchestrator loads runtime/HUNTER_live_cache.tsv), so clearing
    // only the in-memory DB would let everything reappear on the next launch.
    std::error_code ec;
    std::filesystem::remove("runtime/HUNTER_live_cache.tsv", ec);

    // The main DB is otherwise only flushed every ~60s and on shutdown; persist
    // now so the clear survives even if the app dies before the next flush.
    try {
        db->saveToDisk("runtime/HUNTER_config_db.tsv");
    } catch (...) {}

    // Drop UI state that still points at the removed records.
    selected_uris_.clear();
    last_clicked_uri_.clear();
    snapshot_.clear();
    visible_rows_.clear();
    parse_cache_.clear();
    rows_dirty_ = true;
    last_refresh_time_ = -1.0;  // force an immediate re-read of the DB

    showToast("Cleared " + std::to_string(removed) + " live configs");
}

void HunterGuiApp::renderClearLivePopup() {
    // Same lifetime rule as renderQrPopup: OpenPopup fires once, but
    // BeginPopupModal has to be reached every frame the popup is open.
    if (show_clear_live_popup_) {
        ImGui::OpenPopup("Clear All Live");
        show_clear_live_popup_ = false;
    }

    if (ImGui::BeginPopupModal("Clear All Live", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Remove all %d live configs?", cached_alive_);
        ImGui::Spacing();
        ImGui::TextDisabled("They are dropped from the database and from");
        ImGui::TextDisabled("runtime/HUNTER_live_cache.tsv, so they will not");
        ImGui::TextDisabled("come back on restart. Untested configs are kept");
        ImGui::TextDisabled("and can be re-tested.");
        ImGui::Spacing();

        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.20f, 0.20f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.70f, 0.25f, 0.25f, 1.0f));
        if (ImGui::Button("Clear", ImVec2(120, 0))) {
            clearAllLive();
            ImGui::CloseCurrentPopup();
        }
        ImGui::PopStyleColor(2);
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void HunterGuiApp::applyModernTheme() {
     if (theme_applied_) return;

     ImGuiStyle& style = ImGui::GetStyle();
     // Start from defaults so ScaleAllSizes() below never compounds on fields
     // this function does not rewrite explicitly.
     style = ImGuiStyle();
     style.FontSizeBase = kUiFontSizePx;

     // Modern rounded corners (6-8)
     style.WindowRounding = 8.0f;
     style.FrameRounding = 6.0f;
     style.GrabRounding = 6.0f;
     style.TabRounding = 6.0f;
     style.ScrollbarRounding = 8.0f;
     style.PopupRounding = 6.0f;
     style.ChildRounding = 6.0f;

     // Spacing - generous but consistent padding (design px, scaled by ui_scale_)
     style.WindowPadding = ImVec2(Spacing::Large, Spacing::Large);
#ifdef __ANDROID__
     style.FramePadding = ImVec2(Spacing::Medium, 12.0f);  // touch targets
#else
     style.FramePadding = ImVec2(Spacing::Medium, Spacing::Small);
#endif
     style.ItemSpacing = ImVec2(Spacing::Medium, Spacing::Small);
     style.ItemInnerSpacing = ImVec2(Spacing::Small, Spacing::Small);
     style.IndentSpacing = 24.0f;
     style.ScrollbarSize = 12.0f;

     // Sizing - clean, borderless windows
     style.WindowBorderSize = 0.0f;
     style.ChildBorderSize = 1.0f;
     style.PopupBorderSize = 1.0f;
     style.FrameBorderSize = 0.0f;
     style.TabBorderSize = 0.0f;

     // Modern dark color palette
     ImVec4* colors = style.Colors;

     // Background tones
     colors[ImGuiCol_WindowBg]        = kBgPrimary;
     colors[ImGuiCol_ChildBg]         = kBgSecondary;
     colors[ImGuiCol_PopupBg]         = kBgTertiary;
     colors[ImGuiCol_Border]          = kBorderLight;
     colors[ImGuiCol_BorderShadow]    = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);

     // Text hierarchy
     colors[ImGuiCol_Text]            = kTextPrimary;
     colors[ImGuiCol_TextDisabled]    = kTextDisabled;

     // Frames / inputs
     colors[ImGuiCol_FrameBg]         = kBgTertiary;
     colors[ImGuiCol_FrameBgHovered]  = ImVec4(0.14f, 0.16f, 0.20f, 1.0f);
     colors[ImGuiCol_FrameBgActive]   = ImVec4(0.18f, 0.20f, 0.25f, 1.0f);

     // Buttons — accent blue primary
     colors[ImGuiCol_Button]          = kAccentBlue;
     colors[ImGuiCol_ButtonHovered]   = ImVec4(0.38f, 0.60f, 1.00f, 1.0f);
     colors[ImGuiCol_ButtonActive]    = ImVec4(0.24f, 0.45f, 0.85f, 1.0f);

     // Headers / collapsibles
     colors[ImGuiCol_Header]          = kAccentBlue;
     colors[ImGuiCol_HeaderHovered]   = ImVec4(0.38f, 0.60f, 1.00f, 1.0f);
     colors[ImGuiCol_HeaderActive]    = ImVec4(0.24f, 0.45f, 0.85f, 1.0f);

     // Tabs
     colors[ImGuiCol_Tab]             = kBgSecondary;
     colors[ImGuiCol_TabHovered]      = ImVec4(0.18f, 0.30f, 0.55f, 1.0f);
     colors[ImGuiCol_TabActive]       = kAccentBlue;
     colors[ImGuiCol_TabUnfocused]    = kBgPrimary;
     colors[ImGuiCol_TabUnfocusedActive] = kBgSecondary;

     // Title bar
     colors[ImGuiCol_TitleBg]         = kBgPrimary;
     colors[ImGuiCol_TitleBgActive]   = kBgSecondary;
     colors[ImGuiCol_TitleBgCollapsed]= kBgPrimary;

     // Scrollbar
     colors[ImGuiCol_ScrollbarBg]     = kBgPrimary;
     colors[ImGuiCol_ScrollbarGrab]   = kBorderStrong;
     colors[ImGuiCol_ScrollbarGrabHovered] = kAccentBlue;
     colors[ImGuiCol_ScrollbarGrabActive]  = ImVec4(0.40f, 0.65f, 1.00f, 1.0f);

     // Checkmark / slider / separator
     colors[ImGuiCol_CheckMark]       = kSuccessGreen;
     colors[ImGuiCol_SliderGrab]      = kAccentBlue;
     colors[ImGuiCol_SliderGrabActive]= ImVec4(0.40f, 0.70f, 1.00f, 1.0f);
     colors[ImGuiCol_Separator]       = kBorderLight;
     colors[ImGuiCol_SeparatorHovered]= ImVec4(kAccentBlue.x, kAccentBlue.y, kAccentBlue.z, 0.60f);
     colors[ImGuiCol_SeparatorActive] = ImVec4(kAccentBlue.x, kAccentBlue.y, kAccentBlue.z, 1.0f);

     // Resize grip
     colors[ImGuiCol_ResizeGrip]      = ImVec4(0.20f, 0.25f, 0.30f, 0.50f);
     colors[ImGuiCol_ResizeGripHovered] = ImVec4(kAccentBlue.x, kAccentBlue.y, kAccentBlue.z, 0.70f);
     colors[ImGuiCol_ResizeGripActive]  = ImVec4(0.40f, 0.70f, 1.00f, 0.95f);

     // Plot
     colors[ImGuiCol_PlotLines]       = kSuccessGreen;
     colors[ImGuiCol_PlotLinesHovered]= ImVec4(0.40f, 0.85f, 0.50f, 1.0f);
     colors[ImGuiCol_PlotHistogram]   = kSuccessGreen;
     colors[ImGuiCol_PlotHistogramHovered] = ImVec4(0.40f, 0.85f, 0.50f, 1.0f);

     // Table
     colors[ImGuiCol_TableHeaderBg]   = kBgTertiary;
     colors[ImGuiCol_TableBorderStrong] = kBorderStrong;
     colors[ImGuiCol_TableBorderLight]  = kBorderLight;
     colors[ImGuiCol_TableRowBg]      = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
     colors[ImGuiCol_TableRowBgAlt]   = ImVec4(0.08f, 0.10f, 0.12f, 0.50f);

     // Nav
     colors[ImGuiCol_NavHighlight]    = kSuccessGreen;
     colors[ImGuiCol_NavWindowingHighlight] = ImVec4(kAccentBlue.x, kAccentBlue.y, kAccentBlue.z, 0.70f);
     colors[ImGuiCol_NavWindowingDimBg]     = ImVec4(0.06f, 0.07f, 0.09f, 0.50f);

     // Drag-drop target
     colors[ImGuiCol_DragDropTarget]  = ImVec4(kAccentBlue.x, kAccentBlue.y, kAccentBlue.z, 0.90f);

     // Scale every metric by the current display content scale. The base
     // values above are design pixels and are rewritten first on every call,
     // so this is idempotent across DPI changes (never compounds).
     style.ScaleAllSizes(ui_scale_);
     // Fonts are rasterized at FontSizeBase * FontScaleDpi (imgui 1.92 dynamic
     // fonts), so text stays sharp at any DPI instead of being stretched.
     style.FontScaleDpi = ui_scale_;

     theme_applied_ = true;
 }

ImVec2 HunterGuiApp::scaledSize(float w, float h) const {
    return ImVec2(w * ui_scale_, h * ui_scale_);
}

void HunterGuiApp::refreshDpiScale() {
#ifndef __ANDROID__
    if (!window_) return;
    float sx = 1.0f, sy = 1.0f;
    glfwGetWindowContentScale(window_, &sx, &sy);
    float s = (sx + sy) * 0.5f;
    if (s <= 0.0f) s = 1.0f;
    if (s != ui_scale_) {
        ui_scale_ = s;
        theme_applied_ = false;   // rewrite base metrics, then re-scale on top
        applyModernTheme();
    }
#endif
}

void HunterGuiApp::renderTopBar() {
    // One row at every width: [title] [status pill] ........ [Start/Stop] [About]
    const ImGuiStyle& style = ImGui::GetStyle();
    const bool hunting = orchestrator_running_.load();
    const bool stopping = shutdown_in_progress_.load();
    const bool active = orch_.clientStatus().active;

    ImVec4 status_color = kTextDisabled;
    const char* status_text = "Idle";
    if (stopping) { status_color = kWarnAmber; status_text = "Stopping"; }
    else if (active) { status_color = kSuccessGreen; status_text = "Connected"; }
    else if (hunting) { status_color = kWarnAmber; status_text = "Hunting"; }

    const float row_h = ImGui::GetFrameHeight();
    const float avail = ImGui::GetContentRegionAvail().x;
    if (avail >= 420.0f * ui_scale_) {
        ImGui::AlignTextToFramePadding();
        ImGui::PushFont(nullptr, kTitleFontSizePx);
        ImGui::TextColored(kAccentBlue, "Hunter");
        ImGui::PopFont();
        ImGui::SameLine(0, style.ItemSpacing.x * 2.0f);
    }

    // Status pill: a label, not a button (it has no action).
    {
        const ImVec2 text_size = ImGui::CalcTextSize(status_text);
        const ImVec2 pill(text_size.x + style.FramePadding.x * 4.0f, row_h);
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(p0, ImVec2(p0.x + pill.x, p0.y + pill.y),
                          ImGui::GetColorU32(ImVec4(status_color.x, status_color.y, status_color.z, 0.18f)),
                          pill.y * 0.5f);
        const float dot_r = row_h * 0.16f;
        dl->AddCircleFilled(ImVec2(p0.x + style.FramePadding.x * 1.2f + dot_r, p0.y + pill.y * 0.5f), dot_r,
                            ImGui::GetColorU32(status_color));
        dl->AddText(ImVec2(p0.x + style.FramePadding.x * 2.2f + dot_r, p0.y + (pill.y - text_size.y) * 0.5f),
                    ImGui::GetColorU32(status_color), status_text);
        ImGui::Dummy(ImVec2(pill.x + dot_r * 2.0f, pill.y));
    }

    // Right-aligned actions.
    const float btn_w = 96.0f * ui_scale_;
    ImGui::SameLine(std::max(ImGui::GetCursorPosX(),
                             ImGui::GetWindowContentRegionMax().x - btn_w * 2.0f - style.ItemSpacing.x));
    ImGui::BeginDisabled(stopping);
    ImGui::PushStyleColor(ImGuiCol_Button, hunting ? ImVec4(0.55f, 0.20f, 0.20f, 1.0f) : kAccentBlue);
    if (ImGui::Button(hunting ? "Stop" : "Start", ImVec2(btn_w, 0))) {
        if (hunting) {
            requestShutdown();
            showToast("Stopping...");
        } else {
            startOrchestrator();
            showToast("Started");
        }
    }
    ImGui::PopStyleColor();
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (currentView_ == View::Main) {
        if (ImGui::Button("About", ImVec2(btn_w, 0))) currentView_ = View::About;
    } else {
        if (ImGui::Button("< Back", ImVec2(btn_w, 0))) currentView_ = View::Main;
    }
    ImGui::Separator();
}

void HunterGuiApp::renderStatCards() {
    const float card_height = ImGui::GetTextLineHeightWithSpacing() + kStatFontSizePx * ui_scale_ + ImGui::GetStyle().ItemSpacing.y + ImGui::GetStyle().WindowPadding.y * 2.0f;
    const bool narrow = viewport_width_ < kNarrowThreshold;
    
    if (narrow) {
        // Stack vertically: 2 rows of 2 cards
        float card_width = ImGui::GetContentRegionAvail().x * 0.5f - Spacing::Small * 0.5f;
        
        auto renderCard = [&](const char* label, const char* value, ImVec4 color) {
            ImGui::PushID(label); ImGui::BeginChild("##card", ImVec2(card_width, card_height), true);
            ImGui::TextDisabled("%s", label);
            ImGui::PushFont(nullptr, kStatFontSizePx); ImGui::TextColored(color, "%s", value); ImGui::PopFont();
            ImGui::EndChild(); ImGui::PopID();
        };
        
        // Row 1: Alive, Tested
        renderCard("Alive", std::to_string(cached_alive_).c_str(), kSuccessGreen);
        ImGui::SameLine();
        renderCard("Tested", std::to_string(cached_tested_).c_str(), kAccentBlue);
        
        // Row 2: Found, Proxy
        int found_count = (int)found_cache_.size();
        renderCard("Found", std::to_string(found_count).c_str(), kWarnAmber);
        ImGui::SameLine();
        {
            ImGui::PushID("Proxy"); ImGui::BeginChild("##card", ImVec2(card_width, card_height), true);
            ImGui::TextDisabled("Proxy");
            const char* proxy_state = "Off";
            ImVec4 proxy_color = kTextDisabled;
            auto client_status = orch_.clientStatus();
            if (client_status.active) {
                proxy_state = "On";
                proxy_color = kSuccessGreen;
            }
            ImGui::PushFont(nullptr, kStatFontSizePx); ImGui::TextColored(proxy_color, "%s", proxy_state); ImGui::PopFont();
            ImGui::EndChild(); ImGui::PopID();
        }
    } else {
        // Horizontal row of 4 cards
        float card_width = (ImGui::GetContentRegionAvail().x - Spacing::Medium * 3) * 0.25f;
        
        auto renderCard = [&](const char* label, const char* value, ImVec4 color) {
            ImGui::PushID(label); ImGui::BeginChild("##card", ImVec2(card_width, card_height), true);
            ImGui::TextDisabled("%s", label);
            ImGui::PushFont(nullptr, kStatFontSizePx); ImGui::TextColored(color, "%s", value); ImGui::PopFont();
            ImGui::EndChild(); ImGui::PopID();
        };
        
        renderCard("Alive", std::to_string(cached_alive_).c_str(), kSuccessGreen);
        ImGui::SameLine();
        renderCard("Tested", std::to_string(cached_tested_).c_str(), kAccentBlue);
        ImGui::SameLine();
        {
            int found_count = (int)found_cache_.size();
            renderCard("Found", std::to_string(found_count).c_str(), kWarnAmber);
        }
        ImGui::SameLine();
        {
            ImGui::PushID("Proxy"); ImGui::BeginChild("##card", ImVec2(card_width, card_height), true);
            ImGui::TextDisabled("Proxy");
            const char* proxy_state = "Off";
            ImVec4 proxy_color = kTextDisabled;
            auto client_status = orch_.clientStatus();
            if (client_status.active) {
                proxy_state = "On";
                proxy_color = kSuccessGreen;
            }
            ImGui::PushFont(nullptr, kStatFontSizePx); ImGui::TextColored(proxy_color, "%s", proxy_state); ImGui::PopFont();
            ImGui::EndChild(); ImGui::PopID();
        }
    }
    ImGui::Spacing();
}

void HunterGuiApp::renderActionBar() {
    bool active = false;
    auto client_status = orch_.clientStatus();
    if (client_status.active) active = true;
    bool hunting = orchestrator_running_.load();
    bool has_selection = !selected_uris_.empty();
    bool has_snapshot = !snapshot_.empty();
    
    const float btn_height = 0; // auto
    // Flow layout: equal-width buttons fill the row and wrap onto more rows
    // when the window is narrower than kActionCount * min width.
    constexpr int kActionCount = 8;
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const float avail = ImGui::GetContentRegionAvail().x;
    const float min_w = 110.0f * ui_scale_;
    const int per_row = std::max(1, std::min(kActionCount, (int)((avail + spacing) / (min_w + spacing))));
    const int rows = (kActionCount + per_row - 1) / per_row;
    const int cols = (kActionCount + rows - 1) / rows;  // balance rows (e.g. 4+4, not 7+1)
    const float btn_w = std::floor((avail - spacing * (cols - 1)) / cols);
    int slot = 0;
    auto nextSlot = [&]() {
        if (++slot % cols != 0) ImGui::SameLine();
    };
    
    ImGui::PushStyleColor(ImGuiCol_Button, kAccentBlue);
    const bool connect_clicked = ImGui::Button(active ? "Disconnect" : "Connect", ImVec2(btn_w, btn_height));
    ImGui::PopStyleColor();
    if (connect_clicked) {
        if (active) {
            runBackground([this]() {
                orch_.deactivateClientConfig();
                showToast("Disconnected");
            });
        } else if (has_selection) {
            std::string uri = *selected_uris_.begin();
            runBackground([this, uri]() {
                int port = orch_.activateClientConfig(uri);
                if (port > 0) {
                    showToast("Connected: 127.0.0.1:" + std::to_string(port) + " (SOCKS5 + HTTP)");
                } else {
                    std::string why = orch_.clientError(uri);
                    if (why.empty()) why = "unknown error";
                    std::fprintf(stderr, "[gui] connect failed: %s\n", why.c_str());
                    showToast("Connect failed: " + why);
                }
            });
        } else {
            showToast("No profile selected");
        }
    }
    nextSlot();
    
    if (ImGui::Button("Test", ImVec2(btn_w, btn_height))) {
        std::vector<std::string> uris_to_test;
        if (selected_uris_.empty()) {
            for (const auto& row : snapshot_) uris_to_test.push_back(row.rec.uri);
        } else {
            for (const auto& uri : selected_uris_) uris_to_test.push_back(uri);
        }
        if (uris_to_test.empty()) { showToast("No configs to test"); return; }
        runBackground([this, uris_to_test]() {
            network::ProxyTester tester;
            network::BatchTestOptions opts;
            opts.timeout_seconds = 5;
            opts.bulk = false;
            auto results = tester.testBatch(uris_to_test, opts);
            {
                std::lock_guard<std::mutex> lock(client_latency_mu_);
                for (size_t i = 0; i < results.size(); ++i) {
                    const auto& res = results[i];
                    client_latency_results_[uris_to_test[i]] =
                        (res.success && res.has_probe) ? static_cast<int>(res.latency_ms) : -1;
                }
            }
            showToast("Latency test completed for " + std::to_string(uris_to_test.size()) + " configs");
        });
    }
    nextSlot();
    
    if (ImGui::Button("Copy Selected", ImVec2(btn_w, btn_height))) {
        if (has_selection) {
            std::string text = buildClipboardText(true);
            ImGui::SetClipboardText(text.c_str());
            int n = std::count(text.begin(), text.end(), '\n');
            showToast("Copied " + std::to_string(n) + " configs");
        } else {
            showToast("No selection");
        }
    }
    nextSlot();
    
    if (ImGui::Button("Copy All", ImVec2(btn_w, btn_height))) {
        if (has_snapshot) {
            std::string text = buildClipboardText(false);
            ImGui::SetClipboardText(text.c_str());
            int n = std::count(text.begin(), text.end(), '\n');
            showToast("Copied " + std::to_string(n) + " configs");
        } else {
            showToast("No configs");
        }
    }
    nextSlot();
    
    if (ImGui::Button("Copy v2rayN", ImVec2(btn_w, btn_height))) {
        int us = 0;
        std::string text = buildV2rayNTop20Text(us);
        if (text.empty()) {
            showToast("No live configs yet");
        } else {
            ImGui::SetClipboardText(text.c_str());
            writeTop20Files(text);
            int n = std::count(text.begin(), text.end(), '\n');
            showToast("Copied " + std::to_string(n) + " configs for v2rayN (" + std::to_string(us) + " US)");
        }
    }
    nextSlot();
    
    if (ImGui::Button("Export", ImVec2(btn_w, btn_height))) {
        if (has_snapshot) {
            std::string text = buildExportText();
            exportToFile(text, "hunter_export");
        } else {
            showToast("No configs to export");
        }
    }
    nextSlot();
    
    if (ImGui::Button("QR", ImVec2(btn_w, btn_height))) {
        if (has_selection) {
            qr_text_ = client_export::normalizeUri(*selected_uris_.begin());
            show_qr_popup_ = true;
        } else {
            showToast("Select a profile first");
        }
    }
    nextSlot();
    
    if (ImGui::Button("Import", ImVec2(btn_w, btn_height))) {
        ImGui::OpenPopup("##import_menu");
    }
    
    if (ImGui::BeginPopup("##import_menu")) {
        if (ImGui::MenuItem("Paste from clipboard")) {
            std::string clipboard = ImGui::GetClipboardText();
            auto uris = utils::extractRawUrisFromText(clipboard);
            if (!uris.empty()) {
                orch_.configDb()->addConfigsWithPriority(uris, "manual");
                showToast("Added " + std::to_string(uris.size()) + " configs from clipboard");
            } else {
                showToast("No valid URIs found in clipboard");
            }
        }
        ImGui::EndPopup();
    }
}

void HunterGuiApp::renderConfigTable() {
    auto client_status = orch_.clientStatus();
    bool active = client_status.active;

    static ImGuiTableFlags flags =
        ImGuiTableFlags_BordersOuter | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable |
        ImGuiTableFlags_ScrollY | ImGuiTableFlags_Sortable | ImGuiTableFlags_SizingStretchProp |
        ImGuiTableFlags_ContextMenuInBody | ImGuiTableFlags_Hideable | ImGuiTableFlags_Reorderable;

    const float table_height = ImGui::GetContentRegionAvail().y;

    const int num_columns = (viewport_width_ < kNarrowThreshold) ? 3 : 7;
    if (!ImGui::BeginTable("main_profiles", num_columns, flags, ImVec2(0, table_height))) {
        return;  // table clipped this frame; EndTable must not be called

    }

    // Column setup
    ImGui::TableSetupColumn("Latency", ImGuiTableColumnFlags_WidthFixed |
                                           ImGuiTableColumnFlags_DefaultSort |
                                           ImGuiTableColumnFlags_PreferSortAscending, 80 * ui_scale_);
    ImGui::TableSetupColumn("Exit", ImGuiTableColumnFlags_WidthFixed, 130 * ui_scale_);
    ImGui::TableSetupColumn("Remark", ImGuiTableColumnFlags_WidthStretch);
    if (viewport_width_ >= kNarrowThreshold) {
        ImGui::TableSetupColumn("Server", ImGuiTableColumnFlags_WidthFixed, 80 * ui_scale_);
        ImGui::TableSetupColumn("Proto", ImGuiTableColumnFlags_WidthFixed, 70 * ui_scale_);
        ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("URI", ImGuiTableColumnFlags_WidthStretch);
    }
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableHeadersRow();

    // Sort
    std::vector<const UiRow*> rows_sorted;
    rows_sorted.reserve(snapshot_.size());
    for (const auto& row : snapshot_) rows_sorted.push_back(&row);
    if (ImGuiTableSortSpecs* ss = ImGui::TableGetSortSpecs(); ss && ss->SpecsCount > 0) {
        const auto& spec = ss->Specs[0];
        std::unordered_map<std::string, int> lat_copy;
        {
            std::lock_guard<std::mutex> lock(client_latency_mu_);
            lat_copy = client_latency_results_;
        }
        auto lat_of = [&](const UiRow* p) {
            auto it = lat_copy.find(p->rec.uri);
            return (it != lat_copy.end() && it->second >= 0) ? it->second : INT_MAX;
        };
        std::stable_sort(rows_sorted.begin(), rows_sorted.end(),
                         [&](const UiRow* a, const UiRow* b) {
            int c = 0;
            switch (spec.ColumnIndex) {
                case 0: c = lat_of(a) - lat_of(b); break;
                case 1: c = countryCell(a->rec.exit_country).compare(countryCell(b->rec.exit_country)); break;
                case 2: c = a->ps.compare(b->ps); break;
                case 3:
                    if (viewport_width_ >= kNarrowThreshold) {
                        c = countryCell(a->rec.server_country).compare(countryCell(b->rec.server_country));
                    }
                    break;
                case 4:
                    if (viewport_width_ >= kNarrowThreshold) {
                        c = a->protocol.compare(b->protocol);
                    }
                    break;
                case 5:
                    if (viewport_width_ >= kNarrowThreshold) {
                        c = a->address.compare(b->address);
                    }
                    break;
                case 6:
                    if (viewport_width_ >= kNarrowThreshold) {
                        c = a->rec.uri.compare(b->rec.uri);
                    }
                    break;
            }
            if (c == 0) return a->rec.endpoint_key < b->rec.endpoint_key;
            return spec.SortDirection == ImGuiSortDirection_Ascending ? c < 0 : c > 0;
        });
    }

    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(rows_sorted.size()));
    while (clipper.Step()) {
        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
            const UiRow& row = *rows_sorted[i];
            const ConfigHealthRecord& r = row.rec;
            bool selected = selected_uris_.count(r.uri) != 0;

            ImGui::TableNextRow();
            ImGui::TableNextColumn();

            int lat_ms = -2;
            {
                std::lock_guard<std::mutex> lock(client_latency_mu_);
                auto it = client_latency_results_.find(r.uri);
                if (it != client_latency_results_.end()) lat_ms = it->second;
            }

            // Full-row selectable
            ImGui::PushID(r.uri.c_str());
            if (ImGui::Selectable("##row", selected,
                                ImGuiSelectableFlags_SpanAllColumns |
                                ImGuiSelectableFlags_AllowOverlap)) {
                if (selected) selected_uris_.erase(r.uri);
                else selected_uris_.insert(r.uri);
            }
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
                runBackground([this, uri = r.uri]() {
                    int port = orch_.activateClientConfig(uri);
                    if (port > 0) showToast("Connected on port " + std::to_string(port));
                    else showToast("Failed to connect to " + uri);
                });
            }
            if (active && r.uri == client_status.uri) {
                ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,
                    ImGui::ColorConvertFloat4ToU32(ImVec4(kSuccessGreen.x, kSuccessGreen.y, kSuccessGreen.z, 0.3f)));
            }
            ImGui::SameLine();
            if (lat_ms >= 0) {
                ImGui::Text("%.0f ms", (float)lat_ms);
            } else if (lat_ms == -1) {
                ImGui::TextColored(kDangerRed, "failed");
            } else {
                ImGui::TextDisabled("...");
            }
            ImGui::PopID();

            ImGui::TableNextColumn();
            // Exit country
            {
                const bool known = !r.exit_country.empty();
                const bool stale = known && (glfwGetTime() - r.exit_country_at) > (double)std::max(60, orch_.config().exitCountryFreshSeconds());
                if (!known) ImGui::TextDisabled("Unknown");
                else if (stale) ImGui::TextColored(ImVec4(0.65f, 0.65f, 0.55f, 1.0f), "%s ~", countryCell(r.exit_country).c_str());
                else ImGui::TextUnformatted(countryCell(r.exit_country).c_str());
            }

            ImGui::TableNextColumn();
            ImGui::TextUnformatted(row.ps.empty() ? "-" : row.ps.c_str());

            if (viewport_width_ >= kNarrowThreshold) {
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(countryCell(r.server_country).c_str());

                ImGui::TableNextColumn();
                ImGui::TextUnformatted(row.protocol.c_str());

                ImGui::TableNextColumn();
                ImGui::TextUnformatted(row.address.c_str());

                ImGui::TableNextColumn();
                const std::string uri_display = truncateUtf8(r.uri, 57);
                ImGui::TextUnformatted(uri_display.c_str());
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("%s", r.uri.c_str());
                }
            }
        }
    }
    clipper.End();
    ImGui::EndTable();
}

void HunterGuiApp::renderSubscriptionsTab() {
    {
        const float add_w = ImGui::CalcTextSize("Add").x + ImGui::GetStyle().FramePadding.x * 4.0f;
        const float remark_w = std::min(200.0f * ui_scale_, ImGui::GetContentRegionAvail().x * 0.3f);
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - remark_w - add_w - ImGui::GetStyle().ItemSpacing.x * 2.0f);
        ImGui::InputTextWithHint("##new_sub_url", "Subscription URL", client_subscription_url_input_, sizeof(client_subscription_url_input_));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(remark_w);
        ImGui::InputTextWithHint("##new_sub_remark", "Remark", client_add_sub_remark_input_, sizeof(client_add_sub_remark_input_));
        ImGui::SameLine();
        if (ImGui::Button("Add", ImVec2(add_w, 0))) {
            std::string url = client_subscription_url_input_;
            std::string remark = client_add_sub_remark_input_;
            if (!url.empty()) {
                std::string id = orch_.subscriptionManager().add(url, remark);
                if (!id.empty()) {
                    showToast("Subscription added: " + url);
                    memset(client_subscription_url_input_, 0, sizeof(client_subscription_url_input_));
                    memset(client_add_sub_remark_input_, 0, sizeof(client_add_sub_remark_input_));
                } else {
                    showToast("Failed to add subscription (maybe duplicate)");
                }
            } else {
                showToast("URL cannot be empty");
            }
        }
        ImGui::Spacing();
        
        static ImGuiTableFlags sub_flags =
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable |
            ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollY;

        if (ImGui::BeginTable("subscriptions", 7, sub_flags, ImVec2(0, ImGui::GetContentRegionAvail().y))) {
            ImGui::TableSetupColumn("Enabled", ImGuiTableColumnFlags_NoSort | ImGuiTableColumnFlags_WidthFixed, 60 * ui_scale_);
            ImGui::TableSetupColumn("Remarks", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("URL", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Last update", ImGuiTableColumnFlags_WidthFixed, 120 * ui_scale_);
            ImGui::TableSetupColumn("Count", ImGuiTableColumnFlags_WidthFixed, 60 * ui_scale_);
            ImGui::TableSetupColumn("Error", ImGuiTableColumnFlags_WidthFixed, 200 * ui_scale_);
            ImGui::TableSetupColumn("Actions", ImGuiTableColumnFlags_NoSort | ImGuiTableColumnFlags_WidthFixed, 120 * ui_scale_);
            ImGui::TableHeadersRow();

            auto subs = orch_.subscriptionManager().list();
            for (const auto& sub : subs) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                bool enabled = sub.enabled;
                if (ImGui::Checkbox("##sub_enabled", &enabled)) {
                    if (enabled != sub.enabled) {
                        runBackground([this, id = sub.id, enabled]() {
                            orch_.subscriptionManager().setEnabled(id, enabled);
                            showToast(std::string("Subscription ") + (enabled ? "enabled" : "disabled"));
                        });
                    }
                }
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(sub.remarks.empty() ? "-" : sub.remarks.c_str());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(sub.url.c_str());
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("%s", sub.url.c_str());
                }
                ImGui::TableNextColumn();
                if (sub.last_update > 0) {
                    ImGui::TextUnformatted(formatAge(glfwGetTime(), sub.last_update).c_str());
                } else {
                    ImGui::TextDisabled("never");
                }
                ImGui::TableNextColumn();
                ImGui::Text("%d", sub.last_count);
                ImGui::TableNextColumn();
                if (!sub.last_error.empty()) {
                    ImGui::TextColored(kDangerRed, "%s", sub.last_error.c_str());
                } else {
                    ImGui::TextDisabled("-");
                }
                ImGui::TableNextColumn();
                if (ImGui::SmallButton("Update")) {
                    runBackground([this, id = sub.id]() {
                        std::string err;
                        int count = orch_.subscriptionManager().update(id, "", &err);
                        if (count >= 0) {
                            showToast("Subscription updated: " + std::to_string(count) + " configs");
                        } else {
                            showToast("Subscription update failed: " + err);
                        }
                    });
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("Delete")) {
                    runBackground([this, id = sub.id]() {
                        bool removed = orch_.subscriptionManager().remove(id);
                        if (removed) {
                            showToast("Subscription removed");
                        } else {
                            showToast("Failed to remove subscription");
                        }
                    });
                }
            }
            ImGui::EndTable();
        }

    }
}

void HunterGuiApp::refreshFoundCacheIfDue() {
    double now = glfwGetTime();
    if (last_found_refresh_ < 0.0 || now - last_found_refresh_ >= kFoundRefreshIntervalSeconds) {
        last_found_refresh_ = now;
        found_cache_ = orch_.foundHistory().snapshot();
        found_info_by_uri_.clear();
        for (const auto& row : snapshot_) {
            std::string country = row.rec.exit_country.empty() ? row.rec.server_country : row.rec.exit_country;
            found_info_by_uri_[row.rec.uri] = {countryCell(country), row.rec.latency_ms};
        }
    }
}

void HunterGuiApp::renderFoundTab() {
    {
        const double now = glfwGetTime();

        if (ImGui::Button("Copy All", ImVec2(120 * ui_scale_, 0))) {
            std::string text = buildFoundClipboardText();
            if (text.empty()) {
                showToast("No found configs yet");
            } else {
                ImGui::SetClipboardText(text.c_str());
                showToast("Copied " + std::to_string(std::count(text.begin(), text.end(), '\n')) + " found configs");
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Export", ImVec2(100 * ui_scale_, 0))) {
            exportFoundHistory();
        }
        ImGui::SameLine();
        ImGui::TextDisabled("%d found", (int)found_cache_.size());

        ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable;
        if (ImGui::BeginTable("found", 3, flags, ImVec2(0, ImGui::GetContentRegionAvail().y))) {
            ImGui::TableSetupColumn("When", ImGuiTableColumnFlags_WidthFixed, 100 * ui_scale_);
            ImGui::TableSetupColumn("Country", ImGuiTableColumnFlags_WidthFixed, 140 * ui_scale_);
            ImGui::TableSetupColumn("URI", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableHeadersRow();
            for (const auto& e : found_cache_) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(formatAge(now, e.seen_at).c_str());
                ImGui::TableNextColumn();
                auto it = found_info_by_uri_.find(e.uri);
                if (it != found_info_by_uri_.end()) {
                    ImGui::TextUnformatted(it->second.first.c_str());
                    if (ImGui::IsItemHovered() && it->second.second > 0) {
                        ImGui::SetTooltip("Latency: %.0f ms", it->second.second);
                    }
                } else {
                    ImGui::TextDisabled("(removed)");
                }
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(e.uri.c_str());
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("%s", e.uri.c_str());
                }
            }
            ImGui::EndTable();
        }
    }
}

void HunterGuiApp::renderFrame() {
     refreshSnapshotIfDue();
     refreshDpiScale();

     if (!theme_applied_) applyModernTheme();

     ImGuiViewport* viewport = ImGui::GetMainViewport();
     viewport_width_ = viewport->WorkSize.x;

     ImGui::SetNextWindowPos(viewport->WorkPos);
     ImGui::SetNextWindowSize(viewport->WorkSize);
     ImGui::Begin("HunterMain", nullptr,
                  ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                  ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                  ImGuiWindowFlags_NoBringToFrontOnFocus |
                  ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

     // ─── Top bar ───
     renderTopBar();

     // ─── View content ───
     if (currentView_ == View::Main) {
         renderMainView();
     } else {
         renderAboutView();
     }

     // ─── Popups ───
     renderQrPopup();
     renderClearLivePopup();

     // ─── Toast ───
     {
         std::lock_guard<std::mutex> lock(toast_mu_);
         if (!toast_message_.empty() && glfwGetTime() < toast_until_) {
             ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + Spacing::Large,
                                            viewport->WorkPos.y + viewport->WorkSize.y - 80));
             ImGui::SetNextWindowBgAlpha(0.9f);
             ImGui::Begin("##toast", nullptr,
                          ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                          ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize |
                          ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing);
             ImGui::TextColored(kSuccessGreen, "%s", toast_message_.c_str());
             ImGui::End();
         }
     }

     // ─── Shutdown Overlay ───
     if (shutdown_in_progress_.load()) {
         ImGui::OpenPopup("ShutdownOverlay");
     }
     ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
     ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize, ImGuiCond_Always);
     if (ImGui::BeginPopupModal("ShutdownOverlay", nullptr, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize)) {
         ImGui::BeginChild("##shutdown_child", ImVec2(0, 0), true);
         ImGui::Text("Shutting down...");
         ImGui::ProgressBar(orch_.shutdownFraction(), ImVec2(-1, 0), orch_.shutdownLabel().c_str());
         ImGui::EndChild();
         ImGui::EndPopup();
     }

     ImGui::End();
 }

void HunterGuiApp::renderAboutView() {
    ImGui::TextColored(kAccentBlue, "Hunter");
    ImGui::SameLine();
    ImGui::TextDisabled("Censorship Hunter — Autonomous Proxy Discovery Engine");
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::Text("Version: v%s", constants::HUNTER_VERSION);
    ImGui::TextDisabled("GitHub: https://github.com/bahmany/censorship_hunter");
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    renderUpdatePanel();
}

void HunterGuiApp::renderMainView() {
    refreshFoundCacheIfDue();
    auto client_status = orch_.clientStatus();
    bool active = client_status.active;
    bool hunting = orchestrator_running_.load();

    // Stat cards
    renderStatCards();

    // Action bar
    // Secondary (outlined-looking) buttons; Connect re-pushes the accent.
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.16f, 0.18f, 0.23f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.22f, 0.25f, 0.32f, 1.0f));
    renderActionBar();
    ImGui::PopStyleColor(2);
    ImGui::Spacing();

    // One body region, three lists. Only the active list scrolls; the page never does.
    if (ImGui::BeginTabBar("##body_tabs")) {
        char live_label[48], subs_label[48], found_label[48];
        std::snprintf(live_label, sizeof(live_label), "Live configs (%d)###tab_live", cached_alive_);
        std::snprintf(subs_label, sizeof(subs_label), "Subscriptions (%d)###tab_subs",
                      (int)orch_.subscriptionManager().list().size());
        std::snprintf(found_label, sizeof(found_label), "Found (%d)###tab_found", (int)found_cache_.size());
        if (ImGui::BeginTabItem(live_label)) {
            renderConfigTable();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(subs_label)) {
            renderSubscriptionsTab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(found_label)) {
            renderFoundTab();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
}

#ifndef __ANDROID__
namespace {

// GLFW reports the real OS-level reason for a failure (e.g. "WGL: The driver
// does not appear to support OpenGL") only through this callback — glfwInit()
// and glfwCreateWindow() just return a bare bool/nullptr with no detail. On a
// WIN32-subsystem build there is no console for fprintf(stderr, ...) to reach
// either, so without this a startup failure here is completely invisible:
// the process exits in the same instant it started, leaving no trace.
std::string g_lastGlfwError;

void glfwErrorCallback(int code, const char* description) {
    g_lastGlfwError = "GLFW error " + std::to_string(code) + ": " +
                       (description ? description : "(no description)");
}

// Writes to the same hunter_startup.log main.cpp already opens (this class
// lives in a different translation unit, so it reopens in append mode rather
// than sharing the FILE*).
std::string startupLogPath() {
    std::string path = "runtime/hunter_startup.log";
#ifdef _WIN32
    char exePath[MAX_PATH];
    if (GetModuleFileNameA(nullptr, exePath, MAX_PATH)) {
        path = (std::filesystem::path(exePath).parent_path() / "hunter_startup.log").string();
    }
#endif
    return path;
}

void appendToStartupLog(const std::string& line) {
    if (FILE* f = std::fopen(startupLogPath().c_str(), "a")) {
        std::time_t now = std::time(nullptr);
        std::fprintf(f, "[%s] %s\n", std::ctime(&now), line.c_str());
        std::fflush(f);
        std::fclose(f);
    }
}

// Shows a MessageBoxA — the only way a WIN32-subsystem app with no console
// can surface a startup failure to the user.
void reportGuiStartupFailure(const std::string& what) {
    appendToStartupLog("GUI STARTUP FAILURE: " + what);
#ifdef _WIN32
    MessageBoxA(nullptr, what.c_str(), "Hunter failed to start", MB_OK | MB_ICONERROR);
#else
    std::fprintf(stderr, "[gui] %s\n", what.c_str());
#endif
}

#ifdef _WIN32
// Some real Windows machines have no working hardware OpenGL driver at all
// (VMs with a 2D-only display adapter, some RDP sessions, old GPUs) — GLFW
// fails outright with a WGL error and the window can never open. If a
// software-GL fallback is embedded in this binary (see gl_fallback_embed.h),
// extract it next to the exe and relaunch once with the non-JIT `softpipe`
// Gallium driver forced. (Mesa's default llvmpipe driver JIT-compiles shaders
// via LLVM, which needs RWX memory — hardened endpoint security on some
// machines kills that outright as a shellcode-injection heuristic; softpipe
// is a pure interpreter and doesn't trip that.) Returns true if a relaunch
// was started (caller should exit immediately without further UI); false if
// there's nothing to try (no embedded fallback, or already tried once).
bool attemptGlFallbackRelaunch() {
    if (hunter::embed::glFallbackAlreadyPresent()) {
        appendToStartupLog("GL fallback already present next to the exe and hardware OpenGL still "
                            "failed — not relaunching again, reporting failure.");
        return false;
    }
    if (!hunter::embed::hasEmbeddedGlFallback()) {
        return false;
    }
    appendToStartupLog("Hardware OpenGL unavailable; extracting embedded software-GL fallback "
                        "(Mesa opengl32.dll + libgallium_wgl.dll) and relaunching...");
    if (!hunter::embed::ensureGlFallbackExtracted()) {
        appendToStartupLog("GL fallback extraction failed.");
        return false;
    }
    SetEnvironmentVariableA("GALLIUM_DRIVER", "softpipe");
    SetEnvironmentVariableA("LIBGL_ALWAYS_SOFTWARE", "1");

    char exePath[MAX_PATH];
    if (!GetModuleFileNameA(nullptr, exePath, MAX_PATH)) {
        appendToStartupLog("GetModuleFileNameA failed; cannot relaunch.");
        return false;
    }
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    // exePath is passed as both the module and the (quoted) command line —
    // CreateProcessA needs a mutable command-line buffer, hence the copy.
    std::string cmdLine = "\"" + std::string(exePath) + "\"";
    std::vector<char> cmdLineBuf(cmdLine.begin(), cmdLine.end());
    cmdLineBuf.push_back('\0');
    BOOL ok = CreateProcessA(exePath, cmdLineBuf.data(), nullptr, nullptr, FALSE,
                              0, nullptr, nullptr, &si, &pi);
    if (!ok) {
        appendToStartupLog("CreateProcessA relaunch failed, GetLastError=" + std::to_string(GetLastError()));
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    appendToStartupLog("Relaunched with GL fallback (PID " + std::to_string(pi.dwProcessId) + ").");
    return true;
}
#endif

} // namespace

int HunterGuiApp::run() {
    // Enable DPI awareness on Windows (fallback to classic DPI awareness if needed)
    #ifdef _WIN32
    SetProcessDPIAware(); // Must be called before any window is created
    #endif
    glfwSetErrorCallback(glfwErrorCallback);
    if (!glfwInit()) {
#ifdef _WIN32
        if (attemptGlFallbackRelaunch()) return 0;
#endif
        reportGuiStartupFailure("glfwInit() failed: " + g_lastGlfwError);
        return 1;
    }
    glfwWindowHint(GLFW_VISIBLE, GLFW_TRUE);
    window_ = glfwCreateWindow(1280, 800, "Hunter", nullptr, nullptr);
    if (!window_) {
        glfwTerminate();
#ifdef _WIN32
        if (attemptGlFallbackRelaunch()) return 0;
#endif
        reportGuiStartupFailure("glfwCreateWindow() failed: " + g_lastGlfwError);
        return 1;
    }

    // Set the window icon (Linux/macOS — on Windows it comes from resources.rc).
#ifndef _WIN32
    setWindowIcon(window_);
#endif

    glfwMakeContextCurrent(window_);

    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
#ifdef IMGUI_ENABLE_TEST_ENGINE
    ImGui::GetCurrentContext()->TestEngineHookItems = true;  // UI probe build only
#endif
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    // ------------------------------------------------------------
    // DPI scaling (desktop platforms only)
    // ------------------------------------------------------------
    // Style metrics are scaled inside applyModernTheme() — it writes base
    // design-pixel values first, then applies ScaleAllSizes(ui_scale_) on
    // top, so re-applying after a DPI change never compounds the factor.
    #ifndef __ANDROID__
    float scaleX = 1.0f, scaleY = 1.0f;
    glfwGetWindowContentScale(window_, &scaleX, &scaleY);
    ui_scale_ = (scaleX + scaleY) * 0.5f; // average for non-uniform scaling
    if (ui_scale_ <= 0.0f) ui_scale_ = 1.0f;
    #endif
    loadUiFonts(kUiFontSizePx);

    // Apply modern dark theme (will also be called in renderFrame, but
    // applying here ensures the first frame looks right)
    applyModernTheme();

    ImGui_ImplGlfw_InitForOpenGL(window_, true);
    ImGui_ImplOpenGL2_Init();

    // Start the orchestrator on a background thread. Log messages are
    // pushed to utils::LogRingBuffer by the orchestrator workers — the
    // GUI reads from that buffer directly (no std::cout capture needed).
    startOrchestrator();

    // Start the auto-update checker (background thread, fully automatic).
    // Checks GitHub releases 10s after startup, then every 6 hours.
    core::SelfUpdateManager::instance().startAutoCheck(21600, 10);

     while (!glfwWindowShouldClose(window_)) {
         glfwPollEvents();
 
         // A second launch of Hunter exits immediately after leaving a request
         // here; surface this window instead of letting the user think nothing
         // happened. Throttled — this touches the filesystem.
         if (show_request_poll_) {
             double now = glfwGetTime();
             if (last_show_poll_ < 0.0 || now - last_show_poll_ >= kShowPollIntervalSeconds) {
                 last_show_poll_ = now;
                 if (show_request_poll_()) {
                     glfwShowWindow(window_);
                     glfwRestoreWindow(window_);   // un-minimize
                     glfwFocusWindow(window_);
                     glfwRequestWindowAttention(window_);
                 }
             }
         }
 
         ImGui_ImplOpenGL2_NewFrame();
         ImGui_ImplGlfw_NewFrame();
         ImGui::NewFrame();
 
         renderFrame();
 
         ImGui::Render();
         int display_w, display_h;
         glfwGetFramebufferSize(window_, &display_w, &display_h);
         glViewport(0, 0, display_w, display_h);
         glClearColor(0.06f, 0.07f, 0.09f, 1.0f);
         glClear(GL_COLOR_BUFFER_BIT);
         ImGui_ImplOpenGL2_RenderDrawData(ImGui::GetDrawData());
 
         glfwSwapBuffers(window_);
     }
 
      // Instead of blocking stop, request shutdown and wait for it to finish with overlay
      requestShutdown();
      auto shutdown_start = std::chrono::steady_clock::now();
      while (shutdown_in_progress_.load()) {
          // Render frames to show the overlay
          glfwPollEvents();
          ImGui_ImplOpenGL2_NewFrame();
          ImGui_ImplGlfw_NewFrame();
          ImGui::NewFrame();
 
          renderFrame();
 
          ImGui::Render();
          int display_w, display_h;
          glfwGetFramebufferSize(window_, &display_w, &display_h);
          glViewport(0, 0, display_w, display_h);
          glClearColor(0.06f, 0.07f, 0.09f, 1.0f);
          glClear(GL_COLOR_BUFFER_BIT);
          ImGui_ImplOpenGL2_RenderDrawData(ImGui::GetDrawData());
 
          glfwSwapBuffers(window_);
 
          // Hard cap: if shutdown takes too long, force exit
          if (std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - shutdown_start).count() >= 8) {
              utils::LogRingBuffer::instance().push("shutdown timed out, forcing exit");
              break;
          }
          std::this_thread::sleep_for(std::chrono::milliseconds(16)); // ~60 FPS
      }
  
      // Note: orchestrator stop already stopped the updater (we moved it into orchestrator stop)
      // So no need to call stopAutoCheck() here.
  
      ImGui_ImplOpenGL2_Shutdown();
      ImGui_ImplGlfw_Shutdown();
      ImGui::DestroyContext();
  
      glfwDestroyWindow(window_);
      glfwTerminate();
      return 0;
  }

 #endif // __ANDROID__

} // namespace gui
} // namespace hunter
