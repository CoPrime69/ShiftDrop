#pragma once
#include "Log.h"
#include "Utf.h"

#include <windows.h>
#include <objbase.h>

#include <exception>
#include <memory>
#include <thread>
#include <utility>

// Helpers for window code: handing work to a worker thread and the result back (the worker posts an
// owned pointer in lParam; the window takes it back with std::unique_ptr), and bringing a window to
// the front. UI code only.
namespace sd
{
    // Posts `payload` to `hwnd`; if the window is gone the post fails and the payload is freed here.
    template <class T>
    bool PostOwned(HWND hwnd, UINT msg, std::unique_ptr<T> payload, WPARAM wParam = 0)
    {
        auto* raw = payload.release();
        if (PostMessageW(hwnd, msg, wParam, reinterpret_cast<LPARAM>(raw)))
            return true;
        std::unique_ptr<T> reclaim(raw);
        return false;
    }

    // Runs `fn` on a new detached thread with COM in the multithreaded apartment (WinRT and WIC calls
    // block, so never on the UI thread). An exception is logged, not thrown.
    template <class Fn>
    void RunMtaDetached(Fn fn)
    {
        std::thread([fn = std::move(fn)]() mutable {
            const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            try
            {
                fn();
            }
            catch (const std::exception& e)
            {
                log::Warn(L"worker: {}", Widen(e.what()));
            }
            catch (...)
            {
                log::Warn(L"worker: unknown error");
            }
            if (SUCCEEDED(com))
                CoUninitialize();
        }).detach();
    }

    // Brings `hwnd` to the front from a drop. A process that did not receive the last input event
    // cannot take the foreground, and a drop comes through Explorer's drag loop, not our input queue:
    // attaching to the foreground thread's input for a moment lets the window (or a dialog it owns)
    // open in front instead of flashing in the taskbar.
    inline void BringToFront(HWND hwnd)
    {
        const HWND fg = GetForegroundWindow();
        const DWORD fgThread = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
        const DWORD self = GetCurrentThreadId();
        const bool attach = fgThread && fgThread != self && AttachThreadInput(self, fgThread, TRUE);
        SetForegroundWindow(hwnd);
        BringWindowToTop(hwnd);
        if (attach)
            AttachThreadInput(self, fgThread, FALSE);
    }
}
