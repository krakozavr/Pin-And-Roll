cmake_minimum_required(VERSION 3.20)
project(fbx VERSION 3.0.0 LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(fbx WIN32
  src/main.cpp src/tracker.cpp src/render.cpp src/config.cpp src/common.h src/fbx.rc)
target_compile_definitions(fbx PRIVATE UNICODE _UNICODE WINVER=0x0A00 _WIN32_WINNT=0x0A00)
target_link_libraries(fbx PRIVATE dwmapi gdiplus shell32 advapi32 user32 gdi32)

add_executable(fbxtest tests/fbxtest.cpp)
target_compile_definitions(fbxtest PRIVATE UNICODE _UNICODE WINVER=0x0A00 _WIN32_WINNT=0x0A00)
target_link_libraries(fbxtest PRIVATE dwmapi gdiplus user32 gdi32)

if(MSVC)
  # The manifest is embedded through fbx.rc.
  target_link_options(fbx PRIVATE /MANIFEST:NO)
  target_compile_options(fbx PRIVATE /W3 /utf-8)
  set_property(TARGET fbx fbxtest PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
else()
  target_link_options(fbx PRIVATE -municode -static)
  target_link_options(fbxtest PRIVATE -municode -static)
  target_compile_options(fbx PRIVATE -Wall -Wextra)
endif()
