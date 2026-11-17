// FBX Pin & Roll-up - entry point, tray icon and menu.
// License: GPL-3.0-or-later.
#include "common.h"
#include <shellapi.h>

#define FBX_VERSION L"3.0.0-preview"
#define WM_TRAY (WM_APP + 1)
enum { IDM_PIN = 100, IDM_ROLL, IDM_ROLLTOP, IDM_AUTOSTART, IDM_EDIT, IDM_RELOAD, IDM_ABOUT, IDM_EXIT };
enum { HK_PIN = 1, HK_ROLL = 2 };

static HINSTANCE g_hinst;
static NOTIFYICONDATAW g_nid;
static bool g_tray = true;
static UINT g_taskbarCreated;

static void AddTray(HWND wnd)
{
    if (!g_tray) return;
    g_nid = {};
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = wnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAY;
    g_nid.hIcon = CreateAppIcon(GetSystemMetrics(SM_CXSMICON));
    lstrcpynW(g_nid.szTip, L"FBX Pin & Roll-up", 64);
    Shell_NotifyIconW(NIM_ADD, &g_nid);
}

static void RegisterHotkeys(HWND wnd)
{
    UnregisterHotKey(wnd, HK_PIN);
    UnregisterHotKey(wnd, HK_ROLL);
    if (g_cfg.pinKey.vk) RegisterHotKey(wnd, HK_PIN, g_cfg.pinKey.mod | MOD_NOREPEAT, g_cfg.pinKey.vk);
    if (g_cfg.rollKey.vk) RegisterHotKey(wnd, HK_ROLL, g_cfg.rollKey.mod | MOD_NOREPEAT, g_cfg.rollKey.vk);
}

static void ShowMenu(HWND wnd)
{
    HMENU m = CreatePopupMenu();
    auto chk = [](bool b) { return (UINT)(MF_STRING | (b ? MF_CHECKED : 0)); };
    AppendMenuW(m, chk(g_cfg.pin), IDM_PIN, L"Push-pin button to keep windows on top");
    AppendMenuW(m, chk(g_cfg.roll), IDM_ROLL, L"Roll-up button to display only the title bar");
    AppendMenuW(m, chk(g_cfg.rolledTopmost) | (g_cfg.roll ? 0 : MF_GRAYED), IDM_ROLLTOP,
                L"When rolled up, keep the window bar on top");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, chk(IsAutoStart()), IDM_AUTOSTART, L"Start with Windows");
    AppendMenuW(m, MF_STRING, IDM_EDIT, L"Edit settings and exceptions...");
    AppendMenuW(m, MF_STRING, IDM_RELOAD, L"Reload settings");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, IDM_ABOUT, L"About");
    AppendMenuW(m, MF_STRING, IDM_EXIT, L"Exit");
    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(wnd);
    TrackPopupMenu(m, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, wnd, nullptr);
    PostMessageW(wnd, WM_NULL, 0, 0);
    DestroyMenu(m);
}

static LRESULT CALLBACK MainProc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == g_taskbarCreated && g_taskbarCreated) {
        AddTray(wnd); // Explorer restarted
        return 0;
    }
    switch (msg) {
    case WM_TIMER:
        TrackerOnTimer(wp);
        return 0;
    case WM_HOTKEY:
        TrackerToggleForeground(wp == HK_PIN ? BTN_PIN : BTN_ROLL);
        return 0;
    case WM_TRAY:
        if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_LBUTTONUP || LOWORD(lp) == WM_CONTEXTMENU) ShowMenu(wnd);
        return 0;
    case WM_SETTINGCHANGE:
    case WM_DISPLAYCHANGE:
    case WM_THEMECHANGED:
        TrackerRefreshAll();
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDM_PIN:
            g_cfg.pin = !g_cfg.pin;
            SaveToggles(g_cfg);
            TrackerRefreshAll();
            break;
        case IDM_ROLL:
            g_cfg.roll = !g_cfg.roll;
            if (!g_cfg.roll) TrackerUnrollAll(); // as FbX did
            SaveToggles(g_cfg);
            TrackerRefreshAll();
            break;
        case IDM_ROLLTOP:
            g_cfg.rolledTopmost = !g_cfg.rolledTopmost;
            SaveToggles(g_cfg);
            break;
        case IDM_AUTOSTART:
            SetAutoStart(!IsAutoStart());
            break;
        case IDM_EDIT:
            ShellExecuteW(wnd, L"open", L"notepad.exe", (L"\"" + g_cfg.path + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
            break;
        case IDM_RELOAD:
            LoadConfig(g_cfg);
            RegisterHotkeys(wnd);
            if (!g_cfg.roll) TrackerUnrollAll();
            TrackerRefreshAll();
            break;
        case IDM_ABOUT:
            MessageBoxW(wnd,
                        L"FBX Pin & Roll-up " FBX_VERSION L"\n\n"
                        L"Push-pin (always on top) and roll-up title bar buttons for Windows 10 and 11.\n\n"
                        L"A rewrite of these two functions of FileBox eXtender\n"
                        L"(C) 1999-2011 Greg Kochaniak, Hyperionics Technology LLC.\n\n"
                        L"Free software under the GNU General Public License v3 or later.",
                        L"About FBX", MB_OK | MB_ICONINFORMATION);
            break;
        case IDM_EXIT:
            DestroyWindow(wnd);
            break;
        }
        return 0;
    case WM_ENDSESSION:
        if (wp) TrackerShutdown();
        return 0;
    case WM_CLOSE:
        DestroyWindow(wnd);
        return 0;
    case WM_DESTROY:
        TrackerShutdown();
        if (g_tray) Shell_NotifyIconW(NIM_DELETE, &g_nid);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(wnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR, int)
{
    g_hinst = inst;
    bool wantExit = false;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; argv && i < argc; i++) {
        if (!lstrcmpiW(argv[i], L"--exit")) wantExit = true;
        else if (!lstrcmpiW(argv[i], L"--no-tray")) g_tray = false;
        else if (!lstrcmpiW(argv[i], L"--config") && i + 1 < argc) g_cfg.path = argv[++i];
    }
    if (argv) LocalFree(argv);

    if (wantExit) {
        // Ask a running instance to quit (it unrolls all windows first).
        HWND w = FindWindowW(FBX_MAIN_CLASS, nullptr);
        if (w) {
            PostMessageW(w, WM_CLOSE, 0, 0);
            for (int i = 0; i < 100 && IsWindow(w); i++) Sleep(50);
        }
        return 0;
    }

    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\FbxPinRollSingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    LoadConfig(g_cfg);
    RenderInit();

    WNDCLASSW wc = {};
    wc.lpfnWndProc = MainProc;
    wc.hInstance = inst;
    wc.lpszClassName = FBX_MAIN_CLASS;
    RegisterClassW(&wc);
    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    HWND wnd = CreateWindowExW(WS_EX_TOOLWINDOW, FBX_MAIN_CLASS, L"FBX Pin & Roll-up", WS_POPUP, 0, 0, 0, 0, nullptr,
                               nullptr, inst, nullptr);
    if (!wnd) return 1;

    AddTray(wnd);
    RegisterHotkeys(wnd);
    if (!TrackerInit(inst, wnd)) return 1;

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    RenderShutdown();
    if (mutex) CloseHandle(mutex);
    return 0;
}
