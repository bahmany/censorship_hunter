#include "win32/imgui_app.h"
#include "resource.h"
#include <windows.h>
#include <string>

static void AppendUiLog(const wchar_t* line) {
    wchar_t path[MAX_PATH];
    DWORD n = GetTempPathW(MAX_PATH, path);
    if (n == 0 || n >= MAX_PATH) return;
    lstrcatW(path, L"huntercensor_ui.log");

    HANDLE h = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;

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

static void SetCwdToExeDir() {
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return;
    wchar_t* last = wcsrchr(path, L'\\');
    if (!last) return;
    *last = 0;

    std::wstring probe = path;
    for (int i = 0; i < 5; ++i) {
        if (PathExists(probe + L"\\bin\\xray.exe")) {
            SetCurrentDirectoryW(probe.c_str());
            CreateDirectoryW(L"runtime", nullptr);
            return;
        }
        size_t sep = probe.find_last_of(L"\\/");
        if (sep == std::wstring::npos) break;
        probe.resize(sep);
    }

    SetCurrentDirectoryW(path);

    // Ensure local runtime folder exists for relative paths (runtime/...) when launched from bin
    CreateDirectoryW(L"runtime", nullptr);
}

static void EnableDpiAwareness() {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32) {
        using SetDpiAwarenessContextFn = DPI_AWARENESS_CONTEXT(WINAPI*)(DPI_AWARENESS_CONTEXT);
        auto set_context = reinterpret_cast<SetDpiAwarenessContextFn>(GetProcAddress(user32, "SetProcessDpiAwarenessContext"));
        if (set_context && set_context(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {
            return;
        }
    }
    SetProcessDPIAware();
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, PWSTR lpCmdLine, int nCmdShow) {
    (void)hPrevInstance;
    (void)lpCmdLine;

    AppendUiLog(L"wWinMain enter");

    SetCwdToExeDir();

    EnableDpiAwareness();

    // Create and show main window
    hunter::win32::ImGuiApp mainWindow;

    if (!mainWindow.Create(hInstance, nCmdShow)) {
        AppendUiLog(L"MainWindow::Create failed");
        MessageBoxW(nullptr, L"Failed to create main window", L"Error", MB_OK | MB_ICONERROR | MB_TOPMOST);
        return 1;
    }

    AppendUiLog(L"MainWindow::Create ok, entering message loop");
    return mainWindow.Run();
}
