#include "win32/main_window.h"
#include "resource.h"
#include "core/config.h"
#include "core/utils.h"
#include "orchestrator/orchestrator.h"

#include <commctrl.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <vsstyle.h>
#include <string>
#include <thread>
#include <chrono>
#include <mutex>
#include <vector>
#include <utility>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "uxtheme.lib")

namespace hunter {
namespace win32 {

static constexpr UINT WM_APP_DRAIN_LOGS = WM_APP + 1;

// Modern color palette (dark theme)
constexpr COLORREF BG_COLOR = RGB(25, 25, 35);           // Deep dark blue
constexpr COLORREF PANEL_COLOR = RGB(35, 35, 45);       // Dark panel
constexpr COLORREF ACCENT_COLOR = RGB(10, 132, 255);    // Modern blue
constexpr COLORREF ACCENT_HOVER = RGB(64, 156, 255);    // Hover blue
constexpr COLORREF TEXT_COLOR = RGB(240, 240, 245);      // Light text
constexpr COLORREF TEXT_MUTED = RGB(140, 140, 150);      // Muted text
constexpr COLORREF SUCCESS_COLOR = RGB(52, 199, 89);     // Green
constexpr COLORREF WARNING_COLOR = RGB(255, 149, 0);     // Orange
constexpr COLORREF ERROR_COLOR = RGB(255, 69, 58);       // Modern red
constexpr COLORREF BORDER_COLOR = RGB(60, 60, 70);       // Subtle border
constexpr COLORREF BUTTON_FACE = RGB(45, 45, 55);       // Button face
constexpr COLORREF HIGHLIGHT_COLOR = RGB(0, 122, 255);   // Selection highlight

static UINT g_dpi = 96;
static int Scale(int value) { return MulDiv(value, (int)g_dpi, 96); }

static void ShowWin32Error(const wchar_t* stage) {
    DWORD err = GetLastError();
    wchar_t* sys = nullptr;
    FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        err,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        (LPWSTR)&sys,
        0,
        nullptr);

    wchar_t msg[2048];
    swprintf(msg, 2048, L"%s\n\nGetLastError=%lu\n%s", stage, (unsigned long)err, sys ? sys : L"");
    MessageBoxW(nullptr, msg, L"Hunter UI Error", MB_OK | MB_ICONERROR | MB_TOPMOST);

    if (sys) LocalFree(sys);
}

MainWindow::MainWindow() {}

MainWindow::~MainWindow() {
    StopHunting();

    if (font_normal_) DeleteObject(font_normal_);
    if (font_bold_) DeleteObject(font_bold_);
    if (font_title_) DeleteObject(font_title_);
    if (font_status_) DeleteObject(font_status_);

    if (bg_brush_) DeleteObject(bg_brush_);
    if (panel_brush_) DeleteObject(panel_brush_);
    if (header_brush_) DeleteObject(header_brush_);
}

bool MainWindow::Create(HINSTANCE hInstance, int nCmdShow) {
    // Register window class with modern styling
    WNDCLASSEX wc = {};
    wc.cbSize = sizeof(WNDCLASSEX);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(NULL_BRUSH);
    wc.lpszClassName = L"HunterWindow";
    wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    wc.hIconSm = wc.hIcon;
    
    if (!RegisterClassEx(&wc)) {
        DWORD err = GetLastError();
        if (err != ERROR_CLASS_ALREADY_EXISTS) {
            ShowWin32Error(L"RegisterClassExW failed");
            return false;
        }
    }

    // Enable modern visual styling
    SetProcessDPIAware();
    
    // Create main window with modern extended styles
    hwnd_ = CreateWindowEx(
        WS_EX_APPWINDOW,
        L"HunterWindow", L"Hunter C++ - Proxy Config Hunter",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, Scale(1200), Scale(800),
        nullptr, nullptr, hInstance, this);

    if (!hwnd_) {
        ShowWin32Error(L"CreateWindowExW failed");
        return false;
    }

    // Initialize Common Controls with modern styles
    INITCOMMONCONTROLSEX icc = { 
        sizeof(INITCOMMONCONTROLSEX), 
        ICC_WIN95_CLASSES | ICC_DATE_CLASSES | ICC_PROGRESS_CLASS 
    };
    InitCommonControlsEx(&icc);

    // Enable modern themes and visual styles
    SetWindowTheme(hwnd_, L"Explorer", nullptr);

    HDC hdc = GetDC(hwnd_);
    dpi_ = GetDeviceCaps(hdc, LOGPIXELSX);
    ReleaseDC(hwnd_, hdc);
    if (dpi_ == 0) dpi_ = 96;
    g_dpi = dpi_;

    RecreateGdiObjects();

    CreateControls();
    ApplyModernTheme();

    SetRunning(false);

    RECT rc;
    GetClientRect(hwnd_, &rc);
    HandleSize(rc.right - rc.left, rc.bottom - rc.top);

    (void)nCmdShow;
    ShowWindow(hwnd_, SW_SHOWNORMAL);
    ShowWindow(hwnd_, SW_RESTORE);
    SetWindowPos(hwnd_, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    BringWindowToTop(hwnd_);
    SetForegroundWindow(hwnd_);
    UpdateWindow(hwnd_);

    return true;
}

void MainWindow::CreateControls() {
    // Create modern panels with enhanced styling
    CreateHeaderPanel();
    CreateStatusPanel();
    CreateControlPanel();
    CreateLogPanel();
    CreateMetricsPanel();
}

void MainWindow::CreateHeaderPanel() {
    // Header background
    header_panel_ = CreateWindow(L"STATIC", nullptr, WS_CHILD | WS_VISIBLE | SS_NOTIFY,
        0, 0, 0, Scale(60), hwnd_, nullptr, nullptr, nullptr);

    // App title
    title_label_ = CreateWindow(L"STATIC", L"Hunter C++", WS_CHILD | WS_VISIBLE,
        Scale(20), Scale(15), Scale(200), Scale(30), header_panel_, nullptr, nullptr, nullptr);
    
    // Status indicator
    status_indicator_ = CreateWindow(L"STATIC", L"● Stopped", WS_CHILD | WS_VISIBLE,
        0, 0, Scale(200), Scale(30), header_panel_, nullptr, nullptr, nullptr);

    // Minimize to tray checkbox
    minimize_to_tray_ = CreateWindow(L"BUTTON", L"Minimize to tray", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
        0, 0, Scale(150), Scale(30), header_panel_, nullptr, nullptr, nullptr);
}

void MainWindow::CreateStatusPanel() {
    // Status group
    status_group_ = CreateWindow(L"BUTTON", L"Status", WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
        0, 0, 0, 0, hwnd_, nullptr, nullptr, nullptr);

    // Status labels
    cycle_label_ = CreateLabel(status_group_, L"Cycle:");
    phase_label_ = CreateLabel(status_group_, L"Phase:");
    memory_label_ = CreateLabel(status_group_, L"Memory:");
    configs_label_ = CreateLabel(status_group_, L"Configs:");
    uptime_label_ = CreateLabel(status_group_, L"Uptime:");

    // Status values
    cycle_value_ = CreateLabel(status_group_, L"0");
    phase_value_ = CreateLabel(status_group_, L"IDLE");
    memory_value_ = CreateLabel(status_group_, L"0%");
    configs_value_ = CreateLabel(status_group_, L"0/0/0");
    uptime_value_ = CreateLabel(status_group_, L"00:00:00");
}

void MainWindow::CreateControlPanel() {
    // Control group
    control_group_ = CreateWindow(L"BUTTON", L"Control", WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
        0, 0, 0, 0, hwnd_, nullptr, nullptr, nullptr);

    // Control buttons
    start_button_ = CreateButton(control_group_, L"Start Hunting", IDC_START_BUTTON, BS_OWNERDRAW);
    stop_button_ = CreateButton(control_group_, L"Stop", IDC_STOP_BUTTON, BS_OWNERDRAW);
    force_cycle_button_ = CreateButton(control_group_, L"Force Cycle", IDC_FORCE_CYCLE_BUTTON, BS_OWNERDRAW);
    clear_cache_button_ = CreateButton(control_group_, L"Clear Cache", IDC_CLEAR_CACHE_BUTTON, BS_OWNERDRAW);
    export_configs_button_ = CreateButton(control_group_, L"Export Configs", IDC_EXPORT_CONFIGS_BUTTON, BS_OWNERDRAW);

    // Telegram settings
    ctrl_telegram_label_ = CreateLabel(control_group_, L"Telegram:");
    ctrl_telegram_status_ = CreateLabel(control_group_, L"Not configured");
    configure_telegram_button_ = CreateButton(control_group_, L"Configure", IDC_CONFIGURE_TELEGRAM_BUTTON, BS_OWNERDRAW);
}

void MainWindow::CreateLogPanel() {
    // Log group
    log_group_ = CreateWindow(L"BUTTON", L"Activity Log", WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
        0, 0, 0, 0, hwnd_, nullptr, nullptr, nullptr);

    // Log listview
    log_list_ = CreateWindowEx(WS_EX_CLIENTEDGE, L"SysListView32", L"",
        WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL,
        0, 0, 0, 0, log_group_, nullptr, GetModuleHandle(nullptr), nullptr);

    // Add columns
    LVCOLUMN lvc = {};
    lvc.mask = LVCF_TEXT | LVCF_WIDTH;
    
    lvc.cx = Scale(80); lvc.pszText = (LPWSTR)L"Time"; ListView_InsertColumn(log_list_, 0, &lvc);
    lvc.cx = Scale(60); lvc.pszText = (LPWSTR)L"Level"; ListView_InsertColumn(log_list_, 1, &lvc);
    lvc.cx = Scale(540); lvc.pszText = (LPWSTR)L"Message"; ListView_InsertColumn(log_list_, 2, &lvc);
}

void MainWindow::CreateMetricsPanel() {
    // Metrics group
    metrics_group_ = CreateWindow(L"BUTTON", L"Performance Metrics", WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
        0, 0, 0, 0, hwnd_, nullptr, nullptr, nullptr);

    // Metrics labels
    workers_label_ = CreateLabel(metrics_group_, L"Workers:");
    workers_value_ = CreateLabel(metrics_group_, L"0 running");
    balancer_label_ = CreateLabel(metrics_group_, L"Balancer:");
    balancer_value_ = CreateLabel(metrics_group_, L"0 backends");
    metrics_telegram_label_ = CreateLabel(metrics_group_, L"Telegram:");
    telegram_value_ = CreateLabel(metrics_group_, L"Not configured");

    // Hardware info
    hw_label_ = CreateLabel(metrics_group_, L"Hardware:");
    hw_value_ = CreateLabel(metrics_group_, L"4 cores, 8GB RAM");
    mode_label_ = CreateLabel(metrics_group_, L"Resource Mode:");
    mode_value_ = CreateLabel(metrics_group_, L"NORMAL");
    last_publish_label_ = CreateLabel(metrics_group_, L"Last Publish:");
    last_publish_value_ = CreateLabel(metrics_group_, L"Never");
}

HWND MainWindow::CreateLabel(HWND parent, const std::wstring& text) {
    return CreateWindow(L"STATIC", text.c_str(), WS_CHILD | WS_VISIBLE,
        0, 0, Scale(10), Scale(10), parent, nullptr, nullptr, nullptr);
}

HWND MainWindow::CreateButton(HWND parent, const std::wstring& text, int id, DWORD extraStyle) {
    HWND hwnd = CreateWindowW(L"BUTTON", text.c_str(),
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | extraStyle,
        0, 0, Scale(10), Scale(10), parent, (HMENU)(INT_PTR)id, nullptr, nullptr);
    return hwnd;
}

void MainWindow::ApplyModernTheme() {
    // Enable modern dark mode for the entire window
    SetWindowTheme(hwnd_, L"DarkMode_Explorer", nullptr);
    
    // Apply modern styling to header
    SetWindowTheme(header_panel_, L"DarkMode_Explorer", nullptr);
    ApplyControlTheme(title_label_, ACCENT_COLOR, 18, true);
    ApplyControlTheme(status_indicator_, ERROR_COLOR, 16, false);
    ApplyControlTheme(minimize_to_tray_, TEXT_COLOR, 14, false);

    // Status panel with modern styling
    ApplyControlTheme(cycle_label_, TEXT_MUTED, 14, false);
    ApplyControlTheme(phase_label_, TEXT_MUTED, 14, false);
    ApplyControlTheme(memory_label_, TEXT_MUTED, 14, false);
    ApplyControlTheme(configs_label_, TEXT_MUTED, 14, false);
    ApplyControlTheme(uptime_label_, TEXT_MUTED, 14, false);
    
    ApplyControlTheme(cycle_value_, TEXT_COLOR, 14, true);
    ApplyControlTheme(phase_value_, TEXT_COLOR, 14, true);
    ApplyControlTheme(memory_value_, TEXT_COLOR, 14, true);
    ApplyControlTheme(configs_value_, TEXT_COLOR, 14, true);
    ApplyControlTheme(uptime_value_, TEXT_COLOR, 14, true);

    // Modern button styling
    SetWindowTheme(start_button_, L"DarkMode_Explorer", nullptr);
    SetWindowTheme(stop_button_, L"DarkMode_Explorer", nullptr);
    SetWindowTheme(force_cycle_button_, L"DarkMode_Explorer", nullptr);
    SetWindowTheme(clear_cache_button_, L"DarkMode_Explorer", nullptr);
    SetWindowTheme(export_configs_button_, L"DarkMode_Explorer", nullptr);
    SetWindowTheme(configure_telegram_button_, L"DarkMode_Explorer", nullptr);

    // Modern group box styling
    SetWindowTheme(status_group_, L"DarkMode_Explorer", nullptr);
    SetWindowTheme(control_group_, L"DarkMode_Explorer", nullptr);
    SetWindowTheme(log_group_, L"DarkMode_Explorer", nullptr);
    SetWindowTheme(metrics_group_, L"DarkMode_Explorer", nullptr);
    
    // Metrics panel styling
    ApplyControlTheme(workers_label_, TEXT_MUTED, 14, false);
    ApplyControlTheme(balancer_label_, TEXT_MUTED, 14, false);
    ApplyControlTheme(metrics_telegram_label_, TEXT_MUTED, 14, false);
    ApplyControlTheme(hw_label_, TEXT_MUTED, 14, false);
    ApplyControlTheme(mode_label_, TEXT_MUTED, 14, false);
    ApplyControlTheme(last_publish_label_, TEXT_MUTED, 14, false);
    
    ApplyControlTheme(workers_value_, TEXT_COLOR, 14, true);
    ApplyControlTheme(balancer_value_, TEXT_COLOR, 14, true);
    ApplyControlTheme(telegram_value_, TEXT_COLOR, 14, true);
    ApplyControlTheme(hw_value_, TEXT_COLOR, 14, true);
    ApplyControlTheme(mode_value_, TEXT_COLOR, 14, true);
    ApplyControlTheme(last_publish_value_, TEXT_COLOR, 14, true);
    
    ApplyControlTheme(ctrl_telegram_label_, TEXT_MUTED, 14, false);
    ApplyControlTheme(ctrl_telegram_status_, TEXT_COLOR, 14, true);
    
    // Modern listview styling
    SetWindowTheme(log_list_, L"Explorer", nullptr);
    
    ListView_SetBkColor(log_list_, PANEL_COLOR);
    ListView_SetTextBkColor(log_list_, PANEL_COLOR);
    ListView_SetTextColor(log_list_, TEXT_COLOR);

    ListView_SetExtendedListViewStyle(log_list_, 
        LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);
    
    // Enable modern visual styles for all controls
    EnableThemeDialogTexture(hwnd_, ETDT_ENABLETAB);
}

void MainWindow::ApplyControlTheme(HWND hwnd, COLORREF color, int font_size, bool bold) {
    if (!hwnd) return;

    HFONT font = font_normal_;
    if (font_size >= 18) font = font_title_ ? font_title_ : font;
    else if (font_size >= 16) font = font_status_ ? font_status_ : font;
    else if (bold) font = font_bold_ ? font_bold_ : font;

    if (font) SendMessage(hwnd, WM_SETFONT, (WPARAM)font, TRUE);

    text_colors_[hwnd] = color;
}

void MainWindow::RecreateGdiObjects() {
    if (bg_brush_) { DeleteObject(bg_brush_); bg_brush_ = nullptr; }
    if (panel_brush_) { DeleteObject(panel_brush_); panel_brush_ = nullptr; }
    if (header_brush_) { DeleteObject(header_brush_); header_brush_ = nullptr; }

    if (font_normal_) { DeleteObject(font_normal_); font_normal_ = nullptr; }
    if (font_bold_) { DeleteObject(font_bold_); font_bold_ = nullptr; }
    if (font_title_) { DeleteObject(font_title_); font_title_ = nullptr; }
    if (font_status_) { DeleteObject(font_status_); font_status_ = nullptr; }

    bg_brush_ = CreateSolidBrush(BG_COLOR);
    panel_brush_ = CreateSolidBrush(PANEL_COLOR);
    header_brush_ = CreateSolidBrush(PANEL_COLOR);

    const auto makeFont = [this](int point, int weight) -> HFONT {
        int height = -MulDiv(point, (int)dpi_, 72);
        return CreateFontW(height, 0, 0, 0, weight, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    };

    font_normal_ = makeFont(14, FW_NORMAL);
    font_bold_ = makeFont(14, FW_BOLD);
    font_title_ = makeFont(18, FW_BOLD);
    font_status_ = makeFont(16, FW_SEMIBOLD);
}

void MainWindow::AddLogEntry(const std::wstring& level, const std::wstring& message) {
    if (!log_list_ || !IsWindow(log_list_)) return;

    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    auto tm = *std::localtime(&time_t);
    
    wchar_t time_str[32];
    swprintf(time_str, 32, L"%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);

    int sev = 0;
    if (level == L"SUCCESS") sev = 1;
    else if (level == L"WARNING") sev = 2;
    else if (level == L"ERROR") sev = 3;

    LVITEM item = {};
    item.mask = LVIF_TEXT | LVIF_PARAM;
    item.iItem = ListView_GetItemCount(log_list_);
    item.lParam = (LPARAM)sev;

    // Time
    item.pszText = time_str;
    int idx = ListView_InsertItem(log_list_, &item);
    if (idx < 0) return;

    ListView_SetItemText(log_list_, idx, 1, (LPWSTR)level.c_str());
    
    // Message
    ListView_SetItemText(log_list_, idx, 2, (LPWSTR)message.c_str());

    // Auto-scroll to bottom
    ListView_EnsureVisible(log_list_, idx, FALSE);

    // Limit log entries
    int count = ListView_GetItemCount(log_list_);
    if (count > 1000) {
        ListView_DeleteItem(log_list_, 0);
    }
}

void MainWindow::UpdateStatus(const std::wstring& phase, int cycle, float memory_pct,
                              int scraped, int tested, int gold, int silver) {
    SetWindowTextW(phase_value_, phase.c_str());
    SetWindowTextW(cycle_value_, std::to_wstring(cycle).c_str());
    
    wchar_t memory_str[32];
    swprintf(memory_str, 32, L"%.1f%%", memory_pct);
    SetWindowTextW(memory_value_, memory_str);
    
    wchar_t configs_str[64];
    swprintf(configs_str, 64, L"%d/%d/%d", gold, silver, scraped);
    SetWindowTextW(configs_value_, configs_str);
}

void MainWindow::SetRunning(bool running) {
    if (!status_indicator_ || !IsWindow(status_indicator_)) return;

    if (running) {
        SetWindowTextW(status_indicator_, L"● Running");
        ApplyControlTheme(status_indicator_, SUCCESS_COLOR, Scale(16), false);
        if (start_button_ && IsWindow(start_button_)) EnableWindow(start_button_, FALSE);
        if (stop_button_ && IsWindow(stop_button_)) EnableWindow(stop_button_, TRUE);
    } else {
        SetWindowTextW(status_indicator_, L"● Stopped");
        ApplyControlTheme(status_indicator_, ERROR_COLOR, Scale(16), false);
        if (start_button_ && IsWindow(start_button_)) EnableWindow(start_button_, TRUE);
        if (stop_button_ && IsWindow(stop_button_)) EnableWindow(stop_button_, FALSE);
    }
}

LRESULT CALLBACK MainWindow::WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_NCCREATE) {
        CREATESTRUCT* cs = (CREATESTRUCT*)lParam;
        MainWindow* window = (MainWindow*)cs->lpCreateParams;
        SetWindowLongPtr(hwnd, GWLP_USERDATA, (LONG_PTR)window);
        return TRUE;
    }

    MainWindow* window = (MainWindow*)GetWindowLongPtr(hwnd, GWLP_USERDATA);
    if (!window) return DefWindowProc(hwnd, msg, wParam, lParam);

    switch (msg) {
        case WM_COMMAND:
            return window->HandleCommand(LOWORD(wParam), HIWORD(wParam));

        case WM_NOTIFY:
            return window->HandleNotify((NMHDR*)lParam);

        case WM_DRAWITEM:
            return window->HandleDrawItem((const DRAWITEMSTRUCT*)lParam);
        
        case WM_CTLCOLORSTATIC:
            return window->HandleCtlColor((HWND)lParam, (HDC)wParam);

        case WM_CTLCOLORBTN:
            return window->HandleCtlColor((HWND)lParam, (HDC)wParam);

        case WM_ERASEBKGND: {
            RECT rc;
            GetClientRect(hwnd, &rc);
            FillRect((HDC)wParam, &rc, window->bg_brush_ ? window->bg_brush_ : (HBRUSH)GetStockObject(BLACK_BRUSH));
            return 1;
        }
        
        case WM_SIZE:
            window->HandleSize(LOWORD(lParam), HIWORD(lParam));
            return 0;

        case WM_DPICHANGED:
            window->HandleDpiChanged(HIWORD(wParam), (const RECT*)lParam);
            return 0;
        
        case WM_APP_DRAIN_LOGS:
            window->DrainLogs();
            return 0;
        
        case WM_DESTROY:
            window->StopHunting();
            PostQuitMessage(0);
            return 0;
        
        default:
            return DefWindowProc(hwnd, msg, wParam, lParam);
    }
}

LRESULT MainWindow::HandleCommand(int controlId, int notifyCode) {
    (void)notifyCode;
    switch (controlId) {
        case IDC_START_BUTTON:
            StartHunting();
            return 0;
        
        case IDC_STOP_BUTTON:
            StopHunting();
            return 0;
        
        case IDC_FORCE_CYCLE_BUTTON:
            ForceCycle();
            return 0;
        
        case IDC_CLEAR_CACHE_BUTTON:
            ClearCache();
            return 0;
        
        case IDC_EXPORT_CONFIGS_BUTTON:
            ExportConfigs();
            return 0;
        
        case IDC_CONFIGURE_TELEGRAM_BUTTON:
            ConfigureTelegram();
            return 0;
    }
    return 0;
}

LRESULT MainWindow::HandleCtlColor(HWND hwnd, HDC hdc) {
    auto it = text_colors_.find(hwnd);
    COLORREF text = (it == text_colors_.end()) ? TEXT_COLOR : it->second;
    SetTextColor(hdc, text);
    SetBkMode(hdc, TRANSPARENT);

    if (hwnd == header_panel_) return (LRESULT)(header_brush_ ? header_brush_ : bg_brush_);

    HWND parent = GetParent(hwnd);
    if (parent && parent == header_panel_) return (LRESULT)(header_brush_ ? header_brush_ : bg_brush_);
    if (parent && (parent == status_group_ || parent == control_group_ || parent == log_group_ || parent == metrics_group_)) {
        return (LRESULT)(panel_brush_ ? panel_brush_ : bg_brush_);
    }
    return (LRESULT)(bg_brush_ ? bg_brush_ : (HBRUSH)GetStockObject(BLACK_BRUSH));
}

void MainWindow::HandleSize(int width, int height) {
    if (!hwnd_ || !IsWindow(hwnd_)) return;
    const int m = Scale(16);
    const int gap = Scale(12);
    const int header_h = Scale(60);
    const int top_y = header_h + m;

    SetWindowPos(header_panel_, nullptr, 0, 0, width, header_h, SWP_NOZORDER);

    const int content_w = width - (m * 2);
    const bool two_col = content_w >= Scale(900);

    int status_x = m;
    int status_y = top_y;
    int status_w = two_col ? (content_w - gap) / 2 : content_w;
    int control_x = two_col ? (m + status_w + gap) : m;
    int control_y = two_col ? top_y : (top_y + Scale(210) + gap);
    int control_w = status_w;
    int top_h = Scale(210);
    int panels_bottom = two_col ? (top_y + top_h) : (control_y + top_h);

    SetWindowPos(status_group_, nullptr, status_x, status_y, status_w, top_h, SWP_NOZORDER);
    SetWindowPos(control_group_, nullptr, control_x, control_y, control_w, top_h, SWP_NOZORDER);

    const int metrics_h = Scale(140);
    int log_y = panels_bottom + gap;
    int log_h = height - log_y - metrics_h - m;
    if (log_h < Scale(140)) log_h = Scale(140);

    SetWindowPos(metrics_group_, nullptr, m, height - metrics_h - m, content_w, metrics_h, SWP_NOZORDER);
    SetWindowPos(log_group_, nullptr, m, log_y, content_w, log_h, SWP_NOZORDER);

    const int inner = Scale(14);
    const int row = Scale(26);
    const int label_w = Scale(90);
    const int value_w = status_w - inner * 2 - label_w - gap;

    const int s0x = inner;
    const int s0y = inner + Scale(18);

    auto place = [&](HWND h, int x, int y, int w, int hgt) {
        if (h && IsWindow(h)) SetWindowPos(h, nullptr, x, y, w, hgt, SWP_NOZORDER);
    };

    place(cycle_label_, s0x, s0y + row * 0, label_w, row);
    place(cycle_value_, s0x + label_w + gap, s0y + row * 0, value_w, row);
    place(phase_label_, s0x, s0y + row * 1, label_w, row);
    place(phase_value_, s0x + label_w + gap, s0y + row * 1, value_w, row);
    place(memory_label_, s0x, s0y + row * 2, label_w, row);
    place(memory_value_, s0x + label_w + gap, s0y + row * 2, value_w, row);
    place(configs_label_, s0x, s0y + row * 3, label_w, row);
    place(configs_value_, s0x + label_w + gap, s0y + row * 3, value_w, row);
    place(uptime_label_, s0x, s0y + row * 4, label_w, row);
    place(uptime_value_, s0x + label_w + gap, s0y + row * 4, value_w, row);

    const int btn_h = Scale(36);
    const int btn_w = (control_w - inner * 2 - gap) / 2;
    const int c0x = inner;
    const int c0y = inner + Scale(18);

    place(start_button_, c0x, c0y, btn_w, btn_h);
    place(stop_button_, c0x + btn_w + gap, c0y, btn_w, btn_h);
    place(force_cycle_button_, c0x, c0y + btn_h + gap, btn_w, btn_h);
    place(clear_cache_button_, c0x + btn_w + gap, c0y + btn_h + gap, btn_w, btn_h);
    place(export_configs_button_, c0x, c0y + (btn_h + gap) * 2, btn_w * 2 + gap, btn_h);

    const int tg_y = c0y + (btn_h + gap) * 3 + Scale(2);
    place(ctrl_telegram_label_, c0x, tg_y, label_w, row);
    place(ctrl_telegram_status_, c0x + label_w + gap, tg_y, btn_w + gap, row);
    place(configure_telegram_button_, c0x + btn_w + gap, tg_y - Scale(2), btn_w, btn_h);

    const int header_inner = Scale(18);
    const int header_row = Scale(32);
    place(title_label_, header_inner, Scale(14), Scale(260), header_row);
    place(minimize_to_tray_, width - header_inner - Scale(170), Scale(14), Scale(170), header_row);
    place(status_indicator_, width - header_inner - Scale(390), Scale(14), Scale(210), header_row);

    const int log_inner_x = inner;
    const int log_inner_y = inner + Scale(18);
    place(log_list_, log_inner_x, log_inner_y, content_w - inner * 2, log_h - log_inner_y - inner);

    if (log_list_ && IsWindow(log_list_)) {
        int lvw = content_w - inner * 2;
        int time_w = Scale(90);
        int level_w = Scale(80);
        int msg_w = lvw - time_w - level_w - Scale(4);
        if (msg_w < Scale(100)) msg_w = Scale(100);
        ListView_SetColumnWidth(log_list_, 0, time_w);
        ListView_SetColumnWidth(log_list_, 1, level_w);
        ListView_SetColumnWidth(log_list_, 2, msg_w);
    }

    const int met_inner_x = inner;
    const int met_inner_y = inner + Scale(18);
    const int met_row = row;
    const int left_w = (content_w - inner * 2 - gap) / 2;
    const int right_x = met_inner_x + left_w + gap;
    const int val_x = met_inner_x + label_w + gap;
    const int val_w = left_w - label_w - gap;

    place(workers_label_, met_inner_x, met_inner_y + met_row * 0, label_w, met_row);
    place(workers_value_, val_x, met_inner_y + met_row * 0, val_w, met_row);
    place(balancer_label_, met_inner_x, met_inner_y + met_row * 1, label_w, met_row);
    place(balancer_value_, val_x, met_inner_y + met_row * 1, val_w, met_row);
    place(metrics_telegram_label_, met_inner_x, met_inner_y + met_row * 2, label_w, met_row);
    place(telegram_value_, val_x, met_inner_y + met_row * 2, val_w, met_row);

    place(hw_label_, right_x, met_inner_y + met_row * 0, label_w, met_row);
    place(hw_value_, right_x + label_w + gap, met_inner_y + met_row * 0, val_w, met_row);
    place(mode_label_, right_x, met_inner_y + met_row * 1, label_w, met_row);
    place(mode_value_, right_x + label_w + gap, met_inner_y + met_row * 1, val_w, met_row);
    place(last_publish_label_, right_x, met_inner_y + met_row * 2, label_w, met_row);
    place(last_publish_value_, right_x + label_w + gap, met_inner_y + met_row * 2, val_w, met_row);
}

void MainWindow::HandleDpiChanged(UINT dpi, const RECT* suggested) {
    dpi_ = dpi ? dpi : 96;
    g_dpi = dpi_;
    RecreateGdiObjects();
    ApplyModernTheme();

    if (suggested) {
        SetWindowPos(hwnd_, nullptr,
            suggested->left,
            suggested->top,
            suggested->right - suggested->left,
            suggested->bottom - suggested->top,
            SWP_NOZORDER | SWP_NOACTIVATE);
    }

    RECT rc;
    GetClientRect(hwnd_, &rc);
    HandleSize(rc.right - rc.left, rc.bottom - rc.top);
}

LRESULT MainWindow::HandleNotify(NMHDR* hdr) {
    if (!hdr) return 0;
    if (hdr->hwndFrom == log_list_ && hdr->code == NM_CUSTOMDRAW) {
        auto* cd = (NMLVCUSTOMDRAW*)hdr;
        switch (cd->nmcd.dwDrawStage) {
            case CDDS_PREPAINT:
                return CDRF_NOTIFYITEMDRAW;
            case CDDS_ITEMPREPAINT:
                return CDRF_NOTIFYSUBITEMDRAW;
            case (CDDS_ITEMPREPAINT | CDDS_SUBITEM): {
                if (cd->iSubItem == 1) {
                    int sev = (int)cd->nmcd.lItemlParam;
                    if (sev == 3) cd->clrText = ERROR_COLOR;
                    else if (sev == 2) cd->clrText = WARNING_COLOR;
                    else if (sev == 1) cd->clrText = SUCCESS_COLOR;
                    else cd->clrText = TEXT_COLOR;
                } else {
                    cd->clrText = TEXT_COLOR;
                }
                cd->clrTextBk = PANEL_COLOR;
                return CDRF_DODEFAULT;
            }
        }
    }
    return 0;
}

LRESULT MainWindow::HandleDrawItem(const DRAWITEMSTRUCT* dis) {
    if (!dis) return 0;
    if (dis->CtlType != ODT_BUTTON) return 0;

    HWND h = dis->hwndItem;
    bool is_known = (h == start_button_ || h == stop_button_ || h == force_cycle_button_ ||
                     h == clear_cache_button_ || h == export_configs_button_ || h == configure_telegram_button_);
    if (!is_known) return 0;

    HDC hdc = dis->hDC;
    RECT rc = dis->rcItem;

    bool disabled = (dis->itemState & ODS_DISABLED) != 0;
    bool pressed = (dis->itemState & ODS_SELECTED) != 0;

    COLORREF face = BUTTON_FACE;
    if (h == start_button_ && !disabled) face = ACCENT_COLOR;
    else if (h == stop_button_ && !disabled) face = ERROR_COLOR;

    if (disabled) face = RGB(55, 55, 65);
    if (pressed && !disabled) face = RGB(GetRValue(face) / 2, GetGValue(face) / 2, GetBValue(face) / 2);

    HBRUSH br = CreateSolidBrush(face);
    FillRect(hdc, &rc, br);
    DeleteObject(br);

    HPEN pen = CreatePen(PS_SOLID, 1, BORDER_COLOR);
    HGDIOBJ old_pen = SelectObject(hdc, pen);
    HGDIOBJ old_br = SelectObject(hdc, GetStockObject(HOLLOW_BRUSH));
    Rectangle(hdc, rc.left, rc.top, rc.right, rc.bottom);
    SelectObject(hdc, old_br);
    SelectObject(hdc, old_pen);
    DeleteObject(pen);

    wchar_t text[256];
    GetWindowTextW(h, text, 256);

    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, disabled ? TEXT_MUTED : TEXT_COLOR);
    if (font_bold_) SelectObject(hdc, font_bold_);

    RECT tr = rc;
    tr.left += Scale(8);
    tr.right -= Scale(8);
    DrawTextW(hdc, text, -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    return TRUE;
}

void MainWindow::StartHunting() {
    AddLogEntry(L"INFO", L"Starting Hunter orchestrator...");
    
    try {
        if (orchestrator_running_.load()) {
            AddLogEntry(L"INFO", L"Already running");
            return;
        }

        // Create orchestrator if not exists
        if (!orchestrator_) {
            orchestrator_ = std::make_unique<HunterOrchestrator>(config_);
        }

        orchestrator_running_ = true;

        // Start orchestrator in background thread
        if (orchestrator_thread_.joinable()) {
            orchestrator_thread_.join();
        }
        orchestrator_thread_ = std::thread([this]() {
            try {
                if (orchestrator_) orchestrator_->start();
            } catch (const std::exception& e) {
                QueueLog(L"ERROR", std::wstring(L"Orchestrator error: ") + std::wstring(e.what(), e.what() + strlen(e.what())));
            }
            orchestrator_running_ = false;
        });
        
        SetRunning(true);
        AddLogEntry(L"SUCCESS", L"Hunter started successfully");
        
    } catch (const std::exception& e) {
        AddLogEntry(L"ERROR", std::wstring(L"Failed to start: ") + std::wstring(e.what(), e.what() + strlen(e.what())));
    }
}

void MainWindow::StopHunting() {
    AddLogEntry(L"INFO", L"Stopping Hunter orchestrator...");
    
    if (orchestrator_) {
        orchestrator_->stop();
    }

    if (orchestrator_thread_.joinable()) {
        orchestrator_thread_.join();
    }

    orchestrator_running_ = false;
    orchestrator_.reset();
    
    SetRunning(false);
    AddLogEntry(L"SUCCESS", L"Hunter stopped");
}

void MainWindow::QueueLog(const std::wstring& level, const std::wstring& message) {
    {
        std::lock_guard<std::mutex> lock(pending_logs_mutex_);
        pending_logs_.emplace_back(level, message);
    }
    if (hwnd_) {
        PostMessage(hwnd_, WM_APP_DRAIN_LOGS, 0, 0);
    }
}

void MainWindow::DrainLogs() {
    std::vector<std::pair<std::wstring, std::wstring>> logs;
    {
        std::lock_guard<std::mutex> lock(pending_logs_mutex_);
        logs.swap(pending_logs_);
    }
    for (auto& p : logs) {
        AddLogEntry(p.first, p.second);
    }
}

void MainWindow::ForceCycle() {
    AddLogEntry(L"INFO", L"Force cycle requested");
    // TODO: Implement force cycle
}

void MainWindow::ClearCache() {
    AddLogEntry(L"INFO", L"Clearing cache...");
    // TODO: Implement cache clearing
}

void MainWindow::ExportConfigs() {
    AddLogEntry(L"INFO", L"Exporting configs...");
    // TODO: Implement config export
}

void MainWindow::ConfigureTelegram() {
    AddLogEntry(L"INFO", L"Opening Telegram configuration...");
    // TODO: Implement Telegram config dialog
}

} // namespace win32
} // namespace hunter
