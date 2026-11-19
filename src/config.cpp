// Settings: a plain INI file, by default %APPDATA%\FBX\fbx.ini.
// License: GPL-3.0-or-later.
#include "common.h"
#include <shlobj.h>
#include <cwctype>
#include <cstdlib>
#include <shellapi.h>
#include <taskschd.h>
#include <oleauto.h>

static const wchar_t* kDefaultIni =
    L"; Pin&Roll settings. Use \"Reload settings\" in the tray menu after editing.\r\n"
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
    L"; Button size in percent\r\n"
    L"ButtonSize=100\r\n"
    L"; Restart with administrator rights (buttons also on elevated programs)\r\n"
    L"RunAsAdmin=0\r\n"
    L"; Show a rolled-up window in full while the mouse is over it\r\n"
    L"Peek=0\r\n"
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
    L";   down:N        move the buttons down by N pixels (negative = up)\r\n"
    L";   size:N        button size in percent for this program\r\n"
    L";   peek / nopeek show the rolled-up window while the mouse is over it, or never\r\n"
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
        return L"pinroll.ini";
    std::wstring p = std::wstring(dir) + L"\\PinRoll";
    CreateDirectoryW(p.c_str(), nullptr);
    std::wstring ini = p + L"\\pinroll.ini";
    // Settings from when the program was still called FBX.
    std::wstring old = std::wstring(dir) + L"\\FBX\\fbx.ini";
    if (GetFileAttributesW(ini.c_str()) == INVALID_FILE_ATTRIBUTES &&
        GetFileAttributesW(old.c_str()) != INVALID_FILE_ATTRIBUTES)
        CopyFileW(old.c_str(), ini.c_str(), TRUE);
    return ini;
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
        else if (tok.rfind(L"0x", 0) == 0) hk.vk = (UINT)wcstoul(tok.c_str(), nullptr, 16) & 0xFF;
        else if (tok == L"up") hk.vk = VK_UP;
        else if (tok == L"down") hk.vk = VK_DOWN;
        else if (tok == L"space") hk.vk = VK_SPACE;
    }
    if (!hk.vk || !hk.mod) hk = Hotkey(); // require a modifier
    return hk;
}

std::wstring HotkeyText(const Hotkey& hk)
{
    if (!hk.vk) return L"";
    std::wstring s;
    if (hk.mod & MOD_CONTROL) s += L"Ctrl+";
    if (hk.mod & MOD_ALT) s += L"Alt+";
    if (hk.mod & MOD_SHIFT) s += L"Shift+";
    if (hk.mod & MOD_WIN) s += L"Win+";
    wchar_t b[16];
    if ((hk.vk >= 'A' && hk.vk <= 'Z') || (hk.vk >= '0' && hk.vk <= '9')) swprintf(b, 16, L"%c", (wchar_t)hk.vk);
    else if (hk.vk >= VK_F1 && hk.vk <= VK_F24) swprintf(b, 16, L"F%u", hk.vk - VK_F1 + 1);
    else if (hk.vk == VK_UP) wcscpy(b, L"Up");
    else if (hk.vk == VK_DOWN) wcscpy(b, L"Down");
    else if (hk.vk == VK_SPACE) wcscpy(b, L"Space");
    else swprintf(b, 16, L"0x%02X", hk.vk);
    return s + b;
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
    cfg.size = (int)GetPrivateProfileIntW(L"Settings", L"ButtonSize", 100, ini);
    if (cfg.size < 40 || cfg.size > 300) cfg.size = 100;
    cfg.runAsAdmin = GetPrivateProfileIntW(L"Settings", L"RunAsAdmin", 0, ini) != 0;
    cfg.peek = GetPrivateProfileIntW(L"Settings", L"Peek", 0, ini) != 0;

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
            else if (tok == L"peek") r.peek = 1;
            else if (tok == L"nopeek") r.peek = 0;
            else if (tok.rfind(L"offset:", 0) == 0) r.offset = _wtoi(tok.c_str() + 7);
            else if (tok.rfind(L"rollheight:", 0) == 0) r.rollHeight = _wtoi(tok.c_str() + 11);
            else if (tok.rfind(L"size:", 0) == 0) r.size = _wtoi(tok.c_str() + 5);
            else if (tok.rfind(L"down:", 0) == 0) r.down = _wtoi(tok.c_str() + 5);
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
    WritePrivateProfileStringW(L"Settings", L"ButtonSize", std::to_wstring(cfg.size).c_str(), ini);
    WritePrivateProfileStringW(L"Settings", L"RunAsAdmin", cfg.runAsAdmin ? L"1" : L"0", ini);
    WritePrivateProfileStringW(L"Settings", L"Peek", cfg.peek ? L"1" : L"0", ini);
    WritePrivateProfileStringW(L"Settings", L"PinHotkey", HotkeyText(cfg.pinKey).c_str(), ini);
    WritePrivateProfileStringW(L"Settings", L"RollHotkey", HotkeyText(cfg.rollKey).c_str(), ini);

    // Written key by key so that comment lines in the section survive.
    // First drop the programs that are no longer listed.
    std::vector<wchar_t> old(32768);
    DWORD n = GetPrivateProfileSectionW(L"Exceptions", old.data(), (DWORD)old.size(), ini);
    std::vector<std::wstring> gone;
    for (const wchar_t* p = old.data(); n && *p; p += wcslen(p) + 1) {
        std::wstring line = p;
        size_t eq = line.find(L'=');
        if (eq == std::wstring::npos || line[0] == L';') continue;
        std::wstring key = Trim(line.substr(0, eq));
        if (!cfg.Find(Lower(key))) gone.push_back(key);
    }
    for (const auto& k : gone) WritePrivateProfileStringW(L"Exceptions", k.c_str(), nullptr, ini);
    for (const auto& r : cfg.exceptions) {
        std::wstring v;
        auto add = [&v](const std::wstring& t) { v += (v.empty() ? L"" : L",") + t; };
        if (r.ignore) add(L"ignore");
        if (r.noPin) add(L"nopin");
        if (r.noRoll) add(L"noroll");
        if (r.left) add(L"left");
        if (r.offset) add(L"offset:" + std::to_wstring(r.offset));
        if (r.rollHeight) add(L"rollheight:" + std::to_wstring(r.rollHeight));
        if (r.size) add(L"size:" + std::to_wstring(r.size));
        if (r.down) add(L"down:" + std::to_wstring(r.down));
        if (r.peek == 1) add(L"peek");
        if (r.peek == 0) add(L"nopeek");
        WritePrivateProfileStringW(L"Exceptions", r.exe.c_str(), v.c_str(), ini);
    }
}

static const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t* kRunName = L"Pin&Roll";

static const wchar_t* kTaskName = L"PinRoll";

bool IsElevated()
{
    HANDLE tok;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return false;
    TOKEN_ELEVATION e = {};
    DWORD cb = 0;
    bool yes = GetTokenInformation(tok, TokenElevation, &e, sizeof(e), &cb) && e.TokenIsElevated;
    CloseHandle(tok);
    return yes;
}

static bool RunKeyExists()
{
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_QUERY_VALUE, &k) != ERROR_SUCCESS) return false;
    bool on = RegQueryValueExW(k, kRunName, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
    RegCloseKey(k);
    return on;
}

static void SetRunKey(bool on)
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

// The Task Scheduler is used through its programming interface. (An earlier
// build ran schtasks.exe hidden, which security software rightly distrusts.)
static ITaskFolder* TaskRoot()
{
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    ITaskService* svc = nullptr;
    if (FAILED(CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER, IID_ITaskService, (void**)&svc)))
        return nullptr;
    VARIANT e;
    VariantInit(&e);
    ITaskFolder* root = nullptr;
    if (SUCCEEDED(svc->Connect(e, e, e, e))) {
        BSTR path = SysAllocString(L"\\");
        svc->GetFolder(path, &root);
        SysFreeString(path);
    }
    svc->Release();
    return root;
}

static bool TaskExists()
{
    ITaskFolder* root = TaskRoot();
    if (!root) return false;
    BSTR name = SysAllocString(kTaskName);
    IRegisteredTask* task = nullptr;
    bool yes = SUCCEEDED(root->GetTask(name, &task)) && task;
    if (task) task->Release();
    SysFreeString(name);
    root->Release();
    return yes;
}

static std::wstring XmlEscape(const std::wstring& in)
{
    std::wstring o;
    for (wchar_t c : in) {
        if (c == L'&') o += L"&amp;";
        else if (c == L'<') o += L"&lt;";
        else if (c == L'>') o += L"&gt;";
        else o += c;
    }
    return o;
}

// A logon task with "highest privileges" starts an elevated program without
// the Windows permission prompt. Creating it needs administrator rights.
static bool CreateLogonTask()
{
    wchar_t exe[MAX_PATH], dom[256] = L"", usr[256] = L"";
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    GetEnvironmentVariableW(L"USERDOMAIN", dom, 256);
    GetEnvironmentVariableW(L"USERNAME", usr, 256);
    std::wstring user = XmlEscape(*dom ? std::wstring(dom) + L"\\" + usr : std::wstring(usr));
    std::wstring xml =
        L"<?xml version=\"1.0\" encoding=\"UTF-16\"?>\r\n"
        L"<Task version=\"1.2\" xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\">\r\n"
        L"<Triggers><LogonTrigger><Enabled>true</Enabled><UserId>" + user + L"</UserId></LogonTrigger></Triggers>\r\n"
        L"<Principals><Principal id=\"Author\"><UserId>" + user + L"</UserId>"
        L"<LogonType>InteractiveToken</LogonType><RunLevel>HighestAvailable</RunLevel></Principal></Principals>\r\n"
        L"<Settings><MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>"
        L"<DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>"
        L"<StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>"
        L"<ExecutionTimeLimit>PT0S</ExecutionTimeLimit><Enabled>true</Enabled></Settings>\r\n"
        L"<Actions Context=\"Author\"><Exec><Command>" + XmlEscape(exe) + L"</Command></Exec></Actions>\r\n"
        L"</Task>\r\n";
    ITaskFolder* root = TaskRoot();
    if (!root) return false;
    BSTR name = SysAllocString(kTaskName), text = SysAllocString(xml.c_str());
    VARIANT e;
    VariantInit(&e);
    IRegisteredTask* task = nullptr;
    HRESULT hr = root->RegisterTask(name, text, TASK_CREATE_OR_UPDATE, e, e, TASK_LOGON_INTERACTIVE_TOKEN, e, &task);
    if (task) task->Release();
    SysFreeString(name);
    SysFreeString(text);
    root->Release();
    return SUCCEEDED(hr);
}

static void DeleteLogonTask()
{
    if (!TaskExists()) return;
    if (IsElevated()) {
        if (ITaskFolder* root = TaskRoot()) {
            BSTR name = SysAllocString(kTaskName);
            root->DeleteTask(name, 0);
            SysFreeString(name);
            root->Release();
        }
    } else {
        // Removing it needs the rights that created it: Windows will ask.
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        ShellExecuteW(nullptr, L"runas", exe, L"--autostart off", nullptr, SW_SHOWNORMAL);
    }
}

// Start-up entries made under the old name point at the old file: replace them.
void MigrateOldAutoStart(bool asAdmin)
{
    bool had = false;
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_QUERY_VALUE | KEY_SET_VALUE, &k) == ERROR_SUCCESS) {
        if (RegQueryValueExW(k, L"FBX Pin & Roll-up", nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS) {
            RegDeleteValueW(k, L"FBX Pin & Roll-up");
            had = true;
        }
        RegCloseKey(k);
    }
    if (IsElevated()) {
        if (ITaskFolder* root = TaskRoot()) {
            BSTR name = SysAllocString(L"FBX Pin and Roll-up");
            if (SUCCEEDED(root->DeleteTask(name, 0))) had = true;
            SysFreeString(name);
            root->Release();
        }
    }
    if (had) SetAutoStart(true, asAdmin);
}

void ConvertAutoStartIfNeeded()
{
    // Elevated, and start-up is still the plain entry: turn it into the
    // logon task so that logging on does not bring up the permission prompt.
    if (IsElevated() && RunKeyExists() && CreateLogonTask()) SetRunKey(false);
}

bool IsAutoStart()
{
    return RunKeyExists() || TaskExists();
}

void SetAutoStart(bool on, bool asAdmin)
{
    if (on && asAdmin && IsElevated() && CreateLogonTask()) {
        SetRunKey(false);
        return;
    }
    // Plain start-up entry. (With asAdmin but no rights yet, Pin&Roll elevates
    // itself after logon and converts this entry into the task.)
    SetRunKey(on);
    if (!on || !asAdmin) DeleteLogonTask();
}

bool SystemAppsDark()
{
    DWORD v = 1, cb = sizeof(v);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &v, &cb);
    return v == 0;
}
