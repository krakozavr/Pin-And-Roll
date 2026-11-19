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
    bool left = false, manual = false; // manual: position set by the user, no automatic search
    int offset = 0, rollHeight = 0;
    int size = 0, down = 0;

    // The user is dragging the window (system move/size loop).
    bool inMoveSize = false, moveIsResize = false;

    // Roll-up state. "suspended": rolled while not maximized, currently
    // maximized (Windows will bring the strip back on restore).
    bool rolled = false, rollPending = false, forcedTopmost = false;
    bool zoomedAtRoll = false, suspended = false;
    DWORD rollTick = 0;
    int oldHeight = 0, rolledHeight = 0, normalHeight = 0;

    // Measured layout of a title bar the program draws itself.
    bool probeValid = false, reprobe = true;
    UINT probeDpi = 0;
    bool probeZoomed = false;
    int probeCount = 0, probeWidth = 0;
    int probeFromRight = 0, probeHeight = 0, probeShift = 0;
    int probeFreeAdjacent = 0; // free title bar directly left of the system buttons, px
    DWORD probeTick = 0;

    ButtonType btn[2] = { BTN_NONE, BTN_NONE };
    int nBtn = 0;
    int hot = -1, pressed = -1;
    bool dark = false, needSample = true;
    DWORD sampleTick = 0, sampleDue = 0; // sampleDue: not before this time (animations)

    // what is currently on screen, to avoid redundant work
    RECT shownRect = {};
    ButtonDraw shownBtn[2] = {};
    int shownCount = 0;
    bool shownDark = false;
    UINT shownDpi = 0;
    float shownScale = 0;
    bool dead = false;
};

struct Geometry {
    RECT bar;
    int rolledHeight;
    UINT dpi;
    float scale; // button size factor chosen in the settings
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
    t.left = r && r->left;
    t.manual = r && (r->left || r->offset != 0);
    t.offset = r ? r->offset : 0;
    t.rollHeight = r ? r->rollHeight : 0;
    t.size = r ? r->size : 0;
    t.down = r ? r->down : 0;
}

struct Frame {
    RECT wr, efb;
    int top, right, left; // visible edges (on screen when maximized)
    bool zoomed, custom;
    UINT dpi;
};

void GetFrame(HWND h, const RECT& wr, Frame& f)
{
    f.wr = wr;
    f.dpi = GetDpiForWindow(h);
    if (!f.dpi) f.dpi = 96;
    // The visible frame; the window rectangle also covers invisible resize borders.
    f.efb = wr;
    RECT r;
    if (SUCCEEDED(DwmGetWindowAttribute(h, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof(r))) && r.right > r.left &&
        r.bottom > r.top)
        f.efb = r;
    f.zoomed = IsZoomed(h) != 0;
    f.top = f.efb.top;
    f.right = f.efb.right;
    f.left = f.efb.left;
    if (f.zoomed) {
        MONITORINFO mi = {};
        mi.cbSize = sizeof(mi);
        if (GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi)) {
            if (f.top < mi.rcMonitor.top) f.top = mi.rcMonitor.top;
            if (f.right > mi.rcMonitor.right) f.right = mi.rcMonitor.right;
            if (f.left < mi.rcMonitor.left) f.left = mi.rcMonitor.left;
        }
    }
    // Does the client area reach the top edge? Then the program draws its
    // own title bar (browsers, Electron, WinUI) and the system metrics lie.
    POINT c = { 0, 0 };
    f.custom = ClientToScreen(h, &c) && c.y - f.top < MulDiv(12, (int)f.dpi, 96);
}

bool HitTest(HWND h, int x, int y, LRESULT& code)
{
    POINT p = { x, y };
    PhysicalToLogicalPointForPerMonitorDPI(h, &p);
    DWORD_PTR res = 0;
    if (!SendMessageTimeoutW(h, WM_NCHITTEST, 0, MAKELPARAM((short)p.x, (short)p.y), SMTO_ABORTIFHUNG | SMTO_BLOCK, 40,
                             &res))
        return false;
    code = (LRESULT)res;
    return true;
}

inline bool IsCaptionButton(LRESULT c)
{
    return c == HTCLOSE || c == HTMAXBUTTON || c == HTMINBUTTON || c == HTHELP;
}

// Ask a self-drawn title bar where its buttons really are, and where the
// nearest free (draggable) space to their left is. The answers are stored
// relative to the top right corner, so they stay valid while the window moves.
void ProbeCustomTitleBar(Tracked& t, const Frame& f, LONG style, int count)
{
    HWND h = t.hwnd;
    DWORD now = GetTickCount();
    int width = f.wr.right - f.wr.left;
    bool keyChanged = t.probeDpi != f.dpi || t.probeZoomed != f.zoomed || t.probeCount != count;
    if (t.probeWidth != width) t.reprobe = true; // free space depends on the width
    if (!keyChanged && !t.reprobe) return;
    if (t.rolled && !t.suspended && t.probeDpi) return; // keep what we measured before rolling
    if (now - t.probeTick < 400 && t.probeDpi) return;
    t.probeTick = now;
    t.probeDpi = f.dpi;
    t.probeZoomed = f.zoomed;
    t.probeCount = count;
    t.probeWidth = width;
    t.reprobe = false;
    t.probeValid = false;
    if (IsHungAppWindow(h)) return;

    UINT dpi = f.dpi;
    auto D = [dpi](int v) { return MulDiv(v, (int)dpi, 96); };
    const int y0 = f.top + D(10), step = D(6);
    LRESULT code;

    // 1. scan leftwards from the right edge for the system-button area
    int rightHit = -1, leftHit = -1, gap = 0;
    int maxL = 0, maxR = -1;
    bool sawMin = false, sawClose = false;
    for (int x = f.right - D(3); x > f.right - D(400) && x > f.wr.left; x -= step) {
        if (!HitTest(h, x, y0, code)) return;
        if (IsCaptionButton(code)) {
            if (rightHit < 0) rightHit = x;
            leftHit = x;
            gap = 0;
            if (code == HTMINBUTTON) sawMin = true;
            if (code == HTCLOSE) sawClose = true;
            if (code == HTMAXBUTTON) {
                if (maxR < 0) maxR = x;
                maxL = x;
            }
        } else if (rightHit >= 0) {
            if (++gap >= 3) break;
        } else if (x < f.right - D(150)) {
            return; // nothing there: not a title bar we can measure
        }
    }
    if (rightHit < 0) return;
    for (int x = leftHit - 1; x > leftHit - step; x--) { // exact left edge
        if (!HitTest(h, x, y0, code)) return;
        if (!IsCaptionButton(code)) break;
        leftHit = x;
    }
    int capL = leftHit;
    int probeX = capL + D(8);
    if (!sawMin && !sawClose && maxR >= 0) {
        // Only the maximize button identifies itself; assume equal widths.
        int w = maxR - maxL + step;
        probeX = (maxL + maxR) / 2;
        if (style & WS_MINIMIZEBOX) capL = maxL - w;
    }

    // 2. how tall are the buttons?
    LRESULT base;
    if (!HitTest(h, probeX, y0, base)) return;
    int bottom = y0;
    for (int y = y0 + D(3); y < f.top + D(96); y += D(3)) {
        if (!HitTest(h, probeX, y, code)) return;
        if (code != base) break;
        bottom = y;
    }
    for (int y = bottom + 1; y < bottom + D(3); y++) {
        if (!HitTest(h, probeX, y, code)) return;
        if (code != base) break;
        bottom = y;
    }
    int height = bottom + 1 - f.top;
    if (height < D(16)) return;

    // 3. nearest free stretch of title bar left of the buttons
    const int fs = D(8), barW = count * D(32) + D(2), yc = f.top + height / 2;
    const int need = (barW + fs - 1) / fs + 1;
    int run = 0, shift = 0;
    bool found = false;
    // how much room is there right next to the buttons? (tab strips keep a
    // small drag area there that may be narrower than our two buttons)
    int adjacent = 0;
    for (int x = capL - D(2); adjacent < barW + D(8) && x > f.wr.left; x -= D(3)) {
        if (!HitTest(h, x, yc, code)) return;
        if (code != HTCAPTION) break;
        adjacent += D(3);
    }
    t.probeFreeAdjacent = adjacent;
    for (int i = 0; i < D(260) / fs + need; i++) {
        int x = capL - D(3) - i * fs;
        if (x <= f.wr.left) break;
        if (!HitTest(h, x, yc, code)) return;
        run = code == HTCAPTION ? run + 1 : 0;
        if (run >= need) {
            shift = (i + 1 - need) * fs;
            found = true;
            break;
        }
    }
    t.probeFromRight = f.right - capL;
    t.probeHeight = height;
    t.probeShift = found ? shift : 0;
    t.probeValid = true;
}

void ComputeGeometry(const Tracked& t, LONG style, LONG ex, const Frame& f, int count, Geometry& g)
{
    HWND h = t.hwnd;
    const RECT& wr = f.wr;
    UINT dpi = f.dpi;
    g.dpi = dpi;
    auto D = [dpi](int v) { return MulDiv(v, (int)dpi, 96); };

    int capL, capR, capT, capB;
    int extraShift = 0;
    bool measured = false; // caption button rectangle is known, not guessed
    if (f.custom && t.probeValid) {
        capR = f.right;
        capL = f.right - t.probeFromRight;
        capT = f.top;
        capB = capT + t.probeHeight;
        extraShift = t.manual ? 0 : t.probeShift;
        measured = true;
    } else {
        // Where are the minimize / maximize / close buttons? Ask the compositor.
        RECT cb = {};
        bool have = false;
        if (SUCCEEDED(DwmGetWindowAttribute(h, DWMWA_CAPTION_BUTTON_BOUNDS, &cb, sizeof(cb)))) {
            int cw = cb.right - cb.left, ch = cb.bottom - cb.top;
            have = cw >= D(16) && ch >= D(12) && ch <= D(80) && cw <= wr.right - wr.left;
        }
        if (have) {
            capL = wr.left + cb.left;
            capR = wr.left + cb.right;
            capT = wr.top + cb.top;
            capB = wr.top + cb.bottom;
            if (capT < f.top) capT = f.top;
            if (capB < capT + D(12)) capB = capT + D(30);
            measured = true;
        } else {
            // No answer: assume the standard layout.
            int n = 1;
            if (style & (WS_MINIMIZEBOX | WS_MAXIMIZEBOX)) n = 3;
            else if (ex & WS_EX_CONTEXTHELP) n = 2;
            capR = f.right;
            capL = capR - n * D(46);
            capT = f.top;
            capB = capT + D(30);
        }
    }

    int capH = capB - capT;
    int pct = Clamp(t.size > 0 ? t.size : g_cfg.size, 40, 300);
    g.scale = pct / 100.0f;
    int bh = Clamp(capH, D(20), D(32)) * pct / 100;
    int bw = D(32) * pct / 100;
    if (f.custom && t.probeValid && !t.manual && extraShift == 0 && t.probeFreeAdjacent > 0 &&
        t.probeFreeAdjacent < count * bw + D(2)) {
        // Not enough free room for full-size buttons: make them narrower
        // rather than cover the program's own controls (e.g. a "new tab" +).
        int w = (t.probeFreeAdjacent - D(2)) / count;
        if (w >= D(20) * pct / 100) bw = w;
    }
    int top = capT + (capH - bh) / 2 + D(t.down);
    int shift = D(2) + extraShift + D(g_cfg.moveLeft) + (f.zoomed ? D(g_cfg.moveLeftMaximized) : 0) + D(t.offset);
    bool mirrored = (capL + capR) / 2 < (wr.left + wr.right) / 2; // right-to-left windows
    if (t.left) {
        // Chosen by the user: left end of the title bar. A negative offset
        // moves the buttons to the right, clear of an icon or menu.
        g.bar.left = f.left + D(8) - D(t.offset);
        g.bar.right = g.bar.left + count * bw;
    } else if (!mirrored) {
        g.bar.right = capL - shift;
        g.bar.left = g.bar.right - count * bw;
    } else {
        g.bar.left = capR + shift;
        g.bar.right = g.bar.left + count * bw;
    }
    g.bar.top = top;
    g.bar.bottom = top + bh;

    int invisibleBottom = wr.bottom - f.efb.bottom;
    if (invisibleBottom < 0 || invisibleBottom > D(24)) invisibleBottom = 0;
    int captionBottom;
    if (measured)
        captionBottom = capB - wr.top;
    else if ((style & WS_CAPTION) == WS_CAPTION && !f.custom)
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
    t.rolled = t.rollPending = t.forcedTopmost = t.suspended = t.zoomedAtRoll = false;
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
    if ((int)(now - t.sampleDue) < 0 || now - t.sampleTick < 700) return;
    t.sampleTick = now;
    POINT pt = { bar.left - MulDiv(5, (int)dpi, 96), 0 };
    int ys[3] = { bar.top + (bar.bottom - bar.top) / 4, (bar.top + bar.bottom) / 2,
                  bar.bottom - (bar.bottom - bar.top) / 4 };

    // Only trust the screen if nothing covers that spot.
    int guard = 0;
    for (HWND w = GetTopWindow(nullptr); w && w != t.hwnd && guard < 4000; w = GetWindow(w, GW_HWNDNEXT), guard++) {
        if (w == t.bar || !IsWindowVisible(w) || IsIconic(w)) continue;
        LONG wex = GetWindowLongW(w, GWL_EXSTYLE);
        // click-through overlays (screen tools, input helpers) are not real covers
        if ((wex & WS_EX_TRANSPARENT) || ((wex & WS_EX_LAYERED) && (wex & WS_EX_NOACTIVATE))) continue;
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

// Look at the title bar colour again, but only after window animations
// (restore, activation) have finished - sampling mid-animation reads
// whatever is behind the window.
void RequestSample(Tracked& t)
{
    t.needSample = true;
    t.sampleDue = GetTickCount() + 450;
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

    // While the user drags a rolled-up window many programs enforce their
    // minimum height, which puffs the strip up. That is not a resize: keep
    // the rolled state and put the strip back when the drag ends.
    {
        GUITHREADINFO gi = {};
        gi.cbSize = sizeof(gi);
        bool moving = GetGUIThreadInfo(GetWindowThreadProcessId(h, nullptr), &gi) &&
                      (gi.flags & GUI_INMOVESIZE) && gi.hwndMoveSize == h;
        if (moving && !t.inMoveSize) {
            POINT p;
            GetCursorPos(&p);
            int edge = MulDiv(5, (int)GetDpiForWindow(h), 96);
            // grabbed by the top or bottom edge: a real vertical resize
            t.moveIsResize = p.y >= wr.bottom - 2 * edge || p.y <= wr.top + edge;
        } else if (!moving && t.inMoveSize && !t.moveIsResize && t.rolled && !t.suspended && !t.rollPending &&
                   !iconic && Abs((wr.bottom - wr.top) - t.rolledHeight) > 2) {
            SetWindowPos(h, nullptr, 0, 0, wr.right - wr.left, t.rolledHeight,
                         SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOSENDCHANGING | SWP_ASYNCWINDOWPOS);
            t.rollPending = true;
            t.rollTick = GetTickCount();
            Burst();
        }
        t.inMoveSize = moving;
    }

    if (t.rolled && !iconic && !(t.inMoveSize && !t.moveIsResize)) {
        bool strip = Abs((wr.bottom - wr.top) - t.rolledHeight) <= 2;
        bool zoomed = IsZoomed(h) != 0;
        if (t.rollPending) {
            if (strip) t.rollPending = false;
            else if (GetTickCount() - t.rollTick > 1500) ClearRolled(t); // the roll did not take
        } else if (zoomed == t.zoomedAtRoll) {
            if (strip) t.suspended = false;
            else ClearRolled(t); // resized by the user or the program: treat as unrolled
        } else if (zoomed) {
            // Rolled, then maximized: it shows in full for now. Windows brings
            // the strip back on restore, and so do we.
            t.suspended = true;
        } else if (strip && t.normalHeight > t.rolledHeight + 2) {
            // Rolled while maximized, then restored as a strip.
            t.zoomedAtRoll = t.suspended = false;
            t.oldHeight = t.normalHeight;
            SetPropW(h, FBX_PROP_OLDHEIGHT, (HANDLE)(INT_PTR)t.oldHeight);
        } else {
            ClearRolled(t);
        }
    }

    t.nBtn = 0;
    if (g_cfg.roll && !t.noRoll) t.btn[t.nBtn++] = BTN_ROLL;
    if (g_cfg.pin && !t.noPin) t.btn[t.nBtn++] = BTN_PIN;

    bool show = t.nBtn > 0 && !iconic && IsWindowVisible(h) && StyleEligible(style, ex) && !IsCloaked(h) &&
                !IsFullscreen(h, wr);
    Geometry g = {};
    if (show) {
        Frame f;
        GetFrame(h, wr, f);
        if (f.custom) ProbeCustomTitleBar(t, f, style, t.nBtn);
        ComputeGeometry(t, style, ex, f, t.nBtn, g);
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

    if (!IsWindowVisible(t.bar)) RequestSample(t); // coming back from hidden / minimized
    else if (!t.needSample && GetTickCount() - t.sampleTick > 3000) t.needSample = true; // periodic self-check
    if (t.needSample) SampleBackground(t, g.bar, g.dpi);

    bool topmost = (ex & WS_EX_TOPMOST) != 0;
    ButtonDraw bd[2] = {};
    for (int i = 0; i < t.nBtn; i++) {
        bd[i].type = t.btn[i];
        bd[i].active = t.btn[i] == BTN_PIN ? topmost : (t.rolled && !t.suspended);
        bd[i].hot = t.hot == i;
        bd[i].pressed = t.pressed == i && t.hot == i;
    }
    bool sameSize = (g.bar.right - g.bar.left) == (t.shownRect.right - t.shownRect.left) &&
                    (g.bar.bottom - g.bar.top) == (t.shownRect.bottom - t.shownRect.top);
    bool samePos = g.bar.left == t.shownRect.left && g.bar.top == t.shownRect.top;
    bool sameLook = sameSize && t.shownCount == t.nBtn && t.shownDark == t.dark && t.shownDpi == g.dpi &&
                    t.shownScale == g.scale &&
                    !memcmp(bd, t.shownBtn, sizeof(bd));
    if (!sameLook) {
        RenderBar(t.bar, g.bar, bd, t.nBtn, g.dpi, t.dark, g.scale);
        t.shownScale = g.scale;
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
    RequestSample(t);
    ApplyException(t);

    // Adopt a window that was rolled up by a previous instance of FBX.
    int oldH = (int)(INT_PTR)GetPropW(h, FBX_PROP_OLDHEIGHT);
    if (oldH > 0) {
        RECT wr;
        GetWindowRect(h, &wr);
        int rh = (int)(INT_PTR)GetPropW(h, FBX_PROP_ROLLEDHEIGHT);
        t.rolled = true;
        t.oldHeight = t.normalHeight = oldH;
        t.zoomedAtRoll = IsZoomed(h) != 0;
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

int NormalHeight(HWND h)
{
    WINDOWPLACEMENT wp = {};
    wp.length = sizeof(wp);
    if (!GetWindowPlacement(h, &wp)) return 0;
    return wp.rcNormalPosition.bottom - wp.rcNormalPosition.top;
}

void Roll(Tracked& t)
{
    if ((t.rolled && !t.suspended) || IsIconic(t.hwnd)) return;
    HWND h = t.hwnd;
    RECT wr;
    if (!GetWindowRect(h, &wr)) return;
    LONG style = GetWindowLongW(h, GWL_STYLE), ex = GetWindowLongW(h, GWL_EXSTYLE);
    Frame f;
    GetFrame(h, wr, f);
    Geometry g = {};
    ComputeGeometry(t, style, ex, f, t.nBtn ? t.nBtn : 1, g);
    int height = wr.bottom - wr.top, width = wr.right - wr.left;
    const UINT keep = SWP_NOMOVE | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS;

    if (height <= g.rolledHeight + 2) {
        // Already just a title bar and we do not know why. Never leave the
        // button dead: open the window to a sensible height.
        int nh = NormalHeight(h);
        if (nh <= g.rolledHeight + MulDiv(60, (int)g.dpi, 96)) nh = MulDiv(480, (int)g.dpi, 96);
        ClearRolled(t);
        SetWindowPos(h, nullptr, 0, 0, width, nh, keep | SWP_NOZORDER);
        Burst();
        return;
    }

    // Height to return to once the window is neither rolled nor maximized.
    int normal = height;
    if (t.rolled && t.suspended) normal = t.oldHeight;      // was rolled before it got maximized
    else if (f.zoomed) normal = NormalHeight(h);
    bool forced = t.forcedTopmost || (g_cfg.rolledTopmost && !(ex & WS_EX_TOPMOST));

    t.oldHeight = height;
    t.normalHeight = normal;
    t.rolledHeight = g.rolledHeight;
    t.zoomedAtRoll = f.zoomed;
    t.suspended = false;
    t.forcedTopmost = forced;
    SetPropW(h, FBX_PROP_OLDHEIGHT, (HANDLE)(INT_PTR)t.oldHeight);
    SetPropW(h, FBX_PROP_ROLLEDHEIGHT, (HANDLE)(INT_PTR)t.rolledHeight);
    if (forced) SetPropW(h, FBX_PROP_FORCEDTOP, (HANDLE)(INT_PTR)1);

    // SWP_NOSENDCHANGING lets the window go below its normal minimum size
    // (the original FbX subclassed the window to achieve the same).
    UINT fl = keep | SWP_NOSENDCHANGING;
    if (!g_cfg.rolledTopmost) fl |= SWP_NOZORDER;
    SetWindowPos(h, g_cfg.rolledTopmost ? HWND_TOPMOST : nullptr, 0, 0, width, t.rolledHeight, fl);
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
    bool suspended = t.suspended;
    RECT wr;
    bool ok = GetWindowRect(h, &wr) != 0;
    ClearRolled(t);
    if (suspended) {
        // Maximized right now; only its remembered normal size is a strip.
        WINDOWPLACEMENT wp = {};
        wp.length = sizeof(wp);
        if (GetWindowPlacement(h, &wp)) {
            wp.rcNormalPosition.bottom = wp.rcNormalPosition.top + oldH;
            wp.flags = WPF_ASYNCWINDOWPLACEMENT;
            SetWindowPlacement(h, &wp);
        }
    } else if (ok) {
        // Like FbX: keep the current position and width, restore the height.
        SetWindowPos(h, nullptr, 0, 0, wr.right - wr.left, oldH,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
    }
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
        if (t.rolled && !t.suspended) Unroll(t);
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
            t->hot = -1;
            UpdateBar(*t);
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
        if (prev != g_win.end()) RequestSample(prev->second);
        g_lastForeground = hwnd;
        if (it != g_win.end()) {
            RequestSample(it->second);
            it->second.reprobe = true;
        }
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

void TrackerListApps(std::vector<std::pair<std::wstring, std::wstring>>& out)
{
    for (auto& kv : g_win) {
        const Tracked& t = kv.second;
        if (t.exe.empty() || !IsWindowVisible(t.hwnd)) continue;
        bool seen = false;
        for (auto& o : out)
            if (o.first == t.exe) seen = true;
        if (seen) continue;
        wchar_t title[80] = L"";
        GetWindowTextW(t.hwnd, title, 80);
        out.push_back({ t.exe, title });
    }
}

void TrackerDiagnostics(std::wstring& out)
{
    wchar_t b[512];
    swprintf(b, 512, L"FBX diagnostics. elevated=%d size=%d%% moveLeft=%d/%d\r\n"
                     L"Hit-test letters, read from the right edge leftwards in 6 px steps:\r\n"
                     L"  X close, x maximize, m minimize, C draggable title bar, c program control, . other\r\n\r\n",
             IsElevated(), g_cfg.size, g_cfg.moveLeft, g_cfg.moveLeftMaximized);
    out += b;
    for (auto& kv : g_win) {
        Tracked& t = kv.second;
        HWND h = t.hwnd;
        if (!IsWindowVisible(h) || IsIconic(h)) continue;
        wchar_t cls[80] = L"";
        GetClassNameW(h, cls, 80);
        RECT wr;
        GetWindowRect(h, &wr);
        Frame f;
        GetFrame(h, wr, f);
        RECT cb = {};
        HRESULT hr = DwmGetWindowAttribute(h, DWMWA_CAPTION_BUTTON_BOUNDS, &cb, sizeof(cb));
        POINT c = { 0, 0 };
        ClientToScreen(h, &c);
        swprintf(b, 512,
                 L"%ls  [%ls]\r\n  dpi %u  maximized %d  rolled %d  rule: left=%d manual=%d offset=%d down=%d size=%d\r\n"
                 L"  window %ld,%ld-%ld,%ld  frame %ld,%ld-%ld,%ld  client top %ld  self-drawn title bar: %d\r\n",
                 t.exe.c_str(), cls, f.dpi, f.zoomed, t.rolled, t.left, t.manual, t.offset, t.down, t.size, wr.left,
                 wr.top, wr.right, wr.bottom, f.efb.left, f.efb.top, f.efb.right, f.efb.bottom, c.y, f.custom);
        out += b;
        swprintf(b, 512,
                 L"  compositor buttons %ld,%ld-%ld,%ld (hr %08lx)\r\n"
                 L"  measured: valid=%d buttons start %d px from right, height %d, free next to them %d, auto shift %d\r\n"
                 L"  our bar %ld,%ld-%ld,%ld  dark=%d\r\n",
                 cb.left, cb.top, cb.right, cb.bottom, (unsigned long)hr, t.probeValid, t.probeFromRight, t.probeHeight,
                 t.probeFreeAdjacent, t.probeShift, t.shownRect.left, t.shownRect.top, t.shownRect.right,
                 t.shownRect.bottom, t.dark);
        out += b;
        int step = MulDiv(6, (int)f.dpi, 96);
        for (int row = 0; row < 2; row++) {
            int y = f.top + MulDiv(row == 0 ? 10 : 22, (int)f.dpi, 96);
            std::wstring line = row == 0 ? L"  y+10: " : L"  y+22: ";
            if (IsHungAppWindow(h)) {
                line += L"(not responding)";
            } else {
                for (int x = f.right - MulDiv(3, (int)f.dpi, 96), n = 0; n < 80 && x > wr.left; x -= step, n++) {
                    LRESULT code;
                    if (!HitTest(h, x, y, code)) {
                        line += L'?';
                        break;
                    }
                    line += code == HTCLOSE ? L'X' : code == HTMAXBUTTON ? L'x' : code == HTMINBUTTON ? L'm'
                          : code == HTCAPTION ? L'C' : code == HTCLIENT ? L'c' : L'.';
                }
            }
            out += line + L"\r\n";
        }
        out += L"\r\n";
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
