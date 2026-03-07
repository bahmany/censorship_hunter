#include "win32/main_window.h"
#include "resource.h"
#include <windows.h>

static void AppendUiLog(const wchar_t* line) {
    wchar_t path[MAX_PATH];
    DWORD n = GetTempPathW(MAX_PATH, path);
    if (n == 0 || n >= MAX_PATH) return;
    lstrcatW(path, L"hunter_ui.log");

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

static void SetCwdToExeDir() {
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return;
    wchar_t* last = wcsrchr(path, L'\\');
    if (!last) return;
    *last = 0;
    SetCurrentDirectoryW(path);

    // Ensure local runtime folder exists for relative paths (runtime/...) when launched from bin
    CreateDirectoryW(L"runtime", nullptr);
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, PWSTR lpCmdLine, int nCmdShow) {
    (void)hPrevInstance;
    (void)lpCmdLine;

    AppendUiLog(L"wWinMain enter");

    SetCwdToExeDir();

    // Enable modern visual styles
    SetProcessDPIAware();

    // Create and show main window
    hunter::win32::MainWindow mainWindow;

    if (!mainWindow.Create(hInstance, nCmdShow)) {
        AppendUiLog(L"MainWindow::Create failed");
        MessageBoxW(nullptr, L"Failed to create main window", L"Error", MB_OK | MB_ICONERROR | MB_TOPMOST);
        return 1;
    }

    AppendUiLog(L"MainWindow::Create ok, entering message loop");

    // Message loop
    MSG msg;
    while (true) {
        BOOL ok = GetMessageW(&msg, nullptr, 0, 0);
        if (ok > 0) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            continue;
        }
        if (ok == 0) {
            // WM_QUIT
            AppendUiLog(L"GetMessageW returned 0 (WM_QUIT)");
            return (int)msg.wParam;
        }
        // ok == -1
        AppendUiLog(L"GetMessageW returned -1");
        MessageBoxW(nullptr, L"GetMessageW failed (returned -1).", L"Hunter UI Error", MB_OK | MB_ICONERROR | MB_TOPMOST);
        return 2;
    }
}
