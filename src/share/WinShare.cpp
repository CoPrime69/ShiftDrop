// The Windows share sheet for dropped files, through IDataTransferManagerInterop (the desktop-app
// entry point to Windows.ApplicationModel.DataTransfer). Installs itself into share::Platform()
// before main(). Everything here runs on the UI thread.
#include "share/Share.h"
#include "Brand.h"
#include "Log.h"

#include <shobjidl_core.h>
#include <unknwn.h>
#include <winrt/base.h>
#include <winrt/Windows.ApplicationModel.DataTransfer.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Storage.h>

#include <filesystem>
#include <format>

namespace sd::share
{
    namespace
    {
        namespace dt = winrt::Windows::ApplicationModel::DataTransfer;
        namespace storage = winrt::Windows::Storage;

        constexpr wchar_t kOwnerClass[] = SD_NAME L".ShareOwner";

        // The sheet hangs off a window of ours. The wheel's own window comes and goes, so the sheet
        // gets a dedicated one: a 1 x 1 fully transparent, click-through tool window at the cursor
        // (the sheet opens near it), alive for the rest of the session.
        struct State
        {
            HWND window = nullptr;
            dt::DataTransferManager manager{ nullptr };
            winrt::event_token requested{};
            std::vector<std::wstring> items;
        };

        State& Current()
        {
            static State state;
            return state;
        }

        std::wstring Message(const winrt::hresult_error& e)
        {
            std::wstring text(e.message());
            while (!text.empty() && (text.back() == L'\n' || text.back() == L'\r' || text.back() == L' '))
                text.pop_back();
            return text.empty() ? std::format(L"the share sheet could not open (0x{:08X})", static_cast<uint32_t>(e.code().value)) : text;
        }

        void HideOwner()
        {
            if (Current().window)
                ShowWindow(Current().window, SW_HIDE);
        }

        HWND OwnerWindow()
        {
            auto& s = Current();
            if (!s.window)
            {
                WNDCLASSEXW wc{ sizeof(wc) };
                wc.lpfnWndProc = DefWindowProcW;
                wc.hInstance = GetModuleHandleW(nullptr);
                wc.lpszClassName = kOwnerClass;
                RegisterClassExW(&wc);   // fails harmlessly if already registered
                s.window = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST, kOwnerClass, SD_NAME,
                                           WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, wc.hInstance, nullptr);
                if (s.window)
                    SetLayeredWindowAttributes(s.window, 0, 0, LWA_ALPHA);
            }
            return s.window;
        }

        std::wstring TitleFor(const std::vector<std::wstring>& items)
        {
            if (items.size() == 1)
                return std::filesystem::path(items.front()).filename().wstring();
            return std::format(L"{} items", items.size());
        }

        // Fills the request with the files once Windows asks (it asks right after the sheet opens).
        // Resolving paths to StorageFile objects is asynchronous; the deferral holds the sheet.
        winrt::fire_and_forget Provide(dt::DataRequest request, std::vector<std::wstring> items)
        {
            auto deferral = request.GetDeferral();
            std::vector<storage::IStorageItem> resolved;
            std::wstring problem;
            try
            {
                for (const auto& path : items)
                {
                    const DWORD attributes = GetFileAttributesW(path.c_str());
                    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY))
                        resolved.push_back(co_await storage::StorageFolder::GetFolderFromPathAsync(path));
                    else
                        resolved.push_back(co_await storage::StorageFile::GetFileFromPathAsync(path));
                }
            }
            catch (const winrt::hresult_error& e)
            {
                problem = Message(e);
            }
            if (resolved.empty())
            {
                log::Warn(L"share: {}", problem);
                request.FailWithDisplayText(problem.empty() ? L"Nothing to share" : problem);
                deferral.Complete();
                co_return;
            }
            if (!problem.empty())
                log::Warn(L"share: some items left out: {}", problem);
            auto data = request.Data();
            data.Properties().Title(TitleFor(items));
            data.Properties().Description(SD_NAME);
            data.SetStorageItems(winrt::single_threaded_vector(std::move(resolved)), true);   // read-only
            try
            {
                // Hide the helper window again when the sheet is done (Windows 10 2004 and later).
                data.ShareCompleted([](const dt::DataPackage&, const dt::ShareCompletedEventArgs&) { HideOwner(); });
                data.ShareCanceled([](const dt::DataPackage&, const winrt::Windows::Foundation::IInspectable&) { HideOwner(); });
            }
            catch (const winrt::hresult_error&)
            {
            }
            deferral.Complete();
        }

        bool Show(HWND, const std::vector<std::wstring>& items, std::wstring& error)
        {
            if (items.empty())
            {
                error = L"nothing to share";
                return false;
            }
            try
            {
                const HWND window = OwnerWindow();
                if (!window)
                {
                    error = L"the share sheet could not open (no window)";
                    return false;
                }
                POINT at{};
                GetCursorPos(&at);
                SetWindowPos(window, HWND_TOPMOST, at.x, at.y, 1, 1, SWP_NOACTIVATE | SWP_SHOWWINDOW);
                SetForegroundWindow(window);

                auto& s = Current();
                const auto interop = winrt::get_activation_factory<dt::DataTransferManager, IDataTransferManagerInterop>();
                if (!s.manager)
                {
                    const winrt::guid iid = winrt::guid_of<dt::DataTransferManager>();
                    winrt::check_hresult(interop->GetForWindow(window, reinterpret_cast<const GUID&>(iid), winrt::put_abi(s.manager)));
                    s.requested = s.manager.DataRequested([](const dt::DataTransferManager&, const dt::DataRequestedEventArgs& args) {
                        Provide(args.Request(), Current().items);
                    });
                }
                s.items = items;
                winrt::check_hresult(interop->ShowShareUIForWindow(window));
                return true;
            }
            catch (const winrt::hresult_error& e)
            {
                HideOwner();
                error = Message(e);
                return false;
            }
        }

        const bool g_installed = [] {
            Platform().show = &Show;
            return true;
        }();
    }
}
