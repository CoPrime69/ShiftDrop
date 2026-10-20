cmake_minimum_required(VERSION 3.24)

# Static CRT (/MT) so the result is a single ShiftDrop.exe with no VC++ redistributable.
# Must be set before project(); CMP0091 is NEW because of the minimum version above.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

project(ShiftDrop VERSION 0.1.0 LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(SHIFTDROP_M0 "Build the M0 placeholder: plain circle, every drop only logs + notifies" OFF)

if(NOT MSVC)
  message(FATAL_ERROR "ShiftDrop is built with MSVC (Visual Studio 2022). Use the presets in CMakePresets.json.")
endif()

# wil is header-only; vcpkg installs it from vcpkg.json (manifest mode).
find_package(wil CONFIG REQUIRED)

add_executable(ShiftDrop WIN32
  src/main.cpp
  src/Log.cpp
  src/Settings.cpp
  src/MouseHook.cpp
  src/Overlay.cpp
  src/DropTarget.cpp
  src/Tray.cpp
  src/JobQueue.cpp
  src/WicConverter.cpp
  src/OutputPath.cpp
  src/ShiftDrop.rc
  src/app.manifest      # CMake passes .manifest sources to the linker (/MANIFESTINPUT), which embeds it
)

target_compile_definitions(ShiftDrop PRIVATE
  UNICODE _UNICODE            # wide-character Win32 APIs only
  WIN32_LEAN_AND_MEAN NOMINMAX
  _WIN32_WINNT=0x0A00 WINVER=0x0A00   # Windows 10/11 API surface
  $<$<BOOL:${SHIFTDROP_M0}>:SHIFTDROP_M0=1>
)

target_compile_options(ShiftDrop PRIVATE /W4 /permissive- /utf-8 /EHsc /Zc:__cplusplus /MP)

target_link_libraries(ShiftDrop PRIVATE
  WIL::WIL
  d2d1 dwrite windowscodecs dwmapi shcore shlwapi
  ole32 oleaut32 shell32 user32 gdi32 advapi32 propsys
)
