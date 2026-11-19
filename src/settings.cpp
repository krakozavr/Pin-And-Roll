// The settings window. Every change is applied at once so that the buttons
// can be positioned by eye; OK writes the INI file, Cancel puts things back.
// License: GPL-3.0-or-later.
#include "common.h"
#include "resource.h"
#include <commctrl.h>
#include <cwctype>

static HWND g_dlg;
static Config g_backup;
static bool g_loading;

static const int kRuleControls[] = { IDC_REMOVE, IDC_IGNORE, IDC_NOPIN, IDC_NOROLL, IDC_POSLABEL, IDC_RIGHT, IDC_LEFT,
                                     IDC_OFFLABEL, IDC_OFFSET, IDC_OFFSPIN, IDC_OFFLABEL2, IDC_RHLABEL, IDC_ROLLHEIGHT,
                                     IDC_RHLABEL2 };

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
    EnableWindow(GetDlgItem(d, IDC_ROLLTOP), g_cfg.roll);
    if (ExceptionRule* r = SelectedRule(d)) {
        r->ignore = Checked(d, IDC_IGNORE);
        r->noPin = Checked(d, IDC_NOPIN);
        r->noRoll = Checked(d, IDC_NOROLL);
        r->left = Checked(d, IDC_LEFT);
        r->offset = (int)GetDlgItemInt(d, IDC_OFFSET, nullptr, TRUE);
        r->rollHeight = (int)GetDlgItemInt(d, IDC_ROLLHEIGHT, nullptr, FALSE);
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
        HWND lb = GetDlgItem(d, IDC_APPS);
        for (const auto& r : g_cfg.exceptions) SendMessageW(lb, LB_ADDSTRING, 0, (LPARAM)r.exe.c_str());
        if (!g_cfg.exceptions.empty()) SendMessageW(lb, LB_SETCURSEL, 0, 0);
        g_loading = false;
        LoadRule(d);
        SetForegroundWindow(d);
        return TRUE;
    }
    case WM_COMMAND: {
        int id = LOWORD(wp), code = HIWORD(wp);
        switch (id) {
        case IDOK:
            Apply(d);
            SaveConfig(g_cfg);
            if (Checked(d, IDC_AUTOSTART) != IsAutoStart()) SetAutoStart(Checked(d, IDC_AUTOSTART));
            EndDialog(d, IDOK);
            return TRUE;
        case IDCANCEL: {
            bool hadRoll = g_cfg.roll;
            g_cfg = g_backup;
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
        case IDC_PINBTN:
        case IDC_ROLLBTN:
        case IDC_ROLLTOP:
        case IDC_IGNORE:
        case IDC_NOPIN:
        case IDC_NOROLL:
            if (code == BN_CLICKED) Apply(d);
            return TRUE;
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

void ShowSettings(HINSTANCE inst)
{
    if (g_dlg) {
        SetForegroundWindow(g_dlg);
        return;
    }
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_UPDOWN_CLASS | ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);
    DialogBoxParamW(inst, MAKEINTRESOURCEW(IDD_SETTINGS), nullptr, SettingsProc, 0);
}
