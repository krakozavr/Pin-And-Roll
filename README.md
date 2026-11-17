# FBX Pin & Roll-up

A Windows 10 / 11 refresh of two functions of **FileBox eXtender** (FbX) by
Hyperionics Technology LLC: the **push-pin** and **roll-up** title bar buttons.

| Button | What it does |
|---|---|
| Push-pin | Keeps the window above all other windows ("always on top"). Click again to release it. |
| Roll-up | Shrinks the window to its title bar to uncover what is beneath. Click again to roll it back down. |

Everything else FbX did (Favorites / Recent folders in file dialogs, dialog
resizing, Explorer integration) is intentionally left out.

**Status: preview.** The automated test passes on Windows Server 2025 and 2022
runners (see the Actions tab). It has not yet been tried by a person on a
Windows 11 desktop with everyday applications.

## What is different from FbX 2.x

FbX injected a DLL into every program through global hooks and decided where
the title bar is by sending hit-test messages. On current Windows that fails
for many windows (store apps, elevated programs, browsers and other programs
that draw their own title bar) and puts the buttons in the wrong place when
display scaling or invisible resize borders are involved.

This version injects nothing. One small program:

- listens to window events (`SetWinEventHook`, out of process),
- asks the desktop compositor where the minimize / maximize / close buttons
  are (`DWMWA_CAPTION_BUTTON_BOUNDS`, `DWMWA_EXTENDED_FRAME_BOUNDS`) and places
  its two buttons immediately to their left, at any DPI and on any monitor,
- keeps the buttons directly above their window in the Z order,
- works the same for 32-bit, 64-bit and ARM64 programs.

## Behaviour kept from FbX

- Button order: roll-up on the left, push-pin next to the system buttons.
- The pin always shows the real state of the window, also when another tool
  made it topmost.
- A rolled-up window keeps its position and width, can be dragged, and
  unrolls to its previous height where it currently is.
- Option *When rolled up, keep the window bar on top*.
- Switching the roll-up button off, or exiting, unrolls all windows.
- Global offset settings (*move buttons left by N pixels*, plus extra when
  maximized) and per-program exceptions (*ignore*, *no pushpin*, *no roll-up*,
  extra offset).

## Deliberate differences

- A pin set while a window is rolled up is kept after unrolling (FbX lost it).
- Resizing or maximizing a rolled-up window simply ends the rolled state
  (FbX blocked resizing by subclassing the window, which needs injection).
- If FBX is killed, rolled windows stay usable and are recognised again on the
  next start.
- No "Always on top" / "Roll up" entries in the window system menu: adding
  them requires code inside the other program. Optional hot keys replace them.
- Buttons are drawn as flat glyphs in the Windows 11 style and pick a light or
  dark colour from the title bar behind them.

## Known limits

- Programs running as administrator get no buttons unless FBX itself is
  started as administrator (Windows blocks the requests otherwise).
- Programs that draw their own title bar may have controls where the buttons
  go. Use an `offset:N` exception to move them, or `ignore`.
- Exclusive full-screen windows get no buttons.

## Use

Run `fbx.exe`. It lives in the notification area; the menu there has the three
FbX options, *Start with Windows*, and opens the settings file
(`%APPDATA%\FBX\fbx.ini`), which documents hot keys and exceptions.

`fbx.exe --exit` stops a running copy.

## Build

    cmake -B build -A x64
    cmake --build build --config Release
    build\Release\fbxtest.exe build\Release\fbx.exe

`build-mingw.sh` cross-compiles on Linux with mingw-w64.

## Credits and licence

Based on the behaviour of FileBox eXtender v2.01, (C) 1999-2011 Greg
Kochaniak, Hyperionics Technology LLC, released under the GPL. The roll-up
technique without injection follows AltSnap; window tracking follows the
approach of Microsoft PowerToys "Always On Top". No code was copied.

GNU General Public License v3 or later, see `LICENSE`.
