// Pin&Roll - entry point, tray icon and menu.
// License: GPL-3.0-or-later.
#include "common.h"
#include <shellapi.h>
#include "resource.h"
#include <commctrl.h>

#define PR_VERSION L"1.0.0"
#define WM_TRAY (WM_APP + 1)
enum { IDM_PIN = 100, IDM_ROLL, IDM_ROLLTOP, IDM_SETTINGS, IDM_EDIT, IDM_RELOAD, IDM_ABOUT, IDM_EXIT, IDM_DIAG, IDM_HELP, IDM_MORE };
enum { HK_PIN = 1, HK_ROLL = 2 };

static HINSTANCE g_hinst;
static NOTIFYICONDATAW g_nid;
static bool g_tray = true;
static UINT g_taskbarCreated;
static HWND g_mainWnd;
static std::wstring g_args; // our own command line switches, for restarting
static std::wstring g_settingsApp;

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
    lstrcpynW(g_nid.szTip, L"Pin&Roll", 64);
    Shell_NotifyIconW(NIM_ADD, &g_nid);
}

#define PR_MORE_URL L"https://github.com/krakozavr/Pin-And-Roll/blob/HEAD/MORE.md"
static FILETIME g_iniStamp;
static int g_iniTicks;

static FILETIME IniStamp()
{
    WIN32_FILE_ATTRIBUTE_DATA d = {};
    GetFileAttributesExW(g_cfg.path.c_str(), GetFileExInfoStandard, &d);
    return d.ftLastWriteTime;
}

static INT_PTR CALLBACK AboutProc(HWND d, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_INITDIALOG:
        SetDlgItemTextW(d, IDC_ABOUTTITLE, L"Pin&Roll " PR_VERSION);
        SetDlgItemTextW(d, IDC_ABOUTLINKS,
                        L"Developer: Alex Potemkin, ShotPorter\n"
                        L"Photographer, videographer, content creator? Try ShotPorter (Mac only): "
                        L"<a href=\"https://www.shotporter.com\">www.shotporter.com</a>\n"
                        L"Source and new versions: "
                        L"<a href=\"https://github.com/krakozavr/Pin-And-Roll\">github.com/krakozavr/Pin-And-Roll</a>");
        SetForegroundWindow(d);
        return TRUE;
    case WM_NOTIFY: {
        NMHDR* h = (NMHDR*)lp;
        if (h->idFrom == IDC_ABOUTLINKS && (h->code == NM_CLICK || h->code == NM_RETURN))
            ShellExecuteW(nullptr, L"open", ((NMLINK*)lp)->item.szUrl, nullptr, nullptr, SW_SHOWNORMAL);
        return TRUE;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK || LOWORD(wp) == IDCANCEL) EndDialog(d, 0);
        return TRUE;
    }
    return FALSE;
}

static void RegisterHotkeys(HWND wnd);
void AppRegisterHotkeys()
{
    if (g_mainWnd) RegisterHotkeys(g_mainWnd);
}

// Starts an elevated copy of Pin&Roll (Windows shows its permission prompt).
static bool LaunchElevated()
{
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring args = g_args + L" --wait";
    SHELLEXECUTEINFOW sei = {};
    sei.cbSize = sizeof(sei);
    sei.lpVerb = L"runas";
    sei.lpFile = exe;
    sei.lpParameters = args.c_str();
    sei.nShow = SW_SHOWNORMAL;
    return ShellExecuteExW(&sei) != 0;
}

static bool ExtractResource(int id, const std::wstring& file)
{
    HRSRC r = FindResourceW(g_hinst, MAKEINTRESOURCEW(id), (LPCWSTR)RT_RCDATA);
    HGLOBAL g = r ? LoadResource(g_hinst, r) : nullptr;
    const void* data = g ? LockResource(g) : nullptr;
    if (!data) return false;
    HANDLE f = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD w = 0;
    bool ok = WriteFile(f, data, SizeofResource(g_hinst, r), &w, nullptr) != 0;
    CloseHandle(f);
    return ok;
}

// The help page is built into the program; it is written to the temporary
// folder and opened with the default browser.
void AppShowHelp()
{
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    std::wstring dir = std::wstring(tmp) + L"PinRoll-help";
    CreateDirectoryW(dir.c_str(), nullptr);
    ExtractResource(IDR_HELP_PNG, dir + L"\\settings.png");
    if (ExtractResource(IDR_HELP_HTML, dir + L"\\help.html"))
        ShellExecuteW(nullptr, L"open", (dir + L"\\help.html").c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void AppOpenSettings(const std::wstring& exe)
{
    g_settingsApp = exe;
    if (g_mainWnd) PostMessageW(g_mainWnd, WM_COMMAND, IDM_SETTINGS, 0);
}

bool AppRestartElevated()
{
    if (!LaunchElevated()) return false;
    if (g_mainWnd) PostMessageW(g_mainWnd, WM_CLOSE, 0, 0); // the new copy waits for us to finish
    return true;
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
    AppendMenuW(m, MF_STRING, IDM_SETTINGS, L"Settings...");
    SetMenuDefaultItem(m, IDM_SETTINGS, FALSE);
    AppendMenuW(m, MF_STRING, IDM_DIAG, L"Save diagnostics...");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, IDM_HELP, L"Help");
    AppendMenuW(m, MF_STRING, IDM_ABOUT, L"About");
    AppendMenuW(m, MF_STRING, IDM_MORE, L"More from this developer");
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
        // The settings file was edited by hand: pick the changes up.
        if (wp == PR_TIMER_SLOW && ++g_iniTicks % 5 == 0 && !SettingsIsOpen()) {
            FILETIME now = IniStamp();
            if (CompareFileTime(&now, &g_iniStamp) != 0) {
                g_iniStamp = now;
                LoadConfig(g_cfg);
                RegisterHotkeys(wnd);
                if (!g_cfg.roll) TrackerUnrollAll();
                TrackerRefreshAll();
            }
        }
        return 0;
    case WM_HOTKEY:
        TrackerToggleForeground(wp == HK_PIN ? BTN_PIN : BTN_ROLL);
        return 0;
    case WM_TRAY:
        if (LOWORD(lp) == WM_LBUTTONDBLCLK) PostMessageW(wnd, WM_COMMAND, IDM_SETTINGS, 0);
        else if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_LBUTTONUP || LOWORD(lp) == WM_CONTEXTMENU) ShowMenu(wnd);
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
            g_iniStamp = IniStamp();
            TrackerRefreshAll();
            break;
        case IDM_ROLL:
            g_cfg.roll = !g_cfg.roll;
            if (!g_cfg.roll) TrackerUnrollAll(); // as FbX did
            SaveToggles(g_cfg);
            g_iniStamp = IniStamp();
            TrackerRefreshAll();
            break;
        case IDM_ROLLTOP:
            g_cfg.rolledTopmost = !g_cfg.rolledTopmost;
            SaveToggles(g_cfg);
            g_iniStamp = IniStamp();
            break;
        case IDM_SETTINGS: {
            std::wstring app;
            app.swap(g_settingsApp);
            ShowSettings(g_hinst, app);
            g_iniStamp = IniStamp(); // our own save is not a hand edit
            break;
        }
        case IDM_HELP:
            AppShowHelp();
            break;
        case IDM_DIAG: {
            std::wstring text, file;
            TrackerDiagnostics(text);
            wchar_t tmp[MAX_PATH];
            GetTempPathW(MAX_PATH, tmp);
            file = std::wstring(tmp) + L"pinroll-diagnostics.txt";
            HANDLE f = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (f != INVALID_HANDLE_VALUE) {
                DWORD w;
                WriteFile(f, "\xFF\xFE", 2, &w, nullptr);
                WriteFile(f, text.c_str(), (DWORD)(text.size() * sizeof(wchar_t)), &w, nullptr);
                CloseHandle(f);
                ShellExecuteW(wnd, L"open", L"notepad.exe", (L"\"" + file + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
            }
            break;
        }
        case IDM_ABOUT: {
            static bool open = false;
            if (open) break;
            open = true;
            INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_LINK_CLASS };
            InitCommonControlsEx(&icc);
            DialogBoxParamW(g_hinst, MAKEINTRESOURCEW(IDD_ABOUT), nullptr, AboutProc, 0);
            open = false;
            break;
        }
        case IDM_MORE:
            // A page in the repository, so it can be extended without a new release.
            ShellExecuteW(nullptr, L"open", PR_MORE_URL, nullptr, nullptr, SW_SHOWNORMAL);
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
    bool wantExit = false, wait = false;
    int autoStartCmd = -1;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; argv && i < argc; i++) {
        if (!lstrcmpiW(argv[i], L"--exit")) wantExit = true;
        else if (!lstrcmpiW(argv[i], L"--wait")) wait = true;
        else if (!lstrcmpiW(argv[i], L"--autostart") && i + 1 < argc) autoStartCmd = !lstrcmpiW(argv[++i], L"on");
        else if (!lstrcmpiW(argv[i], L"--no-tray")) {
            g_tray = false;
            g_args += L" --no-tray";
        } else if (!lstrcmpiW(argv[i], L"--config") && i + 1 < argc) {
            g_cfg.path = argv[++i];
            g_args += L" --config \"" + g_cfg.path + L"\"";
        }
    }
    if (argv) LocalFree(argv);

    if (wantExit) {
        // Ask a running instance to quit (it unrolls all windows first).
        HWND w = FindWindowW(PR_MAIN_CLASS, nullptr);
        if (w) {
            PostMessageW(w, WM_CLOSE, 0, 0);
            for (int i = 0; i < 100 && IsWindow(w); i++) Sleep(50);
        }
        return 0;
    }

    LoadConfig(g_cfg);

    if (autoStartCmd >= 0) { // pr.exe --autostart on|off
        SetAutoStart(autoStartCmd == 1, g_cfg.runAsAdmin);
        return 0;
    }

    // "Run as administrator" is set and we are not elevated: hand over to
    // an elevated copy. If the prompt is declined we carry on as we are.
    if (g_cfg.runAsAdmin && !IsElevated() && !FindWindowW(PR_MAIN_CLASS, nullptr) && LaunchElevated()) return 0;

    // One instance only. A copy started to replace us (--wait) is patient.
    HANDLE mutex = CreateMutexW(nullptr, FALSE, L"Local\\PinRollSingleInstance");
    DWORD got = mutex ? WaitForSingleObject(mutex, wait ? 10000 : 0) : WAIT_FAILED;
    if (got != WAIT_OBJECT_0 && got != WAIT_ABANDONED) return 0;

    // Elevated now, and start-up is still the plain kind: convert it so that
    // logging on does not bring up the permission prompt.
    MigrateOldAutoStart(g_cfg.runAsAdmin);
    if (g_cfg.runAsAdmin) ConvertAutoStartIfNeeded();

    RenderInit();

    WNDCLASSW wc = {};
    wc.lpfnWndProc = MainProc;
    wc.hInstance = inst;
    wc.lpszClassName = PR_MAIN_CLASS;
    RegisterClassW(&wc);
    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    HWND wnd = CreateWindowExW(WS_EX_TOOLWINDOW, PR_MAIN_CLASS, L"Pin&Roll", WS_POPUP, 0, 0, 0, 0, nullptr,
                               nullptr, inst, nullptr);
    if (!wnd) return 1;

    g_mainWnd = wnd;
    g_iniStamp = IniStamp();
    AddTray(wnd);
    RegisterHotkeys(wnd);
    if (!TrackerInit(inst, wnd)) return 1;

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    RenderShutdown();
    ReleaseMutex(mutex);
    CloseHandle(mutex);
    return 0;
}
