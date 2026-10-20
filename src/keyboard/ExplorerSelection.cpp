#include "keyboard/ExplorerSelection.h"
#include "Log.h"
#include "keyboard/WheelRequest.h"

// ole2.h first: WIN32_LEAN_AND_MEAN leaves it out of windows.h, and exdisp.h needs its COM
// definitions (`interface`, IDispatch) already in place.
#include <ole2.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <exdisp.h>
#include <shlguid.h>
#include <wil/resource.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace sd::explorer
{
    namespace
    {
        std::wstring ClassOf(HWND window)
        {
            wchar_t name[64]{};
            GetClassNameW(window, name, ARRAYSIZE(name));
            return name;
        }

        bool IsDesktop(HWND window)
        {
            const auto cls = ClassOf(window);
            return cls == L"Progman" || cls == L"WorkerW";
        }

        // The selected paths of a browser's active view.
        std::vector<std::wstring> FromBrowser(IShellBrowser* browser)
        {
            std::vector<std::wstring> paths;
            ComPtr<IShellView> view;
            ComPtr<IFolderView2> folder;
            ComPtr<IShellItemArray> items;
            if (!browser || FAILED(browser->QueryActiveShellView(&view)) || FAILED(view.As(&folder)) ||
                FAILED(folder->GetSelection(FALSE, &items)) || !items)
                return paths;
            DWORD count = 0;
            items->GetCount(&count);
            for (DWORD i = 0; i < count; ++i)
            {
                ComPtr<IShellItem> item;
                wil::unique_cotaskmem_string path;
                // Items with no file-system path (This PC, Control Panel, files inside a zip) fail here.
                if (SUCCEEDED(items->GetItemAt(i, &item)) && SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path)
                    paths.emplace_back(path.get());
            }
            return paths;
        }

        ComPtr<IShellBrowser> BrowserOf(IDispatch* window)
        {
            ComPtr<IServiceProvider> services;
            ComPtr<IShellBrowser> browser;
            if (window && SUCCEEDED(window->QueryInterface(IID_PPV_ARGS(&services))))
                services->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(&browser));
            return browser;
        }

        // Windows 11 Explorer has tabs: one top-level window, one shell window per tab. Only the
        // active tab's view window is visible.
        bool ActiveTab(IShellBrowser* browser)
        {
            ComPtr<IShellView> view;
            HWND viewWindow = nullptr;
            return browser && SUCCEEDED(browser->QueryActiveShellView(&view)) && SUCCEEDED(view->GetWindow(&viewWindow)) &&
                   IsWindowVisible(viewWindow);
        }
    }

    bool IsExplorerWindow(HWND window)
    {
        const auto cls = ClassOf(window);
        return cls == L"CabinetWClass" || cls == L"ExploreWClass" || IsDesktop(window);
    }

    std::vector<std::wstring> SelectionOf(HWND window)
    {
        ComPtr<IShellWindows> windows;
        if (FAILED(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&windows))))
            return {};
        if (IsDesktop(window))
        {
            // The desktop is a shell window too, found by its special location.
            wil::unique_variant location, root;
            location.vt = VT_I4;
            location.lVal = CSIDL_DESKTOP;
            long hwnd = 0;
            ComPtr<IDispatch> desktop;
            if (FAILED(windows->FindWindowSW(&location, &root, SWC_DESKTOP, &hwnd, SWFO_NEEDDISPATCH, &desktop)))
                return {};
            return wheelrequest::CleanSelection(FromBrowser(BrowserOf(desktop.Get()).Get()));
        }
        long count = 0;
        windows->get_Count(&count);
        std::vector<std::wstring> fallback;
        for (long i = 0; i < count; ++i)
        {
            wil::unique_variant index;
            index.vt = VT_I4;
            index.lVal = i;
            ComPtr<IDispatch> item;
            ComPtr<IWebBrowserApp> app;
            SHANDLE_PTR top = 0;
            if (FAILED(windows->Item(index, &item)) || !item || FAILED(item.As(&app)) || FAILED(app->get_HWND(&top)) ||
                reinterpret_cast<HWND>(top) != window)
                continue;
            const auto browser = BrowserOf(item.Get());
            auto paths = FromBrowser(browser.Get());
            if (ActiveTab(browser.Get()))
                return wheelrequest::CleanSelection(paths);
            if (fallback.empty())
                fallback = std::move(paths);   // no visible tab found (older Explorer): the first one
        }
        return wheelrequest::CleanSelection(fallback);
    }
}
