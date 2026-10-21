#pragma once
#include "Log.h"
#include "Utf.h"

#include <windows.h>
#include <objbase.h>

#include <exception>
#include <memory>
#include <thread>
#include <utility>

// Handing work from a window to a worker thread and the result back. The worker posts an owned
// pointer in lParam; the window takes it back with std::unique_ptr. UI code only.
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
}
