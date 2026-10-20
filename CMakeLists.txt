cmake_minimum_required(VERSION 3.24)

# Static CRT (/MT) so the result is a single ShiftDrop.exe with no VC++ redistributable.
# Must be set before project(); CMP0091 is NEW because of the minimum version above.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

project(ShiftDrop VERSION 0.3.0 LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(SHIFTDROP_M0 "Build the M0 placeholder: plain circle, every drop only logs + notifies" OFF)
option(SHIFTDROP_TESTS "Build the unit tests (ctest)" ON)

if(NOT MSVC)
  message(FATAL_ERROR "ShiftDrop is built with MSVC (Visual Studio 2022). Use the presets in CMakePresets.json.")
endif()

# All from vcpkg.json (manifest mode), linked statically (x64-windows-static triplet).
find_package(wil CONFIG REQUIRED)
find_package(unofficial-sqlite3 CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(WebP CONFIG REQUIRED)

set(SHIFTDROP_DEFINES
  UNICODE _UNICODE            # wide-character Win32 APIs only
  WIN32_LEAN_AND_MEAN NOMINMAX
  _WIN32_WINNT=0x0A00 WINVER=0x0A00   # Windows 10/11 API surface
)
set(SHIFTDROP_OPTIONS /W4 /permissive- /utf-8 /EHsc /Zc:__cplusplus /MP)

# Everything that does not need the gesture UI: recipes, the image pipeline, history, undo, file
# actions. The app and the tests both link it.
add_library(shiftdrop_core STATIC
  src/Action.cpp
  src/Ffmpeg.cpp
  src/FileActions.cpp
  src/FileMove.cpp
  src/Hash.cpp
  src/HistoryStore.cpp
  src/ImagePipeline.cpp
  src/JobQueue.cpp
  src/Log.cpp
  src/Media.cpp
  src/Modules.cpp
  src/OutputPath.cpp
  src/PipelineRunner.cpp
  src/Recipe.cpp
  src/RecipeStore.cpp
  src/Settings.cpp
  src/StagedOutput.cpp
  src/Startup.cpp
  src/Undo.cpp
  # Video and audio tools (M9, M10): engines and the editor's pure maths; the editor window is in the app
  src/mediaeditor/EditorApi.cpp
  src/mediaeditor/EditorLayout.cpp
  src/mediaeditor/MediaProbe.cpp
  src/mediaeditor/MediaTools.cpp
  src/mediaeditor/MediaToolsModule.cpp
  src/mediaeditor/Timeline.cpp
  src/videotools/VideoEngine.cpp
  src/videotools/VideoTools.cpp
  src/audiotools/AudioEngine.cpp
  src/audiotools/AudioTools.cpp
)
target_include_directories(shiftdrop_core PUBLIC src)
target_compile_definitions(shiftdrop_core PUBLIC ${SHIFTDROP_DEFINES})
target_compile_options(shiftdrop_core PRIVATE ${SHIFTDROP_OPTIONS})
target_link_libraries(shiftdrop_core PUBLIC
  WIL::WIL
  unofficial::sqlite3::sqlite3
  nlohmann_json::nlohmann_json
  WebP::webp
  windowscodecs bcrypt shlwapi ole32 oleaut32 shell32 user32 advapi32 propsys
)

add_executable(ShiftDrop WIN32
  src/main.cpp
  src/MouseHook.cpp
  src/Overlay.cpp
  src/Island.cpp
  src/Canvas.cpp
  src/Theme.cpp
  src/DropDescription.cpp
  src/HiddenCursor.cpp
  src/DropTarget.cpp
  src/Tray.cpp
  src/HistoryWindow.cpp
  # The video and audio tools' timeline editor (registers itself with mediaeditor/EditorApi)
  src/mediaeditor/EditorUi.cpp
  src/mediaeditor/EditorWindow.cpp
  src/mediaeditor/EditorTools.cpp
  src/mediaeditor/MediaLoader.cpp
  src/mediaeditor/Playback.cpp
  src/ShiftDrop.rc
  src/app.manifest      # CMake passes .manifest sources to the linker (/MANIFESTINPUT), which embeds it
)
target_compile_definitions(ShiftDrop PRIVATE $<$<BOOL:${SHIFTDROP_M0}>:SHIFTDROP_M0=1>)
target_compile_options(ShiftDrop PRIVATE ${SHIFTDROP_OPTIONS})
target_link_libraries(ShiftDrop PRIVATE shiftdrop_core d2d1 dwrite dwmapi shcore gdi32 comctl32)

# Video and audio go through ffmpeg.exe in "engines" next to ShiftDrop.exe. scripts/get-ffmpeg.ps1
# downloads it into build/engines; when it is there, every build copies it next to the exe so the
# build folder runs like an install. Without it the app still builds and runs, with media disabled.
set(SHIFTDROP_ENGINES_DIR "${CMAKE_SOURCE_DIR}/build/engines" CACHE PATH "Folder holding ffmpeg.exe (scripts/get-ffmpeg.ps1)")
if(EXISTS "${SHIFTDROP_ENGINES_DIR}/ffmpeg.exe")
  add_custom_command(TARGET ShiftDrop POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:ShiftDrop>/engines"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${SHIFTDROP_ENGINES_DIR}/ffmpeg.exe" "$<TARGET_FILE_DIR:ShiftDrop>/engines/ffmpeg.exe"
    COMMENT "Copying engines/ffmpeg.exe next to ShiftDrop.exe")
else()
  message(STATUS "No ffmpeg.exe in ${SHIFTDROP_ENGINES_DIR}: run scripts/get-ffmpeg.ps1 to enable video and audio")
endif()

if(SHIFTDROP_TESTS)
  enable_testing()
  find_package(GTest CONFIG REQUIRED)
  add_executable(shiftdrop_tests
    tests/TestMain.cpp
    tests/OutputPathTests.cpp
    tests/RecipeTests.cpp
    tests/HistoryTests.cpp
    tests/PipelineTests.cpp
    tests/MediaTests.cpp
    tests/VideoToolsTests.cpp
    tests/AudioToolsTests.cpp
  )
  target_compile_options(shiftdrop_tests PRIVATE ${SHIFTDROP_OPTIONS})
  target_link_libraries(shiftdrop_tests PRIVATE shiftdrop_core GTest::gtest)
  include(GoogleTest)
  # The MediaEngine tests run the real ffmpeg.exe when there is one; they are skipped otherwise.
  if(EXISTS "${SHIFTDROP_ENGINES_DIR}/ffmpeg.exe")
    file(TO_NATIVE_PATH "${SHIFTDROP_ENGINES_DIR}/ffmpeg.exe" SHIFTDROP_FFMPEG_NATIVE)
    gtest_discover_tests(shiftdrop_tests DISCOVERY_MODE PRE_TEST PROPERTIES ENVIRONMENT "SHIFTDROP_FFMPEG=${SHIFTDROP_FFMPEG_NATIVE}")
  else()
    gtest_discover_tests(shiftdrop_tests DISCOVERY_MODE PRE_TEST)
  endif()
endif()
