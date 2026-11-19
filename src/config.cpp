// Settings: a plain INI file, by default %APPDATA%\FBX\fbx.ini.
// License: GPL-3.0-or-later.
#include "common.h"
#include <shlobj.h>
#include <cwctype>
#include <cstdlib>

static const wchar_t* kDefaultIni =
    L"; FBX Pin & Roll-up settings. Use \"Reload settings\" in the tray menu after editing.\r\n"
    L"[Settings]\r\n"
    L"; Push-pin button to keep windows on top\r\n"
    L"PinButton=1\r\n"
    L"; Roll-up button to display only the title bar of a window\r\n"
    L"RollButton=1\r\n"
    L"; When rolled up, keep the window bar on top\r\n"
    L"RolledTopmost=0\r\n"
    L"; Move the buttons to the left by N pixels (measured at 100% scaling)\r\n"
    L"MoveLeft=0\r\n"
    L"; ...and by N more pixels if a window is maximized\r\n"
    L"MoveLeftMaximized=0\r\n"
    L"; Optional hot keys acting on the active window, e.g. Ctrl+Alt+T. Empty = none.\r\n"
    L"PinHotkey=\r\n"
    L"RollHotkey=\r\n"
    L"\r\n"
    L"[Exceptions]\r\n"
    L"; program.exe = comma separated list of:\r\n"
    L";   ignore        do not handle this program at all\r\n"
    L";   nopin         no push-pin button\r\n"
    L";   noroll        no roll-up button\r\n"
    L";   left          put the buttons at the left end of the title bar\r\n"
    L";   offset:N      move the buttons left by N more pixels (negative = to the right)\r\n"
    L";   rollheight:N  height of the rolled-up window in pixels (at 100% scaling)\r\n"
    L"; Example:\r\n"
    L"; someapp.exe=noroll,offset:40\r\n";

static std::wstring Lower(std::wstring s)
{
    for (auto& c : s) c = (wchar_t)towlower(c);
    return s;
}

static std::wstring Trim(const std::wstring& s)
{
    size_t a = 0, b = s.size();
    while (a < b && iswspace(s[a])) a++;
    while (b > a && iswspace(s[b - 1])) b--;
    return s.substr(a, b - a);
}

const ExceptionRule* Config::Find(const std::wstring& exeLower) const
{
    for (const auto& e : exceptions)
        if (e.exe == exeLower) return &e;
    return nullptr;
}

std::wstring DefaultConfigPath()
{
    wchar_t dir[MAX_PATH] = L"";
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_APPDATA | CSIDL_FLAG_CREATE, nullptr, 0, dir)))
        return L"fbx.ini";
    std::wstring p = std::wstring(dir) + L"\\FBX";
    CreateDirectoryW(p.c_str(), nullptr);
    return p + L"\\fbx.ini";
}

static Hotkey ParseHotkey(const std::wstring& text)
{
    Hotkey hk;
    std::wstring s = Lower(text);
    size_t pos = 0;
    while (pos <= s.size()) {
        size_t plus = s.find(L'+', pos);
        if (plus == std::wstring::npos) plus = s.size();
        std::wstring tok = Trim(s.substr(pos, plus - pos));
        pos = plus + 1;
        if (tok.empty()) continue;
        if (tok == L"ctrl" || tok == L"control") hk.mod |= MOD_CONTROL;
        else if (tok == L"alt") hk.mod |= MOD_ALT;
        else if (tok == L"shift") hk.mod |= MOD_SHIFT;
        else if (tok == L"win") hk.mod |= MOD_WIN;
        else if (tok.size() == 1 && ((tok[0] >= L'a' && tok[0] <= L'z') || (tok[0] >= L'0' && tok[0] <= L'9')))
            hk.vk = (UINT)towupper(tok[0]);
        else if (tok[0] == L'f' && tok.size() <= 3) {
            int n = _wtoi(tok.c_str() + 1);
            if (n >= 1 && n <= 24) hk.vk = VK_F1 + n - 1;
        }
        else if (tok == L"up") hk.vk = VK_UP;
        else if (tok == L"down") hk.vk = VK_DOWN;
        else if (tok == L"space") hk.vk = VK_SPACE;
    }
    if (!hk.vk || !hk.mod) hk = Hotkey(); // require a modifier
    return hk;
}

void LoadConfig(Config& cfg)
{
    if (cfg.path.empty()) cfg.path = DefaultConfigPath();
    const wchar_t* ini = cfg.path.c_str();

    if (GetFileAttributesW(ini) == INVALID_FILE_ATTRIBUTES) {
        HANDLE f = CreateFileW(ini, GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f != INVALID_HANDLE_VALUE) {
            // UTF-16 LE with BOM so the profile API reads/writes Unicode.
            DWORD w;
            WriteFile(f, "\xFF\xFE", 2, &w, nullptr);
            WriteFile(f, kDefaultIni, (DWORD)(wcslen(kDefaultIni) * sizeof(wchar_t)), &w, nullptr);
            CloseHandle(f);
        }
    }

    cfg.pin = GetPrivateProfileIntW(L"Settings", L"PinButton", 1, ini) != 0;
    cfg.roll = GetPrivateProfileIntW(L"Settings", L"RollButton", 1, ini) != 0;
    cfg.rolledTopmost = GetPrivateProfileIntW(L"Settings", L"RolledTopmost", 0, ini) != 0;
    cfg.moveLeft = (int)GetPrivateProfileIntW(L"Settings", L"MoveLeft", 0, ini);
    cfg.moveLeftMaximized = (int)GetPrivateProfileIntW(L"Settings", L"MoveLeftMaximized", 0, ini);

    wchar_t buf[128];
    GetPrivateProfileStringW(L"Settings", L"PinHotkey", L"", buf, 128, ini);
    cfg.pinKey = ParseHotkey(buf);
    GetPrivateProfileStringW(L"Settings", L"RollHotkey", L"", buf, 128, ini);
    cfg.rollKey = ParseHotkey(buf);

    cfg.exceptions.clear();
    std::vector<wchar_t> sec(32768);
    DWORD n = GetPrivateProfileSectionW(L"Exceptions", sec.data(), (DWORD)sec.size(), ini);
    for (const wchar_t* p = sec.data(); n && *p; p += wcslen(p) + 1) {
        std::wstring line = p;
        size_t eq = line.find(L'=');
        if (eq == std::wstring::npos || line[0] == L';') continue;
        ExceptionRule r;
        r.exe = Lower(Trim(line.substr(0, eq)));
        if (r.exe.empty()) continue;
        std::wstring val = Lower(line.substr(eq + 1));
        size_t pos = 0;
        while (pos <= val.size()) {
            size_t comma = val.find(L',', pos);
            if (comma == std::wstring::npos) comma = val.size();
            std::wstring tok = Trim(val.substr(pos, comma - pos));
            pos = comma + 1;
            if (tok == L"ignore") r.ignore = true;
            else if (tok == L"nopin") r.noPin = true;
            else if (tok == L"noroll") r.noRoll = true;
            else if (tok == L"left") r.left = true;
            else if (tok.rfind(L"offset:", 0) == 0) r.offset = _wtoi(tok.c_str() + 7);
            else if (tok.rfind(L"rollheight:", 0) == 0) r.rollHeight = _wtoi(tok.c_str() + 11);
        }
        cfg.exceptions.push_back(r);
    }
}

void SaveToggles(const Config& cfg)
{
    const wchar_t* ini = cfg.path.c_str();
    WritePrivateProfileStringW(L"Settings", L"PinButton", cfg.pin ? L"1" : L"0", ini);
    WritePrivateProfileStringW(L"Settings", L"RollButton", cfg.roll ? L"1" : L"0", ini);
    WritePrivateProfileStringW(L"Settings", L"RolledTopmost", cfg.rolledTopmost ? L"1" : L"0", ini);
}

void SaveConfig(const Config& cfg)
{
    const wchar_t* ini = cfg.path.c_str();
    SaveToggles(cfg);
    WritePrivateProfileStringW(L"Settings", L"MoveLeft", std::to_wstring(cfg.moveLeft).c_str(), ini);
    WritePrivateProfileStringW(L"Settings", L"MoveLeftMaximized", std::to_wstring(cfg.moveLeftMaximized).c_str(), ini);

    // The section is a list of NUL-terminated "key=value" strings.
    std::wstring sec;
    for (const auto& r : cfg.exceptions) {
        std::wstring v;
        auto add = [&v](const std::wstring& t) { v += (v.empty() ? L"" : L",") + t; };
        if (r.ignore) add(L"ignore");
        if (r.noPin) add(L"nopin");
        if (r.noRoll) add(L"noroll");
        if (r.left) add(L"left");
        if (r.offset) add(L"offset:" + std::to_wstring(r.offset));
        if (r.rollHeight) add(L"rollheight:" + std::to_wstring(r.rollHeight));
        sec += r.exe + L"=" + v;
        sec.push_back(L'\0');
    }
    sec.push_back(L'\0');
    WritePrivateProfileSectionW(L"Exceptions", sec.c_str(), ini);
}

static const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t* kRunName = L"FBX Pin & Roll-up";

bool IsAutoStart()
{
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_QUERY_VALUE, &k) != ERROR_SUCCESS) return false;
    bool on = RegQueryValueExW(k, kRunName, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
    RegCloseKey(k);
    return on;
}

void SetAutoStart(bool on)
{
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) return;
    if (on) {
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring cmd = L"\"" + std::wstring(exe) + L"\"";
        RegSetValueExW(k, kRunName, 0, REG_SZ, (const BYTE*)cmd.c_str(), (DWORD)((cmd.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(k, kRunName);
    }
    RegCloseKey(k);
}

bool SystemAppsDark()
{
    DWORD v = 1, cb = sizeof(v);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &v, &cb);
    return v == 0;
}
