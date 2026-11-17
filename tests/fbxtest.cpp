// Integration test for FBX Pin & Roll-up. It opens real windows, starts
// fbx.exe and drives the title bar buttons the way a user would.
//   fbxtest <path to fbx.exe> [--shot <png path>]
// License: GPL-3.0-or-later.
#ifndef UNICODE
#define UNICODE
#endif
#include <windows.h>
#include <dwmapi.h>
#include <objidl.h>
#include <gdiplus.h>
#include <cstdio>
#include <string>
#include <functional>
#include <vector>

static const wchar_t* kBarClass = L"FbxPinRollBar";
static const wchar_t* kMainClass = L"FbxPinRollMain";
static const wchar_t* kTargetProp = L"FbxTarget";

static int g_fail = 0, g_pass = 0;
static std::wstring g_fbx, g_ini, g_shot;
static HWND g_target, g_second;
static HANDLE g_ready;
static PROCESS_INFORMATION g_pi;

static void Check(bool ok, const char* what)
{
    printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
    fflush(stdout);
    if (ok) g_pass++; else g_fail++;
}

static bool WaitFor(const std::function<bool()>& f, int ms = 5000)
{
    for (int t = 0; t <= ms; t += 50) {
        if (f()) return true;
        Sleep(50);
    }
    return f();
}

static LRESULT CALLBACK TargetProc(HWND w, UINT m, WPARAM wp, LPARAM lp)
{
    if (m == WM_DESTROY && w == g_target) PostQuitMessage(0);
    return DefWindowProcW(w, m, wp, lp);
}

static DWORD WINAPI TargetThread(void*)
{
    WNDCLASSW wc = {};
    wc.lpfnWndProc = TargetProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.hCursor = LoadCursorW(nullptr, (LPCWSTR)IDC_ARROW);
    wc.lpszClassName = L"FbxTestTarget";
    RegisterClassW(&wc);
    g_target = CreateWindowExW(0, wc.lpszClassName, L"FBX test window", WS_OVERLAPPEDWINDOW, 200, 150, 700, 450,
                               nullptr, nullptr, wc.hInstance, nullptr);
    g_second = CreateWindowExW(0, wc.lpszClassName, L"FBX second window", WS_OVERLAPPEDWINDOW, 260, 420, 500, 300,
                               nullptr, nullptr, wc.hInstance, nullptr);
    ShowWindow(g_target, SW_SHOWNORMAL);
    SetEvent(g_ready);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}

struct FindCtx { HWND target; HWND bar; int count; };
static BOOL CALLBACK FindProc(HWND w, LPARAM lp)
{
    auto* c = (FindCtx*)lp;
    wchar_t cls[64];
    GetClassNameW(w, cls, 64);
    if (wcscmp(cls, kBarClass)) return TRUE;
    c->count++;
    if (IsWindowVisible(w) && (HWND)GetPropW(w, kTargetProp) == c->target) c->bar = w;
    return TRUE;
}
static HWND FindBar(HWND target)
{
    FindCtx c = { target, nullptr, 0 };
    EnumWindows(FindProc, (LPARAM)&c);
    return c.bar;
}
static int CountBars()
{
    FindCtx c = { nullptr, nullptr, 0 };
    EnumWindows(FindProc, (LPARAM)&c);
    return c.count;
}

static bool StartFbx()
{
    std::wstring cmd = L"\"" + g_fbx + L"\" --no-tray --config \"" + g_ini + L"\"";
    STARTUPINFOW si = { sizeof(si) };
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(0);
    if (!CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &g_pi)) return false;
    return WaitFor([] { return FindWindowW(kMainClass, nullptr) != nullptr; }, 10000);
}

static void Click(HWND bar, double fraction)
{
    RECT rc;
    GetClientRect(bar, &rc);
    LPARAM lp = MAKELPARAM((int)(rc.right * fraction), rc.bottom / 2);
    PostMessageW(bar, WM_MOUSEMOVE, 0, lp);
    PostMessageW(bar, WM_LBUTTONDOWN, MK_LBUTTON, lp);
    PostMessageW(bar, WM_LBUTTONUP, 0, lp);
}
static void ClickRoll(HWND t) { Click(FindBar(t), 0.25); } // left button
static void ClickPin(HWND t) { Click(FindBar(t), 0.75); }  // right button

static RECT Rect(HWND w) { RECT r = {}; GetWindowRect(w, &r); return r; }
static int Height(HWND w) { RECT r = Rect(w); return r.bottom - r.top; }
static bool Topmost(HWND w) { return (GetWindowLongW(w, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0; }

static void DumpZ(HWND t)
{
    printf("Z order from the top (target=%p, bar=%p):\n", (void*)t, (void*)FindBar(t));
    int n = 0;
    for (HWND w = GetTopWindow(nullptr); w && n < 40; w = GetWindow(w, GW_HWNDNEXT), n++) {
        wchar_t cls[64] = L"";
        GetClassNameW(w, cls, 64);
        DWORD pid = 0;
        GetWindowThreadProcessId(w, &pid);
        printf("  %p vis=%d top=%d pid=%lu %ls\n", (void*)w, IsWindowVisible(w), Topmost(w), pid, cls);
        if (w == t) break;
    }
}

static const char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static void PrintBase64(const std::vector<BYTE>& d)
{
    std::string out;
    for (size_t i = 0; i < d.size(); i += 3) {
        unsigned v = d[i] << 16 | (i + 1 < d.size() ? d[i + 1] << 8 : 0) | (i + 2 < d.size() ? d[i + 2] : 0);
        out += kB64[v >> 18 & 63];
        out += kB64[v >> 12 & 63];
        out += i + 1 < d.size() ? kB64[v >> 6 & 63] : '=';
        out += i + 2 < d.size() ? kB64[v & 63] : '=';
    }
    for (size_t i = 0; i < out.size(); i += 120) printf("%s\n", out.substr(i, 120).c_str());
}

static void Shot(const RECT& r, const wchar_t* tag)
{
    if (g_shot.empty()) return;
    using namespace Gdiplus;
    int w = r.right - r.left, h = r.bottom - r.top;
    HDC scr = GetDC(nullptr), mem = CreateCompatibleDC(scr);
    HBITMAP bmp = CreateCompatibleBitmap(scr, w, h);
    HGDIOBJ old = SelectObject(mem, bmp);
    BitBlt(mem, 0, 0, w, h, scr, r.left, r.top, SRCCOPY | CAPTUREBLT);
    SelectObject(mem, old);
    CLSID png;
    CLSIDFromString(L"{557CF406-1A04-11D3-9A73-0000F81EF32E}", &png);
    std::wstring path = g_shot + L"-" + tag + L".png";
    {
        Bitmap b(bmp, nullptr);
        b.Save(path.c_str(), &png, nullptr);
    }
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, scr);
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
        std::vector<BYTE> data(GetFileSize(f, nullptr));
        DWORD n = 0;
        ReadFile(f, data.data(), (DWORD)data.size(), &n, nullptr);
        CloseHandle(f);
        printf("-----BEGIN PNG %ls-----\n", tag);
        PrintBase64(data);
        printf("-----END PNG %ls-----\n", tag);
        fflush(stdout);
    }
}

static RECT TitleArea(HWND w)
{
    RECT r = Rect(w), e;
    if (SUCCEEDED(DwmGetWindowAttribute(w, DWMWA_EXTENDED_FRAME_BOUNDS, &e, sizeof(e))) && e.right > e.left) r = e;
    int s = (int)GetDpiForWindow(w);
    RECT out = { r.right - MulDiv(330, s, 96), r.top - 6, r.right + 6, r.top + MulDiv(46, s, 96) };
    return out;
}

int wmain(int argc, wchar_t** argv)
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (argc < 2) {
        printf("usage: fbxtest <fbx.exe> [--shot <prefix>]\n");
        return 2;
    }
    g_fbx = argv[1];
    for (int i = 2; i + 1 < argc; i++)
        if (!wcscmp(argv[i], L"--shot")) g_shot = argv[i + 1];
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    g_ini = std::wstring(tmp) + L"fbxtest.ini";
    DeleteFileW(g_ini.c_str());

    Gdiplus::GdiplusStartupInput gin;
    ULONG_PTR gtok;
    Gdiplus::GdiplusStartup(&gtok, &gin, nullptr);

    g_ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    CreateThread(nullptr, 0, TargetThread, nullptr, 0, nullptr);
    WaitForSingleObject(g_ready, 10000);
    Sleep(500);
    HWND t = g_target;
    int dpi = (int)GetDpiForWindow(t);
    auto D = [dpi](int v) { return MulDiv(v, dpi, 96); };
    printf("target=%p dpi=%d screen=%dx%d\n", (void*)t, dpi, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN));

    Check(StartFbx(), "fbx.exe starts");
    Check(WaitFor([&] { return FindBar(t) != nullptr; }, 10000), "buttons appear on an existing window");
    HWND bar = FindBar(t);
    if (!bar) {
        printf("no bar - aborting\n");
        return 1;
    }

    // --- placement
    RECT wr = Rect(t), efb = wr, cb = {}, br = Rect(bar);
    HRESULT hrE = DwmGetWindowAttribute(t, DWMWA_EXTENDED_FRAME_BOUNDS, &efb, sizeof(efb));
    if (FAILED(hrE) || efb.right <= efb.left) efb = wr;
    HRESULT hrC = DwmGetWindowAttribute(t, DWMWA_CAPTION_BUTTON_BOUNDS, &cb, sizeof(cb));
    bool haveCb = SUCCEEDED(hrC) && cb.right > cb.left;
    printf("window  %ld,%ld-%ld,%ld\nframe   %ld,%ld-%ld,%ld (hr=%08lx)\ncaption %ld,%ld-%ld,%ld (hr=%08lx, window relative)\nbar     %ld,%ld-%ld,%ld\n",
           wr.left, wr.top, wr.right, wr.bottom, efb.left, efb.top, efb.right, efb.bottom, (unsigned long)hrE,
           cb.left, cb.top, cb.right, cb.bottom, (unsigned long)hrC, br.left, br.top, br.right, br.bottom);
    Check(br.top >= efb.top - 1 && br.bottom <= efb.top + D(48), "buttons sit inside the title bar vertically");
    Check(br.right <= efb.right - D(40) && br.left >= efb.right - D(280), "buttons sit left of the caption buttons");
    Check(br.right - br.left == D(64), "two buttons of 32 px (scaled) each");
    if (haveCb) {
        int gap = (wr.left + cb.left) - br.right;
        printf("gap to caption buttons = %d px\n", gap);
        Check(gap >= 0 && gap <= D(6), "buttons touch the minimize button without overlapping it");
    } else {
        printf("note: compositor did not report caption button bounds; fallback layout in use\n");
    }
    bool zok = WaitFor([&] { return GetWindow(t, GW_HWNDPREV) == FindBar(t); }, 3000);
    Check(zok, "bar is directly above its window in the Z order");
    if (!zok) DumpZ(t);
    SetForegroundWindow(t);
    Sleep(1200);
    Shot(TitleArea(t), L"normal");

    // --- push-pin
    ClickPin(t);
    Check(WaitFor([&] { return Topmost(t); }), "pin: window becomes always-on-top");
    Check(WaitFor([&] { return Topmost(FindBar(t)) && GetWindow(t, GW_HWNDPREV) == FindBar(t); }),
          "pin: bar follows into the topmost band");
    Sleep(400);
    Shot(TitleArea(t), L"pinned");
    ClickPin(t);
    Check(WaitFor([&] { return !Topmost(t); }), "pin: second click releases the window");

    // --- roll-up
    int h0 = Height(t), w0 = wr.right - wr.left;
    ClickRoll(t);
    Check(WaitFor([&] { return Height(t) < D(70); }), "roll: window shrinks to its title bar");
    RECT rr = Rect(t);
    printf("rolled height = %d px (was %d)\n", Height(t), h0);
    Check(rr.right - rr.left == w0 && rr.left == wr.left && rr.top == wr.top, "roll: position and width unchanged");
    Check(WaitFor([&] { return FindBar(t) != nullptr; }), "roll: buttons stay on the rolled window");
    Sleep(400);
    Shot(TitleArea(t), L"rolled");

    RECT b0 = Rect(FindBar(t));
    SetWindowPos(t, nullptr, wr.left + 120, wr.top + 60, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    Check(WaitFor([&] { RECT b = Rect(FindBar(t)); return b.left == b0.left + 120 && b.top == b0.top + 60; }),
          "buttons follow the window when it is moved");
    Check(Height(t) < D(70), "roll: window stays rolled while moved");
    ClickRoll(t);
    Check(WaitFor([&] { return Height(t) == h0; }), "unroll: original height restored");
    rr = Rect(t);
    Check(rr.left == wr.left + 120 && rr.top == wr.top + 60, "unroll: window stays where it was moved to");

    // --- pin while rolled survives unroll
    ClickRoll(t);
    WaitFor([&] { return Height(t) < D(70); });
    ClickPin(t);
    WaitFor([&] { return Topmost(t); });
    ClickRoll(t);
    Check(WaitFor([&] { return Height(t) == h0; }) && Topmost(t), "pin set while rolled is kept after unroll");
    ClickPin(t);
    WaitFor([&] { return !Topmost(t); });

    // --- minimize / restore, second window
    ShowWindow(t, SW_MINIMIZE);
    Check(WaitFor([&] { return FindBar(t) == nullptr; }), "buttons hide when the window is minimized");
    ShowWindow(t, SW_RESTORE);
    Check(WaitFor([&] { return FindBar(t) != nullptr; }), "buttons return when it is restored");
    ShowWindow(g_second, SW_SHOWNOACTIVATE);
    Check(WaitFor([&] { return FindBar(g_second) != nullptr; }), "a newly shown window gets buttons");
    ShowWindow(g_second, SW_HIDE);
    Check(WaitFor([&] { return FindBar(g_second) == nullptr; }), "buttons hide with their window");

    // --- maximized
    ShowWindow(t, SW_MAXIMIZE);
    Sleep(800);
    WaitFor([&] { return FindBar(t) != nullptr; });
    if (HWND mb = FindBar(t)) {
        RECT m = Rect(mb);
        MONITORINFO mi = { sizeof(mi) };
        GetMonitorInfoW(MonitorFromWindow(t, MONITOR_DEFAULTTONEAREST), &mi);
        printf("maximized: bar %ld,%ld-%ld,%ld monitor %ld,%ld-%ld,%ld\n", m.left, m.top, m.right, m.bottom,
               mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right, mi.rcMonitor.bottom);
        Check(m.top >= mi.rcMonitor.top && m.right <= mi.rcMonitor.right - D(40) && m.bottom <= mi.rcMonitor.top + D(48),
              "maximized: buttons are on screen inside the title bar");
        RECT area = { mi.rcMonitor.right - D(330), mi.rcMonitor.top, mi.rcMonitor.right, mi.rcMonitor.top + D(46) };
        Shot(area, L"maximized");
    } else {
        Check(false, "maximized: buttons are shown");
    }
    ShowWindow(t, SW_RESTORE);
    WaitFor([&] { return Height(t) == h0; });
    Sleep(300);

    // --- clean exit unrolls
    ClickRoll(t);
    Check(WaitFor([&] { return Height(t) < D(70); }), "roll again before exit");
    {
        std::wstring cmd = L"\"" + g_fbx + L"\" --exit";
        STARTUPINFOW si = { sizeof(si) };
        PROCESS_INFORMATION pi;
        std::vector<wchar_t> buf(cmd.begin(), cmd.end());
        buf.push_back(0);
        if (CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
            WaitForSingleObject(pi.hProcess, 10000);
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
        }
    }
    Check(WaitForSingleObject(g_pi.hProcess, 10000) == WAIT_OBJECT_0, "fbx.exe --exit stops the running instance");
    Check(WaitFor([&] { return Height(t) == h0; }), "exit: rolled windows are unrolled");
    Check(CountBars() == 0, "exit: no button windows are left behind");

    // --- crash recovery
    Check(StartFbx() && WaitFor([&] { return FindBar(t) != nullptr; }, 10000), "restart");
    ClickRoll(t);
    WaitFor([&] { return Height(t) < D(70); });
    TerminateProcess(g_pi.hProcess, 9);
    WaitForSingleObject(g_pi.hProcess, 5000);
    Check(Height(t) < D(70), "after a crash the window is still rolled");
    Check(StartFbx() && WaitFor([&] { return FindBar(t) != nullptr; }, 10000), "restart after crash");
    Sleep(500);
    ClickRoll(t);
    Check(WaitFor([&] { return Height(t) == h0; }), "crash recovery: one click restores the original height");

    PostMessageW(FindWindowW(kMainClass, nullptr), WM_CLOSE, 0, 0);
    WaitForSingleObject(g_pi.hProcess, 10000);
    PostMessageW(t, WM_CLOSE, 0, 0);
    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
