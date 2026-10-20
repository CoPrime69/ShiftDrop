#pragma once
#include "Engine.h"

#include <windows.h>

#include <memory>
#include <string>
#include <vector>

// The Share file action: Windows' share sheet (Nearby sharing, Phone Link, mail and any app that
// accepts files), the Windows counterpart of AirDrop. The WinRT part is WinShare.cpp (MSVC only),
// which installs itself here at startup.
namespace sd::share
{
    struct Hooks
    {
        // Opens the share sheet for `items` (files or folders). UI thread only; returns at once, the
        // sheet runs on its own. False with `error` when it could not open.
        bool (*show)(HWND owner, const std::vector<std::wstring>& items, std::wstring& error) = nullptr;
    };
    Hooks& Platform();

    std::unique_ptr<IEngine> MakeShareEngine(const EngineServices& services);
}
