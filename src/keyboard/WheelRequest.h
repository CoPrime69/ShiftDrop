#pragma once
#include <windows.h>

#include <string>
#include <string_view>
#include <vector>

// Opening the wheel without a drag: on Explorer's selection (the hotkey) or from Explorer's
// right-click menu ("ShiftDrop.exe --wheel <path>"). Explorer starts one process per selected item,
// so each forwards its path to the running copy with WM_COPYDATA and exits; the running copy waits a
// moment for the rest and opens one wheel for all of them.
namespace sd::wheelrequest
{
    inline constexpr wchar_t kWheelArg[] = L"--wheel";
    inline constexpr wchar_t kRegisterShellArg[] = L"--register-shell";
    inline constexpr wchar_t kUnregisterShellArg[] = L"--unregister-shell";
    // COPYDATASTRUCT::dwData of a forwarded request ("SDWH").
    inline constexpr ULONG_PTR kCopyDataId = 0x53445748;

    // The paths after --wheel (all remaining arguments). Empty when there is no --wheel.
    std::vector<std::wstring> WheelPaths(const std::vector<std::wstring>& args);
    // This process's own arguments, without the program name.
    std::vector<std::wstring> CommandLineArgs();
    bool HasArg(const std::vector<std::wstring>& args, std::wstring_view arg);   // case-insensitive

    // WM_COPYDATA payload: UTF-16 paths, each ending in a NUL.
    std::wstring Encode(const std::vector<std::wstring>& paths);
    std::vector<std::wstring> Decode(const wchar_t* data, size_t chars);

    // Shell items to paths: drops the ones with no file-system path (Control Panel, This PC, items
    // inside a zip) and duplicates (case-insensitive, as NTFS compares), keeping the order.
    std::vector<std::wstring> CleanSelection(const std::vector<std::wstring>& paths);
}
