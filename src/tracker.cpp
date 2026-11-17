// Window tracking, button placement, pin and roll-up actions.
//
// Unlike the original FileBox eXtender, nothing is injected into other
// programs. FBX listens to accessibility (WinEvent) notifications from its
// own process and keeps a small layered "bar" window directly above each
// eligible top-level window. All calls made on foreign windows are either
// pure window-manager queries or asynchronous, so a hung program cannot
// hang FBX.
//
// License: GPL-3.0-or-later.
#include "common.h"
#include <dwmapi.h>
#include <unordered_map>
#include <cwctype>

Config g_cfg;

namespace {

struct Tracked {
    HWND hwnd = nullptr;
    HWND bar = nullptr;
    std::wstring exe;
    bool noPin = false, noRoll = false;
    int offset = 0, rollHeight = 0;

    bool rolled = false, rollPending = false, forcedTopmost = false;
    DWORD rollTick = 0;
    int oldHeight = 0, rolledHeight = 0;

    ButtonType btn[2] = { BTN_NONE, BTN_NONE };
    int nBtn = 0;
    int hot = -1, pressed = -1;
    bool dark = false, needSample = true;
    DWORD sampleTick = 0;

    // what is currently on screen, to avoid redundant work
    RECT shownRect = {};
    ButtonDraw shownBtn[2] = {};
    int shownCount = 0;
    bool shownDark = false;
    UINT shownDpi = 0;
    bool dead = false;
};

struct Geometry {
    RECT bar;
    int rolledHeight;
    UINT dpi;
};

HINSTANCE g_inst;
HWND g_main;
std::vector<HWINEVENTHOOK> g_hooks;
std::unordered_map<HWND, Tracked> g_win;
std::unordered_map<HWND, DWORD> g_rejected;
DWORD g_myPid, g_myIntegrity = 0;
bool g_sysDark = false;
int g_burst = 0, g_slowTicks = 0;
HWND g_lastForeground = nullptr;

inline int Abs(int v) { return v < 0 ? -v : v; }
inline int Clamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

void Burst()
{
    g_burst = 10;
    SetTimer(g_main, FBX_TIMER_BURST, 40, nullptr);
}

bool IsCloaked(HWND h)
{
    DWORD v = 0;
    return SUCCEEDED(DwmGetWindowAttribute(h, DWMWA_CLOAKED, &v, sizeof(v))) && v != 0;
}

bool StyleEligible(LONG style, LONG ex)
{
    if (style & WS_CHILD) return false;
    if (ex & (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE)) return false;
    // A regular title bar...
    bool caption = (style & WS_CAPTION) == WS_CAPTION && (style & WS_SYSMENU);
    // ...or a resizable application window that draws its own one.
    bool custom = (style & WS_THICKFRAME) && (style & (WS_MINIMIZEBOX | WS_MAXIMIZEBOX));
    return caption || custom;
}

bool ExcludedClass(HWND h)
{
    static const wchar_t* k[] = {
        L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd", L"Progman", L"WorkerW",
        L"Windows.UI.Core.CoreWindow", L"XamlExplorerHostIslandWindow",
        L"TopLevelWindowForOverflowXamlIsland", L"#32768", FBX_BAR_CLASS, FBX_MAIN_CLASS,
    };
    wchar_t cls[128] = L"";
    GetClassNameW(h, cls, 128);
    for (auto c : k)
        if (!wcscmp(cls, c)) return true;
    return false;
}

DWORD TokenIntegrity(HANDLE process)
{
    DWORD rid = 0;
    HANDLE tok;
    if (!OpenProcessToken(process, TOKEN_QUERY, &tok)) return 0;
    BYTE buf[256];
    DWORD cb = 0;
    if (GetTokenInformation(tok, TokenIntegrityLevel, buf, sizeof(buf), &cb)) {
        auto* tml = (TOKEN_MANDATORY_LABEL*)buf;
        DWORD n = *GetSidSubAuthorityCount(tml->Label.Sid);
        if (n) rid = *GetSidSubAuthority(tml->Label.Sid, n - 1);
    }
    CloseHandle(tok);
    return rid;
}

// Returns false if we must not (or cannot) manage windows of this process.
bool ProcessInfo(HWND h, std::wstring& exeLower)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (!pid || pid == g_myPid) return false;
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) return false; // protected or elevated: Windows would block us anyway
    wchar_t path[MAX_PATH * 2];
    DWORD n = (DWORD)(sizeof(path) / sizeof(path[0]));
    if (QueryFullProcessImageNameW(p, 0, path, &n)) {
        const wchar_t* base = wcsrchr(path, L'\\');
        exeLower = base ? base + 1 : path;
        for (auto& c : exeLower) c = (wchar_t)towlower(c);
    }
    DWORD integ = TokenIntegrity(p);
    CloseHandle(p);
    // A window of a higher integrity level (e.g. "Run as administrator")
    // ignores our requests, so do not show buttons that would do nothing.
    if (integ && g_myIntegrity && integ > g_myIntegrity) return false;
    return true;
}

void ApplyException(Tracked& t)
{
    const ExceptionRule* r = g_cfg.Find(t.exe);
    t.noPin = r && r->noPin;
    t.noRoll = r && r->noRoll;
    t.offset = r ? r->offset : 0;
    t.rollHeight = r ? r->rollHeight : 0;
}

void ComputeGeometry(const Tracked& t, LONG style, LONG ex, const RECT& wr, int count, Geometry& g)
{
    HWND h = t.hwnd;
    UINT dpi = GetDpiForWindow(h);
    if (!dpi) dpi = 96;
    g.dpi = dpi;
    auto D = [dpi](int v) { return MulDiv(v, (int)dpi, 96); };

    // The visible frame; the window rectangle also covers invisible resize borders.
    RECT efb = wr;
    RECT r;
    if (SUCCEEDED(DwmGetWindowAttribute(h, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof(r))) &&
        r.right > r.left && r.bottom > r.top)
        efb = r;

    bool zoomed = IsZoomed(h) != 0;
    int frameTop = efb.top, frameRight = efb.right;
    if (zoomed) {
        MONITORINFO mi = {};
        mi.cbSize = sizeof(mi);
        if (GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi)) {
            if (frameTop < mi.rcMonitor.top) frameTop = mi.rcMonitor.top;
            if (frameRight > mi.rcMonitor.right) frameRight = mi.rcMonitor.right;
        }
    }

    // Where are the minimize / maximize / close buttons? Ask the compositor.
    RECT cb = {};
    bool have = false;
    if (SUCCEEDED(DwmGetWindowAttribute(h, DWMWA_CAPTION_BUTTON_BOUNDS, &cb, sizeof(cb)))) {
        int cw = cb.right - cb.left, ch = cb.bottom - cb.top;
        have = cw >= D(16) && ch >= D(12) && ch <= D(80) && cw <= wr.right - wr.left;
    }
    int capL, capR, capT, capB;
    if (have) {
        capL = wr.left + cb.left;
        capR = wr.left + cb.right;
        capT = wr.top + cb.top;
        capB = wr.top + cb.bottom;
        if (capT < frameTop) capT = frameTop;
        if (capB < capT + D(12)) capB = capT + D(30);
    } else {
        // No answer (custom-drawn title bar): assume the standard layout.
        int n = 1;
        if (style & (WS_MINIMIZEBOX | WS_MAXIMIZEBOX)) n = 3;
        else if (ex & WS_EX_CONTEXTHELP) n = 2;
        capR = frameRight;
        capL = capR - n * D(46);
        capT = frameTop;
        capB = capT + D(30);
    }

    int capH = capB - capT;
    int bh = Clamp(capH, D(20), D(32));
    int bw = D(32);
    int top = capT + (capH - bh) / 2;
    int shift = D(2) + D(g_cfg.moveLeft) + (zoomed ? D(g_cfg.moveLeftMaximized) : 0) + D(t.offset);
    bool mirrored = (capL + capR) / 2 < (wr.left + wr.right) / 2; // right-to-left windows
    if (!mirrored) {
        g.bar.right = capL - shift;
        g.bar.left = g.bar.right - count * bw;
    } else {
        g.bar.left = capR + shift;
        g.bar.right = g.bar.left + count * bw;
    }
    g.bar.top = top;
    g.bar.bottom = top + bh;

    int invisibleBottom = wr.bottom - efb.bottom;
    if (invisibleBottom < 0 || invisibleBottom > D(24)) invisibleBottom = 0;
    int captionBottom;
    if (have)
        captionBottom = capB - wr.top;
    else if ((style & WS_CAPTION) == WS_CAPTION)
        captionBottom = GetSystemMetricsForDpi(SM_CYCAPTION, dpi) + GetSystemMetricsForDpi(SM_CYSIZEFRAME, dpi) +
                        GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
    else
        captionBottom = (capB - wr.top) + D(2);
    g.rolledHeight = t.rollHeight > 0 ? D(t.rollHeight) : captionBottom + D(1) + invisibleBottom;
}

bool IsFullscreen(HWND h, const RECT& wr)
{
    if (IsZoomed(h)) return false;
    MONITORINFO mi = {};
        mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi)) return false;
    return wr.left <= mi.rcMonitor.left && wr.top <= mi.rcMonitor.top && wr.right >= mi.rcMonitor.right &&
           wr.bottom >= mi.rcMonitor.bottom;
}

void ClearRollProps(HWND h)
{
    RemovePropW(h, FBX_PROP_OLDHEIGHT);
    RemovePropW(h, FBX_PROP_ROLLEDHEIGHT);
    RemovePropW(h, FBX_PROP_FORCEDTOP);
}

// The window is no longer rolled up (we unrolled it, or it was resized).
void ClearRolled(Tracked& t)
{
    bool forced = t.forcedTopmost;
    t.rolled = t.rollPending = t.forcedTopmost = false;
    ClearRollProps(t.hwnd);
    if (forced)
        SetWindowPos(t.hwnd, HWND_NOTOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
}

void HideBar(Tracked& t)
{
    if (t.bar && IsWindowVisible(t.bar)) ShowWindow(t.bar, SW_HIDE);
    t.hot = t.pressed = -1;
}

// Keep the bar directly above its window in the Z order.
void PlaceAbove(Tracked& t, bool topmost)
{
    HWND prev = GetWindow(t.hwnd, GW_HWNDPREV);
    if (prev == t.bar) return;
    const UINT f = SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER;
    bool barTop = (GetWindowLongW(t.bar, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
    if (barTop != topmost) SetWindowPos(t.bar, topmost ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, f);
    HWND after = prev;
    if (!prev)
        after = HWND_TOP;
    else if (!topmost && (GetWindowLongW(prev, GWL_EXSTYLE) & WS_EX_TOPMOST))
        after = HWND_TOP; // our window is the first non-topmost one
    SetWindowPos(t.bar, after, 0, 0, 0, 0, f);
}

// Is the title bar dark or light just left of our buttons? Decides the glyph colour.
void SampleBackground(Tracked& t, const RECT& bar, UINT dpi)
{
    DWORD now = GetTickCount();
    if (now - t.sampleTick < 700) return;
    t.sampleTick = now;
    POINT pt = { bar.left - MulDiv(5, (int)dpi, 96), 0 };
    int ys[3] = { bar.top + (bar.bottom - bar.top) / 4, (bar.top + bar.bottom) / 2,
                  bar.bottom - (bar.bottom - bar.top) / 4 };

    // Only trust the screen if nothing covers that spot.
    int guard = 0;
    for (HWND w = GetTopWindow(nullptr); w && w != t.hwnd && guard < 4000; w = GetWindow(w, GW_HWNDNEXT), guard++) {
        if (w == t.bar || !IsWindowVisible(w) || IsIconic(w)) continue;
        RECT r;
        if (!GetWindowRect(w, &r)) continue;
        if (pt.x >= r.left && pt.x < r.right && ys[2] >= r.top && ys[0] < r.bottom) {
            if (IsCloaked(w)) continue;
            return; // covered; try again later
        }
    }
    HDC dc = GetDC(nullptr);
    int lum[3], n = 0;
    for (int y : ys) {
        COLORREF c = GetPixel(dc, pt.x, y);
        if (c == CLR_INVALID) continue;
        lum[n++] = (GetRValue(c) * 299 + GetGValue(c) * 587 + GetBValue(c) * 114) / 1000;
    }
    ReleaseDC(nullptr, dc);
    if (n == 0) return;
    int dark = 0;
    for (int i = 0; i < n; i++)
        if (lum[i] < 128) dark++;
    t.dark = dark * 2 > n;
    t.needSample = false;
}

LRESULT CALLBACK BarProc(HWND, UINT, WPARAM, LPARAM);

void UpdateBar(Tracked& t)
{
    HWND h = t.hwnd;
    if (!IsWindow(h)) {
        t.dead = true;
        HideBar(t);
        return;
    }
    LONG style = GetWindowLongW(h, GWL_STYLE), ex = GetWindowLongW(h, GWL_EXSTYLE);
    RECT wr;
    if (!GetWindowRect(h, &wr)) return;
    bool iconic = IsIconic(h) != 0;

    if (t.rolled && !iconic) {
        int height = wr.bottom - wr.top;
        if (Abs(height - t.rolledHeight) <= 2)
            t.rollPending = false;
        else if (!t.rollPending || GetTickCount() - t.rollTick > 1500)
            ClearRolled(t); // resized by the user or the program: treat as unrolled
    }

    t.nBtn = 0;
    if (g_cfg.roll && !t.noRoll) t.btn[t.nBtn++] = BTN_ROLL;
    if (g_cfg.pin && !t.noPin) t.btn[t.nBtn++] = BTN_PIN;

    bool show = t.nBtn > 0 && !iconic && IsWindowVisible(h) && StyleEligible(style, ex) && !IsCloaked(h) &&
                !IsFullscreen(h, wr);
    Geometry g = {};
    if (show) {
        ComputeGeometry(t, style, ex, wr, t.nBtn, g);
        // not enough room in a very narrow window
        if (g.bar.left < wr.left || g.bar.right > wr.right) show = false;
    }
    if (!show) {
        HideBar(t);
        return;
    }

    if (!t.bar) {
        t.bar = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, FBX_BAR_CLASS, L"", WS_POPUP,
                                g.bar.left, g.bar.top, g.bar.right - g.bar.left, g.bar.bottom - g.bar.top, nullptr,
                                nullptr, g_inst, nullptr);
        if (!t.bar) return;
        SetPropW(t.bar, FBX_PROP_TARGET, (HANDLE)h);
        t.shownCount = 0;
    }

    if (t.needSample) SampleBackground(t, g.bar, g.dpi);

    bool topmost = (ex & WS_EX_TOPMOST) != 0;
    ButtonDraw bd[2] = {};
    for (int i = 0; i < t.nBtn; i++) {
        bd[i].type = t.btn[i];
        bd[i].active = t.btn[i] == BTN_PIN ? topmost : t.rolled;
        bd[i].hot = t.hot == i;
        bd[i].pressed = t.pressed == i && t.hot == i;
    }
    bool sameSize = (g.bar.right - g.bar.left) == (t.shownRect.right - t.shownRect.left) &&
                    (g.bar.bottom - g.bar.top) == (t.shownRect.bottom - t.shownRect.top);
    bool samePos = g.bar.left == t.shownRect.left && g.bar.top == t.shownRect.top;
    bool sameLook = sameSize && t.shownCount == t.nBtn && t.shownDark == t.dark && t.shownDpi == g.dpi &&
                    !memcmp(bd, t.shownBtn, sizeof(bd));
    if (!sameLook) {
        RenderBar(t.bar, g.bar, bd, t.nBtn, g.dpi, t.dark);
        memcpy(t.shownBtn, bd, sizeof(bd));
        t.shownCount = t.nBtn;
        t.shownDark = t.dark;
        t.shownDpi = g.dpi;
    } else if (!samePos) {
        SetWindowPos(t.bar, nullptr, g.bar.left, g.bar.top, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    t.shownRect = g.bar;
    if (!IsWindowVisible(t.bar)) ShowWindow(t.bar, SW_SHOWNOACTIVATE);
    PlaceAbove(t, topmost);
}

Tracked* TryTrack(HWND h)
{
    auto it = g_win.find(h);
    if (it != g_win.end()) return &it->second;
    if (!IsWindow(h) || !IsWindowVisible(h) || GetAncestor(h, GA_ROOT) != h) return nullptr;
    LONG style = GetWindowLongW(h, GWL_STYLE), ex = GetWindowLongW(h, GWL_EXSTYLE);
    if (!StyleEligible(style, ex)) return nullptr;

    DWORD now = GetTickCount();
    auto rj = g_rejected.find(h);
    if (rj != g_rejected.end() && now - rj->second < 5000) return nullptr;

    std::wstring exe;
    const ExceptionRule* rule = nullptr;
    if (ExcludedClass(h) || !ProcessInfo(h, exe) || ((rule = g_cfg.Find(exe)) && rule->ignore)) {
        g_rejected[h] = now;
        return nullptr;
    }
    g_rejected.erase(h);

    Tracked& t = g_win[h];
    t.hwnd = h;
    t.exe = exe;
    t.dark = g_sysDark;
    ApplyException(t);

    // Adopt a window that was rolled up by a previous instance of FBX.
    int oldH = (int)(INT_PTR)GetPropW(h, FBX_PROP_OLDHEIGHT);
    if (oldH > 0) {
        RECT wr;
        GetWindowRect(h, &wr);
        int rh = (int)(INT_PTR)GetPropW(h, FBX_PROP_ROLLEDHEIGHT);
        t.rolled = true;
        t.oldHeight = oldH;
        t.rolledHeight = rh > 0 ? rh : wr.bottom - wr.top;
        t.forcedTopmost = GetPropW(h, FBX_PROP_FORCEDTOP) != nullptr;
    }
    UpdateBar(t);
    return &t;
}

void Untrack(HWND h)
{
    auto it = g_win.find(h);
    if (it == g_win.end()) return;
    if (it->second.bar) DestroyWindow(it->second.bar);
    g_win.erase(it);
}

void Roll(Tracked& t)
{
    if (t.rolled || IsIconic(t.hwnd)) return;
    HWND h = t.hwnd;
    RECT wr;
    if (!GetWindowRect(h, &wr)) return;
    LONG style = GetWindowLongW(h, GWL_STYLE), ex = GetWindowLongW(h, GWL_EXSTYLE);
    Geometry g = {};
    ComputeGeometry(t, style, ex, wr, t.nBtn ? t.nBtn : 1, g);
    int height = wr.bottom - wr.top;
    if (height <= g.rolledHeight + 2) return;

    t.oldHeight = height;
    t.rolledHeight = g.rolledHeight;
    t.forcedTopmost = g_cfg.rolledTopmost && !(ex & WS_EX_TOPMOST);
    SetPropW(h, FBX_PROP_OLDHEIGHT, (HANDLE)(INT_PTR)t.oldHeight);
    SetPropW(h, FBX_PROP_ROLLEDHEIGHT, (HANDLE)(INT_PTR)t.rolledHeight);
    if (t.forcedTopmost) SetPropW(h, FBX_PROP_FORCEDTOP, (HANDLE)(INT_PTR)1);

    // SWP_NOSENDCHANGING lets the window go below its normal minimum size
    // (the original FbX subclassed the window to achieve the same).
    UINT f = SWP_NOMOVE | SWP_NOACTIVATE | SWP_NOSENDCHANGING | SWP_ASYNCWINDOWPOS;
    if (!g_cfg.rolledTopmost) f |= SWP_NOZORDER;
    SetWindowPos(h, g_cfg.rolledTopmost ? HWND_TOPMOST : nullptr, 0, 0, wr.right - wr.left, t.rolledHeight, f);
    t.rolled = true;
    t.rollPending = true;
    t.rollTick = GetTickCount();
    Burst();
}

void Unroll(Tracked& t)
{
    if (!t.rolled) return;
    HWND h = t.hwnd;
    int oldH = t.oldHeight;
    RECT wr;
    bool ok = GetWindowRect(h, &wr) != 0;
    ClearRolled(t);
    // Like FbX: keep the current position and width, restore the height.
    if (ok)
        SetWindowPos(h, nullptr, 0, 0, wr.right - wr.left, oldH,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
    Burst();
}

void TogglePin(Tracked& t)
{
    HWND h = t.hwnd;
    bool topmost = (GetWindowLongW(h, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
    if (t.rolled && t.forcedTopmost) {
        // The user decided; do not undo it when the window is unrolled.
        t.forcedTopmost = false;
        RemovePropW(h, FBX_PROP_FORCEDTOP);
    }
    SetWindowPos(h, topmost ? HWND_NOTOPMOST : HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
    Burst();
}

void DoButton(Tracked& t, ButtonType b)
{
    if (b == BTN_PIN)
        TogglePin(t);
    else if (b == BTN_ROLL) {
        if (t.rolled) Unroll(t);
        else Roll(t);
    }
}

Tracked* FromBar(HWND bar)
{
    HWND target = (HWND)GetPropW(bar, FBX_PROP_TARGET);
    auto it = g_win.find(target);
    return (it != g_win.end() && it->second.bar == bar) ? &it->second : nullptr;
}

int ButtonAt(const Tracked& t, HWND bar, LPARAM lp)
{
    RECT rc;
    GetClientRect(bar, &rc);
    int x = (short)LOWORD(lp), y = (short)HIWORD(lp);
    if (t.nBtn <= 0 || x < 0 || y < 0 || x >= rc.right || y >= rc.bottom) return -1;
    int i = x * t.nBtn / (rc.right ? rc.right : 1);
    return i < t.nBtn ? i : t.nBtn - 1;
}

LRESULT CALLBACK BarProc(HWND bar, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_MOUSEMOVE:
        if (Tracked* t = FromBar(bar)) {
            int i = ButtonAt(*t, bar, lp);
            if (i != t->hot) {
                t->hot = i;
                UpdateBar(*t);
            }
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, bar, 0 };
            TrackMouseEvent(&tme);
        }
        return 0;
    case WM_MOUSELEAVE:
        if (Tracked* t = FromBar(bar)) {
            if (GetCapture() != bar) {
                t->hot = -1;
                UpdateBar(*t);
            }
        }
        return 0;
    case WM_LBUTTONDOWN:
        if (Tracked* t = FromBar(bar)) {
            t->hot = t->pressed = ButtonAt(*t, bar, lp);
            SetCapture(bar);
            UpdateBar(*t);
        }
        return 0;
    case WM_LBUTTONUP:
        if (Tracked* t = FromBar(bar)) {
            int i = ButtonAt(*t, bar, lp);
            int pressed = t->pressed;
            t->pressed = -1;
            t->hot = i;
            if (GetCapture() == bar) ReleaseCapture();
            if (i >= 0 && i == pressed) DoButton(*t, t->btn[i]);
            UpdateBar(*t);
        }
        return 0;
    case WM_CAPTURECHANGED:
        if (Tracked* t = FromBar(bar)) {
            if (t->pressed != -1) {
                t->pressed = -1;
                UpdateBar(*t);
            }
        }
        return 0;
    case WM_NCDESTROY:
        RemovePropW(bar, FBX_PROP_TARGET);
        break;
    }
    return DefWindowProcW(bar, msg, wp, lp);
}

void CALLBACK WinEventProc(HWINEVENTHOOK, DWORD ev, HWND hwnd, LONG idObject, LONG idChild, DWORD, DWORD)
{
    if (!hwnd || idObject != OBJID_WINDOW || idChild != CHILDID_SELF) return;
    auto it = g_win.find(hwnd);
    if (ev == EVENT_OBJECT_DESTROY) {
        if (it != g_win.end()) Untrack(hwnd);
        g_rejected.erase(hwnd);
        return;
    }
    if (ev == EVENT_SYSTEM_FOREGROUND) {
        // Title bar colours change with activation.
        auto prev = g_win.find(g_lastForeground);
        if (prev != g_win.end()) prev->second.needSample = true;
        g_lastForeground = hwnd;
        if (it != g_win.end()) it->second.needSample = true;
        Burst();
    }
    if (it != g_win.end())
        UpdateBar(it->second);
    else if (ev != EVENT_OBJECT_HIDE && ev != EVENT_SYSTEM_MINIMIZESTART && ev != EVENT_OBJECT_CLOAKED)
        TryTrack(hwnd);
}

BOOL CALLBACK EnumProc(HWND h, LPARAM)
{
    TryTrack(h);
    return TRUE;
}

void Hook(DWORD lo, DWORD hi)
{
    HWINEVENTHOOK hk =
        SetWinEventHook(lo, hi, nullptr, WinEventProc, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    if (hk) g_hooks.push_back(hk);
}

} // namespace

bool TrackerInit(HINSTANCE inst, HWND mainWnd)
{
    g_inst = inst;
    g_main = mainWnd;
    g_myPid = GetCurrentProcessId();
    g_myIntegrity = TokenIntegrity(GetCurrentProcess());
    g_sysDark = SystemAppsDark();

    WNDCLASSW wc = {};
    wc.lpfnWndProc = BarProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, (LPCWSTR)IDC_ARROW);
    wc.lpszClassName = FBX_BAR_CLASS;
    if (!RegisterClassW(&wc)) return false;

    Hook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND);
    Hook(EVENT_SYSTEM_MINIMIZESTART, EVENT_SYSTEM_MINIMIZEEND);
    Hook(EVENT_OBJECT_DESTROY, EVENT_OBJECT_HIDE); // destroy, show, hide
    Hook(EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE);
    Hook(EVENT_OBJECT_CLOAKED, EVENT_OBJECT_UNCLOAKED);

    EnumWindows(EnumProc, 0);
    SetTimer(g_main, FBX_TIMER_SLOW, 300, nullptr);
    return true;
}

void TrackerRefreshAll()
{
    g_sysDark = SystemAppsDark();
    g_rejected.clear();
    std::vector<HWND> drop;
    for (auto& kv : g_win) {
        Tracked& t = kv.second;
        ApplyException(t);
        const ExceptionRule* r = g_cfg.Find(t.exe);
        if (r && r->ignore) {
            if (t.rolled) Unroll(t);
            drop.push_back(kv.first);
            continue;
        }
        UpdateBar(t);
    }
    for (HWND h : drop) Untrack(h);
    EnumWindows(EnumProc, 0);
}

void TrackerOnTimer(UINT_PTR id)
{
    if (id == FBX_TIMER_BURST) {
        if (--g_burst <= 0) KillTimer(g_main, FBX_TIMER_BURST);
    } else if (id != FBX_TIMER_SLOW) {
        return;
    }
    std::vector<HWND> dead;
    for (auto& kv : g_win) {
        UpdateBar(kv.second);
        if (kv.second.dead) dead.push_back(kv.first);
    }
    for (HWND h : dead) Untrack(h);

    if (id == FBX_TIMER_SLOW && ++g_slowTicks % 5 == 0) {
        // Safety net for windows whose events we missed.
        EnumWindows(EnumProc, 0);
        if (g_slowTicks % 100 == 0) {
            for (auto it = g_rejected.begin(); it != g_rejected.end();)
                it = IsWindow(it->first) ? ++it : g_rejected.erase(it);
        }
    }
}

void TrackerUnrollAll()
{
    for (auto& kv : g_win)
        if (kv.second.rolled) Unroll(kv.second);
}

void TrackerToggleForeground(ButtonType which)
{
    HWND fg = GetForegroundWindow();
    if (!fg) return;
    Tracked* t = TryTrack(GetAncestor(fg, GA_ROOT));
    if (!t) return;
    if ((which == BTN_PIN && t->noPin) || (which == BTN_ROLL && t->noRoll)) return;
    DoButton(*t, which);
}

void TrackerShutdown()
{
    for (HWINEVENTHOOK h : g_hooks) UnhookWinEvent(h);
    g_hooks.clear();
    if (g_main) {
        KillTimer(g_main, FBX_TIMER_SLOW);
        KillTimer(g_main, FBX_TIMER_BURST);
    }
    // Like FbX: leave no window rolled up behind.
    TrackerUnrollAll();
    for (auto& kv : g_win)
        if (kv.second.bar) DestroyWindow(kv.second.bar);
    g_win.clear();
}
