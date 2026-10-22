#pragma once

// The product name lives here and only here (plus the VERSIONINFO block in ShiftDrop.rc and the
// CMake project/target name), so a rename is a one-line change. Macros rather than constexpr so the
// name can be concatenated into other wide literals at compile time: L"" SD_NAME L".Overlay".
// The version is not here: it is written in VERSION at the repository root (see AppVersion.h).
#define SD_NAME L"ShiftDrop"
// Folder under %LOCALAPPDATA% for logs and settings.
#define SD_APPDATA_FOLDER SD_NAME
