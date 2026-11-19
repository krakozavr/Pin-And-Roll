cmake_minimum_required(VERSION 3.20)
project(pinroll VERSION 1.0.0 LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(pr WIN32
  src/main.cpp src/tracker.cpp src/render.cpp src/config.cpp src/settings.cpp
  src/common.h src/resource.h src/pr.rc)
target_compile_definitions(pr PRIVATE UNICODE _UNICODE WINVER=0x0A00 _WIN32_WINNT=0x0A00)
target_link_libraries(pr PRIVATE dwmapi gdiplus shell32 advapi32 user32 gdi32 comctl32 taskschd ole32 oleaut32 uuid)

add_executable(prtest tests/prtest.cpp)
target_compile_definitions(prtest PRIVATE UNICODE _UNICODE WINVER=0x0A00 _WIN32_WINNT=0x0A00)
target_link_libraries(prtest PRIVATE dwmapi gdiplus user32 gdi32)

if(MSVC)
  # The manifest is embedded through pr.rc.
  target_link_options(pr PRIVATE /MANIFEST:NO)
  target_compile_options(pr PRIVATE /W3 /utf-8)
  set_property(TARGET pr prtest PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
else()
  target_link_options(pr PRIVATE -municode -static)
  target_link_options(prtest PRIVATE -municode -static)
  target_compile_options(pr PRIVATE -Wall -Wextra)
endif()
