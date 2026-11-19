// The settings window. Every change is applied at once so that the buttons
// can be positioned by eye; OK writes the INI file, Cancel puts things back.
// License: GPL-3.0-or-later.
#include "common.h"
#include "resource.h"
#include <commctrl.h>
#include <shellapi.h>
#include <cwctype>

static HWND g_dlg;
static Config g_backup;
static std::wstring g_selectExe;
static bool g_loading = true; // controls notify before WM_INITDIALOG: ignore until filled in

static const int kRuleControls[] = { IDC_REMOVE, IDC_IGNORE, IDC_NOPIN, IDC_NOROLL, IDC_POSLABEL, IDC_RIGHT, IDC_LEFT,
                                     IDC_OFFLABEL, IDC_OFFSET, IDC_OFFSPIN, IDC_OFFLABEL2, IDC_RHLABEL, IDC_ROLLHEIGHT,
                                     IDC_RHLABEL2, IDC_DOWNLABEL, IDC_DOWN, IDC_DOWNSPIN, IDC_DOWNLABEL2,
                                     IDC_APPSIZELABEL, IDC_APPSIZE, IDC_APPSIZELABEL2, IDC_APPPEEK };

// Hot key control <-> our Hotkey (the control cannot take the Win key).
static Hotkey GetKey(HWND d, int id)
{
    WORD v = LOWORD(SendDlgItemMessageW(d, id, HKM_GETHOTKEY, 0, 0));
    Hotkey hk;
    hk.vk = LOBYTE(v);
    BYTE m = HIBYTE(v);
    if (m & HOTKEYF_CONTROL) hk.mod |= MOD_CONTROL;
    if (m & HOTKEYF_ALT) hk.mod |= MOD_ALT;
    if (m & HOTKEYF_SHIFT) hk.mod |= MOD_SHIFT;
    if (!hk.vk || !hk.mod) hk = Hotkey();
    return hk;
}
static void SetKey(HWND d, int id, const Hotkey& hk)
{
    BYTE m = 0;
    if (hk.mod & MOD_CONTROL) m |= HOTKEYF_CONTROL;
    if (hk.mod & MOD_ALT) m |= HOTKEYF_ALT;
    if (hk.mod & MOD_SHIFT) m |= HOTKEYF_SHIFT;
    // a plain key or Shift+key would steal ordinary typing: force Ctrl+Alt
    SendDlgItemMessageW(d, id, HKM_SETRULES, HKCOMB_NONE | HKCOMB_S, MAKELPARAM(HOTKEYF_CONTROL | HOTKEYF_ALT, 0));
    SendDlgItemMessageW(d, id, HKM_SETHOTKEY, MAKEWORD(hk.vk, m), 0);
}

static bool Checked(HWND d, int id) { return IsDlgButtonChecked(d, id) == BST_CHECKED; }
static void Check(HWND d, int id, bool on) { CheckDlgButton(d, id, on ? BST_CHECKED : BST_UNCHECKED); }

static ExceptionRule* SelectedRule(HWND d)
{
    HWND lb = GetDlgItem(d, IDC_APPS);
    int sel = (int)SendMessageW(lb, LB_GETCURSEL, 0, 0);
    if (sel == LB_ERR) return nullptr;
    wchar_t exe[MAX_PATH] = L"";
    if (SendMessageW(lb, LB_GETTEXTLEN, sel, 0) >= MAX_PATH) return nullptr;
    SendMessageW(lb, LB_GETTEXT, sel, (LPARAM)exe);
    for (auto& r : g_cfg.exceptions)
        if (r.exe == exe) return &r;
    return nullptr;
}

static void LoadRule(HWND d)
{
    g_loading = true;
    ExceptionRule* r = SelectedRule(d);
    for (int id : kRuleControls) EnableWindow(GetDlgItem(d, id), r != nullptr);
    ShowWindow(GetDlgItem(d, IDC_HINT), r ? SW_HIDE : SW_SHOW);
    ExceptionRule none;
    if (!r) r = &none;
    Check(d, IDC_IGNORE, r->ignore);
    Check(d, IDC_NOPIN, r->noPin);
    Check(d, IDC_NOROLL, r->noRoll);
    CheckRadioButton(d, IDC_RIGHT, IDC_LEFT, r->left ? IDC_LEFT : IDC_RIGHT);
    SetDlgItemInt(d, IDC_OFFSET, (UINT)r->offset, TRUE);
    SetDlgItemInt(d, IDC_ROLLHEIGHT, (UINT)r->rollHeight, FALSE);
    SetDlgItemInt(d, IDC_DOWN, (UINT)r->down, TRUE);
    CheckDlgButton(d, IDC_APPPEEK, r->peek == 1 ? BST_CHECKED : r->peek == 0 ? BST_UNCHECKED : BST_INDETERMINATE);
    SetDlgItemInt(d, IDC_APPSIZE, (UINT)r->size, FALSE);
    g_loading = false;
}

static void SelectApp(HWND d, const std::wstring& exe)
{
    HWND lb = GetDlgItem(d, IDC_APPS);
    int i = (int)SendMessageW(lb, LB_FINDSTRINGEXACT, (WPARAM)-1, (LPARAM)exe.c_str());
    if (i == LB_ERR) i = (int)SendMessageW(lb, LB_ADDSTRING, 0, (LPARAM)exe.c_str());
    SendMessageW(lb, LB_SETCURSEL, i, 0);
    LoadRule(d);
}

// Read the controls into g_cfg and let the buttons on screen follow.
static void Apply(HWND d)
{
    if (g_loading) return;
    bool hadRoll = g_cfg.roll;
    g_cfg.pin = Checked(d, IDC_PINBTN);
    g_cfg.roll = Checked(d, IDC_ROLLBTN);
    g_cfg.rolledTopmost = Checked(d, IDC_ROLLTOP);
    g_cfg.moveLeft = (int)GetDlgItemInt(d, IDC_MOVELEFT, nullptr, TRUE);
    g_cfg.moveLeftMaximized = (int)GetDlgItemInt(d, IDC_MOVELEFTMAX, nullptr, TRUE);
    int size = (int)GetDlgItemInt(d, IDC_BTNSIZE, nullptr, FALSE);
    if (size >= 40 && size <= 300) g_cfg.size = size; // ignore half-typed values
    g_cfg.runAsAdmin = Checked(d, IDC_ADMIN);
    g_cfg.peek = Checked(d, IDC_PEEK);
    EnableWindow(GetDlgItem(d, IDC_PEEK), g_cfg.roll);
    Hotkey pk = GetKey(d, IDC_PINKEY), rk = GetKey(d, IDC_ROLLKEY);
    if (pk.vk != g_cfg.pinKey.vk || pk.mod != g_cfg.pinKey.mod || rk.vk != g_cfg.rollKey.vk ||
        rk.mod != g_cfg.rollKey.mod) {
        g_cfg.pinKey = pk;
        g_cfg.rollKey = rk;
        AppRegisterHotkeys();
    }
    EnableWindow(GetDlgItem(d, IDC_ROLLTOP), g_cfg.roll);
    if (ExceptionRule* r = SelectedRule(d)) {
        r->ignore = Checked(d, IDC_IGNORE);
        r->noPin = Checked(d, IDC_NOPIN);
        r->noRoll = Checked(d, IDC_NOROLL);
        r->left = Checked(d, IDC_LEFT);
        r->offset = (int)GetDlgItemInt(d, IDC_OFFSET, nullptr, TRUE);
        r->rollHeight = (int)GetDlgItemInt(d, IDC_ROLLHEIGHT, nullptr, FALSE);
        r->down = (int)GetDlgItemInt(d, IDC_DOWN, nullptr, TRUE);
        UINT pk = IsDlgButtonChecked(d, IDC_APPPEEK);
        r->peek = pk == BST_CHECKED ? 1 : pk == BST_UNCHECKED ? 0 : -1;
        int rs = (int)GetDlgItemInt(d, IDC_APPSIZE, nullptr, FALSE);
        r->size = (rs >= 40 && rs <= 300) ? rs : 0;
    }
    if (hadRoll && !g_cfg.roll) TrackerUnrollAll(); // as FbX did
    TrackerRefreshAll();
}

static void PickApp(HWND d)
{
    std::vector<std::pair<std::wstring, std::wstring>> apps;
    TrackerListApps(apps);
    HMENU m = CreatePopupMenu();
    if (apps.empty()) AppendMenuW(m, MF_STRING | MF_GRAYED, 0, L"(no program with buttons is running)");
    for (size_t i = 0; i < apps.size(); i++) {
        std::wstring title = apps[i].second;
        for (size_t p = 0; (p = title.find(L'&', p)) != std::wstring::npos; p += 2) title.insert(p, 1, L'&');
        std::wstring item = apps[i].first + (title.empty() ? L"" : L"   -   " + title);
        AppendMenuW(m, MF_STRING, (UINT_PTR)(i + 1), item.c_str());
    }
    RECT rc;
    GetWindowRect(GetDlgItem(d, IDC_PICK), &rc);
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, rc.left, rc.bottom, 0, d, nullptr);
    DestroyMenu(m);
    if (cmd < 1 || cmd > (int)apps.size()) return;
    const std::wstring& exe = apps[cmd - 1].first;
    if (!g_cfg.Find(exe)) {
        ExceptionRule r;
        r.exe = exe;
        g_cfg.exceptions.push_back(r);
    }
    SelectApp(d, exe);
}

static INT_PTR CALLBACK SettingsProc(HWND d, UINT msg, WPARAM wp, LPARAM)
{
    switch (msg) {
    case WM_INITDIALOG: {
        g_dlg = d;
        g_backup = g_cfg;
        g_loading = true;
        Check(d, IDC_PINBTN, g_cfg.pin);
        Check(d, IDC_ROLLBTN, g_cfg.roll);
        Check(d, IDC_ROLLTOP, g_cfg.rolledTopmost);
        EnableWindow(GetDlgItem(d, IDC_ROLLTOP), g_cfg.roll);
        SetDlgItemInt(d, IDC_MOVELEFT, (UINT)g_cfg.moveLeft, TRUE);
        SetDlgItemInt(d, IDC_MOVELEFTMAX, (UINT)g_cfg.moveLeftMaximized, TRUE);
        Check(d, IDC_AUTOSTART, IsAutoStart());
        SendDlgItemMessageW(d, IDC_OFFSPIN, UDM_SETRANGE32, (WPARAM)-3000, 3000);
        SendDlgItemMessageW(d, IDC_DOWNSPIN, UDM_SETRANGE32, (WPARAM)-200, 200);
        SetDlgItemInt(d, IDC_BTNSIZE, (UINT)g_cfg.size, FALSE);
        Check(d, IDC_ADMIN, g_cfg.runAsAdmin);
        Check(d, IDC_PEEK, g_cfg.peek);
        EnableWindow(GetDlgItem(d, IDC_PEEK), g_cfg.roll);
        SetKey(d, IDC_PINKEY, g_cfg.pinKey);
        SetKey(d, IDC_ROLLKEY, g_cfg.rollKey);
        HWND lb = GetDlgItem(d, IDC_APPS);
        for (const auto& r : g_cfg.exceptions) SendMessageW(lb, LB_ADDSTRING, 0, (LPARAM)r.exe.c_str());
        if (!g_cfg.exceptions.empty()) SendMessageW(lb, LB_SETCURSEL, 0, 0);
        g_loading = false;
        LoadRule(d);
        if (!g_selectExe.empty()) { // opened from a button's right-click menu
            if (!g_cfg.Find(g_selectExe)) {
                ExceptionRule r;
                r.exe = g_selectExe;
                g_cfg.exceptions.push_back(r);
            }
            SelectApp(d, g_selectExe);
        }
        SetForegroundWindow(d);
        return TRUE;
    }
    case WM_COMMAND: {
        int id = LOWORD(wp), code = HIWORD(wp);
        switch (id) {
        case IDOK: {
            Apply(d);
            SaveConfig(g_cfg);
            bool autoStart = Checked(d, IDC_AUTOSTART);
            bool adminChanged = g_cfg.runAsAdmin != g_backup.runAsAdmin;
            if (autoStart != IsAutoStart() || adminChanged) SetAutoStart(autoStart, g_cfg.runAsAdmin);
            EndDialog(d, IDOK);
            if (g_cfg.runAsAdmin && !IsElevated()) {
                // Windows asks for permission once; a new elevated copy takes over.
                if (!AppRestartElevated())
                    MessageBoxW(nullptr,
                                L"Pin&Roll is still running without administrator rights.\n"
                                L"It will ask again the next time it starts.",
                                L"Pin&Roll", MB_OK | MB_ICONINFORMATION);
            } else if (adminChanged && !g_cfg.runAsAdmin && IsElevated()) {
                MessageBoxW(nullptr, L"Pin&Roll will run without administrator rights the next time it starts.",
                            L"Pin&Roll", MB_OK | MB_ICONINFORMATION);
            }
            return TRUE;
        }
        case IDCANCEL: {
            bool hadRoll = g_cfg.roll;
            g_cfg = g_backup;
            AppRegisterHotkeys();
            if (hadRoll && !g_cfg.roll) TrackerUnrollAll();
            TrackerRefreshAll();
            EndDialog(d, IDCANCEL);
            return TRUE;
        }
        case IDC_APPS:
            if (code == LBN_SELCHANGE) LoadRule(d);
            return TRUE;
        case IDC_PICK:
            PickApp(d);
            return TRUE;
        case IDC_HELPBTN:
            AppShowHelp();
            return TRUE;
        case IDC_OPENINI: {
            // Save what is on screen and close first, so that this window
            // cannot overwrite what is then typed into the file.
            std::wstring path = g_cfg.path;
            SendMessageW(d, WM_COMMAND, IDOK, 0);
            ShellExecuteW(nullptr, L"open", L"notepad.exe", (L"\"" + path + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
            return TRUE;
        }
        case IDC_REMOVE:
            if (ExceptionRule* r = SelectedRule(d)) {
                HWND lb = GetDlgItem(d, IDC_APPS);
                int sel = (int)SendMessageW(lb, LB_GETCURSEL, 0, 0);
                g_cfg.exceptions.erase(g_cfg.exceptions.begin() + (r - g_cfg.exceptions.data()));
                SendMessageW(lb, LB_DELETESTRING, sel, 0);
                int n = (int)SendMessageW(lb, LB_GETCOUNT, 0, 0);
                if (n > 0) SendMessageW(lb, LB_SETCURSEL, sel < n ? sel : n - 1, 0);
                LoadRule(d);
                Apply(d);
            }
            return TRUE;
        case IDC_LEFT:
        case IDC_RIGHT:
            if (code == BN_CLICKED && !g_loading) {
                // Sensible starting points: at the left, clear the window icon.
                int off = (int)GetDlgItemInt(d, IDC_OFFSET, nullptr, TRUE);
                g_loading = true;
                if (id == IDC_LEFT && off >= 0) SetDlgItemInt(d, IDC_OFFSET, (UINT)-40, TRUE);
                if (id == IDC_RIGHT && off < 0) SetDlgItemInt(d, IDC_OFFSET, 0, TRUE);
                g_loading = false;
                Apply(d);
            }
            return TRUE;
        case IDC_ADMIN:
        case IDC_PEEK:
        case IDC_APPPEEK:
        case IDC_PINBTN:
        case IDC_ROLLBTN:
        case IDC_ROLLTOP:
        case IDC_IGNORE:
        case IDC_NOPIN:
        case IDC_NOROLL:
            if (code == BN_CLICKED) Apply(d);
            return TRUE;
        case IDC_PINKEY:
        case IDC_ROLLKEY:
        case IDC_BTNSIZE:
        case IDC_DOWN:
        case IDC_APPSIZE:
        case IDC_MOVELEFT:
        case IDC_MOVELEFTMAX:
        case IDC_OFFSET:
        case IDC_ROLLHEIGHT:
            if (code == EN_CHANGE) Apply(d);
            return TRUE;
        }
        break;
    }
    case WM_DESTROY:
        g_dlg = nullptr;
        break;
    }
    return FALSE;
}

bool SettingsIsOpen()
{
    return g_dlg != nullptr;
}

void ShowSettings(HINSTANCE inst, const std::wstring& selectExe)
{
    if (g_dlg) {
        if (!selectExe.empty()) {
            if (!g_cfg.Find(selectExe)) {
                ExceptionRule r;
                r.exe = selectExe;
                g_cfg.exceptions.push_back(r);
            }
            SelectApp(g_dlg, selectExe);
        }
        SetForegroundWindow(g_dlg);
        return;
    }
    g_selectExe = selectExe;
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_UPDOWN_CLASS | ICC_STANDARD_CLASSES | ICC_HOTKEY_CLASS };
    InitCommonControlsEx(&icc);
    g_loading = true;
    DialogBoxParamW(inst, MAKEINTRESOURCEW(IDD_SETTINGS), nullptr, SettingsProc, 0);
    g_loading = true;
}
