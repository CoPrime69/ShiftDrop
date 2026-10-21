cmake_minimum_required(VERSION 3.24)

# Static CRT (/MT) so the result is a single ShiftDrop.exe with no VC++ redistributable.
# Must be set before project(); CMP0091 is NEW because of the minimum version above.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

project(ShiftDrop VERSION 0.5.0 LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(SHIFTDROP_M0 "Build the placeholder gesture build: plain circle, every drop only logs + notifies" OFF)
option(SHIFTDROP_TESTS "Build the unit tests (ctest)" ON)

if(NOT MSVC)
  message(FATAL_ERROR "ShiftDrop is built with MSVC (Visual Studio 2022). Use the presets in CMakePresets.json.")
endif()

# All from vcpkg.json (manifest mode), linked statically (x64-windows-static triplet).
find_package(wil CONFIG REQUIRED)
find_package(unofficial-sqlite3 CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(WebP CONFIG REQUIRED)
find_package(qpdf CONFIG REQUIRED)   # PDF structure: merge, organize, compress, text layer (Apache-2.0)
find_package(ZLIB REQUIRED)          # Flate streams of the PDFs we write
find_package(libavif CONFIG REQUIRED)   # AVIF: dav1d decodes, aom encodes (BSD-2-Clause)
find_package(lunasvg CONFIG REQUIRED)   # SVG input (MIT)
find_package(miniz CONFIG REQUIRED)    # ZIP containers: DOCX, XLSX, ODS, EPUB (MIT)
find_package(pugixml CONFIG REQUIRED)  # XML of those containers (MIT)
find_package(md4c CONFIG REQUIRED)     # Markdown parsing, CommonMark + GitHub tables (MIT)
find_package(LibArchive REQUIRED)    # ZIP, TAR, 7z, RAR (BSD-2); vcpkg's wrapper adds its static codecs
# QPDF's exported link interface names its static dependencies by bare name (zlib's "zs"), so the
# linker must be able to find them in vcpkg's lib folder.
if(DEFINED VCPKG_INSTALLED_DIR AND DEFINED VCPKG_TARGET_TRIPLET)
  link_directories("${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}$<$<CONFIG:Debug>:/debug>/lib")
endif()
get_target_property(SHIFTDROP_QPDF_LINKS qpdf::libqpdf INTERFACE_LINK_LIBRARIES)
message(STATUS "qpdf links: ${SHIFTDROP_QPDF_LINKS}")

set(SHIFTDROP_DEFINES
  UNICODE _UNICODE            # wide-character Win32 APIs only
  WIN32_LEAN_AND_MEAN NOMINMAX
  _WIN32_WINNT=0x0A00 WINVER=0x0A00   # Windows 10/11 API surface
)
set(SHIFTDROP_OPTIONS /W4 /permissive- /utf-8 /EHsc /Zc:__cplusplus /MP)
# Smaller Release exes: each global in its own section (/Gw) so the linker can drop unreferenced
# functions and data (/OPT:REF) and fold identical ones (/OPT:ICF). No debug info is written.
list(APPEND SHIFTDROP_OPTIONS $<$<CONFIG:Release>:/Gw>)
add_link_options($<$<CONFIG:Release>:/OPT:REF> $<$<CONFIG:Release>:/OPT:ICF>)

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
  src/Features.cpp
  src/StagedOutput.cpp
  src/Startup.cpp
  src/Undo.cpp
  src/VirtualDrop.cpp
  src/rename/RenamePattern.cpp   # File actions › Rename…: names from a pattern
  src/toast/ToastXml.cpp         # what a finished batch's toast says
  # Keyboard picking and the wheel without a drag, predicted output, metadata inspector
  src/keyboard/Hotkey.cpp
  src/keyboard/KeyNav.cpp
  src/keyboard/ShellVerb.cpp
  src/keyboard/WheelLayout.cpp
  src/keyboard/WheelRequest.cpp
  src/preview/Prediction.cpp
  src/preview/Predictor.cpp
  src/inspect/InspectModule.cpp
  src/inspect/Metadata.cpp
  src/inspect/MetadataReader.cpp
  src/inspect/ProbeReport.cpp
  src/shelf/Carry.cpp
  src/shelf/ShelfGeometry.cpp
  src/shelf/ShelfModule.cpp
  src/shelf/ShelfStore.cpp
  src/canvas/Covers.cpp
  src/canvas/Editor.cpp
  src/canvas/Geometry.cpp
  src/canvas/Raster.cpp
  src/canvas/RegionTool.cpp
  src/canvas/Regions.cpp
  src/crop/CropEngine.cpp
  src/crop/CropMath.cpp
  src/crop/CropTool.cpp
  src/imagetools/ImageToolsModule.cpp
  src/metadata/Container.cpp
  src/metadata/Exif.cpp
  src/metadata/MediaMetadata.cpp
  src/metadata/Metadata.cpp
  src/redact/Pii.cpp
  src/redact/RedactEngine.cpp
  src/redact/RedactTool.cpp
  src/redact/Vision.cpp
  src/sharesafe/ShareSafeEngine.cpp
  # PDF module: plain C++ and QPDF; the WinRT and UI parts are in the libraries below
  src/pdf/ImagePdf.cpp
  src/pdf/PageRenderer.cpp
  src/pdf/PagePlan.cpp
  src/pdf/PdfCompress.cpp
  src/pdf/PdfContent.cpp
  src/pdf/PdfDoc.cpp
  src/pdf/PdfImages.cpp
  src/pdf/PdfEngine.cpp
  src/pdf/PdfModule.cpp
  src/pdf/PdfText.cpp
  src/pdf/PdfWriter.cpp
  src/pdf/TextLayout.cpp
  src/pdf/Wic.cpp
  src/ocr/Ocr.cpp
  src/ocr/OcrEngine.cpp
  src/ocr/TextLayer.cpp
  src/share/ShareEngine.cpp
  # Documents, spreadsheets, subtitles and the Office bridge
  src/docs/Bytes.cpp
  src/docs/DocModel.cpp
  src/docs/DocsModule.cpp
  src/docs/Docx.cpp
  src/docs/Epub.cpp
  src/docs/Html.cpp
  src/docs/Markdown.cpp
  src/docs/PageLayout.cpp
  src/docs/Pages.cpp
  src/docs/Paper.cpp
  src/docs/PdfPrint.cpp
  src/docs/PdfSource.cpp
  src/docs/PlainText.cpp
  src/docs/Rtf.cpp
  src/docs/Zip.cpp
  src/sheets/Ods.cpp
  src/sheets/SheetsModule.cpp
  src/sheets/Workbook.cpp
  src/sheets/Xls.cpp
  src/sheets/Xlsx.cpp
  src/subtitles/Subtitles.cpp
  src/subtitles/SubtitlesModule.cpp
  src/officebridge/OfficeBridge.cpp
  src/officebridge/OfficeCom.cpp
  src/officebridge/OfficeModule.cpp
  # Video and audio tools: engines and the editor's pure maths; the editor window is in the app
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
  src/archive/ArchiveFormats.cpp
  src/archive/ArchiveModule.cpp
  src/archive/ArchiveReader.cpp
  src/archive/ArchiveSource.cpp
  src/archive/ArchiveWriter.cpp
  src/archive/Entries.cpp
  src/archive/EntryPipe.cpp
  src/archive/Sinks.cpp
  src/sandbox/MediaSandbox.cpp
  src/sandbox/Sandbox.cpp
  src/sandbox/Worker.cpp
  # Image formats beyond WIC (AVIF, SVG, HEIC through Windows' codec), plugged into ImagePipeline,
  # and every frame of an animated GIF or multi-page TIFF
  src/formats/Avif.cpp
  src/formats/Formats.cpp
  src/formats/Frames.cpp
  src/formats/Heic.cpp
  src/formats/Pixels.cpp
  src/formats/Svg.cpp
  # Photo tools: Edit Photos, Annotate, Background, Collage; Direct2D on the GPU (WARP without one)
  src/photo/Adjust.cpp
  src/photo/AnnotateTool.cpp
  src/photo/Annotations.cpp
  src/photo/BackgroundTool.cpp
  src/photo/CollageTool.cpp
  src/photo/Compose.cpp
  src/photo/EditTool.cpp
  src/photo/Effects.cpp
  src/photo/Filters.cpp
  src/photo/Gpu.cpp
  src/photo/Layout.cpp
  src/photo/PhotoEngine.cpp
  src/photo/PhotoModule.cpp
)
target_include_directories(shiftdrop_core PUBLIC src)
target_compile_definitions(shiftdrop_core PUBLIC ${SHIFTDROP_DEFINES})
target_compile_options(shiftdrop_core PRIVATE ${SHIFTDROP_OPTIONS})
target_link_libraries(shiftdrop_core PUBLIC
  WIL::WIL
  unofficial::sqlite3::sqlite3
  nlohmann_json::nlohmann_json
  WebP::webp
  WebP::libwebpmux   # animated WebP (a GIF converted to WebP stays animated)
  qpdf::libqpdf
  ZLIB::ZLIB
  avif
  lunasvg::lunasvg
  miniz::miniz
  pugixml::pugixml
  md4c::md4c
  LibArchive::LibArchive
  windowscodecs bcrypt shlwapi ole32 oleaut32 shell32 user32 advapi32 propsys
  dwrite gdi32 winspool   # documents to PDF: DirectWrite layout printed to Microsoft Print to PDF
  d2d1 d3d11 dxgi dxguid  # photo tools: Direct2D on the GPU (WARP without one); dxguid has the effect CLSIDs
)

# Windows Runtime parts of the PDF module: Windows.Data.Pdf rendering, Windows.Media.Ocr, the
# share sheet and toasts, through C++/WinRT (Windows SDK headers). Object libraries, so each file's static
# initialiser that installs it into the core's hooks is always linked in.
add_library(shiftdrop_winrt OBJECT
  src/pdf/WinPdfRenderer.cpp
  src/ocr/WinOcr.cpp
  src/share/WinShare.cpp
  src/redact/WinFaces.cpp
  src/toast/WinToasts.cpp
)
target_compile_options(shiftdrop_winrt PRIVATE ${SHIFTDROP_OPTIONS} /bigobj)
target_link_libraries(shiftdrop_winrt PUBLIC shiftdrop_core windowsapp)

# The PDF editor windows (Merge, Organize): app-only UI, drawn with Direct2D like the wheel.
add_library(shiftdrop_pdf_ui OBJECT
  src/pdf/EditorWindow.cpp
  src/pdf/MergeWindow.cpp
  src/pdf/OrganizeWindow.cpp
)
target_compile_options(shiftdrop_pdf_ui PRIVATE ${SHIFTDROP_OPTIONS})
target_link_libraries(shiftdrop_pdf_ui PUBLIC shiftdrop_core d2d1 dwrite dwmapi)

add_executable(ShiftDrop WIN32
  src/main.cpp
  src/MouseHook.cpp
  src/Overlay.cpp
  src/Island.cpp
  src/Accessibility.cpp   # screen readers: the wheel and the top panel through UI Automation
  src/Canvas.cpp
  src/Theme.cpp
  src/DropDescription.cpp
  src/HiddenCursor.cpp
  src/DropTarget.cpp
  src/Tray.cpp
  src/HistoryWindow.cpp
  src/SettingsWindow.cpp
  src/rename/RenameWindow.cpp
  src/keyboard/ExplorerSelection.cpp
  src/keyboard/KeyboardController.cpp
  src/preview/PreviewCard.cpp
  src/inspect/InspectorWindow.cpp
  src/shelf/ShelfWindow.cpp
  # The video and audio tools' timeline editor (registers itself with mediaeditor/EditorApi)
  src/mediaeditor/EditorUi.cpp
  src/mediaeditor/EditorWindow.cpp
  src/mediaeditor/EditorTools.cpp
  src/mediaeditor/MediaLoader.cpp
  src/mediaeditor/Playback.cpp
  src/canvas/EditorWindow.cpp
  src/ShiftDrop.rc
  src/app.manifest      # CMake passes .manifest sources to the linker (/MANIFESTINPUT), which embeds it
)
target_compile_definitions(ShiftDrop PRIVATE $<$<BOOL:${SHIFTDROP_M0}>:SHIFTDROP_M0=1>)
target_compile_options(ShiftDrop PRIVATE ${SHIFTDROP_OPTIONS})
target_link_libraries(ShiftDrop PRIVATE shiftdrop_core shiftdrop_winrt shiftdrop_pdf_ui d2d1 dwrite dwmapi shcore gdi32 comctl32 uxtheme uiautomationcore)

# The sealed engine host: parses archives inside an AppContainer with no capabilities. A
# separate small exe (no UI, COM or shell code) so the sandboxed image holds only the parsers. It
# lives in "engines" next to ffmpeg.exe; every build copies it there so the build folder runs like
# an install.
add_executable(ShiftDropWorker WIN32 src/sandbox/WorkerExe.cpp)
target_compile_options(ShiftDropWorker PRIVATE ${SHIFTDROP_OPTIONS})
target_link_libraries(ShiftDropWorker PRIVATE shiftdrop_core)
add_dependencies(ShiftDrop ShiftDropWorker)
add_custom_command(TARGET ShiftDropWorker POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:ShiftDropWorker>/engines"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:ShiftDropWorker>" "$<TARGET_FILE_DIR:ShiftDropWorker>/engines/ShiftDropWorker.exe"
  COMMENT "Copying ShiftDropWorker.exe into engines")

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
    tests/KeyboardTests.cpp
    tests/PreviewTests.cpp
    tests/InspectTests.cpp
    tests/ShelfTests.cpp
    tests/PdfTests.cpp
    tests/OcrTests.cpp
    tests/DocsTests.cpp
    tests/SheetsTests.cpp
    tests/SubtitlesTests.cpp
    tests/OfficeTests.cpp
    tests/VideoToolsTests.cpp
    tests/AudioToolsTests.cpp
    tests/CanvasTests.cpp
    tests/CropTests.cpp
    tests/RedactTests.cpp
    tests/ShareSafeTests.cpp
    tests/ArchiveTests.cpp
    tests/SandboxTests.cpp
    tests/VirtualDropTests.cpp
    tests/FormatsTests.cpp
    tests/FramesTests.cpp
    tests/PhotoTests.cpp
    tests/BenchTests.cpp
    tests/FileActionsTests.cpp
    tests/RenameTests.cpp
    tests/ToastTests.cpp
    tests/FfmpegComponentsTests.cpp
  )
  target_compile_options(shiftdrop_tests PRIVATE ${SHIFTDROP_OPTIONS})
  # What the bundled ffmpeg.exe is built with; FfmpegComponentsTests checks it covers every argument builder.
  target_compile_definitions(shiftdrop_tests PRIVATE SHIFTDROP_FFMPEG_COMPONENTS="${CMAKE_SOURCE_DIR}/scripts/ffmpeg-components.txt")
  # ws2_32: only the sandbox test's probe (a socket that must fail inside the AppContainer).
  target_link_libraries(shiftdrop_tests PRIVATE shiftdrop_core shiftdrop_winrt GTest::gtest ws2_32)
  include(GoogleTest)
  # The MediaEngine tests run the real ffmpeg.exe when there is one; they are skipped otherwise.
  if(EXISTS "${SHIFTDROP_ENGINES_DIR}/ffmpeg.exe")
    file(TO_NATIVE_PATH "${SHIFTDROP_ENGINES_DIR}/ffmpeg.exe" SHIFTDROP_FFMPEG_NATIVE)
    gtest_discover_tests(shiftdrop_tests DISCOVERY_MODE PRE_TEST PROPERTIES ENVIRONMENT "SHIFTDROP_FFMPEG=${SHIFTDROP_FFMPEG_NATIVE}")
  else()
    gtest_discover_tests(shiftdrop_tests DISCOVERY_MODE PRE_TEST)
  endif()
endif()
