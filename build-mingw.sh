#!/bin/sh
# Cross-compile on Linux with mingw-w64 (the official build uses CMake + MSVC).
set -e
cd "$(dirname "$0")"
mkdir -p build-mingw
D="-DUNICODE -D_UNICODE -DWINVER=0x0A00 -D_WIN32_WINNT=0x0A00"
x86_64-w64-mingw32-windres -I src src/pr.rc -O coff -o build-mingw/pr.res
x86_64-w64-mingw32-g++ -std=c++17 -O2 -Wall -Wextra $D -municode -mwindows -static \
  src/main.cpp src/tracker.cpp src/render.cpp src/config.cpp src/settings.cpp build-mingw/pr.res \
  -o build-mingw/pr.exe -ldwmapi -lgdiplus -lshell32 -ladvapi32 -lole32 -lgdi32 -lcomctl32 -ltaskschd -loleaut32 -luuid -lwinhttp
x86_64-w64-mingw32-g++ -std=c++17 -O2 -Wall $D -municode -mconsole -static \
  tests/prtest.cpp -o build-mingw/prtest.exe -ldwmapi -lgdiplus -lole32 -lgdi32
ls -l build-mingw/*.exe
