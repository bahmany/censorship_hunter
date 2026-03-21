// imgui_app.cpp — Complete UI rewrite with proper multithreading
#include "core/utils.h"
#include "win32/imgui_app.h"
#include "orchestrator/orchestrator.h"

#include "third_party/imgui/imgui.h"
#include "third_party/imgui/backends/imgui_impl_dx9.h"
#include "third_party/imgui/backends/imgui_impl_win32.h"

#include <commdlg.h>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <chrono>

IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace hunter {
namespace win32 {

// ── Anonymous helpers ──
namespace {

const char* PageLabel(ImGuiApp::Page p) {
    switch (p) {
        case ImGuiApp::Page::Home:       return "  Home  ";
        case ImGuiApp::Page::Configs:    return " Configs ";
        case ImGuiApp::Page::Censorship: return " Censorship ";
        case ImGuiApp::Page::Logs:       return "  Logs  ";
        case ImGuiApp::Page::Advanced:   return " Advanced ";
    }
    return "Home";
}

const char* ModeLbl(ResourceMode m) {
    switch (m) {
        case ResourceMode::NORMAL:        return "Normal";
        case ResourceMode::MODERATE:      return "Moderate";
        case ResourceMode::SCALED:        return "Scaled";
        case ResourceMode::CONSERVATIVE:  return "Conservative";
        case ResourceMode::REDUCED:       return "Reduced";
        case ResourceMode::MINIMAL:       return "Minimal";
        case ResourceMode::ULTRA_MINIMAL: return "Ultra-Min";
    }
    return "?";
}

std::string FmtTs(double ts) {
    if (ts <= 0.0) return "-";
    std::time_t v = static_cast<std::time_t>(ts);
    std::tm tm{};
    localtime_s(&tm, &v);
    char buf[64];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &tm);
    return buf;
}

std::string Trim(const std::string& v, size_t n = 120) {
    if (v.size() <= n) return v;
    return v.substr(0, n - 3) + "...";
}

bool IContains(const std::string& text, const char* needle) {
    if (!needle || !*needle) return true;
    std::string lo = text;
    std::transform(lo.begin(), lo.end(), lo.begin(), [](unsigned char c){ return (char)std::tolower(c); });
    std::string nl(needle);
    std::transform(nl.begin(), nl.end(), nl.begin(), [](unsigned char c){ return (char)std::tolower(c); });
    return lo.find(nl) != std::string::npos;
}

// Colour palette
static const ImVec4 COL_BG       = {0.07f, 0.07f, 0.09f, 1.0f};
static const ImVec4 COL_CARD     = {0.11f, 0.12f, 0.15f, 1.0f};
static const ImVec4 COL_ACCENT   = {0.22f, 0.47f, 0.95f, 1.0f};
static const ImVec4 COL_GREEN    = {0.18f, 0.80f, 0.44f, 1.0f};
static const ImVec4 COL_RED      = {0.92f, 0.34f, 0.34f, 1.0f};
static const ImVec4 COL_YELLOW   = {1.00f, 0.78f, 0.24f, 1.0f};
static const ImVec4 COL_CYAN     = {0.30f, 0.78f, 1.00f, 1.0f};
static const ImVec4 COL_DIM      = {0.55f, 0.57f, 0.62f, 1.0f};
static const ImVec4 COL_TEXT     = {0.88f, 0.90f, 0.94f, 1.0f};

ImVec4 ToastBg(ImGuiApp::ToastKind kind) {
    switch (kind) {
        case ImGuiApp::ToastKind::Success: return ImVec4(0.12f, 0.42f, 0.16f, 0.96f);
        case ImGuiApp::ToastKind::Warning: return ImVec4(0.52f, 0.34f, 0.08f, 0.96f);
        case ImGuiApp::ToastKind::Error:   return ImVec4(0.48f, 0.12f, 0.12f, 0.96f);
        case ImGuiApp::ToastKind::Info:
        default:                           return ImVec4(0.12f, 0.24f, 0.46f, 0.96f);
    }
}

std::string ShortenForLog(const std::string& value, size_t max_len = 180) {
    if (value.size() <= max_len) return value;
    return value.substr(0, max_len - 3) + "...";
}

std::string TimestampLogLine(const std::string& line) {
    if (line.size() >= 11 && line[0] == '[' && line[3] == ':' && line[6] == ':' && line[9] == ']') {
        return line;
    }
    std::time_t now = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &now);
    char buf[16];
    std::strftime(buf, sizeof(buf), "%H:%M:%S", &tm);
    return std::string("[") + buf + "] " + line;
}

bool LooksLikeIpv4(const std::string& token) {
    unsigned a = 0, b = 0, c = 0, d = 0;
    if (std::sscanf(token.c_str(), "%u.%u.%u.%u", &a, &b, &c, &d) != 4) return false;
    return a <= 255 && b <= 255 && c <= 255 && d <= 255;
}

std::string RedactIpv4(const std::string& ip) {
    unsigned a = 0, b = 0, c = 0, d = 0;
    if (std::sscanf(ip.c_str(), "%u.%u.%u.%u", &a, &b, &c, &d) != 4) return ip;
    if (a == 10) return "10.x.x.x";
    if (a == 172 && b >= 16 && b <= 31) return "172." + std::to_string(b) + ".x.x";
    if (a == 192 && b == 168) return "192.168.x.x";
    if (a == 127) return "127.x.x.x";
    if (a == 169 && b == 254) return "169.254.x.x";
    return std::to_string(a) + "." + std::to_string(b) + ".x.x";
}

bool LooksLikeMac(const std::string& token) {
    if (token.size() != 17) return false;
    const char sep = token[2];
    if (sep != ':' && sep != '-') return false;
    for (size_t i = 0; i < token.size(); ++i) {
        if ((i + 1) % 3 == 0) {
            if (token[i] != sep) return false;
        } else if (!std::isxdigit(static_cast<unsigned char>(token[i]))) {
            return false;
        }
    }
    return true;
}

std::string RedactMac(const std::string& mac) {
    if (!LooksLikeMac(mac)) return mac;
    const char sep = mac[2];
    return mac.substr(0, 2) + sep + mac.substr(3, 2) + sep + "xx" + sep + "xx" + sep + "xx" + sep + mac.substr(15, 2);
}

std::string RedactInterface(const std::string& value) {
    if (value.empty()) return value;
    return "(hidden)";
}

std::string MaskSensitiveTokens(const std::string& line) {
    std::string out;
    size_t i = 0;
    while (i < line.size()) {
        const unsigned char ch = static_cast<unsigned char>(line[i]);
        if (std::isalnum(ch)) {
            size_t j = i;
            while (j < line.size()) {
                const unsigned char tj = static_cast<unsigned char>(line[j]);
                if (std::isalnum(tj) || line[j] == '.' || line[j] == ':' || line[j] == '-') ++j;
                else break;
            }
            const std::string token = line.substr(i, j - i);
            if (LooksLikeMac(token)) out += RedactMac(token);
            else if (LooksLikeIpv4(token)) out += RedactIpv4(token);
            else out += token;
            i = j;
            continue;
        }
        out.push_back(line[i]);
        ++i;
    }
    return out;
}

std::string SanitizeNetworkSensitiveText(const std::string& line) {
    std::string sanitized = MaskSensitiveTokens(line);
    const std::string friendly_prefix = "[DISCOVERY] Friendly interface:";
    if (sanitized.rfind(friendly_prefix, 0) == 0) {
        return friendly_prefix + " (hidden)";
    }
    const size_t iface_pos = sanitized.find("iface=");
    if (iface_pos != std::string::npos) {
        const size_t value_start = iface_pos + 5;
        size_t value_end = sanitized.find_first_of(" ,)", value_start);
        if (value_end == std::string::npos) value_end = sanitized.size();
        sanitized.replace(value_start, value_end - value_start, "(hidden)");
    }
    return sanitized;
}

std::string DisplayNetworkValue(const std::string& value, const char* kind, bool reveal) {
    if (reveal || value.empty()) return value;
    if (std::strcmp(kind, "ip") == 0) {
        const size_t pos = value.find(' ');
        const std::string base = pos == std::string::npos ? value : value.substr(0, pos);
        const std::string suffix = pos == std::string::npos ? "" : value.substr(pos);
        return RedactIpv4(base) + suffix;
    }
    if (std::strcmp(kind, "mac") == 0) return RedactMac(value);
    if (std::strcmp(kind, "iface") == 0) return RedactInterface(value);
    return value;
}

void HelpMarker(const char* desc) {
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 20.0f);
        ImGui::TextUnformatted(desc);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

} // namespace

// ── DPI / Style / Fonts ──
void ImGuiApp::ReloadFonts() {
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();
    ImFontConfig cfg{};
    cfg.OversampleH = 3;
    cfg.OversampleV = 2;
    cfg.PixelSnapH = false;
    cfg.RasterizerMultiply = 1.1f;
    float sz = 16.0f * dpi_scale_;
    if (!io.Fonts->AddFontFromFileTTF("third_party/imgui/misc/fonts/Roboto-Medium.ttf", sz, &cfg))
        io.Fonts->AddFontDefault();
    io.FontGlobalScale = 1.0f;
    if (d3d_device_) {
        ImGui_ImplDX9_InvalidateDeviceObjects();
        ImGui_ImplDX9_CreateDeviceObjects();
    }
}

void ImGuiApp::UpdateDpiScale(float s) {
    s = std::max(1.0f, s);
    if (std::fabs(s - dpi_scale_) < 0.01f) return;
    dpi_scale_ = s;
    SetupStyle();
    ReloadFonts();
}

float ImGuiApp::QueryDpiScale() const {
    UINT dpi = 96;
    HMODULE u32 = GetModuleHandleW(L"user32.dll");
    if (u32 && hwnd_) {
        using Fn = UINT(WINAPI*)(HWND);
        auto f = reinterpret_cast<Fn>(GetProcAddress(u32, "GetDpiForWindow"));
        if (f) dpi = f(hwnd_);
    }
    return std::max(1.0f, dpi / 96.0f);
}

void ImGuiApp::SetupStyle() {
    ImGuiStyle& s = ImGui::GetStyle();
    s = ImGuiStyle();
    ImGui::StyleColorsDark();
    s.WindowRounding    = 6.0f;
    s.FrameRounding     = 5.0f;
    s.GrabRounding      = 4.0f;
    s.TabRounding       = 5.0f;
    s.ChildRounding     = 6.0f;
    s.ScrollbarRounding = 6.0f;
    s.WindowPadding     = {10, 10};
    s.FramePadding      = {8, 5};
    s.ItemSpacing       = {8, 6};
    s.ItemInnerSpacing  = {6, 4};
    s.CellPadding       = {6, 4};
    s.ScrollbarSize     = 14.0f;
    s.WindowBorderSize  = 0.0f;
    s.ChildBorderSize   = 1.0f;

    auto& c = s.Colors;
    c[ImGuiCol_WindowBg]       = COL_BG;
    c[ImGuiCol_ChildBg]        = {0.09f, 0.09f, 0.12f, 1.0f};
    c[ImGuiCol_PopupBg]        = {0.10f, 0.10f, 0.13f, 0.96f};
    c[ImGuiCol_Border]         = {0.20f, 0.21f, 0.26f, 0.60f};
    c[ImGuiCol_FrameBg]        = {0.13f, 0.14f, 0.18f, 1.0f};
    c[ImGuiCol_FrameBgHovered] = {0.18f, 0.20f, 0.26f, 1.0f};
    c[ImGuiCol_FrameBgActive]  = {0.22f, 0.25f, 0.33f, 1.0f};
    c[ImGuiCol_TitleBg]        = COL_BG;
    c[ImGuiCol_TitleBgActive]  = COL_BG;
    c[ImGuiCol_Button]         = {0.16f, 0.34f, 0.80f, 1.0f};
    c[ImGuiCol_ButtonHovered]  = {0.22f, 0.44f, 0.92f, 1.0f};
    c[ImGuiCol_ButtonActive]   = {0.12f, 0.28f, 0.70f, 1.0f};
    c[ImGuiCol_Header]         = {0.16f, 0.18f, 0.24f, 1.0f};
    c[ImGuiCol_HeaderHovered]  = {0.22f, 0.26f, 0.34f, 1.0f};
    c[ImGuiCol_HeaderActive]   = {0.18f, 0.24f, 0.34f, 1.0f};
    c[ImGuiCol_Tab]            = {0.12f, 0.14f, 0.20f, 1.0f};
    c[ImGuiCol_TabHovered]     = COL_ACCENT;
    c[ImGuiCol_TabActive]      = {0.18f, 0.38f, 0.82f, 1.0f};
    c[ImGuiCol_Separator]      = {0.22f, 0.24f, 0.30f, 0.80f};
    c[ImGuiCol_Text]           = COL_TEXT;
    c[ImGuiCol_TextDisabled]   = COL_DIM;
    s.ScaleAllSizes(dpi_scale_);
}

// ── Constructor / Destructor ──
ImGuiApp::ImGuiApp() {
    auto z = [](auto& a){ std::fill(a.begin(), a.end(), 0); };
    z(xray_path_); z(singbox_path_); z(mihomo_path_); z(tor_path_);
    z(import_path_); z(export_path_);
    z(edge_target_mac_); z(edge_exit_ip_); z(edge_iface_);
    z(telegram_api_id_); z(telegram_api_hash_); z(telegram_phone_);
    z(telegram_targets_); z(github_urls_); z(manual_configs_);
}

ImGuiApp::~ImGuiApp() {
    StopBackgroundWorker();
    JoinThread(probe_thread_);
    JoinThread(discovery_thread_);
    JoinThread(edge_bypass_thread_);
    StopHunter();
    if (hwnd_) {
        ImGui_ImplDX9_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
    }
    CleanupDeviceD3D();
    if (hwnd_) { DestroyWindow(hwnd_); hwnd_ = nullptr; }
}

// ── Background worker ──
void ImGuiApp::StartBackgroundWorker() {
    bg_stop_ = false;
    bg_worker_ = std::thread([this]{ BackgroundWorkerLoop(); });
    cmd_stop_ = false;
    cmd_worker_ = std::thread([this]{
        while (!cmd_stop_.load()) {
            std::function<void()> fn;
            {
                std::unique_lock<std::mutex> lk(cmd_mutex_);
                cmd_cv_.wait_for(lk, std::chrono::milliseconds(100),
                    [this]{ return cmd_stop_.load() || !cmd_queue_.empty(); });
                if (cmd_stop_.load()) break;
                if (cmd_queue_.empty()) continue;
                fn = std::move(cmd_queue_.front());
                cmd_queue_.pop_front();
            }
            if (!fn) continue;
            try {
                fn();
            } catch (const std::exception& e) {
                AppendLog(std::string("[ERR] Command worker exception: ") + e.what());
            } catch (...) {
                AppendLog("[ERR] Command worker unknown exception");
            }
        }
    });
}

void ImGuiApp::JoinThread(std::thread& worker) {
    if (worker.joinable()) worker.join();
}

void ImGuiApp::StopBackgroundWorker() {
    bg_stop_ = true;
    cmd_stop_ = true;
    cmd_cv_.notify_all();
    JoinThread(bg_worker_);
    JoinThread(cmd_worker_);
}

void ImGuiApp::BackgroundWorkerLoop() {
    while (!bg_stop_.load()) {
        Snapshot s = BuildSnapshot();
        { std::lock_guard<std::mutex> lk(snap_mutex_); snap_ = std::move(s); }
        for (int i = 0; i < 10 && !bg_stop_.load(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

ImGuiApp::Snapshot ImGuiApp::BuildSnapshot() {
    Snapshot s;
    s.hardware = HardwareSnapshot::detect();
    EnsureOrchestrator();
    HunterOrchestrator* orchestrator = nullptr;
    {
        std::lock_guard<std::mutex> orchestrator_lock(orchestrator_mutex_);
        orchestrator = orchestrator_.get();
    }
    if (!orchestrator) return s;
    const int snapshot_limit = snapshot_config_limit_.load();
    std::lock_guard<std::mutex> call_lock(orchestrator_call_mutex_);
    s.cycle_count = orchestrator->cycleCount();
    s.cached_count = orchestrator->cachedConfigCount();
    s.speed_profile = orchestrator->getSpeedProfile();
    s.provisioned_ports = orchestrator->getProvisionedPorts();
    if (auto* db = orchestrator->configDb()) {
        auto st = db->getStats();
        s.db_total = st.total; s.db_alive = st.alive;
        s.db_tested = st.tested_unique; s.db_untested = st.untested_unique;
        s.db_avg_latency = st.avg_latency_ms;
        s.healthy_records = db->getHealthyRecords(snapshot_limit);
        s.all_records = db->getAllRecords(snapshot_limit);
    }
    if (auto* dpi = orchestrator->dpiEvasion()) {
        s.dpi_metrics = dpi->getMetrics();
        s.edge_bypass_active = dpi->isEdgeRouterBypassActive();
        s.edge_bypass_status = dpi->getEdgeRouterBypassStatus();
    }
    return s;
}

void ImGuiApp::PostCommand(std::function<void()> fn) {
    { std::lock_guard<std::mutex> lk(cmd_mutex_); cmd_queue_.push_back(std::move(fn)); }
    cmd_cv_.notify_one();
}

void ImGuiApp::RunCommandAsync(const std::string& json) {
    PostCommand([this, json]{
        const auto seq = command_seq_.fetch_add(1) + 1;
        const uint64_t started_ms = utils::nowMs();
        EnsureOrchestrator();
        const std::string command = ExtractJsonString(json, "command");
        AppendLog("[CMD] #" + std::to_string(seq) + " queued" + (command.empty() ? std::string() : std::string(": ") + command));
        HunterOrchestrator* orchestrator = nullptr;
        {
            std::lock_guard<std::mutex> lk(orchestrator_mutex_);
            orchestrator = orchestrator_.get();
        }
        if (!orchestrator) {
            AppendLog("[ERR] #" + std::to_string(seq) + " orchestrator unavailable");
            return;
        }
        std::lock_guard<std::mutex> call_lock(orchestrator_call_mutex_);
        auto resp = orchestrator->processRealtimeCommand(json);
        bool ok = JsonSuccess(resp);
        std::string msg = JsonMessage(resp);
        const uint64_t elapsed_ms = utils::nowMs() - started_ms;
        AppendLog(std::string(ok ? "[CMD] #" : "[ERR] #") + std::to_string(seq) +
                  " finished in " + std::to_string(elapsed_ms) + " ms: " +
                  (msg.empty() ? "done" : msg));
        { std::lock_guard<std::mutex> lk(result_mutex_); cmd_results_.push_back({ok, msg}); }
    });
}

void ImGuiApp::DrainCommandResults() {
    std::lock_guard<std::mutex> lk(result_mutex_);
    while (!cmd_results_.empty()) {
        auto r = std::move(cmd_results_.front());
        cmd_results_.pop_front();
        if (!r.message.empty()) SetToast(r.message, r.ok ? ToastKind::Success : ToastKind::Error, 3000);
    }
}

// ── Real-time log consumer ──
void ImGuiApp::ConsumeNewLogs() {
    auto fresh = utils::LogRingBuffer::instance().fetchSince(log_generation_, 500);
    if (fresh.empty()) return;
    for (auto& l : fresh) ui_logs_.push_back(std::move(l));
    if (ui_logs_.size() > MAX_UI_LOGS)
        ui_logs_.erase(ui_logs_.begin(), ui_logs_.begin() + (int)(ui_logs_.size() - MAX_UI_LOGS));
}

// ── Create / Run ──
bool ImGuiApp::Create(HINSTANCE hInstance, int nCmdShow) {
    hinstance_ = hInstance;
    LoadConfig();
    EnsureOrchestrator();

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc); wc.style = CS_CLASSDC;
    wc.lpfnWndProc = WindowProc; wc.hInstance = hinstance_;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"HunterCensorWindow";
    if (!RegisterClassExW(&wc)) return false;

    hwnd_ = CreateWindowW(wc.lpszClassName, L"huntercensor",
        WS_OVERLAPPEDWINDOW, 100, 100, 1440, 920,
        nullptr, nullptr, hinstance_, this);
    if (!hwnd_) return false;

    if (!CreateDeviceD3D()) {
        CleanupDeviceD3D(); DestroyWindow(hwnd_); hwnd_ = nullptr; return false;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = "runtime/huntercensor_imgui.ini";

    dpi_scale_ = QueryDpiScale();
    SetupStyle();
    ImGui_ImplWin32_Init(hwnd_);
    ImGui_ImplDX9_Init(d3d_device_);
    ReloadFonts();

    ShowWindow(hwnd_, nCmdShow == 0 ? SW_SHOWDEFAULT : nCmdShow);
    UpdateWindow(hwnd_);
    StartBackgroundWorker();
    AppendLog("[UI] huntercensor ready");
    return true;
}

int ImGuiApp::Run() {
    MSG msg{};
    while (msg.message != WM_QUIT) {
        while (PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            TranslateMessage(&msg); DispatchMessage(&msg);
        }
        if (msg.message == WM_QUIT) break;

        ConsumeNewLogs();
        DrainCommandResults();
        if (discovery_apply_requested_.exchange(false)) {
            security::DpiEvasionOrchestrator::NetworkDiscoveryResult copy;
            bool has_result = false;
            {
                std::lock_guard<std::mutex> lk(data_mutex_);
                if (has_discovery_result_) {
                    copy = discovery_result_;
                    has_result = true;
                }
            }
            if (has_result) ApplyDiscoveryToEdgeInputs(copy);
        }
        if (pending_resize_) {
            pending_resize_ = false;
            d3dpp_.BackBufferWidth = pending_resize_width_;
            d3dpp_.BackBufferHeight = pending_resize_height_;
            ResetDevice();
        }

        ImGui_ImplDX9_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        DrawFrame();
        ImGui::EndFrame();

        d3d_device_->SetRenderState(D3DRS_ZENABLE, FALSE);
        d3d_device_->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        d3d_device_->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
        d3d_device_->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER,
                           D3DCOLOR_RGBA(18, 18, 23, 255), 1.0f, 0);
        if (d3d_device_->BeginScene() >= 0) {
            ImGui::Render();
            ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
            d3d_device_->EndScene();
        }
        HRESULT hr = d3d_device_->Present(nullptr, nullptr, nullptr, nullptr);
        if (hr == D3DERR_DEVICELOST && d3d_device_->TestCooperativeLevel() == D3DERR_DEVICENOTRESET)
            ResetDevice();
    }
    return (int)msg.wParam;
}

// ── D3D9 boilerplate ──
bool ImGuiApp::CreateDeviceD3D() {
    d3d_ = Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d_) return false;
    ZeroMemory(&d3dpp_, sizeof(d3dpp_));
    d3dpp_.Windowed = TRUE; d3dpp_.SwapEffect = D3DSWAPEFFECT_DISCARD;
    d3dpp_.BackBufferFormat = D3DFMT_UNKNOWN;
    d3dpp_.EnableAutoDepthStencil = TRUE; d3dpp_.AutoDepthStencilFormat = D3DFMT_D16;
    d3dpp_.PresentationInterval = D3DPRESENT_INTERVAL_ONE;
    HRESULT hr = d3d_->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd_,
        D3DCREATE_HARDWARE_VERTEXPROCESSING, &d3dpp_, &d3d_device_);
    if (FAILED(hr))
        hr = d3d_->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd_,
            D3DCREATE_SOFTWARE_VERTEXPROCESSING, &d3dpp_, &d3d_device_);
    return SUCCEEDED(hr);
}

void ImGuiApp::CleanupDeviceD3D() {
    if (d3d_device_) { d3d_device_->Release(); d3d_device_ = nullptr; }
    if (d3d_) { d3d_->Release(); d3d_ = nullptr; }
}

void ImGuiApp::ResetDevice() {
    if (!d3d_device_) return;
    ImGui_ImplDX9_InvalidateDeviceObjects();
    if (d3d_device_->Reset(&d3dpp_) != D3DERR_INVALIDCALL)
        ImGui_ImplDX9_CreateDeviceObjects();
}

// ── Config persistence ──
void ImGuiApp::LoadConfig() {
    if (utils::fileExists(config_path_)) config_.loadFromFile(config_path_);
    else config_.saveToFile(config_path_);
    SyncBuffersFromConfig();
}

void ImGuiApp::SyncBuffersFromConfig() {
    max_total_ = config_.maxTotal(); max_workers_ = config_.maxWorkers();
    scan_limit_ = config_.scanLimit(); sleep_seconds_ = config_.sleepSeconds();
    telegram_limit_ = config_.telegramLimit();
    telegram_timeout_ms_ = config_.getInt("telegram_timeout_ms", 12000);
    telegram_enabled_ = config_.getBool("telegram_enabled", false);
    CopyBuf(config_.xrayPath(), xray_path_.data(), xray_path_.size());
    CopyBuf(config_.singBoxPath(), singbox_path_.data(), singbox_path_.size());
    CopyBuf(config_.mihomoPath(), mihomo_path_.data(), mihomo_path_.size());
    CopyBuf(config_.torPath(), tor_path_.data(), tor_path_.size());
    CopyBuf(config_.getString("telegram_api_id",""), telegram_api_id_.data(), telegram_api_id_.size());
    CopyBuf(config_.getString("telegram_api_hash",""), telegram_api_hash_.data(), telegram_api_hash_.size());
    CopyBuf(config_.getString("telegram_phone",""), telegram_phone_.data(), telegram_phone_.size());
    CopyBuf(JoinLines(config_.telegramTargets()), telegram_targets_.data(), telegram_targets_.size());
    CopyBuf(JoinLines(config_.githubUrls()), github_urls_.data(), github_urls_.size());
    CopyBuf(config_.getString("suggested_iface",""), edge_iface_.data(), edge_iface_.size());
    snapshot_config_limit_ = config_limit_;
    CopyBuf("runtime/HUNTER_config_db_export.txt", export_path_.data(), export_path_.size());
}

void ImGuiApp::SaveConfigToDisk() { config_.saveToFile(config_path_); }

// ── Orchestrator lifecycle ──
void ImGuiApp::EnsureOrchestrator() {
    std::lock_guard<std::mutex> lk(orchestrator_mutex_);
    if (!orchestrator_) orchestrator_ = std::make_unique<HunterOrchestrator>(config_);
}

void ImGuiApp::StartHunter() {
    if (orchestrator_running_.load()) { AppendLog("[UI] Already running"); return; }
    EnsureOrchestrator();
    JoinThread(orchestrator_thread_);
    AppendLog("[UI] Start requested");
    orchestrator_running_ = true;
    orchestrator_thread_ = std::thread([this]{
        try {
            HunterOrchestrator* orchestrator = nullptr;
            {
                std::lock_guard<std::mutex> lk(orchestrator_mutex_);
                orchestrator = orchestrator_.get();
            }
            if (orchestrator) orchestrator->start();
        }
        catch (const std::exception& e) { AppendLog(std::string("[ERR] ") + e.what()); }
        catch (...) { AppendLog("[ERR] Unknown orchestrator error"); }
        orchestrator_running_ = false;
        AppendLog("[UI] Orchestrator thread exited");
    });
    AppendLog("[UI] Hunter started");
}

void ImGuiApp::StopHunter() {
    AppendLog("[UI] Stop requested");
    {
        std::lock_guard<std::mutex> lk(orchestrator_mutex_);
        if (orchestrator_) { try { orchestrator_->stop(); } catch (...) {} }
    }
    JoinThread(orchestrator_thread_);
    orchestrator_running_ = false;
    AppendLog("[UI] Hunter stopped");
}

// ── Logging ──
void ImGuiApp::AppendLog(const std::string& line) {
    utils::LogRingBuffer::instance().push(line);
}

void ImGuiApp::AppendDiscoveryLog(const std::string& line) {
    const std::string stamped = TimestampLogLine(line);
    { std::lock_guard<std::mutex> lk(data_mutex_);
      discovery_logs_.push_back(stamped);
      if (discovery_logs_.size() > 500)
          discovery_logs_.erase(discovery_logs_.begin(), discovery_logs_.begin() + (int)(discovery_logs_.size()-500));
    }
    AppendLog(stamped);
}

void ImGuiApp::SetToast(const std::string& text, ToastKind kind, uint64_t duration_ms) {
    toast_text_ = text;
    toast_kind_ = kind;
    toast_until_ms_ = utils::nowMs() + duration_ms;
}

// ── Async operations ──
void ImGuiApp::RunProbeAsync() {
    if (probe_in_flight_.exchange(true)) return;
    EnsureOrchestrator();
    JoinThread(probe_thread_);
    AppendLog("[UI] Starting censorship probe");
    probe_thread_ = std::thread([this]{
        try {
            const uint64_t started_ms = utils::nowMs();
            security::DpiEvasionOrchestrator::DetailedProbeResult r;
            hunter::security::DpiEvasionOrchestrator* dpi = nullptr;
            {
                std::lock_guard<std::mutex> lk(orchestrator_mutex_);
                if (orchestrator_) dpi = orchestrator_->dpiEvasion();
            }
            if (dpi) {
                std::lock_guard<std::mutex> call_lock(orchestrator_call_mutex_);
                r = dpi->probeWithDetails();
                AppendLog("[UI] Probe done in " + std::to_string(utils::nowMs() - started_ms) +
                          " ms, strategy=" + r.recommended_strategy +
                          ", steps=" + std::to_string(r.steps.size()));
            } else AppendLog("[UI] DPI unavailable");
            { std::lock_guard<std::mutex> lk(data_mutex_); probe_result_ = std::move(r); }
            has_probe_result_ = true;
        } catch (const std::exception& e) {
            AppendLog(std::string("[ERR] Probe thread exception: ") + e.what());
        } catch (...) {
            AppendLog("[ERR] Probe thread unknown exception");
        }
        probe_in_flight_ = false;
    });
}

void ImGuiApp::RunDiscoveryAsync() {
    if (discovery_in_flight_.exchange(true)) return;
    EnsureOrchestrator();
    { std::lock_guard<std::mutex> lk(data_mutex_); discovery_logs_.clear(); has_discovery_result_ = false; }
    JoinThread(discovery_thread_);
    AppendDiscoveryLog("[DISCOVERY] Starting native discovery");
    discovery_thread_ = std::thread([this]{
        try {
            const uint64_t started_ms = utils::nowMs();
            security::DpiEvasionOrchestrator::NetworkDiscoveryResult r;
            hunter::security::DpiEvasionOrchestrator* dpi = nullptr;
            {
                std::lock_guard<std::mutex> lk(orchestrator_mutex_);
                if (orchestrator_) dpi = orchestrator_->dpiEvasion();
            }
            if (dpi) {
                std::lock_guard<std::mutex> call_lock(orchestrator_call_mutex_);
                r = dpi->discoverExitIp([this](const std::string& l){ AppendDiscoveryLog(l); });
                AppendDiscoveryLog("[DISCOVERY] Exit IP: " + r.suggested_exit_ip);
                AppendDiscoveryLog("[DISCOVERY] Finished in " + std::to_string(utils::nowMs() - started_ms) + " ms");
            } else AppendDiscoveryLog("[DISCOVERY] DPI unavailable");
            {
                std::lock_guard<std::mutex> lk(data_mutex_);
                discovery_result_ = std::move(r);
            }
            has_discovery_result_ = true;
            discovery_apply_requested_ = true;
        } catch (const std::exception& e) {
            AppendLog(std::string("[ERR] Discovery thread exception: ") + e.what());
        } catch (...) {
            AppendLog("[ERR] Discovery thread unknown exception");
        }
        discovery_in_flight_ = false;
    });
}

void ImGuiApp::RunEdgeBypassAsync() {
    if (edge_bypass_in_flight_.exchange(true)) return;
    EnsureOrchestrator();
    security::DpiEvasionOrchestrator::NetworkDiscoveryResult discovered;
    {
        std::lock_guard<std::mutex> lk(data_mutex_);
        if (!has_discovery_result_) {
            AppendLog("[ERR] Run discovery first so the app can build the bypass target set from discovered network data");
            edge_bypass_in_flight_ = false;
            return;
        }
        discovered = discovery_result_;
    }
    ApplyDiscoveryToEdgeInputs(discovered);
    std::string mac = discovered.gateway_mac;
    std::string eip = discovered.suggested_exit_ip;
    std::string ifc = discovered.suggested_iface;
    if (eip.empty()) {
        AppendLog("[ERR] Discovery did not produce an exit target yet; run discovery again and inspect the discovery result");
        edge_bypass_in_flight_ = false;
        return;
    }
    JoinThread(edge_bypass_thread_);
    {
        std::lock_guard<std::mutex> lk(data_mutex_);
        edge_bypass_logs_.clear();
    }
    AppendLog("[UI] Edge bypass requested from discovered targets only: gateway_mac=" + (mac.empty() ? std::string("(unresolved)") : mac) + " exit_ip=" + eip + " iface=" + (ifc.empty() ? std::string("(auto)") : ifc));
    edge_bypass_thread_ = std::thread([this, mac, eip, ifc]{
        try {
            const uint64_t started_ms = utils::nowMs();
            bool ok = false;
            std::vector<std::string> edge_logs;
            std::string edge_status;
            hunter::security::DpiEvasionOrchestrator* dpi = nullptr;
            {
                std::lock_guard<std::mutex> lk(orchestrator_mutex_);
                if (orchestrator_) dpi = orchestrator_->dpiEvasion();
            }
            if (dpi) {
                std::lock_guard<std::mutex> call_lock(orchestrator_call_mutex_);
                ok = dpi->attemptEdgeRouterBypass(mac, eip, ifc.empty() ? "eth0" : ifc);
                edge_logs = dpi->getEdgeRouterBypassLog();
                edge_status = dpi->getEdgeRouterBypassStatus();
            } else {
                edge_status = "✗ DPI unavailable";
                edge_logs.push_back("[!] DPI orchestrator unavailable for edge bypass execution");
            }
            {
                std::lock_guard<std::mutex> lk(data_mutex_);
                edge_bypass_logs_.clear();
                edge_bypass_logs_.reserve(edge_logs.size());
                for (const auto& line : edge_logs) {
                    edge_bypass_logs_.push_back(TimestampLogLine(line));
                }
            }
            for (const auto& line : edge_logs) {
                AppendLog("[EDGE] " + line);
            }
            const bool local_only = edge_status.find("local host-route injection confirmed") != std::string::npos ||
                                    edge_status.find("local host-route evidence confirmed") != std::string::npos ||
                                    edge_status.find("local host-route") != std::string::npos;
            AppendLog(std::string(ok ? (local_only ? "[UI] Edge bypass produced local route evidence" : "[UI] Edge bypass active") : "[ERR] Edge bypass failed") +
                      " after " + std::to_string(utils::nowMs() - started_ms) + " ms :: " +
                      (edge_status.empty() ? "-" : edge_status));
        } catch (const std::exception& e) {
            AppendLog(std::string("[ERR] Edge bypass thread exception: ") + e.what());
        } catch (...) {
            AppendLog("[ERR] Edge bypass thread unknown exception");
        }
        edge_bypass_in_flight_ = false;
    });
}

void ImGuiApp::ApplyDiscoveryToEdgeInputs(const security::DpiEvasionOrchestrator::NetworkDiscoveryResult& result) {
    if (!result.gateway_mac.empty()) CopyBuf(result.gateway_mac, edge_target_mac_.data(), edge_target_mac_.size());
    if (!result.suggested_exit_ip.empty()) CopyBuf(result.suggested_exit_ip, edge_exit_ip_.data(), edge_exit_ip_.size());
    if (!result.suggested_iface.empty()) CopyBuf(result.suggested_iface, edge_iface_.data(), edge_iface_.size());
    AppendLog("[UI] Edge router inputs auto-filled from in-app discovery");
}

// ── Advanced settings (async) ──
void ImGuiApp::ApplyAdvancedSettings() {
    std::ostringstream cmd;
    cmd << "{\"command\":\"update_runtime_settings\""
        << ",\"xray_path\":\"" << JsonEscape(xray_path_.data()) << "\""
        << ",\"singbox_path\":\"" << JsonEscape(singbox_path_.data()) << "\""
        << ",\"mihomo_path\":\"" << JsonEscape(mihomo_path_.data()) << "\""
        << ",\"tor_path\":\"" << JsonEscape(tor_path_.data()) << "\""
        << ",\"max_total\":" << max_total_
        << ",\"max_workers\":" << max_workers_
        << ",\"scan_limit\":" << scan_limit_
        << ",\"sleep_seconds\":" << sleep_seconds_
        << ",\"multiproxy_port\":" << config_.multiproxyPort()
        << ",\"gemini_port\":" << config_.geminiPort()
        << ",\"telegram_enabled\":" << (telegram_enabled_ ? "true" : "false")
        << ",\"telegram_api_id\":\"" << JsonEscape(telegram_api_id_.data()) << "\""
        << ",\"telegram_api_hash\":\"" << JsonEscape(telegram_api_hash_.data()) << "\""
        << ",\"telegram_phone\":\"" << JsonEscape(telegram_phone_.data()) << "\""
        << ",\"telegram_limit\":" << telegram_limit_
        << ",\"telegram_timeout_ms\":" << telegram_timeout_ms_
        << ",\"targets_text\":\"" << JsonEscape(telegram_targets_.data()) << "\""
        << ",\"github_urls_text\":\"" << JsonEscape(github_urls_.data()) << "\""
        << "}";

    config_.set("xray_path", std::string(xray_path_.data()));
    config_.set("singbox_path", std::string(singbox_path_.data()));
    config_.set("mihomo_path", std::string(mihomo_path_.data()));
    config_.set("tor_path", std::string(tor_path_.data()));
    config_.set("max_total", max_total_); config_.set("max_workers", max_workers_);
    config_.set("scan_limit", scan_limit_); config_.set("sleep_seconds", sleep_seconds_);
    config_.set("telegram_enabled", telegram_enabled_);
    config_.set("telegram_api_id", std::string(telegram_api_id_.data()));
    config_.set("telegram_api_hash", std::string(telegram_api_hash_.data()));
    config_.set("telegram_phone", std::string(telegram_phone_.data()));
    config_.set("telegram_limit", telegram_limit_);
    config_.set("telegram_timeout_ms", telegram_timeout_ms_);
    config_.set("targets", BuildJsonStringArray(SplitLines(telegram_targets_.data())));
    config_.setGithubUrls(SplitLines(github_urls_.data()));
    SaveConfigToDisk();

    RunCommandAsync(cmd.str());
    SetToast("Settings saved", ToastKind::Success);
}

void ImGuiApp::ImportConfigsFromFile() {
    std::string path = import_path_.data();
    if (path.empty()) {
        path = OpenFileDialog("Text Files\0*.txt;*.json;*.conf\0All Files\0*.*\0");
        if (path.empty()) return;
        CopyBuf(path, import_path_.data(), import_path_.size());
    }
    RunCommandAsync("{\"command\":\"import_config_file\",\"path\":\"" + JsonEscape(path) + "\"}");
}

void ImGuiApp::ExportConfigsToFile() {
    std::string path = export_path_.data();
    if (path.empty()) {
        path = SaveFileDialog("Text Files\0*.txt\0All Files\0*.*\0", "txt");
        if (path.empty()) return;
        CopyBuf(path, export_path_.data(), export_path_.size());
    }
    RunCommandAsync("{\"command\":\"export_config_db\",\"path\":\"" + JsonEscape(path) + "\"}");
}

std::string ImGuiApp::RunRealtimeCommand(const std::string& json) {
    EnsureOrchestrator();
    HunterOrchestrator* orchestrator = nullptr;
    {
        std::lock_guard<std::mutex> lk(orchestrator_mutex_);
        orchestrator = orchestrator_.get();
    }
    if (!orchestrator) { AppendLog("[UI] Orchestrator unavailable"); return {}; }
    std::lock_guard<std::mutex> call_lock(orchestrator_call_mutex_);
    auto resp = orchestrator->processRealtimeCommand(json);
    auto ok = JsonSuccess(resp); auto msg = JsonMessage(resp);
    AppendLog(std::string(ok ? "[CMD] " : "[ERR] ") + (msg.empty() ? "done" : msg));
    return resp;
}

// ── JSON helpers ──
bool ImGuiApp::JsonSuccess(const std::string& j) const { return j.find("\"ok\":true") != std::string::npos; }
std::string ImGuiApp::JsonMessage(const std::string& j) const { return ExtractJsonString(j, "message"); }

std::string ImGuiApp::ExtractJsonString(const std::string& j, const std::string& key) const {
    std::string needle = "\"" + key + "\"";
    size_t kp = j.find(needle); if (kp == std::string::npos) return {};
    size_t col = j.find(':', kp + needle.size()); if (col == std::string::npos) return {};
    size_t fq = j.find('"', col + 1); if (fq == std::string::npos) return {};
    std::string out; bool esc = false;
    for (size_t i = fq+1; i < j.size(); ++i) {
        char c = j[i];
        if (!esc && c == '"') break;
        if (esc) { out.push_back(c=='n'?'\n':c=='r'?'\r':c=='t'?'\t':c); esc=false; }
        else if (c == '\\') esc = true;
        else out.push_back(c);
    }
    return out;
}

std::string ImGuiApp::JsonEscape(const std::string& v) const {
    std::string e; e.reserve(v.size()+16);
    for (char c : v) switch(c) {
        case '\\': e+="\\\\"; break; case '"': e+="\\\""; break;
        case '\n': e+="\\n"; break; case '\r': e+="\\r"; break;
        case '\t': e+="\\t"; break; default: e.push_back(c);
    }
    return e;
}

// ── File dialogs / buffer helpers ──
std::string ImGuiApp::OpenFileDialog(const char* filter) {
    char fn[MAX_PATH]={}; OPENFILENAMEA ofn{};
    ofn.lStructSize=sizeof(ofn); ofn.hwndOwner=hwnd_;
    ofn.lpstrFilter=filter; ofn.lpstrFile=fn; ofn.nMaxFile=MAX_PATH;
    ofn.Flags=OFN_PATHMUSTEXIST|OFN_FILEMUSTEXIST|OFN_HIDEREADONLY;
    return GetOpenFileNameA(&ofn) ? std::string(fn) : std::string();
}

std::string ImGuiApp::SaveFileDialog(const char* filter, const char* ext) {
    char fn[MAX_PATH]={}; OPENFILENAMEA ofn{};
    ofn.lStructSize=sizeof(ofn); ofn.hwndOwner=hwnd_;
    ofn.lpstrFilter=filter; ofn.lpstrFile=fn; ofn.nMaxFile=MAX_PATH;
    ofn.Flags=OFN_OVERWRITEPROMPT|OFN_PATHMUSTEXIST; ofn.lpstrDefExt=ext;
    return GetSaveFileNameA(&ofn) ? std::string(fn) : std::string();
}

void ImGuiApp::CopyBuf(const std::string& v, char* d, size_t sz) {
    if (!d||!sz) return; std::memset(d,0,sz);
    strncpy_s(d, sz, v.c_str(), _TRUNCATE);
}

std::string ImGuiApp::JoinLines(const std::vector<std::string>& lines) const {
    std::ostringstream o;
    for (size_t i=0; i<lines.size(); ++i) { if (i) o<<"\n"; o<<lines[i]; }
    return o.str();
}

std::vector<std::string> ImGuiApp::SplitLines(const char* t) const {
    if (!t) return {};
    std::vector<std::string> r; std::istringstream s(t); std::string l;
    while (std::getline(s, l)) { l=utils::trim(l); if (!l.empty()) r.push_back(l); }
    return r;
}

std::string ImGuiApp::BuildJsonStringArray(const std::vector<std::string>& v) const {
    std::ostringstream o; o<<"["; bool f=true;
    for (auto& x:v) { if (!f) o<<","; f=false; o<<"\""<<JsonEscape(x)<<"\""; }
    o<<"]"; return o.str();
}

std::string ImGuiApp::JoinLogLines(const std::vector<std::string>& values) const {
    std::ostringstream out;
    for (size_t i = 0; i < values.size(); ++i) {
        if (i > 0) out << "\r\n";
        out << values[i];
    }
    return out.str();
}

bool ImGuiApp::CopyTextToClipboard(const std::string& value) const {
    if (value.empty()) return false;
    int wide_len = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, nullptr, 0);
    if (wide_len <= 0) return false;
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, static_cast<SIZE_T>(wide_len) * sizeof(wchar_t));
    if (!memory) return false;
    wchar_t* buffer = static_cast<wchar_t*>(GlobalLock(memory));
    if (!buffer) {
        GlobalFree(memory);
        return false;
    }
    MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, buffer, wide_len);
    GlobalUnlock(memory);
    if (!OpenClipboard(hwnd_)) {
        GlobalFree(memory);
        return false;
    }
    EmptyClipboard();
    if (!SetClipboardData(CF_UNICODETEXT, memory)) {
        CloseClipboard();
        GlobalFree(memory);
        return false;
    }
    CloseClipboard();
    return true;
}

// ── Drawing helpers ──
void ImGuiApp::DrawCard(const char* label, const char* value) {
    ImGui::BeginGroup();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, COL_CARD);
    ImGui::BeginChild(label, ImVec2(0, 52*dpi_scale_), true);
    ImGui::TextDisabled("%s", label);
    ImGui::TextColored(COL_TEXT, "%s", value);
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::EndGroup();
}

void ImGuiApp::DrawLogLine(const std::string& line) const {
    ImVec4 col = COL_TEXT;
    if (IContains(line, "[ERR]") || IContains(line, "failed") || IContains(line, "error"))
        col = COL_RED;
    else if (IContains(line, "[CMD]"))
        col = COL_CYAN;
    else if (IContains(line, "[DISCOVERY]"))
        col = COL_GREEN;
    else if (IContains(line, "[UI]"))
        col = COL_YELLOW;
    else if (IContains(line, "warn"))
        col = {0.95f, 0.65f, 0.20f, 1.0f};
    ImGui::PushStyleColor(ImGuiCol_Text, col);
    ImGui::TextWrapped("%s", line.c_str());
    ImGui::PopStyleColor();
}

void ImGuiApp::DrawStatusBadge(bool running) {
    ImVec4 col = running ? COL_GREEN : COL_RED;
    const char* txt = running ? "RUNNING" : "STOPPED";
    ImGui::PushStyleColor(ImGuiCol_Button, col);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, col);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, col);
    ImGui::SmallButton(txt);
    ImGui::PopStyleColor(3);
}

// ── Main frame layout ──
void ImGuiApp::DrawFrame() {
    const ImVec2 disp = ImGui::GetIO().DisplaySize;

    ImGui::SetNextWindowPos({0,0}, ImGuiCond_Always);
    ImGui::SetNextWindowSize(disp, ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0,0});
    ImGui::Begin("##root", nullptr,
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
    ImGui::PopStyleVar();

    // Nav bar
    DrawNavBar();

    // Toast
    if (!toast_text_.empty() && utils::nowMs() < toast_until_ms_) {
        const float toast_w = std::min(440.0f * dpi_scale_, std::max(240.0f * dpi_scale_, disp.x * 0.40f));
        const float toast_x = std::max(12.0f * dpi_scale_, disp.x - toast_w - 16.0f * dpi_scale_);
        ImGui::SetCursorPos(ImVec2(toast_x, 92.0f * dpi_scale_));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ToastBg(toast_kind_));
        ImGui::BeginChild("##toast", ImVec2(toast_w, 46*dpi_scale_), true);
        ImGui::TextWrapped("%s", toast_text_.c_str());
        ImGui::EndChild();
        ImGui::PopStyleColor();
    } else if (!toast_text_.empty() && utils::nowMs() >= toast_until_ms_) {
        toast_text_.clear();
    }

    // Content area with padding
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {16*dpi_scale_, 12*dpi_scale_});
    ImGui::BeginChild("##content", {0,0}, false, ImGuiWindowFlags_AlwaysUseWindowPadding);
    ImGui::PopStyleVar();

    switch (page_) {
        case Page::Home:       DrawHomePage(); break;
        case Page::Configs:    DrawConfigsPage(); break;
        case Page::Censorship: DrawCensorshipPage(); break;
        case Page::Logs:       DrawLogsPage(); break;
        case Page::Advanced:   DrawAdvancedPage(); break;
    }

    ImGui::EndChild();
    ImGui::End();
}

// ── Navigation bar ──
void ImGuiApp::DrawNavBar() {
    Snapshot s;
    { std::lock_guard<std::mutex> lk(snap_mutex_); s = snap_; }

    const float bar_h = 88 * dpi_scale_;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.08f, 0.08f, 0.11f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {12*dpi_scale_, 8*dpi_scale_});
    ImGui::BeginChild("##navbar", ImVec2(0, bar_h), true);
    ImGui::PopStyleVar();

    ImGui::PushStyleColor(ImGuiCol_Text, COL_ACCENT);
    ImGui::Text("huntercensor");
    ImGui::PopStyleColor();
    ImGui::SameLine(0, 12*dpi_scale_);
    DrawStatusBadge(orchestrator_running_.load());
    ImGui::SameLine(0, 12*dpi_scale_);
    ImGui::TextColored(COL_DIM, "Alive %d/%d  Avg %.0fms  Cycles %d",
        s.db_alive, s.db_total, s.db_avg_latency, s.cycle_count);
    ImGui::Separator();

    const Page pages[] = {Page::Home, Page::Configs, Page::Censorship, Page::Logs, Page::Advanced};
    for (auto p : pages) {
        bool sel = (page_ == p);
        if (sel) {
            ImGui::PushStyleColor(ImGuiCol_Button, COL_ACCENT);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, COL_ACCENT);
        }
        if (ImGui::Button(PageLabel(p), {0, 30*dpi_scale_})) page_ = p;
        if (sel) ImGui::PopStyleColor(2);
        ImGui::SameLine(0, 4*dpi_scale_);
    }

    bool running = orchestrator_running_.load();
    if (running) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.70f,0.18f,0.18f,1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.85f,0.22f,0.22f,1.0f));
        if (ImGui::Button("Stop", {64*dpi_scale_, 30*dpi_scale_})) StopHunter();
        ImGui::PopStyleColor(2);
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.14f,0.62f,0.30f,1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.18f,0.72f,0.36f,1.0f));
        if (ImGui::Button("Start", {64*dpi_scale_, 30*dpi_scale_})) StartHunter();
        ImGui::PopStyleColor(2);
    }
    ImGui::SameLine(0, 4*dpi_scale_);
    if (ImGui::Button("Cycle", {56*dpi_scale_, 30*dpi_scale_}))
        RunCommandAsync("{\"command\":\"run_cycle\"}");

    ImGui::EndChild();
    ImGui::PopStyleColor();
}

// ── Home page ──
void ImGuiApp::DrawHomePage() {
    Snapshot s;
    { std::lock_guard<std::mutex> lk(snap_mutex_); s = snap_; }

    ImGui::TextColored(COL_ACCENT, "Overview");
    ImGui::Spacing();

    // Stats cards row
    if (ImGui::BeginTable("##stats", 4, ImGuiTableFlags_SizingStretchSame, ImVec2(0, 0))) {
        ImGui::TableNextRow();
        char buf[64];

        ImGui::TableNextColumn();
        snprintf(buf, sizeof(buf), "%d", s.cycle_count);
        DrawCard("Cycles", buf);

        ImGui::TableNextColumn();
        snprintf(buf, sizeof(buf), "%d", s.db_alive);
        DrawCard("Alive", buf);

        ImGui::TableNextColumn();
        snprintf(buf, sizeof(buf), "%d", s.db_total);
        DrawCard("Total", buf);

        ImGui::TableNextColumn();
        snprintf(buf, sizeof(buf), "%.0f ms", s.db_avg_latency);
        DrawCard("Avg Latency", buf);

        ImGui::EndTable();
    }

    ImGui::Spacing();
    if (ImGui::BeginTable("##sys", 4, ImGuiTableFlags_SizingStretchSame)) {
        ImGui::TableNextRow();
        char buf[64];

        ImGui::TableNextColumn();
        snprintf(buf, sizeof(buf), "%d cores", s.hardware.cpu_count);
        DrawCard("CPU", buf);

        ImGui::TableNextColumn();
        snprintf(buf, sizeof(buf), "%.1f / %.1f GB", s.hardware.ram_used_gb, s.hardware.ram_total_gb);
        DrawCard("RAM", buf);

        ImGui::TableNextColumn();
        DrawCard("Mode", ModeLbl(s.hardware.mode));

        ImGui::TableNextColumn();
        DrawCard("Speed", s.speed_profile.profile_name.empty() ? "-" : s.speed_profile.profile_name.c_str());

        ImGui::EndTable();
    }

    ImGui::Spacing();
    ImGui::Spacing();
    ImGui::TextColored(COL_ACCENT, "Healthy Configs");
    ImGui::Spacing();

    const float avail_h = ImGui::GetContentRegionAvail().y - 8;
    if (ImGui::BeginTable("##healthy", 4,
        ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable,
        ImVec2(0, avail_h > 100 ? avail_h : 200)))
    {
        ImGui::TableSetupColumn("Latency", ImGuiTableColumnFlags_WidthFixed, 80*dpi_scale_);
        ImGui::TableSetupColumn("Engine",  ImGuiTableColumnFlags_WidthFixed, 90*dpi_scale_);
        ImGui::TableSetupColumn("Last OK", ImGuiTableColumnFlags_WidthFixed, 130*dpi_scale_);
        ImGui::TableSetupColumn("URI");
        ImGui::TableHeadersRow();
        for (auto& r : s.healthy_records) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextColored(r.latency_ms < 500 ? COL_GREEN : r.latency_ms < 1500 ? COL_YELLOW : COL_RED,
                "%.0f", r.latency_ms);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(r.engine_used.empty() ? "-" : r.engine_used.c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(FmtTs(r.last_alive_time).c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(Trim(r.uri, 200).c_str());
        }
        ImGui::EndTable();
    }
    if (s.healthy_records.empty()) {
        ImGui::TextColored(COL_DIM, "No healthy configs yet. Start the engine or run a cycle to populate this view.");
    }
}

// ── Configs page ──
void ImGuiApp::DrawConfigsPage() {
    Snapshot s;
    { std::lock_guard<std::mutex> lk(snap_mutex_); s = snap_; }

    ImGui::TextColored(COL_ACCENT, "Config Database");
    ImGui::Spacing();

    // Controls row
    ImGui::Checkbox("Alive only", &show_alive_only_);
    ImGui::SameLine(0, 16*dpi_scale_);
    ImGui::SetNextItemWidth(100*dpi_scale_);
    ImGui::InputInt("##limit", &config_limit_);
    if (config_limit_ < 10) config_limit_ = 10;
    snapshot_config_limit_ = config_limit_;
    ImGui::SameLine(0, 16*dpi_scale_);
    if (ImGui::Button("Load Raw")) RunCommandAsync("{\"command\":\"load_raw_files\"}");
    ImGui::SameLine(0, 4*dpi_scale_);
    if (ImGui::Button("Load Bundle")) RunCommandAsync("{\"command\":\"load_bundle_files\"}");

    ImGui::Spacing();

    // Import/Export section
    if (ImGui::CollapsingHeader("Import / Export", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Indent(8*dpi_scale_);
        if (ImGui::Button("Browse##imp")) {
            auto p = OpenFileDialog("Text Files\0*.txt;*.json;*.conf\0All\0*.*\0");
            if (!p.empty()) CopyBuf(p, import_path_.data(), import_path_.size());
        }
        ImGui::SameLine(); ImGui::SetNextItemWidth(-1);
        ImGui::InputText("##imppath", import_path_.data(), import_path_.size());
        if (ImGui::Button("Import")) ImportConfigsFromFile();

        ImGui::Spacing();
        if (ImGui::Button("Browse##exp")) {
            auto p = SaveFileDialog("Text\0*.txt\0All\0*.*\0", "txt");
            if (!p.empty()) CopyBuf(p, export_path_.data(), export_path_.size());
        }
        ImGui::SameLine(); ImGui::SetNextItemWidth(-1);
        ImGui::InputText("##exppath", export_path_.data(), export_path_.size());
        if (ImGui::Button("Export")) ExportConfigsToFile();
        ImGui::Unindent(8*dpi_scale_);
    }

    // Manual add
    if (ImGui::CollapsingHeader("Manual Add")) {
        ImGui::Indent(8*dpi_scale_);
        ImGui::InputTextMultiline("##manual", manual_configs_.data(), manual_configs_.size(),
            ImVec2(-1, 80*dpi_scale_));
        if (ImGui::Button("Add Configs")) {
            RunCommandAsync("{\"command\":\"add_configs\",\"configs\":\"" + JsonEscape(manual_configs_.data()) + "\"}");
            manual_configs_[0] = 0;
        }
        ImGui::Unindent(8*dpi_scale_);
    }

    ImGui::Spacing();

    // Table
    const auto& rows = show_alive_only_ ? s.healthy_records : s.all_records;
    const float avail_h = ImGui::GetContentRegionAvail().y - 4;
    if (ImGui::BeginTable("##cfgtbl", 7,
        ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY,
        ImVec2(0, avail_h > 100 ? avail_h : 200)))
    {
        ImGui::TableSetupColumn("OK",   ImGuiTableColumnFlags_WidthFixed, 40*dpi_scale_);
        ImGui::TableSetupColumn("Latency",   ImGuiTableColumnFlags_WidthFixed, 60*dpi_scale_);
        ImGui::TableSetupColumn("Engine",  ImGuiTableColumnFlags_WidthFixed, 80*dpi_scale_);
        ImGui::TableSetupColumn("Tests",ImGuiTableColumnFlags_WidthFixed, 50*dpi_scale_);
        ImGui::TableSetupColumn("Passes", ImGuiTableColumnFlags_WidthFixed, 50*dpi_scale_);
        ImGui::TableSetupColumn("Tag",  ImGuiTableColumnFlags_WidthFixed, 80*dpi_scale_);
        ImGui::TableSetupColumn("URI");
        ImGui::TableHeadersRow();
        for (auto& r : rows) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextColored(r.alive ? COL_GREEN : COL_RED, r.alive ? "Y" : "N");
            ImGui::TableNextColumn(); ImGui::Text("%.0f", r.latency_ms);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(r.engine_used.empty() ? "-" : r.engine_used.c_str());
            ImGui::TableNextColumn(); ImGui::Text("%d", r.total_tests);
            ImGui::TableNextColumn(); ImGui::Text("%d", r.total_passes);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(r.tag.empty() ? "-" : r.tag.c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(Trim(r.uri, 200).c_str());
        }
        ImGui::EndTable();
    }
    if (rows.empty()) {
        ImGui::TextColored(COL_DIM, "No configs match the current filter yet.");
    }
}

// ── Censorship page ──
void ImGuiApp::DrawCensorshipPage() {
    Snapshot s;
    { std::lock_guard<std::mutex> lk(snap_mutex_); s = snap_; }

    ImGui::TextColored(COL_ACCENT, "Censorship Lab");
    ImGui::Spacing();

    // Action buttons
    {
        bool probing = probe_in_flight_.load();
        if (probing) ImGui::BeginDisabled();
        if (ImGui::Button("Probe Censorship")) RunProbeAsync();
        if (probing) ImGui::EndDisabled();
    }
    ImGui::SameLine(0, 8*dpi_scale_);
    {
        bool disc = discovery_in_flight_.load();
        if (disc) ImGui::BeginDisabled();
        if (ImGui::Button("Discover Exit IP")) RunDiscoveryAsync();
        if (disc) ImGui::EndDisabled();
    }

    ImGui::Spacing();
    ImGui::TextColored(COL_DIM, "Network: %s   Strategy: %s   Pressure: %s",
        s.dpi_metrics.network_type.c_str(), s.dpi_metrics.strategy.c_str(), s.dpi_metrics.pressure_level.c_str());
    ImGui::TextColored(COL_DIM, "This page shows raw discovery values and raw bypass execution logs.");

    ImGui::Spacing();
    ImGui::TextColored(COL_ACCENT, "Workflow");
    if (ImGui::BeginTable("##censorship_workflow", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders, ImVec2(0, 0))) {
        ImGui::TableSetupColumn("Step", ImGuiTableColumnFlags_WidthFixed, 52*dpi_scale_);
        ImGui::TableSetupColumn("Goal", ImGuiTableColumnFlags_WidthFixed, 150*dpi_scale_);
        ImGui::TableSetupColumn("What to click", ImGuiTableColumnFlags_WidthFixed, 170*dpi_scale_);
        ImGui::TableSetupColumn("What you should inspect");
        ImGui::TableHeadersRow();
        auto workflow_row = [](const char* step, const char* goal, const char* action, const char* inspect) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(step);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(goal);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(action);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(inspect);
        };
        workflow_row("1", "Measure censorship", "Probe Censorship", "Check the recommended strategy and every probe detail row.");
        workflow_row("2", "Discover edge path", "Discover Exit IP", "Check raw local IP, gateway, gateway MAC, interface and suggested exit IP.");
        workflow_row("3", "Review route evidence", "Read Discovery Result + Trace Hops", "Confirm where traffic exits domestic hops and where the first international hop appears.");
        workflow_row("4", "Execute bypass", "Execute Bypass", "Watch the exact delivery attempt, payload mode and verification probes in the edge log.");
        workflow_row("5", "Decide if it worked", "Review status + Edge Router Bypass Log", "Look for verification probe success or fallback attempts; if all probes fail, treat bypass as not confirmed.");
        ImGui::EndTable();
    }

    // Probe result
    if (has_probe_result_) {
        ImGui::Spacing();
        ImGui::TextColored(COL_ACCENT, "Probe Result");
        std::lock_guard<std::mutex> lk(data_mutex_);
        ImGui::Text("Strategy: %s  Duration: %d ms", probe_result_.recommended_strategy.c_str(), probe_result_.total_duration_ms);
        if (ImGui::BeginTable("##probe", 5,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY, ImVec2(0, 160*dpi_scale_)))
        {
            ImGui::TableSetupColumn("Target");
            ImGui::TableSetupColumn("Category", ImGuiTableColumnFlags_WidthFixed, 80*dpi_scale_);
            ImGui::TableSetupColumn("Method",   ImGuiTableColumnFlags_WidthFixed, 90*dpi_scale_);
            ImGui::TableSetupColumn("OK",       ImGuiTableColumnFlags_WidthFixed, 36*dpi_scale_);
            ImGui::TableSetupColumn("Detail");
            ImGui::TableHeadersRow();
            for (auto& step : probe_result_.steps) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::TextUnformatted(step.target.c_str());
                ImGui::TableNextColumn(); ImGui::TextUnformatted(step.category.c_str());
                ImGui::TableNextColumn(); ImGui::TextUnformatted(step.method.c_str());
                ImGui::TableNextColumn(); ImGui::TextColored(step.success?COL_GREEN:COL_RED, step.success?"Y":"N");
                ImGui::TableNextColumn(); ImGui::TextUnformatted(Trim(step.detail,120).c_str());
            }
            ImGui::EndTable();
        }
    }

    // Discovery result
    if (has_discovery_result_) {
        ImGui::Spacing();
        ImGui::TextColored(COL_ACCENT, "Discovery Result");
        std::lock_guard<std::mutex> lk(data_mutex_);
        if (ImGui::BeginTable("##disc", 2, ImGuiTableFlags_RowBg|ImGuiTableFlags_Borders, ImVec2(0,0))) {
            auto row = [](const char* k, const std::string& v) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::TextColored(COL_DIM, "%s", k);
                ImGui::TableNextColumn(); ImGui::TextUnformatted(v.empty() ? "-" : v.c_str());
            };
            ImGui::TableSetupColumn("Key", ImGuiTableColumnFlags_WidthFixed, 140*1.0f);
            ImGui::TableSetupColumn("Value");
            row("Local IP",     discovery_result_.local_ip);
            row("Gateway",      discovery_result_.default_gateway);
            row("Gateway MAC",  discovery_result_.gateway_mac);
            row("Public IP",    discovery_result_.public_ip);
            row("ISP",          discovery_result_.isp_info);
            row("Interface",    discovery_result_.suggested_iface);
            row("Exit IP",      discovery_result_.suggested_exit_ip);
            char dur[32]; snprintf(dur, sizeof(dur), "%d ms", discovery_result_.total_duration_ms);
            row("Duration", dur);
            ImGui::EndTable();
        }

        if (!discovery_result_.trace_hops.empty()) {
            ImGui::Spacing();
            ImGui::TextColored(COL_ACCENT, "Trace Hops");
            if (ImGui::BeginTable("##trace_hops", 5,
                ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY, ImVec2(0, 150*dpi_scale_)))
            {
                ImGui::TableSetupColumn("TTL", ImGuiTableColumnFlags_WidthFixed, 42*dpi_scale_);
                ImGui::TableSetupColumn("IP", ImGuiTableColumnFlags_WidthFixed, 150*dpi_scale_);
                ImGui::TableSetupColumn("Latency", ImGuiTableColumnFlags_WidthFixed, 72*dpi_scale_);
                ImGui::TableSetupColumn("Zone", ImGuiTableColumnFlags_WidthFixed, 92*dpi_scale_);
                ImGui::TableSetupColumn("ASN / Host");
                ImGui::TableHeadersRow();
                for (const auto& hop : discovery_result_.trace_hops) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn(); ImGui::Text("%d", hop.ttl);
                    ImGui::TableNextColumn(); ImGui::TextUnformatted(hop.ip.empty() ? "-" : hop.ip.c_str());
                    ImGui::TableNextColumn();
                    if (hop.latency_ms >= 0) ImGui::Text("%d ms", hop.latency_ms);
                    else ImGui::TextUnformatted("timeout");
                    ImGui::TableNextColumn(); ImGui::TextUnformatted(hop.is_domestic ? "domestic" : "international");
                    ImGui::TableNextColumn();
                    const std::string tail = !hop.asn_info.empty() ? hop.asn_info : hop.hostname;
                    ImGui::TextUnformatted(tail.empty() ? "-" : tail.c_str());
                }
                ImGui::EndTable();
            }
        }
    }

    // Discovery log
    ImGui::Spacing();
    ImGui::TextColored(COL_ACCENT, "Discovery Log");
    ImGui::SameLine();
    if (ImGui::SmallButton("Copy Logs##disc")) {
        std::vector<std::string> discovery_copy;
        {
            std::lock_guard<std::mutex> lk(data_mutex_);
            discovery_copy = discovery_logs_;
        }
        const bool copied = CopyTextToClipboard(JoinLogLines(discovery_copy));
        SetToast(copied ? "Discovery logs copied" : "Failed to copy discovery logs",
                 copied ? ToastKind::Success : ToastKind::Error);
        AppendLog(copied ? "[UI] Discovery logs copied to clipboard" : "[ERR] Discovery logs copy failed");
    }
    ImGui::PushStyleColor(ImGuiCol_ChildBg, COL_CARD);
    if (ImGui::BeginChild("##disclog", ImVec2(0, 190*dpi_scale_), true)) {
        std::lock_guard<std::mutex> lk(data_mutex_);
        for (auto& l : discovery_logs_) DrawLogLine(l);
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) ImGui::SetScrollHereY(1.0f);
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();

    // Edge bypass
    ImGui::Spacing();
    if (ImGui::CollapsingHeader("Edge Router Bypass")) {
        ImGui::Indent(8*dpi_scale_);
        ImGui::TextColored(COL_DIM, "This action is discovery-driven only. Manual MAC/IP/interface entry is disabled.");
        if (has_discovery_result_) {
            std::lock_guard<std::mutex> lk(data_mutex_);
            if (ImGui::BeginTable("##edge_auto_targets", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders, ImVec2(0, 0))) {
                ImGui::TableSetupColumn("Key", ImGuiTableColumnFlags_WidthFixed, 140*dpi_scale_);
                ImGui::TableSetupColumn("Discovered value");
                auto edge_row = [](const char* key, const std::string& value) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn(); ImGui::TextColored(COL_DIM, "%s", key);
                    ImGui::TableNextColumn(); ImGui::TextUnformatted(value.empty() ? "-" : value.c_str());
                };
                edge_row("Gateway MAC", discovery_result_.gateway_mac);
                edge_row("Exit IP", discovery_result_.suggested_exit_ip);
                edge_row("Interface", discovery_result_.suggested_iface);
                edge_row("Gateway", discovery_result_.default_gateway);
                edge_row("Local IP", discovery_result_.local_ip);
                edge_row("ISP", discovery_result_.isp_info);
                ImGui::EndTable();
            }
        } else {
            ImGui::TextColored(COL_DIM, "Run Discover Exit IP first. The app will then use only discovered targets.");
        }
        ImGui::TextColored(COL_DIM, "Step 4: run discovery, review the discovered targets above, then execute the bypass and inspect the raw verification log below.");
        {
            bool busy = edge_bypass_in_flight_.load();
            bool ready = has_discovery_result_;
            if (busy || !ready) ImGui::BeginDisabled();
            if (ImGui::Button("Execute Bypass")) RunEdgeBypassAsync();
            if (busy || !ready) ImGui::EndDisabled();
        }
        ImGui::TextColored(COL_DIM, "Status: %s", s.edge_bypass_status.empty() ? "-" : s.edge_bypass_status.c_str());
        ImGui::TextColored(COL_ACCENT, "Edge Router Bypass Log");
        ImGui::SameLine();
        if (ImGui::SmallButton("Copy Logs##edge")) {
            std::vector<std::string> edge_copy;
            {
                std::lock_guard<std::mutex> lk(data_mutex_);
                edge_copy = edge_bypass_logs_;
            }
            const bool copied = CopyTextToClipboard(JoinLogLines(edge_copy));
            SetToast(copied ? "Edge bypass logs copied" : "Failed to copy edge bypass logs",
                     copied ? ToastKind::Success : ToastKind::Error);
            AppendLog(copied ? "[UI] Edge bypass logs copied to clipboard" : "[ERR] Edge bypass logs copy failed");
        }
        ImGui::PushStyleColor(ImGuiCol_ChildBg, COL_CARD);
        if (ImGui::BeginChild("##edgebypasslog", ImVec2(0, 200*dpi_scale_), true)) {
            std::lock_guard<std::mutex> lk(data_mutex_);
            if (edge_bypass_logs_.empty()) {
                ImGui::TextColored(COL_DIM, "No edge bypass log yet. Run Discover Exit IP, review the discovered targets, then click Execute Bypass.");
            } else {
                for (const auto& line : edge_bypass_logs_) DrawLogLine(line);
                if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) ImGui::SetScrollHereY(1.0f);
            }
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();
        ImGui::Unindent(8*dpi_scale_);
    }
}

// ── Logs page ──
void ImGuiApp::DrawLogsPage() {
    ImGui::TextColored(COL_ACCENT, "Real-time Logs");
    ImGui::Spacing();

    // Controls
    ImGui::Checkbox("Auto-scroll", &auto_scroll_logs_);
    ImGui::SameLine(0, 16*dpi_scale_);
    ImGui::Checkbox("Errors only", &show_only_errors_);
    ImGui::SameLine(0, 16*dpi_scale_);
    if (ImGui::Button("Copy Logs")) {
        std::vector<std::string> visible_logs;
        visible_logs.reserve(ui_logs_.size());
        for (const auto& line : ui_logs_) {
            if (show_only_errors_ && !IContains(line, "[ERR]") && !IContains(line, "error") && !IContains(line, "failed")) {
                continue;
            }
            visible_logs.push_back(line);
        }
        const bool copied = CopyTextToClipboard(JoinLogLines(visible_logs));
        SetToast(copied ? "Logs copied" : "Failed to copy logs",
                 copied ? ToastKind::Success : ToastKind::Error);
        AppendLog(copied ? "[UI] Logs copied to clipboard" : "[ERR] Logs copy failed");
    }
    ImGui::SameLine(0, 8*dpi_scale_);
    if (ImGui::Button("Clear")) { ui_logs_.clear(); }
    ImGui::SameLine();
    ImGui::TextColored(COL_DIM, "(%d lines)", (int)ui_logs_.size());

    ImGui::Spacing();

    // Log area
    const float avail_h = ImGui::GetContentRegionAvail().y - 4;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, COL_CARD);
    if (ImGui::BeginChild("##logarea", ImVec2(0, avail_h > 50 ? avail_h : 200), true)) {
        ImGuiListClipper clipper;
        // Build filtered indices for clipper
        std::vector<int> visible;
        if (show_only_errors_) {
            visible.reserve(ui_logs_.size());
            for (int i = 0; i < (int)ui_logs_.size(); ++i) {
                if (IContains(ui_logs_[i], "[ERR]") || IContains(ui_logs_[i], "error") || IContains(ui_logs_[i], "failed"))
                    visible.push_back(i);
            }
            clipper.Begin((int)visible.size());
            while (clipper.Step()) {
                for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row)
                    DrawLogLine(ui_logs_[visible[row]]);
            }
        } else {
            clipper.Begin((int)ui_logs_.size());
            while (clipper.Step()) {
                for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row)
                    DrawLogLine(ui_logs_[row]);
            }
        }
        if (auto_scroll_logs_ && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 20)
            ImGui::SetScrollHereY(1.0f);
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

// ── Advanced page ──
void ImGuiApp::DrawAdvancedPage() {
    Snapshot s;
    { std::lock_guard<std::mutex> lk(snap_mutex_); s = snap_; }

    ImGui::TextColored(COL_ACCENT, "Advanced Settings");
    ImGui::Spacing();

    if (ImGui::BeginTabBar("##advtabs")) {
        // Runtime tab
        if (ImGui::BeginTabItem("Runtime")) {
            ImGui::Spacing();
            const float lw = 240*dpi_scale_;
            ImGui::SetNextItemWidth(lw); ImGui::InputText("Xray path", xray_path_.data(), xray_path_.size());
            ImGui::SetNextItemWidth(lw); ImGui::InputText("Sing-box path", singbox_path_.data(), singbox_path_.size());
            ImGui::SetNextItemWidth(lw); ImGui::InputText("Mihomo path", mihomo_path_.data(), mihomo_path_.size());
            ImGui::SetNextItemWidth(lw); ImGui::InputText("Tor path", tor_path_.data(), tor_path_.size());
            ImGui::Spacing();
            const float nw = 120*dpi_scale_;
            ImGui::SetNextItemWidth(nw); ImGui::InputInt("Max total", &max_total_);
            ImGui::SetNextItemWidth(nw); ImGui::InputInt("Max workers", &max_workers_);
            ImGui::SetNextItemWidth(nw); ImGui::InputInt("Scan limit", &scan_limit_);
            ImGui::SetNextItemWidth(nw); ImGui::InputInt("Sleep sec", &sleep_seconds_);
            ImGui::Spacing();
            if (ImGui::Button("Apply")) ApplyAdvancedSettings();
            if (advanced_restart_notice_) {
                ImGui::SameLine();
                ImGui::TextColored(COL_YELLOW, "Port restart required.");
            }
            ImGui::EndTabItem();
        }

        // Telegram tab
        if (ImGui::BeginTabItem("Telegram")) {
            ImGui::Spacing();
            ImGui::Checkbox("Enabled", &telegram_enabled_);
            const float lw = 240*dpi_scale_;
            ImGui::SetNextItemWidth(lw); ImGui::InputText("API ID", telegram_api_id_.data(), telegram_api_id_.size());
            ImGui::SetNextItemWidth(lw); ImGui::InputText("API Hash", telegram_api_hash_.data(), telegram_api_hash_.size());
            ImGui::SetNextItemWidth(lw); ImGui::InputText("Phone", telegram_phone_.data(), telegram_phone_.size());
            const float nw = 120*dpi_scale_;
            ImGui::SetNextItemWidth(nw); ImGui::InputInt("Limit", &telegram_limit_);
            ImGui::SetNextItemWidth(nw); ImGui::InputInt("Timeout ms", &telegram_timeout_ms_);
            ImGui::InputTextMultiline("Targets", telegram_targets_.data(), telegram_targets_.size(),
                ImVec2(-1, 150*dpi_scale_));
            if (ImGui::Button("Save Telegram")) ApplyAdvancedSettings();
            ImGui::EndTabItem();
        }

        // Sources tab
        if (ImGui::BeginTabItem("Sources")) {
            ImGui::Spacing();
            ImGui::InputTextMultiline("GitHub URLs", github_urls_.data(), github_urls_.size(),
                ImVec2(-1, 200*dpi_scale_));
            if (ImGui::Button("Save Sources")) ApplyAdvancedSettings();
            ImGui::EndTabItem();
        }

        // Maintenance tab
        if (ImGui::BeginTabItem("Maintenance")) {
            ImGui::Spacing();
            if (ImGui::Button("Refresh Ports"))    RunCommandAsync("{\"command\":\"refresh_ports\"}");
            ImGui::SameLine(0, 8*dpi_scale_);
            if (ImGui::Button("Reprovision"))      RunCommandAsync("{\"command\":\"reprovision_ports\"}");
            ImGui::SameLine(0, 8*dpi_scale_);
            if (ImGui::Button("Recheck Live"))     RunCommandAsync("{\"command\":\"recheck_live_ports\"}");

            ImGui::Spacing();
            ImGui::SetNextItemWidth(100*dpi_scale_);
            ImGui::InputInt("Clear older than (h)", &clear_old_hours_);
            if (clear_old_hours_ < 1) clear_old_hours_ = 1;
            if (ImGui::Button("Clear Old")) {
                RunCommandAsync("{\"command\":\"clear_old\",\"hours\":" + std::to_string(clear_old_hours_) + "}");
            }
            ImGui::SameLine(0, 8*dpi_scale_);
            if (ImGui::Button("Clear Alive")) RunCommandAsync("{\"command\":\"clear_alive\"}");

            ImGui::SameLine(0, 8*dpi_scale_);
            if (ImGui::Button("Pause"))  RunCommandAsync("{\"command\":\"pause\"}");
            ImGui::SameLine(0, 4*dpi_scale_);
            if (ImGui::Button("Resume")) RunCommandAsync("{\"command\":\"resume\"}");

            ImGui::EndTabItem();
        }

        // Ports tab
        if (ImGui::BeginTabItem("Ports")) {
            ImGui::Spacing();
            const float avail_h = ImGui::GetContentRegionAvail().y - 4;
            if (ImGui::BeginTable("##porttbl", 7,
                ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable,
                ImVec2(0, avail_h > 100 ? avail_h : 200)))
            {
                ImGui::TableSetupColumn("Port",   ImGuiTableColumnFlags_WidthFixed, 60*dpi_scale_);
                ImGui::TableSetupColumn("HTTP",   ImGuiTableColumnFlags_WidthFixed, 60*dpi_scale_);
                ImGui::TableSetupColumn("Alive",  ImGuiTableColumnFlags_WidthFixed, 48*dpi_scale_);
                ImGui::TableSetupColumn("SOCKS",  ImGuiTableColumnFlags_WidthFixed, 48*dpi_scale_);
                ImGui::TableSetupColumn("HTTP ok",ImGuiTableColumnFlags_WidthFixed, 56*dpi_scale_);
                ImGui::TableSetupColumn("Engine", ImGuiTableColumnFlags_WidthFixed, 80*dpi_scale_);
                ImGui::TableSetupColumn("URI");
                ImGui::TableHeadersRow();
                for (auto& sl : s.provisioned_ports) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn(); ImGui::Text("%d", sl.port);
                    ImGui::TableNextColumn(); ImGui::Text("%d", sl.http_port);
                    ImGui::TableNextColumn(); ImGui::TextColored(sl.alive?COL_GREEN:COL_RED, sl.alive?"Y":"N");
                    ImGui::TableNextColumn(); ImGui::TextColored(sl.socks_ready?COL_GREEN:COL_RED, sl.socks_ready?"Y":"N");
                    ImGui::TableNextColumn(); ImGui::TextColored(sl.http_ready?COL_GREEN:COL_RED, sl.http_ready?"Y":"N");
                    ImGui::TableNextColumn(); ImGui::TextUnformatted(sl.engine_used.empty()?"-":sl.engine_used.c_str());
                    ImGui::TableNextColumn(); ImGui::TextUnformatted(Trim(sl.uri, 160).c_str());
                }
                ImGui::EndTable();
            }
            if (s.provisioned_ports.empty()) {
                ImGui::TextColored(COL_DIM, "No provisioned ports are active yet.");
            }
            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }
}

// ── WndProc ──
LRESULT CALLBACK ImGuiApp::WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    auto* app = reinterpret_cast<ImGuiApp*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (app) return app->HandleMessage(hwnd, msg, wParam, lParam);
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT ImGuiApp::HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam)) return true;
    switch (msg) {
        case WM_SIZE:
            if (d3d_device_ && wParam != SIZE_MINIMIZED) {
                pending_resize_width_ = LOWORD(lParam);
                pending_resize_height_ = HIWORD(lParam);
                pending_resize_ = pending_resize_width_ > 0 && pending_resize_height_ > 0;
            }
            return 0;
        case WM_DPICHANGED:
            UpdateDpiScale(static_cast<float>(HIWORD(wParam)) / 96.0f);
            if (lParam) {
                auto* r = reinterpret_cast<RECT*>(lParam);
                SetWindowPos(hwnd, nullptr, r->left, r->top, r->right-r->left, r->bottom-r->top,
                    SWP_NOZORDER|SWP_NOACTIVATE);
            }
            return 0;
        case WM_SYSCOMMAND:
            if ((wParam & 0xfff0) == SC_KEYMENU) return 0;
            break;
        case WM_DESTROY:
            PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace win32
} // namespace hunter
