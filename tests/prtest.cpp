// Integration test for Pin&Roll. It opens real windows, starts
// pr.exe and drives the title bar buttons the way a user would.
//   prtest <path to pr.exe> [--shot <png path>]
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

static const wchar_t* kBarClass = L"PinRollBar";
static const wchar_t* kMainClass = L"PinRollMain";
static const wchar_t* kTargetProp = L"PinRollTarget";

static int g_fail = 0, g_pass = 0;
static std::wstring g_app, g_ini, g_shot;
static HWND g_target, g_second, g_custom;
// Layout of the self-drawn title bar of the "custom" test window (pixels at 96 dpi).
enum { CT_H = 48, CT_BTN = 60, CT_CTRL = 90 };
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
    if (m == WM_GETMINMAXINFO && w == g_second && g_second) {
        // like many real programs: refuses to be shorter than 200 px
        ((MINMAXINFO*)lp)->ptMinTrackSize.y = MulDiv(200, (int)GetDpiForWindow(w), 96);
        return 0;
    }
    return DefWindowProcW(w, m, wp, lp);
}

// A window that draws its own title bar the way browsers and Electron apps
// do: no non-client area, three wide buttons, and a control next to them.
static LRESULT CALLBACK CustomProc(HWND w, UINT m, WPARAM wp, LPARAM lp)
{
    int dpi = (int)GetDpiForWindow(w);
    auto D = [dpi](int v) { return MulDiv(v, dpi, 96); };
    switch (m) {
    case WM_NCCALCSIZE:
        return 0; // no non-client area at all: the client covers the whole window
    case WM_NCHITTEST: {
        RECT r;
        GetWindowRect(w, &r);
        int x = (short)LOWORD(lp), y = (short)HIWORD(lp);
        if (x < r.left || x >= r.right || y < r.top || y >= r.bottom) return HTNOWHERE;
        if (y >= r.top + D(CT_H)) return HTCLIENT;
        int fromRight = r.right - x;
        if (fromRight <= D(CT_BTN)) return HTCLOSE;
        if (fromRight <= 2 * D(CT_BTN)) return HTMAXBUTTON;
        if (fromRight <= 3 * D(CT_BTN)) return HTMINBUTTON;
        if (fromRight <= 3 * D(CT_BTN) + D(CT_CTRL)) return HTCLIENT; // the app's own control
        return HTCAPTION;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(w, &ps);
        RECT c;
        GetClientRect(w, &c);
        RECT bar = { 0, 0, c.right, D(CT_H) };
        HBRUSH b1 = CreateSolidBrush(RGB(32, 32, 40)), b2 = CreateSolidBrush(RGB(200, 120, 0)),
               b3 = CreateSolidBrush(RGB(90, 90, 110));
        FillRect(dc, &bar, b1);
        RECT ctrl = { c.right - 3 * D(CT_BTN) - D(CT_CTRL), D(8), c.right - 3 * D(CT_BTN) - D(6), D(CT_H) - D(8) };
        FillRect(dc, &ctrl, b2);
        for (int i = 0; i < 3; i++) {
            RECT bt = { c.right - (i + 1) * D(CT_BTN) + D(4), D(4), c.right - i * D(CT_BTN) - D(4), D(CT_H) - D(4) };
            FillRect(dc, &bt, b3);
        }
        DeleteObject(b1); DeleteObject(b2); DeleteObject(b3);
        EndPaint(w, &ps);
        return 0;
    }
    }
    return DefWindowProcW(w, m, wp, lp);
}

static DWORD WINAPI TargetThread(void*)
{
    WNDCLASSW wc = {};
    wc.lpfnWndProc = TargetProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.hCursor = LoadCursorW(nullptr, (LPCWSTR)IDC_ARROW);
    wc.lpszClassName = L"PinRollTestTarget";
    RegisterClassW(&wc);
    g_target = CreateWindowExW(0, wc.lpszClassName, L"Pin&Roll test window", WS_OVERLAPPEDWINDOW, 200, 150, 700, 450,
                               nullptr, nullptr, wc.hInstance, nullptr);
    g_second = CreateWindowExW(0, wc.lpszClassName, L"Second window", WS_OVERLAPPEDWINDOW, 260, 420, 500, 300,
                               nullptr, nullptr, wc.hInstance, nullptr);
    WNDCLASSW cc = wc;
    cc.lpfnWndProc = CustomProc;
    cc.lpszClassName = L"PinRollTestCustom";
    RegisterClassW(&cc);
    g_custom = CreateWindowExW(0, cc.lpszClassName, L"Custom title bar", WS_OVERLAPPEDWINDOW, 300, 480, 760, 260,
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

static bool StartApp()
{
    std::wstring cmd = L"\"" + g_app + L"\" --no-tray --config \"" + g_ini + L"\"";
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

// A genuine mouse click through the input system, as a person would make it.
static void RealClick(int x, int y)
{
    SetCursorPos(x, y);
    Sleep(120);
    INPUT in[2] = {};
    in[0].type = in[1].type = INPUT_MOUSE;
    in[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    in[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
    SendInput(1, &in[0], sizeof(INPUT));
    Sleep(80);
    SendInput(1, &in[1], sizeof(INPUT));
    Sleep(150);
    SetCursorPos(5, 400);
}
static void RealClickPin(HWND t)
{
    RECT b = {};
    GetWindowRect(FindBar(t), &b);
    RealClick(b.left + (b.right - b.left) * 3 / 4, (b.top + b.bottom) / 2);
}

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

static RECT TitleAreaLeft(HWND w)
{
    RECT r = Rect(w);
    int s = (int)GetDpiForWindow(w);
    RECT out = { r.left, r.top - 6, r.left + MulDiv(330, s, 96), r.top + MulDiv(46, s, 96) };
    return out;
}

int wmain(int argc, wchar_t** argv)
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (argc < 2) {
        printf("usage: prtest <pr.exe> [--shot <prefix>]\n");
        return 2;
    }
    g_app = argv[1];
    for (int i = 2; i + 1 < argc; i++)
        if (!wcscmp(argv[i], L"--shot")) g_shot = argv[i + 1];
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    g_ini = std::wstring(tmp) + L"prtest.ini";
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

    Check(StartApp(), "pr.exe starts");
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

    // --- real mouse input, on the active and on an inactive window
    SetForegroundWindow(t);
    Sleep(300);
    RealClickPin(t);
    bool realActive = WaitFor([&] { return Topmost(t); }, 2000);
    Check(realActive, "real mouse click pins the active window");
    if (realActive) {
        RealClickPin(t);
        Check(WaitFor([&] { return !Topmost(t); }, 2000), "real mouse click unpins it");
    }
    ShowWindow(g_second, SW_SHOW);
    SetForegroundWindow(g_second);
    Sleep(500);
    printf("foreground is %s\n", GetForegroundWindow() == t ? "the target (could not deactivate it)" : "another window");
    RealClickPin(t);
    bool realInactive = WaitFor([&] { return Topmost(t); }, 2000);
    Check(realInactive, "real mouse click pins an INACTIVE window");
    if (!realInactive) DumpZ(t);
    if (Topmost(t)) {
        ClickPin(t);
        WaitFor([&] { return !Topmost(t); });
    }
    ShowWindow(g_second, SW_HIDE);
    SetForegroundWindow(t);
    Sleep(300);

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

    // --- dragging a rolled-up window whose program enforces a minimum height
    ShowWindow(g_second, SW_SHOW);
    SetForegroundWindow(g_second);
    WaitFor([&] { return FindBar(g_second) != nullptr; });
    Sleep(400);
    {
        int sh0 = Height(g_second);
        ClickRoll(g_second);
        Check(WaitFor([&] { return Height(g_second) < D(70); }), "minimum-height window rolls up below its minimum");
        RECT s0 = Rect(g_second);
        SetCursorPos(s0.left + D(120), s0.top + D(15));
        Sleep(150);
        INPUT in = {};
        in.type = INPUT_MOUSE;
        in.mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
        SendInput(1, &in, sizeof(in));
        Sleep(150);
        int during = 0;
        int cx = GetSystemMetrics(SM_CXSCREEN), cy = GetSystemMetrics(SM_CYSCREEN);
        for (int i = 1; i <= 12; i++) {
            // absolute pointer positions, as a tablet or remote session sends them
            int x = s0.left + D(120) + i * 6, y = s0.top + D(15) + i * 4;
            in.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;
            in.mi.dx = x * 65535 / (cx - 1);
            in.mi.dy = y * 65535 / (cy - 1);
            SendInput(1, &in, sizeof(in));
            Sleep(40);
            if (Height(g_second) > during) during = Height(g_second);
        }
        Sleep(200);
        in.mi.dx = in.mi.dy = 0;
        in.mi.dwFlags = MOUSEEVENTF_LEFTUP;
        SendInput(1, &in, sizeof(in));
        RECT s1 = Rect(g_second);
        Sleep(200);
        s1 = Rect(g_second);
        printf("drag: moved by %ld,%ld; tallest during drag %d px; rolled height %ld\n", s1.left - s0.left,
               s1.top - s0.top, during, s0.bottom - s0.top);
        Check(s1.left - s0.left > D(15), "rolled window was dragged with the mouse");
        Check(WaitFor([&] { return Height(g_second) < D(70); }, 4000), "after the drag it is a rolled-up strip again");
        Sleep(600);
        ClickRoll(g_second);
        Check(WaitFor([&] { return Height(g_second) == sh0; }), "...and still unrolls to its original height");
        SetCursorPos(5, 400);
    }
    ShowWindow(g_second, SW_HIDE);

    // --- a program that draws its own (taller, wider) title bar
    ShowWindow(g_custom, SW_SHOWNOACTIVATE);
    Check(WaitFor([&] { return FindBar(g_custom) != nullptr; }), "custom title bar: window gets buttons");
    Sleep(900);
    if (HWND cbar = FindBar(g_custom)) {
        RECT c = Rect(g_custom), b = Rect(cbar);
        int ctrlLeft = c.right - 3 * D(CT_BTN) - D(CT_CTRL);
        int mid = (b.top + b.bottom) / 2 - c.top;
        printf("custom: window %ld,%ld-%ld,%ld bar %ld,%ld-%ld,%ld (app control starts at x=%d, title bar %d px)\n", c.left,
               c.top, c.right, c.bottom, b.left, b.top, b.right, b.bottom, ctrlLeft, D(CT_H));
        Check(b.right <= ctrlLeft && b.right >= ctrlLeft - D(20),
              "custom title bar: buttons go into free space left of the app's own control");
        Check(mid >= D(CT_H) / 2 - D(3) && mid <= D(CT_H) / 2 + D(3),
              "custom title bar: buttons are centred in the real (taller) title bar");
        SetForegroundWindow(g_custom);
        Sleep(1300);
        RECT area = { c.right - D(420), c.top - 4, c.right + 4, c.top + D(CT_H) + D(12) };
        Shot(area, L"custom");
        int ch0 = Height(g_custom);
        ClickRoll(g_custom);
        Check(WaitFor([&] { return Height(g_custom) >= D(CT_H) && Height(g_custom) <= D(CT_H) + D(12); }),
              "custom title bar: rolls up to its real title bar height");
        ClickRoll(g_custom);
        Check(WaitFor([&] { return Height(g_custom) == ch0; }), "custom title bar: unrolls");
    }
    ShowWindow(g_custom, SW_HIDE);
    SetForegroundWindow(t);
    Sleep(300);

    // --- rolled, then maximized, then restored
    ClickRoll(t);
    Check(WaitFor([&] { return Height(t) < D(70); }), "roll before maximize");
    ShowWindow(t, SW_MAXIMIZE);
    Sleep(900);
    ShowWindow(t, SW_RESTORE);
    Sleep(900);
    printf("after roll > maximize > restore: height %d\n", Height(t));
    if (Height(t) < D(70)) {
        ClickRoll(t);
        Check(WaitFor([&] { return Height(t) == h0; }), "roll > maximize > restore: button still unrolls the window");
    } else {
        Check(Height(t) == h0, "roll > maximize > restore: window came back unrolled at its old height");
    }
    ClickRoll(t);
    Check(WaitFor([&] { return Height(t) < D(70); }), "...and the button rolls it again");
    ClickRoll(t);
    WaitFor([&] { return Height(t) == h0; });

    // --- maximized, then rolled, then restored
    ShowWindow(t, SW_MAXIMIZE);
    Sleep(900);
    ClickRoll(t);
    Check(WaitFor([&] { return Height(t) < D(70); }), "maximize > roll: maximized window rolls up");
    ShowWindow(t, SW_RESTORE);
    Sleep(900);
    printf("after maximize > roll > restore: height %d\n", Height(t));
    if (Height(t) < D(70)) ClickRoll(t);
    Check(WaitFor([&] { return Height(t) == h0; }), "maximize > roll > restore: ends at the original height in at most one click");
    ClickRoll(t);
    Check(WaitFor([&] { return Height(t) < D(70); }), "...and the button rolls it again");
    ClickRoll(t);
    Check(WaitFor([&] { return Height(t) == h0; }), "...and unrolls it");
    Sleep(300);

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
        std::wstring cmd = L"\"" + g_app + L"\" --exit";
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
    Check(WaitForSingleObject(g_pi.hProcess, 10000) == WAIT_OBJECT_0, "pr.exe --exit stops the running instance");
    Check(WaitFor([&] { return Height(t) == h0; }), "exit: rolled windows are unrolled");
    Check(CountBars() == 0, "exit: no button windows are left behind");

    // --- crash recovery
    Check(StartApp() && WaitFor([&] { return FindBar(t) != nullptr; }, 10000), "restart");
    ClickRoll(t);
    WaitFor([&] { return Height(t) < D(70); });
    TerminateProcess(g_pi.hProcess, 9);
    WaitForSingleObject(g_pi.hProcess, 5000);
    Check(Height(t) < D(70), "after a crash the window is still rolled");
    Check(StartApp() && WaitFor([&] { return FindBar(t) != nullptr; }, 10000), "restart after crash");
    Sleep(500);
    ClickRoll(t);
    Check(WaitFor([&] { return Height(t) == h0; }), "crash recovery: one click restores the original height");

    // --- settings window (opened the way the tray menu does)
    PostMessageW(FindWindowW(kMainClass, nullptr), WM_COMMAND, 103, 0);
    HWND dlg = nullptr;
    Check(WaitFor([&] { return (dlg = FindWindowW(nullptr, L"Pin&Roll - Settings")) != nullptr; }),
          "settings window opens");
    if (dlg) {
        Sleep(800);
        RECT dr = Rect(dlg);
        Shot(dr, L"settings");
        PostMessageW(dlg, WM_COMMAND, IDCANCEL, 0);
        Check(WaitFor([&] { return !IsWindow(dlg); }), "settings window closes on Cancel");
        Sleep(700);
        HWND sb = FindBar(t);
        RECT sr = Rect(sb);
        Check(sb && sr.right - sr.left == D(64), "opening and cancelling settings changes nothing");
    }

    PostMessageW(FindWindowW(kMainClass, nullptr), WM_CLOSE, 0, 0);
    WaitForSingleObject(g_pi.hProcess, 10000);

    // --- per-program rule: buttons at the left end of the title bar
    WritePrivateProfileStringW(L"Exceptions", L"prtest.exe", L"left,offset:-40,peek", g_ini.c_str());
    Check(StartApp() && WaitFor([&] { return FindBar(t) != nullptr; }, 10000), "restart with a 'left' rule");
    Sleep(400);
    {
        RECT e = Rect(t), b = Rect(FindBar(t));
        DwmGetWindowAttribute(t, DWMWA_EXTENDED_FRAME_BOUNDS, &e, sizeof(e));
        printf("left rule: frame left %ld, bar %ld-%ld\n", e.left, b.left, b.right);
        Check(b.left == e.left + D(48), "left rule: buttons sit 48 px from the left edge");
        Shot(TitleAreaLeft(t), L"left");
    }

    // settings window again, now with a program in the list (used for the help page)
    PostMessageW(FindWindowW(kMainClass, nullptr), WM_COMMAND, 103, 0);
    {
        HWND d2 = nullptr;
        WaitFor([&] { return (d2 = FindWindowW(nullptr, L"Pin&Roll - Settings")) != nullptr; });
        if (d2) {
            Sleep(800);
            RECT dr = Rect(d2);
            InflateRect(&dr, -1, -1);
            Shot(dr, L"settings-app");
            PostMessageW(d2, WM_COMMAND, IDCANCEL, 0);
            WaitFor([&] { return !IsWindow(d2); });
            Sleep(500);
        }
    }

    // --- peek (enabled for this program by the rule above)
    SetForegroundWindow(t);
    SetCursorPos(2, 2);
    Sleep(300);
    ClickRoll(t);
    Check(WaitFor([&] { return Height(t) < D(70); }), "peek: window rolls up");
    Sleep(900);
    Check(Height(t) < D(70), "peek: stays rolled while the mouse is elsewhere");
    {
        RECT r = Rect(t), b = Rect(FindBar(t));
        SetCursorPos((r.left + r.right) / 2, r.top + D(14));
        Sleep(1200);
        Check(Height(t) < D(70), "peek: resting on the title bar alone does not open it");
        SetCursorPos(b.left + (b.right - b.left) / 4, (b.top + b.bottom) / 2); // the roll-up button
        Check(WaitFor([&] { return Height(t) == h0; }, 4000), "peek: opens while the mouse rests on the roll-up button");
        SetCursorPos((r.left + r.right) / 2, r.top + D(14));
        Sleep(1200);
        Check(Height(t) == h0, "peek: stays open while the mouse is on the title bar");
        SetCursorPos((r.left + r.right) / 2, r.top + D(200)); // into the window's content
        Check(WaitFor([&] { return Height(t) < D(70); }, 4000), "peek: rolls back up when the mouse leaves the title bar");
        SetCursorPos(2, 2);
        Sleep(700);
        ClickRoll(t);
        Check(WaitFor([&] { return Height(t) == h0; }), "peek: the button still unrolls for good");
        Sleep(900);
        Check(Height(t) == h0, "...and it stays open afterwards");
    }
    PostMessageW(FindWindowW(kMainClass, nullptr), WM_CLOSE, 0, 0);
    WaitForSingleObject(g_pi.hProcess, 10000);
    PostMessageW(t, WM_CLOSE, 0, 0);
    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
