#include "win32/imgui_app.h"
#include "resource.h"
#include <windows.h>
#include <shellapi.h>
#include <securitybaseapi.h>
#include <string>

// Global mutex handle for single-instance check - kept alive for application lifetime
static HANDLE g_hSingleInstanceMutex = nullptr;

static void AppendUiLog(const wchar_t* line) {
    wchar_t path[MAX_PATH];
    DWORD n = GetTempPathW(MAX_PATH, path);
    if (n == 0 || n >= MAX_PATH) return;
    lstrcatW(path, L"huntercensor_ui.log");

    HANDLE h = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        // Silently fail for logging - can't log a logging failure
        (void)err;
        return;
    }

    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t buf[2048];
    int len = swprintf(buf, 2048, L"%04u-%02u-%02u %02u:%02u:%02u.%03u %s\r\n",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, line ? line : L"");

    if (len > 0) {
        DWORD bytes = 0;
        WriteFile(h, buf, (DWORD)(len * sizeof(wchar_t)), &bytes, nullptr);
    }
    CloseHandle(h);
}

static bool PathExists(const std::wstring& path) {
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

// Check if current process is running with administrator privileges
static bool IsRunningAsAdmin() {
    BOOL isAdmin = FALSE;
    PSID administratorsGroup = nullptr;
    SID_IDENTIFIER_AUTHORITY ntAuthority = SECURITY_NT_AUTHORITY;
    
    if (AllocateAndInitializeSid(&ntAuthority, 2, SECURITY_BUILTIN_DOMAIN_RID, 
                                 DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, 
                                 &administratorsGroup)) {
        CheckTokenMembership(nullptr, administratorsGroup, &isAdmin);
        FreeSid(administratorsGroup);
    }
    
    return isAdmin == TRUE;
}

// Restart current application with administrator privileges
// Returns true if restart was initiated, false on failure
static bool RestartAsAdmin(const std::wstring& args = L"") {
    wchar_t exePath[MAX_PATH];
    if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) == 0) {
        DWORD err = GetLastError();
        AppendUiLog((L"RestartAsAdmin: GetModuleFileNameW failed, error=" + std::to_wstring(err)).c_str());
        return false;
    }
    
    AppendUiLog(L"RestartAsAdmin: Requesting elevation...");
    
    SHELLEXECUTEINFOW sei = { sizeof(sei) };
    sei.lpVerb = L"runas";  // This triggers UAC elevation
    sei.lpFile = exePath;
    sei.lpParameters = args.empty() ? nullptr : args.c_str();
    sei.nShow = SW_NORMAL;
    sei.fMask = SEE_MASK_NO_CONSOLE | SEE_MASK_FLAG_NO_UI;
    
    if (!ShellExecuteExW(&sei)) {
        DWORD err = GetLastError();
        AppendUiLog((L"RestartAsAdmin: ShellExecuteExW failed, error=" + std::to_wstring(err)).c_str());
        return false;
    }
    
    AppendUiLog(L"RestartAsAdmin: Elevation request succeeded, new process started");
    return true;
}

static void SetCwdToExeDir() {
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        DWORD err = GetLastError();
        AppendUiLog((L"SetCwdToExeDir: GetModuleFileNameW failed, error=" + std::to_wstring(err)).c_str());
        return;
    }
    wchar_t* last = wcsrchr(path, L'\\');
    if (!last) {
        AppendUiLog(L"SetCwdToExeDir: No backslash in module path");
        return;
    }
    *last = 0;

    std::wstring probe = path;
    for (int i = 0; i < 5; ++i) {
        if (PathExists(probe + L"\\bin\\xray.exe")) {
            if (!SetCurrentDirectoryW(probe.c_str())) {
                DWORD err = GetLastError();
                AppendUiLog((L"SetCwdToExeDir: SetCurrentDirectoryW failed, error=" + std::to_wstring(err)).c_str());
                return;
            }
            CreateDirectoryW(L"runtime", nullptr);
            return;
        }
        size_t sep = probe.find_last_of(L"\\/");
        if (sep == std::wstring::npos) break;
        probe.resize(sep);
    }

    if (!SetCurrentDirectoryW(path)) {
        DWORD err = GetLastError();
        AppendUiLog((L"SetCwdToExeDir: SetCurrentDirectoryW (fallback) failed, error=" + std::to_wstring(err)).c_str());
        return;
    }

    // Ensure local runtime folder exists for relative paths (runtime/...) when launched from bin
    CreateDirectoryW(L"runtime", nullptr);
}

static void EnableDpiAwareness() {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32) {
        using SetDpiAwarenessContextFn = DPI_AWARENESS_CONTEXT(WINAPI*)(DPI_AWARENESS_CONTEXT);
        auto set_context = reinterpret_cast<SetDpiAwarenessContextFn>(GetProcAddress(user32, "SetProcessDpiAwarenessContext"));
        if (set_context && set_context(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {
            AppendUiLog(L"DPI awareness: PerMonitorAwareV2 enabled");
            return;
        }
        // Fallback to PerMonitorAware (Windows 8.1+)
        if (set_context && set_context(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE)) {
            AppendUiLog(L"DPI awareness: PerMonitorAware enabled");
            return;
        }
    }
    // Final fallback: Windows 7 compatible DPI awareness
    SetProcessDPIAware();
    AppendUiLog(L"DPI awareness: SetProcessDPIAware (Windows 7 compatible)");
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, PWSTR lpCmdLine, int nCmdShow) {
    (void)hPrevInstance;

    AppendUiLog(L"wWinMain enter");

    // Single-instance check: create named mutex
    g_hSingleInstanceMutex = CreateMutexW(nullptr, FALSE, L"HunterCensor_SingleInstance_Mutex_v1");
    if (!g_hSingleInstanceMutex) {
        DWORD err = GetLastError();
        AppendUiLog((L"CreateMutex failed, error=" + std::to_wstring(err)).c_str());
        // Continue anyway - mutex creation failed but we can still run
    } else if (GetLastError() == ERROR_ALREADY_EXISTS) {
        AppendUiLog(L"Another instance already running - bringing to front");
        // Find existing window and bring to front
        HWND existingWnd = FindWindowW(L"HunterCensorImGuiClass", L"Hunter Censor");
        if (!existingWnd) {
            // Try without specific title
            existingWnd = FindWindowW(L"HunterCensorImGuiClass", nullptr);
        }
        if (existingWnd) {
            // Restore if minimized
            if (IsIconic(existingWnd)) {
                ShowWindow(existingWnd, SW_RESTORE);
            }
            // Bring to front
            SetForegroundWindow(existingWnd);
            FlashWindow(existingWnd, TRUE);
            AppendUiLog(L"Existing window brought to front");
        } else {
            AppendUiLog(L"Could not find existing window");
        }
        CloseHandle(g_hSingleInstanceMutex);
        g_hSingleInstanceMutex = nullptr;
        return 0; // Exit this instance
    }

    const std::wstring cmd_line = lpCmdLine ? lpCmdLine : L"";
    const bool resume_edge_bypass = cmd_line.find(L"--resume-edge-bypass") != std::wstring::npos;
    const bool require_admin = cmd_line.find(L"--require-admin") != std::wstring::npos;

    // Check if admin elevation is required but not active
    if (require_admin && !IsRunningAsAdmin()) {
        AppendUiLog(L"Admin required but not running as admin - restarting with elevation...");
        if (RestartAsAdmin(cmd_line)) {
            // Successfully initiated restart with elevation - exit current instance
            if (g_hSingleInstanceMutex) {
                CloseHandle(g_hSingleInstanceMutex);
                g_hSingleInstanceMutex = nullptr;
            }
            AppendUiLog(L"Exiting to allow elevated instance to take over");
            return 0;
        } else {
            AppendUiLog(L"Failed to restart with elevation - continuing without admin rights");
            // Optionally show error message
            MessageBoxW(nullptr, L"Failed to obtain administrator privileges. Some features may not work correctly.", 
                       L"Elevation Failed", MB_OK | MB_ICONWARNING | MB_TOPMOST);
        }
    }

    SetCwdToExeDir();

    EnableDpiAwareness();

    // Create and show main window
    hunter::win32::ImGuiApp mainWindow;
    if (resume_edge_bypass) {
        AppendUiLog(L"wWinMain resume-edge-bypass detected");
        mainWindow.ArmPendingElevatedEdgeBypass();
    }

    if (!mainWindow.Create(hInstance, nCmdShow)) {
        AppendUiLog(L"MainWindow::Create failed");
        MessageBoxW(nullptr, L"Failed to create main window", L"Error", MB_OK | MB_ICONERROR | MB_TOPMOST);
        return 1;
    }

    AppendUiLog(L"MainWindow::Create ok, entering message loop");
    return mainWindow.Run();
}
