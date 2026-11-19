// Pin&Roll - a Windows 10/11 refresh of the push-pin and roll-up
// title bar buttons of FileBox eXtender (Hyperionics Technology LLC, GPL).
// Copyright (C) 2026 Pin&Roll contributors. License: GPL-3.0-or-later.
#pragma once

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef WINVER
#define WINVER 0x0A00
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif

#include <windows.h>
#include <string>
#include <vector>

#define PR_MAIN_CLASS L"PinRollMain"
#define PR_BAR_CLASS L"PinRollBar"
// Window properties. The first one is set on our bar windows, the other
// three on a rolled-up target so that the state survives a crash of Pin&Roll.
#define PR_PROP_TARGET L"PinRollTarget"
#define PR_PROP_OLDHEIGHT L"PinRollOldHeight"
#define PR_PROP_ROLLEDHEIGHT L"PinRollRolledHeight"
#define PR_PROP_FORCEDTOP L"PinRollForcedTopmost"

enum ButtonType { BTN_NONE = 0, BTN_ROLL = 1, BTN_PIN = 2 };

struct ExceptionRule {
    std::wstring exe;   // lower-case file name, e.g. "chrome.exe"
    bool ignore = false;
    bool noPin = false;
    bool noRoll = false;
    bool left = false;  // put the buttons at the left end of the title bar
    int offset = 0;     // move buttons left by N pixels (at 100% scale); negative = right
    int rollHeight = 0; // rolled-up height override (at 100% scale), 0 = auto
    int size = 0;       // button size in percent, 0 = use the global size
    int down = 0;       // move buttons down by N pixels (at 100% scale); negative = up
    int peek = -1;      // show the rolled window while hovered: 1 yes, 0 no, -1 as set globally
};

struct Hotkey {
    UINT mod = 0;
    UINT vk = 0;
};

struct Config {
    std::wstring path;
    bool pin = true;
    bool roll = true;
    bool rolledTopmost = false;
    int moveLeft = 0;
    int moveLeftMaximized = 0;
    int size = 100;          // button size in percent
    bool runAsAdmin = false; // restart elevated so that elevated programs get buttons too
    bool peek = false;       // show a rolled-up window in full while the mouse is over it
    bool autoUpdate = false; // look for a new version once a week
    long long lastUpdateCheck = 0; // seconds since 1970
    Hotkey pinKey, rollKey;
    std::vector<ExceptionRule> exceptions;

    const ExceptionRule* Find(const std::wstring& exeLower) const;
};

// config.cpp
std::wstring DefaultConfigPath();
void LoadConfig(Config& cfg);
void SaveToggles(const Config& cfg);
void SaveConfig(const Config& cfg); // everything the settings window edits
bool IsAutoStart();
// asAdmin: start elevated at logon without a prompt (uses a scheduled task).
void SetAutoStart(bool on, bool asAdmin);
void ConvertAutoStartIfNeeded();
void MigrateOldAutoStart(bool asAdmin);
bool IsElevated();
std::wstring HotkeyText(const Hotkey& hk);
bool SystemAppsDark();

// render.cpp
struct ButtonDraw {
    ButtonType type;
    bool active; // pinned / rolled
    bool hot;
    bool pressed;
};
void RenderInit();
void RenderShutdown();
void RenderBar(HWND bar, const RECT& rc, const ButtonDraw* btns, int count, UINT dpi, bool darkBackground,
               float scale);
HICON CreateAppIcon(int size);

// tracker.cpp
extern Config g_cfg;
bool TrackerInit(HINSTANCE inst, HWND mainWnd);
void TrackerShutdown();
void TrackerOnTimer(UINT_PTR id);
void TrackerRefreshAll();
void TrackerUnrollAll();
void TrackerToggleForeground(ButtonType which);
// Programs that currently have windows with buttons: (exe name, a window title).
void TrackerListApps(std::vector<std::pair<std::wstring, std::wstring>>& out);

// main.cpp
void AppRegisterHotkeys();
bool AppRestartElevated(); // false if the user declined the Windows prompt
void AppOpenSettings(const std::wstring& exe); // settings window with this program selected
void AppShowHelp(); // opens the built-in help page in the browser
// Opens a web address or file with the user's own (not elevated) default program.
void AppOpen(const std::wstring& target);

// Text report of how each window's title bar was measured (no window titles).
void TrackerDiagnostics(std::wstring& out);

// settings.cpp
void ShowSettings(HINSTANCE inst, const std::wstring& selectExe);
bool SettingsIsOpen();
#define PR_TIMER_SLOW 1
#define PR_TIMER_BURST 2
