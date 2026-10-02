#include "gui/app.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>

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

namespace hunter {
namespace gui {

namespace {

// Load the application icon from hunter_icon.png and set it as the GLFW
// window icon. On Linux this makes the icon appear in the taskbar, window
// decoration, and alt-tab preview. The PNG is copied next to the binary
// at build time (see CMakeLists.txt).
//
// We provide multiple sizes (16, 32, 48, 64, 128, 256) so the window
// manager can pick the best fit for each context.
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
        case HealthState::Healthy: return ImVec4(0.30f, 0.80f, 0.40f, 1.0f);
        case HealthState::Degraded: return ImVec4(0.90f, 0.78f, 0.25f, 1.0f);
        case HealthState::Unstable: return ImVec4(0.95f, 0.55f, 0.20f, 1.0f);
        case HealthState::Dead: return ImVec4(0.85f, 0.35f, 0.35f, 1.0f);
        case HealthState::Testing: return ImVec4(0.45f, 0.65f, 0.95f, 1.0f);
        default: return ImVec4(0.55f, 0.55f, 0.55f, 1.0f);
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
    if (std::strcmp(label, "Stable") == 0) return ImVec4(0.30f, 0.85f, 0.45f, 1.0f);
    if (std::strcmp(label, "Provisional") == 0) return ImVec4(0.55f, 0.80f, 0.90f, 1.0f);
    if (std::strcmp(label, "Unstable") == 0) return ImVec4(0.95f, 0.55f, 0.20f, 1.0f);
    if (std::strcmp(label, "Dead") == 0) return ImVec4(0.85f, 0.35f, 0.35f, 1.0f);
    return ImVec4(0.55f, 0.55f, 0.55f, 1.0f);
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

} // namespace

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
    stopSnapshotWorker();   // also stops the country + baseline workers
    if (auto* db = orch_.configDb()) db->setBatchPrioritizer(nullptr);
    stopOrchestrator();
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
    if (orchestrator_running_.load()) return;
    transitioning_ = true;
    orchestrator_running_ = true;
    orchestrator_thread_ = std::thread([this]() {
        try {
            orch_.start();
        } catch (const std::exception& e) {
            std::fprintf(stderr, "[gui] orchestrator error: %s\n", e.what());
        }
        orchestrator_running_ = false;
    });
    transitioning_ = false;
}

void HunterGuiApp::stopOrchestrator() {
    if (!orchestrator_running_.load()) {
        if (orchestrator_thread_.joinable()) orchestrator_thread_.join();
        return;
    }
    transitioning_ = true;
    orch_.stop();
    if (orchestrator_thread_.joinable()) orchestrator_thread_.join();
    transitioning_ = false;
}

void HunterGuiApp::requestClose() {
#ifndef __ANDROID__
    if (window_) glfwSetWindowShouldClose(window_, GLFW_TRUE);
#endif
}

void HunterGuiApp::setShowRequestPoll(std::function<bool()> poll) {
    show_request_poll_ = std::move(poll);
}

void HunterGuiApp::triggerRescan() {
    if (rescan_in_progress_.exchange(true)) return;
    std::thread([this]() {
        try {
            orch_.runCycle();
        } catch (...) {}
        rescan_in_progress_ = false;
    }).detach();
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
                    row.ps = std::get<2>(pc->second);
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
    std::string out;
    for (const auto& row : snapshot_) {
        const auto& r = row.rec;
        if (!r.alive) continue;
        if (selected_only && selected_uris_.find(r.uri) == selected_uris_.end()) continue;
        out += r.uri;
        out += '\n';
    }
    return out;
}

std::string HunterGuiApp::buildGeminiOkClipboardText() const {
    std::string out;
    for (const auto& row : snapshot_) {
        const auto& r = row.rec;
        if (!r.alive) continue;
        if (r.gemini_status != 1) continue;  // only Gemini-accessible
        out += r.uri;
        out += '\n';
    }
    return out;
}

std::string HunterGuiApp::buildExportText() const {
    // Export includes a header comment + the visible alive config URIs, one per line.
    // This is suitable for importing into v2rayN, v2rayNG, Clash, etc.
    std::time_t now = std::time(nullptr);
    char timebuf[64];
    std::strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S", std::gmtime(&now));

    std::string out;
    out += "# Hunter live configs export\n";
    out += "# Generated: ";
    out += timebuf;
    out += "\n# Alive count: ";
    out += std::to_string(cached_alive_);
    out += "\n\n";
    for (const auto& row : snapshot_) {
        if (!row.rec.alive) continue;
        out += row.rec.uri;
        out += '\n';
    }
    return out;
}

void HunterGuiApp::showToast(const std::string& msg) {
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

void HunterGuiApp::renderHeader() {
    ImGui::Text("Hunter");
    ImGui::SameLine();
    ImGui::TextDisabled("- live v2ray configs");
    ImGui::Separator();

    // Uses cached stats from refreshSnapshotIfDue() — NOT a live getStats() call, which is O(N)
    // over the full DB under the mutex and would stall the UI if invoked every frame.
    ImGui::Text("Total: %d", cached_total_);
    ImGui::SameLine(0, 24);
    ImGui::TextColored(healthColor(HealthState::Healthy), "Alive: %d", cached_alive_);
    ImGui::SameLine(0, 24);
    ImGui::TextColored(stabilityColor("Stable"), "Stable: %d", cached_stable_);
    ImGui::SameLine(0, 24);
    ImGui::TextColored(healthColor(HealthState::Dead), "Dead: %d", cached_dead_);
    ImGui::SameLine(0, 24);
    ImGui::Text("Tested: %d / %d", cached_tested_, cached_total_);
    ImGui::SameLine(0, 24);
    ImGui::Text("Avg latency: %.0f ms", cached_avg_latency_ms_);
    ImGui::SameLine(0, 24);
    ImGui::Text("Cycle: %d", orch_.cycleCount());

    if (!orchestrator_running_.load()) {
        ImGui::SameLine(0, 24);
        ImGui::TextColored(ImVec4(0.85f, 0.35f, 0.35f, 1.0f), "STOPPED");
    }

    // Local connectivity overlay: does NOT change any server's stored health (D1); it only explains
    // why probes are being excluded / not penalized right now.
    ImGui::SameLine(0, 24);
    switch (baseline_state_.load()) {
        case 1: ImGui::TextColored(ImVec4(0.30f, 0.80f, 0.40f, 1.0f), "Local network: online"); break;
        case 2: ImGui::TextColored(ImVec4(0.85f, 0.35f, 0.35f, 1.0f), "Local network: OFFLINE (penalties suspended)"); break;
        case 3: ImGui::TextColored(ImVec4(0.90f, 0.78f, 0.25f, 1.0f), "Local connectivity unconfirmed"); break;
        case 4: ImGui::TextDisabled("Direct baseline off"); break;
        default: ImGui::TextDisabled("Local network: idle"); break;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Direct (non-proxied) baseline. Offline/unconfirmed rounds never mark servers dead.\n"
                          "Disable under Configs > 'Direct baseline probes' for privacy.");
    }
}

void HunterGuiApp::renderControls() {
    bool busy = transitioning_.load();

    ImGui::BeginDisabled(busy || orchestrator_running_.load());
    if (ImGui::Button("Start")) startOrchestrator();
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::BeginDisabled(busy || !orchestrator_running_.load());
    if (ImGui::Button("Stop")) {
        std::thread([this]() { stopOrchestrator(); }).detach();
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::BeginDisabled(rescan_in_progress_.load());
    if (ImGui::Button(rescan_in_progress_.load() ? "Rescanning..." : "Rescan now")) {
        triggerRescan();
    }
    ImGui::EndDisabled();

    ImGui::SameLine(0, 24);
    ImGui::TextDisabled("%d shown / %d match", (int)snapshot_.size(), cached_matched_);

    // ─── Copy / Export ───
    ImGui::SameLine(0, 16);
    if (ImGui::Button("Copy Live")) {
        std::string text = buildClipboardText(false);
        ImGui::SetClipboardText(text.c_str());
        showToast("Copied " + std::to_string(std::count(text.begin(), text.end(), '\n')) + " live configs to clipboard");
    }
    ImGui::SameLine();
    if (ImGui::Button("Copy Gemini OK")) {
        std::string text = buildGeminiOkClipboardText();
        ImGui::SetClipboardText(text.c_str());
        int count = text.empty() ? 0 : std::count(text.begin(), text.end(), '\n');
        showToast("Copied " + std::to_string(count) + " Gemini OK configs to clipboard");
    }
    ImGui::SameLine();
    if (ImGui::Button("Export Live")) {
        exportToFile(buildExportText(), "hunter_live_configs");
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(cached_alive_ == 0);
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.20f, 0.20f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.70f, 0.25f, 0.25f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.45f, 0.15f, 0.15f, 1.0f));
    if (ImGui::Button("Clear All Live")) {
        show_clear_live_popup_ = true;
    }
    ImGui::PopStyleColor(3);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("Drop every live config from the database and the live cache.");
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(selected_uris_.empty());
    if (ImGui::Button("Copy Selected")) {
        std::string text = buildClipboardText(true);
        ImGui::SetClipboardText(text.c_str());
        showToast("Copied " + std::to_string(selected_uris_.size()) + " selected configs to clipboard");
    }
    ImGui::EndDisabled();

    // ─── QR code for selected ───
    ImGui::SameLine();
    ImGui::BeginDisabled(selected_uris_.empty() || selected_uris_.size() > 1);
    if (ImGui::Button("QR Code")) {
        qr_text_ = *selected_uris_.begin();
        show_qr_popup_ = true;
    }
    ImGui::EndDisabled();

    if (!toast_message_.empty() && glfwGetTime() < toast_until_) {
        ImGui::SameLine(0, 16);
        ImGui::TextColored(ImVec4(0.4f, 0.85f, 0.4f, 1.0f), "%s", toast_message_.c_str());
    }

    renderFilterBar();
}

void HunterGuiApp::renderFilterBar() {
    // View: live only vs every record (Unknown/Unrated/Dead included).
    bool changed = false;
    ImGui::SetNextItemWidth(130);
    const char* views[] = {"Live configs", "All configs"};
    if (ImGui::Combo("##view", &view_all_ui_, views, 2)) changed = true;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("All configs includes Unknown, Unrated and Dead records");

    // Country mode: which column the country filter and bare country searches apply to.
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120);
    const char* modes[] = {"Any country", "Exit", "Server"};
    if (ImGui::Combo("##cmode", &country_mode_ui_, modes, 3)) changed = true;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Exit = measured through the tunnel. Server = offline IP-database hint (not an exit guarantee).");

    // Country choice: Any / Unknown / Mixed / each country present in the database.
    ImGui::SameLine();
    ImGui::SetNextItemWidth(190);
    std::string preview = country_choice_ui_.empty() ? "All countries"
                          : country_choice_ui_ == "?" ? "Unknown"
                          : country_choice_ui_ == "~" ? "Mixed" : countryCell(country_choice_ui_);
    if (ImGui::BeginCombo("##ccountry", preview.c_str())) {
        if (ImGui::Selectable("All countries", country_choice_ui_.empty())) { country_choice_ui_.clear(); changed = true; }
        if (ImGui::Selectable("Unknown", country_choice_ui_ == "?")) { country_choice_ui_ = "?"; changed = true; }
        if (country_mode_ui_ != 1 && ImGui::Selectable("Mixed (server hints disagree)", country_choice_ui_ == "~")) {
            country_choice_ui_ = "~"; changed = true;
        }
        std::map<std::string, int> merged;
        if (country_mode_ui_ != 2) for (auto& kv : cached_exit_counts_) merged[kv.first] += kv.second;
        if (country_mode_ui_ != 1) for (auto& kv : cached_server_counts_) if (kv.first != geo::kMixed) merged[kv.first] += kv.second;
        for (auto& kv : merged) {
            std::string label = countryCell(kv.first) + "  (" + std::to_string(kv.second) + ")";
            if (ImGui::Selectable(label.c_str(), country_choice_ui_ == kv.first)) { country_choice_ui_ = kv.first; changed = true; }
        }
        ImGui::EndCombo();
    }

    ImGui::SameLine();
    ImGui::SetNextItemWidth(260);
    if (ImGui::InputTextWithHint("##filter", "search: text, DE, germany, exit:DE, server:US", filter_text_, sizeof(filter_text_)))
        changed = true;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Terms are AND-combined. exit:DE / server:US restrict one column;\n"
                          "values may be an ISO code, a country name, 'unknown' or (server only) 'mixed'.");

    if (changed) {
        {
            std::lock_guard<std::mutex> lk(view_mutex_);
            view_spec_.all_view = view_all_ui_ == 1;
            view_spec_.mode = (geo::CountryMode)country_mode_ui_;
            view_spec_.choice = geo::CountryChoice();
            if (country_choice_ui_ == "?") view_spec_.choice.kind = geo::CountryChoice::Unknown;
            else if (country_choice_ui_ == "~") view_spec_.choice.kind = geo::CountryChoice::Mixed;
            else if (!country_choice_ui_.empty()) { view_spec_.choice.kind = geo::CountryChoice::Specific; view_spec_.choice.iso = country_choice_ui_; }
            view_spec_.query = filter_text_;
        }
        markViewDirty();
    }

    // Country-targeted discovery + strict exit target (persisted; consumed by failover).
    ImGui::SameLine(0, 20);
    ImGui::TextDisabled("Exit target:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(40);
    bool tgt_changed = ImGui::InputText("##target", target_input_, sizeof(target_input_),
                                        ImGuiInputTextFlags_CharsUppercase | ImGuiInputTextFlags_CharsNoBlank |
                                        ImGuiInputTextFlags_EnterReturnsTrue);
    if (ImGui::IsItemDeactivatedAfterEdit()) tgt_changed = true;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Two-letter country (e.g. DE). Discovery tests fresh matching exits first, then server hints,\n"
                          "then Unknown (at least 20%% exploration). Empty = no target.");
    ImGui::SameLine();
    if (ImGui::Checkbox("Strict", &target_strict_ui_)) tgt_changed = true;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Strict: recommendations and failover may only use a FRESH MEASURED exit in the target.\n"
                          "If none exists: 'No verified exit in target'. Never silently switches outside it.");
    if (tgt_changed) {
        std::string iso = target_input_;
        if (!iso.empty() && !geo::isValidIso(iso)) {
            showToast("Unknown country code: " + iso);
            std::string cur = orch_.config().exitCountryTarget();
            std::snprintf(target_input_, sizeof(target_input_), "%s", cur.c_str());
        } else {
            applyExitTarget(iso, target_strict_ui_);
            saveConfigNow();
            showToast(iso.empty() ? "Exit target cleared" : "Exit target: " + countryCell(iso));
        }
    }
    if (!orch_.config().exitCountryTarget().empty() && target_strict_ui_) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.90f, 0.78f, 0.25f, 1.0f), "strict %s", orch_.config().exitCountryTarget().c_str());
    }

    // M5 direct-baseline toggle (privacy): when off, attribution uses control tunnels only.
    ImGui::SameLine(0, 20);
    if (ImGui::Checkbox("Direct baseline probes", &baseline_enabled_ui_)) {
        network::ConnectivityBaseline::shared()->setEnabled(baseline_enabled_ui_);
        orch_.config().set("direct_baseline_enabled", baseline_enabled_ui_);
        saveConfigNow();
        if (!baseline_enabled_ui_) baseline_state_ = 4;
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Direct (non-proxied) connectivity checks to tell local outages from dead servers.\n"
                          "Off: only already-confirmed control tunnels attribute failures, otherwise rounds are 'Indeterminate'.");
}

void HunterGuiApp::renderTable() {
    static ImGuiTableFlags flags =
        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable |
        ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX | ImGuiTableFlags_Sortable |
        ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ContextMenuInBody |
        ImGuiTableFlags_Hideable | ImGuiTableFlags_Reorderable;

    // Leave space at the bottom for the log panel + status bar.
    // Tab bar (~35px) + status bar (~40px) + log panel (~180px) + spacing
    float table_height = ImGui::GetContentRegionAvail().y - 260.0f;
    if (table_height < 100.0f) table_height = 100.0f;

    if (!ImGui::BeginTable("configs", 14, flags, ImVec2(0, table_height))) return;

    ImGui::TableSetupColumn("Health", ImGuiTableColumnFlags_WidthFixed, 72.0f);
    ImGui::TableSetupColumn("Stability", ImGuiTableColumnFlags_WidthFixed, 84.0f);
    ImGui::TableSetupColumn("Score", ImGuiTableColumnFlags_WidthFixed |
                                     ImGuiTableColumnFlags_PreferSortDescending, 52.0f);
    ImGui::TableSetupColumn("p90", ImGuiTableColumnFlags_WidthFixed, 66.0f);
    ImGui::TableSetupColumn("Latency", ImGuiTableColumnFlags_WidthFixed, 70.0f);
    ImGui::TableSetupColumn("Exit country", ImGuiTableColumnFlags_WidthFixed, 130.0f);
    ImGui::TableSetupColumn("Server country", ImGuiTableColumnFlags_WidthFixed, 130.0f);
    ImGui::TableSetupColumn("Protocol", ImGuiTableColumnFlags_WidthFixed, 72.0f);
    ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthFixed, 280.0f);
    ImGui::TableSetupColumn("Engine", ImGuiTableColumnFlags_WidthFixed, 70.0f);
    ImGui::TableSetupColumn("Last tested", ImGuiTableColumnFlags_WidthFixed, 90.0f);
    ImGui::TableSetupColumn("Source", ImGuiTableColumnFlags_WidthFixed, 80.0f);
    ImGui::TableSetupColumn("Gemini", ImGuiTableColumnFlags_WidthFixed, 60.0f);
    ImGui::TableSetupColumn("Proxy", ImGuiTableColumnFlags_WidthFixed, 140.0f);
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableHeadersRow();

    int sort_col = -1;
    bool sort_asc = true;
    if (ImGuiTableSortSpecs* sort_specs = ImGui::TableGetSortSpecs()) {
        if (sort_specs->SpecsCount > 0) {
            sort_col = sort_specs->Specs[0].ColumnIndex;
            sort_asc = sort_specs->Specs[0].SortDirection == ImGuiSortDirection_Ascending;
        }
    }
    rebuildVisibleRowsIfDirty(sort_col, sort_asc);
    const auto& rows = visible_rows_;

    const double now = utils::nowTimestamp();
    const double exit_fresh_s = (double)std::max(60, orch_.config().exitCountryFreshSeconds());
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(rows.size()));
    while (clipper.Step()) {
        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
            const UiRow& row = *rows[i];
            const ConfigHealthRecord& r = row.rec;
            const HealthEvaluation& ev = row.ev;
            bool selected = selected_uris_.count(r.uri) != 0;

            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::PushID(r.uri.c_str());
            ImGuiSelectableFlags sel_flags = ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap;
            if (ImGui::Selectable("##row", selected, sel_flags)) {
                ImGuiIO& io = ImGui::GetIO();
                if (io.KeyCtrl) {
                    if (selected) selected_uris_.erase(r.uri); else selected_uris_.insert(r.uri);
                } else if (io.KeyShift && !last_clicked_uri_.empty()) {
                    auto it_a = std::find_if(rows.begin(), rows.end(), [&](auto* p) { return p->rec.uri == last_clicked_uri_; });
                    auto it_b = rows.begin() + i;
                    if (it_a != rows.end()) {
                        auto [lo, hi] = std::minmax(it_a, it_b);
                        for (auto it = lo; it <= hi; ++it) selected_uris_.insert((*it)->rec.uri);
                    }
                } else {
                    selected_uris_.clear();
                    selected_uris_.insert(r.uri);
                }
                last_clicked_uri_ = r.uri;
            }
            ImGui::SameLine();
            ImGui::TextColored(healthColor(r.ev.state), "%s", healthLabel(r.ev.state));
            if (ImGui::IsItemHovered()) {
                ImGui::BeginTooltip();
                ImGui::Text("State: %s   Last outcome: %s", healthLabel(r.ev.state),
                            r.ev.last_outcome.empty() ? "-" : r.ev.last_outcome.c_str());
                ImGui::Text("Failure streak: %u   Success streak: %u   Eligible rounds: %u",
                            r.ev.failure_streak, r.ev.success_streak, r.ev.eligible_count);
                ImGui::Text("EWMA success: %.2f   Freshness: %.2f   Protocol factor: %.1f",
                            ev.ewma, ev.freshness, ev.protocol_factor);
                if (!r.ev.session_confirmed) ImGui::TextDisabled("Not yet re-confirmed in this session");
                ImGui::EndTooltip();
            }
            ImGui::PopID();

            // ─── Right-click context menu per row ───
            if (ImGui::BeginPopupContextItem(("ctx_" + r.uri).c_str())) {
                if (ImGui::MenuItem("Copy URI")) {
                    ImGui::SetClipboardText(r.uri.c_str());
                    showToast("Copied URI to clipboard");
                }
                if (ImGui::MenuItem("Show QR Code")) {
                    qr_text_ = r.uri;
                    show_qr_popup_ = true;
                }
                ImGui::EndPopup();
            }

            ImGui::TableNextColumn();
            const char* stab = stabilityLabel(r, ev);
            ImGui::TextColored(stabilityColor(stab), "%s", stab);

            ImGui::TableNextColumn();
            if (r.ev.eligible_count > 0) ImGui::Text("%.0f", ev.score); else ImGui::TextDisabled("-");

            ImGui::TableNextColumn();
            if (ev.has_p90) ImGui::Text("%.0f ms", ev.p90_ms); else ImGui::TextDisabled("-");

            ImGui::TableNextColumn();
            if (r.alive && r.latency_ms > 0) ImGui::Text("%.0f ms", r.latency_ms);
            else ImGui::TextUnformatted("-");

            // ─── Exit country (measured through the tunnel) ───
            ImGui::TableNextColumn();
            {
                const bool known = !r.exit_country.empty();
                const bool stale = known && (now - r.exit_country_at) > exit_fresh_s;
                if (!known) ImGui::TextDisabled("Unknown");
                else if (stale) ImGui::TextColored(ImVec4(0.65f, 0.65f, 0.55f, 1.0f), "%s ~", countryCell(r.exit_country).c_str());
                else ImGui::TextUnformatted(countryCell(r.exit_country).c_str());
                if (ImGui::IsItemHovered()) {
                    ImGui::BeginTooltip();
                    ImGui::Text("Exit country: %s", countryCell(r.exit_country).c_str());
                    ImGui::Text("Source: %s", sourceDescription(r.exit_country_source));
                    if (!r.exit_ip.empty()) ImGui::Text("Exit IP: %s", r.exit_ip.c_str());
                    ImGui::Text("Measured: %s%s", formatAge(now, r.exit_country_at).c_str(),
                                stale ? "  (STALE - unconfirmed for strict targets)" : "");
                    ImGui::EndTooltip();
                }
            }

            // ─── Server country (offline DB hint; not an exit guarantee) ───
            ImGui::TableNextColumn();
            {
                const bool known = !r.server_country.empty();
                if (!known) ImGui::TextDisabled("Unknown");
                else if (r.server_country == geo::kMixed) ImGui::TextColored(ImVec4(0.90f, 0.78f, 0.25f, 1.0f), "Mixed");
                else ImGui::TextUnformatted(countryCell(r.server_country).c_str());
                if (ImGui::IsItemHovered()) {
                    ImGui::BeginTooltip();
                    ImGui::Text("Server country (hint): %s", countryCell(r.server_country).c_str());
                    ImGui::Text("Source: %s", sourceDescription(r.server_country_source));
                    if (!r.server_ips.empty()) {
                        std::string ips;
                        for (size_t k = 0; k < r.server_ips.size() && k < 6; k++) ips += (k ? ", " : "") + r.server_ips[k];
                        ImGui::Text("IPs: %s", ips.c_str());
                    }
                    ImGui::Text("Determined: %s   DB: %s", formatAge(now, r.server_country_at).c_str(),
                                r.geo_db_version.empty() ? "-" : r.geo_db_version.c_str());
                    ImGui::TextDisabled("Server location is only a hint; the exit may differ.");
                    ImGui::EndTooltip();
                }
            }

            ImGui::TableNextColumn();
            ImGui::TextUnformatted(row.protocol.empty() ? "-" : row.protocol.c_str());

            ImGui::TableNextColumn();
            std::string addr = row.address.empty() ? r.uri : (row.address + ":" + std::to_string(row.port));
            if (!row.ps.empty()) addr += "  (" + row.ps + ")";
            ImGui::TextUnformatted(addr.c_str());

            ImGui::TableNextColumn();
            ImGui::TextUnformatted(r.engine_used.empty() ? "-" : r.engine_used.c_str());

            ImGui::TableNextColumn();
            ImGui::TextUnformatted(formatAge(now, r.last_tested).c_str());

            ImGui::TableNextColumn();
            ImGui::TextUnformatted(r.tag.empty() ? "-" : r.tag.c_str());

            ImGui::TableNextColumn();
            // Gemini status: -1=unknown, 0=blocked, 1=accessible
            if (r.gemini_status == 1) {
                ImGui::TextColored(ImVec4(0.2f, 0.9f, 0.3f, 1.0f), "OK");
            } else if (r.gemini_status == 0) {
                ImGui::TextColored(ImVec4(0.9f, 0.3f, 0.3f, 1.0f), "Blocked");
            } else {
                ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "?");
            }

            ImGui::TableNextColumn();
            // Proxy server control: Start/Stop button + port/status/traffic display
            {
                auto& psm = orch_.proxyServerManager();
                auto status = psm.getStatus(r.uri);
                int proxy_port = psm.getPort(r.uri);

                // Helper to format bytes human-readable
                auto fmtBytes = [](unsigned long long b) -> std::string {
                    if (b < 1024) return std::to_string(b) + "B";
                    if (b < 1024 * 1024) return std::to_string(b / 1024) + "KB";
                    if (b < 1024ULL * 1024 * 1024) return std::to_string(b / (1024 * 1024)) + "MB";
                    return std::to_string(b / (1024ULL * 1024 * 1024)) + "GB";
                };

                ImGui::PushID(("proxy_" + r.uri).c_str());
                if (status == proxy::ProxyStatus::Running) {
                    // Line 1: port + Stop button
                    ImGui::TextColored(ImVec4(0.2f, 0.9f, 0.3f, 1.0f), ":%d", proxy_port);
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Stop")) {
                        psm.stopProxy(r.uri);
                        showToast("Stopped proxy on port " + std::to_string(proxy_port));
                    }
                    // Line 2: traffic stats (down/up)
                    auto [bin, bout] = psm.getTraffic(r.uri);
                    ImGui::TextColored(ImVec4(0.5f, 0.7f, 1.0f, 1.0f), "v %s  ^ %s",
                                       fmtBytes(bin).c_str(), fmtBytes(bout).c_str());
                } else if (status == proxy::ProxyStatus::Starting) {
                    ImGui::TextColored(ImVec4(0.9f, 0.8f, 0.2f, 1.0f), "Starting...");
                } else if (status == proxy::ProxyStatus::Error) {
                    ImGui::TextColored(ImVec4(0.9f, 0.3f, 0.3f, 1.0f), "Error");
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Retry")) {
                        int port = psm.startProxy(r.uri);
                        if (port > 0) {
                            showToast("Started proxy on port " + std::to_string(port));
                        } else {
                            std::string err = psm.getError(r.uri);
                            showToast(err.empty() ? "Failed to start proxy" : err);
                        }
                    }
                } else {
                    // Stopped — show Start button
                    if (ImGui::SmallButton("Start Proxy")) {
                        int port = psm.startProxy(r.uri);
                        if (port > 0) {
                            showToast("Started proxy on port " + std::to_string(port));
                        } else {
                            std::string err = psm.getError(r.uri);
                            showToast(err.empty() ? "Failed to start proxy" : err);
                        }
                    }
                }
                // Tooltip with details
                if (ImGui::IsItemHovered()) {
                    ImGui::BeginTooltip();
                    if (status == proxy::ProxyStatus::Running) {
                        auto [bin, bout] = psm.getTraffic(r.uri);
                        ImGui::Text("SOCKS5 proxy on localhost:%d", proxy_port);
                        ImGui::Text("Download: %s  Upload: %s",
                                    fmtBytes(bin).c_str(), fmtBytes(bout).c_str());
                        ImGui::Separator();
                        ImGui::Text("Connect external apps to localhost:%d", proxy_port);
                    } else if (status == proxy::ProxyStatus::Error) {
                        std::string err = psm.getError(r.uri);
                        ImGui::TextUnformatted(err.empty() ? "Unknown error" : err.c_str());
                    } else {
                        ImGui::Text("Start a local SOCKS5 proxy (port 3110-3120)");
                        ImGui::Text("routing traffic through this config.");
                    }
                    ImGui::EndTooltip();
                }
                ImGui::PopID();
            }
        }
    }

    ImGui::EndTable();
}

void HunterGuiApp::renderLogPanel() {
    // ─── Read logs from utils::LogRingBuffer ───
    // This is the orchestrator's own clean, timestamped log buffer. It
    // contains only messages explicitly pushed via push() — no ANSI codes,
    // no terminal dashboard frames, no clear-screen sequences. Each entry
    // is already timestamped with [HH:MM:SS] prefix.
    auto& orch_log = utils::LogRingBuffer::instance();

    // Check for new log entries using the generation counter (efficient —
    // avoids copying the buffer every frame when nothing changed).
    double now = glfwGetTime();
    size_t current_gen = orch_log.generation();
    if (last_log_refresh_ < 0.0 || now - last_log_refresh_ >= kLogRefreshIntervalSeconds ||
        current_gen != log_generation_) {
        last_log_refresh_ = now;
        log_generation_ = current_gen;
        // Fetch recent lines (capped to keep total under ~10KB).
        // 200 lines × ~50 bytes/line ≈ 10KB max.
        cached_log_lines_ = orch_log.recent(200);
    }

    ImGui::Separator();
    ImGui::Text("Live Logs");
    ImGui::SameLine(0, 16);
    ImGui::TextDisabled("(%zu lines)", cached_log_lines_.size());

    // Log action buttons
    ImGui::SameLine(0, 24);
    if (ImGui::SmallButton("Copy Logs")) {
        if (!cached_log_lines_.empty()) {
            std::string text;
            for (const auto& line : cached_log_lines_) {
                text += line;
                text += '\n';
            }
            ImGui::SetClipboardText(text.c_str());
            showToast("Logs copied to clipboard");
        } else {
            showToast("No logs to copy");
        }
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Export Logs")) {
        if (!cached_log_lines_.empty()) {
            std::string text;
            for (const auto& line : cached_log_lines_) {
                text += line;
                text += '\n';
            }
            exportToFile(text, "hunter_logs");
        } else {
            showToast("No logs to export");
        }
    }
    ImGui::SameLine(0, 24);
    ImGui::Checkbox("Auto-scroll", &log_auto_scroll_);

    // ─── Log list view ───
    // Each log line is rendered as a separate row using TextUnformatted.
    // This gives a clean list view — one line per row, never concatenated.
    // Auto-scroll keeps the latest line visible at the bottom.
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.03f, 0.03f, 0.03f, 1.0f));
    ImGui::BeginChild("log_area", ImVec2(0, 180.0f), true,
                      ImGuiWindowFlags_HorizontalScrollbar);

    const auto& lines = cached_log_lines_;

    // Track whether we're at the bottom before rendering new content.
    bool at_bottom = ImGui::GetScrollY() >= ImGui::GetScrollMaxY();

    // Use ImGuiListClipper for efficient rendering of large log lists.
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(lines.size()));
    while (clipper.Step()) {
        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
            const std::string& line = lines[i];

            // Color-code lines based on content for quick scanning.
            ImVec4 color(0.75f, 0.75f, 0.75f, 1.0f); // default light gray
            if (line.find("] OK ") != std::string::npos || line.find("TG-OK") != std::string::npos ||
                line.find("PASSED") != std::string::npos || line.find("SUCCESS") != std::string::npos) {
                color = ImVec4(0.4f, 0.85f, 0.4f, 1.0f); // green
            } else if (line.find("FAIL") != std::string::npos || line.find("DEAD") != std::string::npos ||
                       line.find("ERROR") != std::string::npos || line.find("EXCEPTION") != std::string::npos) {
                color = ImVec4(0.85f, 0.35f, 0.35f, 1.0f); // red
            } else if (line.find("WARNING") != std::string::npos || line.find("SKIP") != std::string::npos ||
                       line.find("WARNING") != std::string::npos) {
                color = ImVec4(0.85f, 0.75f, 0.35f, 1.0f); // yellow
            } else if (line.find("[Validator]") != std::string::npos ||
                       line.find("[Download]") != std::string::npos ||
                       line.find("[Scanner]") != std::string::npos) {
                color = ImVec4(0.55f, 0.75f, 0.95f, 1.0f); // blue for worker tags
            }

            ImGui::PushStyleColor(ImGuiCol_Text, color);
            ImGui::TextUnformatted(line.c_str());
            ImGui::PopStyleColor();
        }
    }
    clipper.End();

    // Auto-scroll to bottom: if auto-scroll is on and we were at the bottom
    // (or this is the first render), scroll to the latest line.
    if (log_auto_scroll_ && (at_bottom || lines.size() <= 1) && !lines.empty()) {
        ImGui::SetScrollHereY(1.0f);
    }

    ImGui::EndChild();
    ImGui::PopStyleColor();
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

    ImGui::SetNextWindowSize(ImVec2(380, 420), ImGuiCond_Appearing);
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
                ImGui::TextColored(ImVec4(0.9f, 0.5f, 0.4f, 1.0f),
                                   "URI too long to encode as a QR code.");
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

    // Modern rounded corners
    style.WindowRounding = 8.0f;
    style.FrameRounding = 6.0f;
    style.GrabRounding = 6.0f;
    style.TabRounding = 6.0f;
    style.ScrollbarRounding = 8.0f;
    style.PopupRounding = 6.0f;
    style.ChildRounding = 6.0f;

    // Spacing
    style.WindowPadding = ImVec2(16, 16);
    style.FramePadding = ImVec2(10, 6);
    style.ItemSpacing = ImVec2(10, 8);
    style.ItemInnerSpacing = ImVec2(8, 6);
    style.IndentSpacing = 20.0f;
    style.ScrollbarSize = 14.0f;

    // Sizing
    style.WindowBorderSize = 0.0f;
    style.ChildBorderSize = 1.0f;
    style.PopupBorderSize = 1.0f;
    style.FrameBorderSize = 0.0f;
    style.TabBorderSize = 0.0f;

    // Modern dark color palette
    ImVec4* colors = style.Colors;

    // Background tones (deep dark blue-gray)
    colors[ImGuiCol_WindowBg]        = ImVec4(0.06f, 0.07f, 0.09f, 1.0f);
    colors[ImGuiCol_ChildBg]         = ImVec4(0.08f, 0.09f, 0.11f, 1.0f);
    colors[ImGuiCol_PopupBg]         = ImVec4(0.10f, 0.11f, 0.13f, 0.98f);
    colors[ImGuiCol_Border]          = ImVec4(0.18f, 0.20f, 0.24f, 0.50f);
    colors[ImGuiCol_BorderShadow]    = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);

    // Text
    colors[ImGuiCol_Text]            = ImVec4(0.90f, 0.92f, 0.95f, 1.0f);
    colors[ImGuiCol_TextDisabled]    = ImVec4(0.50f, 0.54f, 0.58f, 1.0f);

    // Frames / inputs
    colors[ImGuiCol_FrameBg]         = ImVec4(0.12f, 0.14f, 0.17f, 1.0f);
    colors[ImGuiCol_FrameBgHovered]  = ImVec4(0.18f, 0.20f, 0.24f, 1.0f);
    colors[ImGuiCol_FrameBgActive]   = ImVec4(0.22f, 0.25f, 0.30f, 1.0f);

    // Buttons — accent blue
    colors[ImGuiCol_Button]          = ImVec4(0.20f, 0.36f, 0.60f, 1.0f);
    colors[ImGuiCol_ButtonHovered]   = ImVec4(0.26f, 0.45f, 0.75f, 1.0f);
    colors[ImGuiCol_ButtonActive]    = ImVec4(0.16f, 0.30f, 0.52f, 1.0f);

    // Headers / collapsibles
    colors[ImGuiCol_Header]          = ImVec4(0.20f, 0.36f, 0.60f, 1.0f);
    colors[ImGuiCol_HeaderHovered]   = ImVec4(0.26f, 0.45f, 0.75f, 1.0f);
    colors[ImGuiCol_HeaderActive]    = ImVec4(0.16f, 0.30f, 0.52f, 1.0f);

    // Tabs
    colors[ImGuiCol_Tab]             = ImVec4(0.10f, 0.12f, 0.15f, 1.0f);
    colors[ImGuiCol_TabHovered]      = ImVec4(0.20f, 0.36f, 0.60f, 1.0f);
    colors[ImGuiCol_TabActive]       = ImVec4(0.16f, 0.28f, 0.48f, 1.0f);
    colors[ImGuiCol_TabUnfocused]    = ImVec4(0.08f, 0.09f, 0.11f, 1.0f);
    colors[ImGuiCol_TabUnfocusedActive] = ImVec4(0.14f, 0.16f, 0.20f, 1.0f);

    // Title bar
    colors[ImGuiCol_TitleBg]         = ImVec4(0.08f, 0.09f, 0.11f, 1.0f);
    colors[ImGuiCol_TitleBgActive]   = ImVec4(0.10f, 0.12f, 0.15f, 1.0f);
    colors[ImGuiCol_TitleBgCollapsed]= ImVec4(0.06f, 0.07f, 0.09f, 1.0f);

    // Scrollbar
    colors[ImGuiCol_ScrollbarBg]     = ImVec4(0.04f, 0.05f, 0.06f, 1.0f);
    colors[ImGuiCol_ScrollbarGrab]   = ImVec4(0.20f, 0.22f, 0.26f, 1.0f);
    colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.28f, 0.30f, 0.36f, 1.0f);
    colors[ImGuiCol_ScrollbarGrabActive]  = ImVec4(0.35f, 0.38f, 0.44f, 1.0f);

    // Checkmark / slider / separator
    colors[ImGuiCol_CheckMark]       = ImVec4(0.30f, 0.55f, 0.90f, 1.0f);
    colors[ImGuiCol_SliderGrab]      = ImVec4(0.30f, 0.55f, 0.90f, 1.0f);
    colors[ImGuiCol_SliderGrabActive]= ImVec4(0.40f, 0.65f, 1.0f, 1.0f);
    colors[ImGuiCol_Separator]       = ImVec4(0.18f, 0.20f, 0.24f, 0.50f);
    colors[ImGuiCol_SeparatorHovered]= ImVec4(0.30f, 0.55f, 0.90f, 0.60f);
    colors[ImGuiCol_SeparatorActive] = ImVec4(0.30f, 0.55f, 0.90f, 1.0f);

    // Resize grip
    colors[ImGuiCol_ResizeGrip]      = ImVec4(0.20f, 0.22f, 0.26f, 0.50f);
    colors[ImGuiCol_ResizeGripHovered] = ImVec4(0.30f, 0.55f, 0.90f, 0.70f);
    colors[ImGuiCol_ResizeGripActive]  = ImVec4(0.40f, 0.65f, 1.0f, 0.95f);

    // Plot
    colors[ImGuiCol_PlotLines]       = ImVec4(0.30f, 0.55f, 0.90f, 1.0f);
    colors[ImGuiCol_PlotLinesHovered]= ImVec4(0.40f, 0.65f, 1.0f, 1.0f);
    colors[ImGuiCol_PlotHistogram]   = ImVec4(0.30f, 0.55f, 0.90f, 1.0f);
    colors[ImGuiCol_PlotHistogramHovered] = ImVec4(0.40f, 0.65f, 1.0f, 1.0f);

    // Table
    colors[ImGuiCol_TableHeaderBg]   = ImVec4(0.10f, 0.12f, 0.15f, 1.0f);
    colors[ImGuiCol_TableBorderStrong] = ImVec4(0.18f, 0.20f, 0.24f, 1.0f);
    colors[ImGuiCol_TableBorderLight]  = ImVec4(0.14f, 0.16f, 0.19f, 1.0f);
    colors[ImGuiCol_TableRowBg]      = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    colors[ImGuiCol_TableRowBgAlt]   = ImVec4(0.06f, 0.07f, 0.09f, 0.40f);

    // Selectable (uses Header colors in ImGui)
    // ImGuiCol_Header already set above handles selectable styling

    // Nav
    colors[ImGuiCol_NavHighlight]    = ImVec4(0.30f, 0.55f, 0.90f, 1.0f);
    colors[ImGuiCol_NavWindowingHighlight] = ImVec4(0.30f, 0.55f, 0.90f, 0.70f);
    colors[ImGuiCol_NavWindowingDimBg]     = ImVec4(0.06f, 0.07f, 0.09f, 0.50f);

    // Drag-drop target
    colors[ImGuiCol_DragDropTarget]  = ImVec4(0.30f, 0.55f, 0.90f, 0.90f);

    theme_applied_ = true;
}

void HunterGuiApp::renderStatusCard(const char* label, const char* value, const ImVec4& color, float width) {
    ImGui::BeginChild(label, ImVec2(width, 70), true, ImGuiWindowFlags_NoScrollbar);
    {
        // Colored top accent bar
        ImVec2 pos = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(pos, ImVec2(pos.x + ImGui::GetContentRegionAvail().x, pos.y + 3), ImGui::ColorConvertFloat4ToU32(color));

        ImGui::Dummy(ImVec2(0, 5));  // space after accent bar

        // Label (small, muted)
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.50f, 0.54f, 0.58f, 1.0f));
        ImGui::TextUnformatted(label);
        ImGui::PopStyleColor();

        // Value (large, colored)
        ImGui::PushFont(nullptr);  // Use default large text
        ImGui::PushStyleColor(ImGuiCol_Text, color);
        ImGui::SetWindowFontScale(1.4f);
        ImGui::TextUnformatted(value);
        ImGui::SetWindowFontScale(1.0f);
        ImGui::PopStyleColor();
        ImGui::PopFont();
    }
    ImGui::EndChild();
}

void HunterGuiApp::renderTabBar() {
    if (ImGui::BeginTabBar("##MainTabs", ImGuiTabBarFlags_None)) {
        if (ImGui::BeginTabItem("Dashboard")) {
            active_tab_ = Tab::Dashboard;
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Configs")) {
            active_tab_ = Tab::Configs;
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Settings")) {
            active_tab_ = Tab::Settings;
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("About")) {
            active_tab_ = Tab::About;
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
}

void HunterGuiApp::renderDashboardTab() {
    // ─── Status cards row ───
    float card_width = 160.0f;
    float spacing = 12.0f;
    float total_width = card_width * 4 + spacing * 3;
    float start_x = ImGui::GetCursorPosX() + (ImGui::GetContentRegionAvail().x - total_width) / 2.0f;
    if (start_x < 0) start_x = 0;

    ImGui::SetCursorPosX(start_x);

    // Total configs card
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", cached_total_);
    renderStatusCard("Total Configs", buf, ImVec4(0.30f, 0.55f, 0.90f, 1.0f), card_width);

    ImGui::SameLine(0, spacing);
    snprintf(buf, sizeof(buf), "%d", cached_alive_);
    renderStatusCard("Alive", buf, ImVec4(0.30f, 0.80f, 0.40f, 1.0f), card_width);

    ImGui::SameLine(0, spacing);
    snprintf(buf, sizeof(buf), "%d / %d", cached_tested_, cached_total_);
    renderStatusCard("Tested", buf, ImVec4(0.85f, 0.65f, 0.20f, 1.0f), card_width);

    ImGui::SameLine(0, spacing);
    snprintf(buf, sizeof(buf), "%.0f ms", cached_avg_latency_ms_);
    renderStatusCard("Avg Latency", buf, ImVec4(0.65f, 0.50f, 0.85f, 1.0f), card_width);

    ImGui::Spacing();
    ImGui::Spacing();

    // ─── Controls ───
    renderControls();

    ImGui::Spacing();

    // ─── Configs table ───
    renderTable();

    // ─── Log panel ───
    renderLogPanel();
}

void HunterGuiApp::renderConfigsTab() {
    renderHeader();
    ImGui::Spacing();
    renderControls();
    ImGui::Spacing();
    renderTable();
    renderLogPanel();
}

void HunterGuiApp::renderSettingsTab() {
    ImGui::TextColored(ImVec4(0.30f, 0.55f, 0.90f, 1.0f), "Settings");
    ImGui::Separator();
    ImGui::Spacing();

    // Orchestrator controls
    ImGui::TextColored(ImVec4(0.70f, 0.72f, 0.76f, 1.0f), "Orchestrator");
    ImGui::Spacing();

    bool busy = transitioning_.load();
    ImGui::BeginDisabled(busy || orchestrator_running_.load());
    if (ImGui::Button("Start Orchestrator", ImVec2(180, 0))) startOrchestrator();
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::BeginDisabled(busy || !orchestrator_running_.load());
    if (ImGui::Button("Stop Orchestrator", ImVec2(180, 0))) {
        std::thread([this]() { stopOrchestrator(); }).detach();
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::BeginDisabled(rescan_in_progress_.load());
    if (ImGui::Button(rescan_in_progress_.load() ? "Rescanning..." : "Rescan Now", ImVec2(180, 0))) {
        triggerRescan();
    }
    ImGui::EndDisabled();

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Status
    ImGui::TextColored(ImVec4(0.70f, 0.72f, 0.76f, 1.0f), "Status");
    ImGui::Spacing();

    ImGui::Text("Orchestrator: %s", orchestrator_running_.load() ? "Running" : "Stopped");
    ImGui::Text("Cycle count: %d", orch_.cycleCount());
    ImGui::Text("Version: v%s", constants::HUNTER_VERSION);

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Country detection
    ImGui::TextColored(ImVec4(0.70f, 0.72f, 0.76f, 1.0f), "Country detection");
    ImGui::Spacing();
    if (ImGui::Checkbox("Resolve domain endpoints with verified DoH (server-country hints)", &doh_enabled_ui_)) {
        orch_.config().set("country_doh_enabled", doh_enabled_ui_);
        if (country_service_) country_service_->setDohEnabled(doh_enabled_ui_);
        saveConfigNow();
    }
    ImGui::TextDisabled("Off by default: sends endpoint hostnames to the DoH provider (1.1.1.1, TLS-verified). Local DNS is never used.");
    if (ImGui::Checkbox("Direct baseline probes (local connectivity checks)", &baseline_enabled_ui_)) {
        network::ConnectivityBaseline::shared()->setEnabled(baseline_enabled_ui_);
        orch_.config().set("direct_baseline_enabled", baseline_enabled_ui_);
        saveConfigNow();
    }
    ImGui::TextDisabled("Server countries filled so far: %d   Exit target: %s%s",
                        country_filled_.load(),
                        orch_.config().exitCountryTarget().empty() ? "none" : orch_.config().exitCountryTarget().c_str(),
                        target_strict_ui_ ? " (strict)" : "");

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Export options
    ImGui::TextColored(ImVec4(0.70f, 0.72f, 0.76f, 1.0f), "Export");
    ImGui::Spacing();

    if (ImGui::Button("Export All Live Configs", ImVec2(220, 0))) {
        exportToFile(buildExportText(), "hunter_live_configs");
    }
    ImGui::SameLine();
    if (ImGui::Button("Copy All Live to Clipboard", ImVec2(220, 0))) {
        std::string text = buildClipboardText(false);
        ImGui::SetClipboardText(text.c_str());
        showToast("Copied " + std::to_string(std::count(text.begin(), text.end(), '\n')) + " configs");
    }

    ImGui::Spacing();
    ImGui::SameLine();
    if (ImGui::Button("Copy Gemini-OK Configs", ImVec2(220, 0))) {
        std::string text = buildGeminiOkClipboardText();
        ImGui::SetClipboardText(text.c_str());
        int count = text.empty() ? 0 : std::count(text.begin(), text.end(), '\n');
        showToast("Copied " + std::to_string(count) + " Gemini-OK configs");
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Dangerous actions
    ImGui::TextColored(ImVec4(0.85f, 0.35f, 0.35f, 1.0f), "Danger Zone");
    ImGui::Spacing();

    ImGui::BeginDisabled(cached_alive_ == 0);
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.20f, 0.20f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.70f, 0.25f, 0.25f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.45f, 0.15f, 0.15f, 1.0f));
    if (ImGui::Button("Clear All Live Configs", ImVec2(220, 0))) {
        show_clear_live_popup_ = true;
    }
    ImGui::PopStyleColor(3);
    ImGui::EndDisabled();

    if (!toast_message_.empty() && glfwGetTime() < toast_until_) {
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.4f, 0.85f, 0.4f, 1.0f), "%s", toast_message_.c_str());
    }
}

void HunterGuiApp::renderAboutTab() {
    ImGui::TextColored(ImVec4(0.30f, 0.55f, 0.90f, 1.0f), "Hunter");
    ImGui::SameLine();
    ImGui::TextDisabled("Censorship Hunter — Autonomous Proxy Discovery Engine");
    ImGui::Separator();
    ImGui::Spacing();

    // Version info
    ImGui::Text("Application Version: v%s", constants::HUNTER_VERSION);
    ImGui::Text("GitHub: https://github.com/bahmany/censorship_hunter");
    ImGui::Spacing();

    renderUpdatePanel();

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // About text
    ImGui::TextWrapped(
        "Hunter is an autonomous anti-censorship proxy configuration discovery, "
        "validation, and load-balancing system. It scrapes proxy configs from "
        "Telegram channels and GitHub repos, tests them for liveness, and exposes "
        "working proxies through SOCKS5 balancers."
    );
    ImGui::Spacing();
    ImGui::TextDisabled("License: MIT");

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::TextColored(ImVec4(0.70f, 0.72f, 0.76f, 1.0f), "Attribution");
    ImGui::Spacing();
    ImGui::TextUnformatted("IP Geolocation by DB-IP");
    ImGui::TextDisabled("https://db-ip.com  -  IP to Country Lite, licensed CC BY 4.0");
    ImGui::TextDisabled("https://creativecommons.org/licenses/by/4.0/");
    {
        auto& gdb = geo::CountryDatabase::instance();
        if (gdb.isLoaded()) {
            ImGui::TextDisabled("Database: %s  (%zu IPv4 + %zu IPv6 ranges, %zu countries)", gdb.dbVersion().c_str(),
                                gdb.ipv4Count(), gdb.ipv6Count(), gdb.countryCount());
        } else {
            ImGui::TextDisabled("Offline country database not embedded in this build (server countries stay Unknown).");
        }
    }
    ImGui::TextWrapped(
        "The database was converted from the vendor CSV into a compact binary range table (HCGEO1) and "
        "compressed; no ranges were otherwise modified. Country values are approximate hints: a server's "
        "location does not determine where its traffic exits. Exit countries are measured through the tunnel. "
        "See THIRD_PARTY_NOTICES.md.");
}

void HunterGuiApp::renderUpdatePanel() {
    ImGui::TextColored(ImVec4(0.70f, 0.72f, 0.76f, 1.0f), "Auto-Update");
    ImGui::Spacing();

    auto& updater = core::SelfUpdateManager::instance();
    auto state = updater.state();
    float progress = updater.downloadProgress();
    std::string status_msg = updater.statusMessage();

    // State indicator
    ImVec4 state_color;
    const char* state_label;
    switch (state) {
        case core::SelfUpdateManager::UpdateState::Idle:
            state_color = ImVec4(0.50f, 0.54f, 0.58f, 1.0f);
            state_label = "Idle";
            break;
        case core::SelfUpdateManager::UpdateState::Checking:
            state_color = ImVec4(0.85f, 0.65f, 0.20f, 1.0f);
            state_label = "Checking...";
            break;
        case core::SelfUpdateManager::UpdateState::UpToDate:
            state_color = ImVec4(0.30f, 0.80f, 0.40f, 1.0f);
            state_label = "Up to Date";
            break;
        case core::SelfUpdateManager::UpdateState::UpdateAvailable:
            state_color = ImVec4(0.30f, 0.55f, 0.90f, 1.0f);
            state_label = "Update Available";
            break;
        case core::SelfUpdateManager::UpdateState::Downloading:
            state_color = ImVec4(0.30f, 0.55f, 0.90f, 1.0f);
            state_label = "Downloading...";
            break;
        case core::SelfUpdateManager::UpdateState::DownloadComplete:
            state_color = ImVec4(0.30f, 0.80f, 0.40f, 1.0f);
            state_label = "Download Complete";
            break;
        case core::SelfUpdateManager::UpdateState::Applying:
            state_color = ImVec4(0.85f, 0.65f, 0.20f, 1.0f);
            state_label = "Applying...";
            break;
        case core::SelfUpdateManager::UpdateState::UpdateComplete:
            state_color = ImVec4(0.30f, 0.80f, 0.40f, 1.0f);
            state_label = "Update Complete — Relaunching";
            break;
        case core::SelfUpdateManager::UpdateState::Failed:
            state_color = ImVec4(0.85f, 0.35f, 0.35f, 1.0f);
            state_label = "Failed";
            break;
        default:
            state_color = ImVec4(0.50f, 0.54f, 0.58f, 1.0f);
            state_label = "Unknown";
            break;
    }

    // Status badge
    ImGui::PushStyleColor(ImGuiCol_Text, state_color);
    ImGui::Text("[ %s ]", state_label);
    ImGui::PopStyleColor();

    if (!status_msg.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("(%s)", status_msg.c_str());
    }

    // Download progress bar
    if (state == core::SelfUpdateManager::UpdateState::Downloading ||
        state == core::SelfUpdateManager::UpdateState::DownloadComplete) {
        ImGui::Spacing();
        char progress_label[32];
        snprintf(progress_label, sizeof(progress_label), "%.0f%%", progress * 100.0f);
        ImGui::ProgressBar(progress, ImVec2(-1, 0), progress_label);
    }

    ImGui::Spacing();

    // Manual check button
    if (state == core::SelfUpdateManager::UpdateState::Idle ||
        state == core::SelfUpdateManager::UpdateState::UpToDate ||
        state == core::SelfUpdateManager::UpdateState::Failed) {
        if (ImGui::Button("Check for Updates", ImVec2(180, 0))) {
            std::thread([]() {
                core::SelfUpdateManager::instance().checkForUpdate();
            }).detach();
        }
    }

    // Manual apply button (if update is available)
    if (state == core::SelfUpdateManager::UpdateState::UpdateAvailable) {
        ImGui::SameLine();
        if (ImGui::Button("Download & Install Now", ImVec2(200, 0))) {
            std::thread([]() {
                core::SelfUpdateManager::instance().downloadAndApply();
            }).detach();
        }
    }

    // Show release info if available
    auto release = updater.latestRelease();
    if (!release.tag_name.empty()) {
        ImGui::Spacing();
        ImGui::TextDisabled("Latest release: v%s", release.tag_name.c_str());
        if (!release.release_name.empty()) {
            ImGui::TextDisabled("Release name: %s", release.release_name.c_str());
        }
        if (!release.body.empty()) {
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(0.70f, 0.72f, 0.76f, 1.0f), "Release Notes:");
            ImGui::Indent();
            // Truncate very long release notes for display
            std::string notes = release.body;
            if (notes.size() > 1000) notes = notes.substr(0, 1000) + "...";
            ImGui::TextWrapped("%s", notes.c_str());
            ImGui::Unindent();
        }
    }
}

void HunterGuiApp::renderStatusBar() {
    // Bottom status bar with quick stats
    ImGui::Spacing();
    ImGui::Separator();

    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.08f, 0.09f, 0.11f, 1.0f));
    ImGui::BeginChild("##statusbar", ImVec2(0, 28), true, ImGuiWindowFlags_NoScrollbar);

    // Left: orchestrator status
    if (orchestrator_running_.load()) {
        ImGui::TextColored(ImVec4(0.30f, 0.80f, 0.40f, 1.0f), "[Running]");
    } else {
        ImGui::TextColored(ImVec4(0.85f, 0.35f, 0.35f, 1.0f), "[Stopped]");
    }

    // Center: cycle count
    ImGui::SameLine(0, 24);
    ImGui::TextDisabled("Cycle: %d", orch_.cycleCount());

    // Right: update status
    auto& updater = core::SelfUpdateManager::instance();
    auto update_state = updater.state();
    if (update_state != core::SelfUpdateManager::UpdateState::Idle &&
        update_state != core::SelfUpdateManager::UpdateState::UpToDate) {
        ImGui::SameLine(0, 24);
        const char* update_label = "";
        ImVec4 update_color = ImVec4(0.50f, 0.54f, 0.58f, 1.0f);
        switch (update_state) {
            case core::SelfUpdateManager::UpdateState::Checking:
                update_label = "Checking for updates...";
                update_color = ImVec4(0.85f, 0.65f, 0.20f, 1.0f);
                break;
            case core::SelfUpdateManager::UpdateState::UpdateAvailable:
                update_label = "Update available";
                update_color = ImVec4(0.30f, 0.55f, 0.90f, 1.0f);
                break;
            case core::SelfUpdateManager::UpdateState::Downloading:
                update_label = "Downloading update...";
                update_color = ImVec4(0.30f, 0.55f, 0.90f, 1.0f);
                break;
            case core::SelfUpdateManager::UpdateState::Applying:
                update_label = "Applying update...";
                update_color = ImVec4(0.85f, 0.65f, 0.20f, 1.0f);
                break;
            case core::SelfUpdateManager::UpdateState::UpdateComplete:
                update_label = "Update complete — relaunching";
                update_color = ImVec4(0.30f, 0.80f, 0.40f, 1.0f);
                break;
            case core::SelfUpdateManager::UpdateState::Failed:
                update_label = "Update failed";
                update_color = ImVec4(0.85f, 0.35f, 0.35f, 1.0f);
                break;
            default: break;
        }
        if (*update_label) {
            ImGui::TextColored(update_color, "%s", update_label);
        }
    }

    // Far right: version
    float version_width = ImGui::CalcTextSize("vX.X.X").x + 16;
    ImGui::SameLine(ImGui::GetWindowWidth() - version_width - 16);
    ImGui::TextDisabled("v%s", constants::HUNTER_VERSION);

    ImGui::EndChild();
    ImGui::PopStyleColor();
}

void HunterGuiApp::renderFrame() {
    refreshSnapshotIfDue();

    if (!theme_applied_) applyModernTheme();

    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::Begin("HunterMain", nullptr,
                  ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                  ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                  ImGuiWindowFlags_NoBringToFrontOnFocus);

    // ─── Tab bar ───
    renderTabBar();
    ImGui::Spacing();

    // ─── Tab content ───
    switch (active_tab_) {
        case Tab::Dashboard: renderDashboardTab(); break;
        case Tab::Configs:   renderConfigsTab(); break;
        case Tab::Settings:  renderSettingsTab(); break;
        case Tab::About:     renderAboutTab(); break;
    }

    // ─── Status bar (always visible) ───
    renderStatusBar();

    // ─── Popups ───
    renderQrPopup();
    renderClearLivePopup();

    // ─── Toast ───
    if (!toast_message_.empty() && glfwGetTime() < toast_until_) {
        ImGui::SameLine(0, 16);
        ImGui::TextColored(ImVec4(0.4f, 0.85f, 0.4f, 1.0f), "%s", toast_message_.c_str());
    }

    ImGui::End();
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
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

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

    stopOrchestrator();
    core::SelfUpdateManager::instance().stopAutoCheck();

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
