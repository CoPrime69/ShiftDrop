// Toasts through Windows.UI.Notifications and the desktop-app COM activator. See Toasts.h.
#include "toast/Toasts.h"
#include "Brand.h"
#include "ErrorText.h"
#include "Log.h"
#include "PathUtil.h"
#include "Strings.h"

#include <windows.h>
#include <NotificationActivationCallback.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <unknwn.h>
#include <wil/resource.h>
#include <wrl/client.h>
#include <wrl/implements.h>
#include <winrt/base.h>
#include <winrt/Windows.Data.Xml.Dom.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.UI.Notifications.h>

#include <format>

using Microsoft::WRL::ClassicCom;
using Microsoft::WRL::ComPtr;
using Microsoft::WRL::RuntimeClass;
using Microsoft::WRL::RuntimeClassFlags;

namespace sd::toast
{
    namespace
    {
        namespace notifications = winrt::Windows::UI::Notifications;

        // System.AppUserModel.ID and System.AppUserModel.ToastActivatorCLSID (propkey.h), spelled out
        // so no import library has to provide them.
        constexpr PROPERTYKEY kAppIdKey = { { 0x9F4C2855, 0x9F79, 0x4B39, { 0xA8, 0xD0, 0xE1, 0xD4, 0x2D, 0xE1, 0xD5, 0xF3 } }, 5 };
        constexpr PROPERTYKEY kActivatorKey = { { 0x9F4C2855, 0x9F79, 0x4B39, { 0xA8, 0xD0, 0xE1, 0xD4, 0x2D, 0xE1, 0xD5, 0xF3 } }, 26 };

        struct State
        {
            Activated onActivated;
            DWORD cookie = 0;   // CoRegisterClassObject
            notifications::ToastNotifier notifier{ nullptr };
        };
        State& Current()
        {
            static State state;   // UI thread only
            return state;
        }

        CLSID ActivatorClsid()
        {
            CLSID clsid{};
            CLSIDFromString(kActivatorClsid, &clsid);
            return clsid;
        }

        std::wstring ClassKey()
        {
            return std::format(L"Software\\Classes\\CLSID\\{}", kActivatorClsid);
        }

        // Windows calls this on our STA (through its message loop) for a click on one of our toasts.
        class Activator final : public RuntimeClass<RuntimeClassFlags<ClassicCom>, INotificationActivationCallback>
        {
        public:
            IFACEMETHODIMP Activate(LPCWSTR, LPCWSTR invokedArgs, const NOTIFICATION_USER_INPUT_DATA*, ULONG) override
            {
                const auto activation = ParseArguments(invokedArgs ? invokedArgs : L"");
                log::Info(L"toast: clicked ({})", activation ? (activation->verb == Verb::Open ? L"open" : activation->verb == Verb::Update ? L"update" : L"show in Explorer")
                                                           : L"nothing to do");
                // The click gave us the right to take the foreground; pass it on to Explorer or the app
                // that opens the file.
                AllowSetForegroundWindow(ASFW_ANY);
                if (activation && Current().onActivated)
                    Current().onActivated(*activation);
                return S_OK;
            }
        };

        class Factory final : public RuntimeClass<RuntimeClassFlags<ClassicCom>, IClassFactory>
        {
        public:
            IFACEMETHODIMP CreateInstance(IUnknown* outer, REFIID riid, void** object) override
            {
                if (!object)
                    return E_POINTER;
                *object = nullptr;
                if (outer)
                    return CLASS_E_NOAGGREGATION;
                const auto activator = Microsoft::WRL::Make<Activator>();
                return activator ? activator.CopyTo(riid, object) : E_OUTOFMEMORY;
            }
            IFACEMETHODIMP LockServer(BOOL) override { return S_OK; }
        };

        bool RegisterActivator(DWORD flags)
        {
            const auto factory = Microsoft::WRL::Make<Factory>();
            const HRESULT hr = factory ? CoRegisterClassObject(ActivatorClsid(), factory.Get(), CLSCTX_LOCAL_SERVER, flags, &Current().cookie)
                                       : E_OUTOFMEMORY;
            if (FAILED(hr))
                log::Warn(L"toast: CoRegisterClassObject failed ({})", HexCode(hr));
            return SUCCEEDED(hr);
        }

        // The Start-menu shortcut the installer made, with our AppUserModelID and activator on it.
        bool ShortcutIsOurs()
        {
            wil::unique_cotaskmem_string programs;
            if (FAILED(SHGetKnownFolderPath(FOLDERID_Programs, 0, nullptr, &programs)))
                return false;
            const auto path = std::format(L"{}\\" SD_NAME L".lnk", programs.get());
            ComPtr<IShellLinkW> link;
            ComPtr<IPersistFile> file;
            ComPtr<IPropertyStore> store;
            if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link))) || FAILED(link.As(&file)) ||
                FAILED(file->Load(path.c_str(), STGM_READ)) || FAILED(link.As(&store)))
            {
                log::Info(L"toast: no Start menu shortcut at {}; using balloon notifications", path);
                return false;
            }
            PROPVARIANT id, activator;
            PropVariantInit(&id);
            PropVariantInit(&activator);
            auto clear = wil::scope_exit([&] {
                PropVariantClear(&id);
                PropVariantClear(&activator);
            });
            const bool idOk = SUCCEEDED(store->GetValue(kAppIdKey, &id)) && id.vt == VT_LPWSTR && id.pwszVal &&
                              EqualsNoCase(id.pwszVal, kAppId);
            const bool activatorOk = SUCCEEDED(store->GetValue(kActivatorKey, &activator)) && activator.vt == VT_CLSID && activator.puuid &&
                                     IsEqualCLSID(*activator.puuid, ActivatorClsid());
            if (!idOk || !activatorOk)
                log::Info(L"toast: the Start menu shortcut has no {}; using balloon notifications",
                          idOk ? L"toast activator" : L"AppUserModelID");
            return idOk && activatorOk;
        }

        // HKCU\Software\Classes\CLSID\{...}\LocalServer32 = "<this exe>" -ToastActivated, so a click
        // after the app has quit starts this copy.
        void RegisterServer()
        {
            const auto command = std::format(L"\"{}\" {}", ThisExe(), kActivatedArg);
            const auto key = ClassKey() + L"\\LocalServer32";
            const LSTATUS status = RegSetKeyValueW(HKEY_CURRENT_USER, key.c_str(), nullptr, REG_SZ, command.c_str(),
                                                   static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
            if (status != ERROR_SUCCESS)
                log::Warn(L"toast: could not register the activator ({})", status);
        }
    }

    bool Start(Activated onActivated)
    {
        auto& s = Current();
        s.onActivated = std::move(onActivated);
        if (!ShortcutIsOurs())
            return false;
        // The shortcut and the process share the ID, so Windows ties our toasts (and windows) to it.
        if (const HRESULT hr = SetCurrentProcessExplicitAppUserModelID(kAppId); FAILED(hr))
        {
            log::Warn(L"toast: SetCurrentProcessExplicitAppUserModelID failed ({})", HexCode(hr));
            return false;
        }
        RegisterServer();
        if (!RegisterActivator(REGCLS_MULTIPLEUSE))
            return false;
        try
        {
            s.notifier = notifications::ToastNotificationManager::CreateToastNotifier(kAppId);
        }
        catch (const winrt::hresult_error& e)
        {
            log::Warn(L"toast: no notifier ({}); using balloon notifications", HexCode(e.code().value));
            Stop();
            return false;
        }
        log::Info(L"toast: ready ({})", kAppId);
        return true;
    }

    bool Show(const Content& content)
    {
        auto& s = Current();
        if (!s.notifier)
            return false;
        try
        {
            // Turned off for ShiftDrop (or all apps, or by policy): a balloon would not be seen either,
            // but trying it costs nothing.
            if (s.notifier.Setting() != notifications::NotificationSetting::Enabled)
                return false;
            winrt::Windows::Data::Xml::Dom::XmlDocument xml;
            xml.LoadXml(BuildXml(content));
            s.notifier.Show(notifications::ToastNotification(xml));
            return true;
        }
        catch (const winrt::hresult_error& e)
        {
            log::Warn(L"toast: not shown ({})", HexCode(e.code().value));
            return false;
        }
    }

    void Stop()
    {
        auto& s = Current();
        if (s.cookie)
            CoRevokeClassObject(s.cookie);
        s.cookie = 0;
        s.notifier = nullptr;
        s.onActivated = nullptr;
    }

    int ServeActivation(Activated onActivated)
    {
        // One click, then quit: the activator posts WM_QUIT once it has done what was asked.
        Current().onActivated = [onActivated = std::move(onActivated)](const Activation& a) {
            onActivated(a);
            PostQuitMessage(0);
        };
        SetCurrentProcessExplicitAppUserModelID(kAppId);
        if (!RegisterActivator(REGCLS_SINGLEUSE))
            return 1;
        // A thread timer (no window): if COM never calls, give up rather than linger.
        const UINT_PTR timer = SetTimer(nullptr, 0, 30000, nullptr);
        MSG m{};
        while (GetMessageW(&m, nullptr, 0, 0) > 0)
        {
            if (m.message == WM_TIMER && !m.hwnd && m.wParam == timer)
            {
                log::Warn(L"toast: started for a click that never came");
                break;
            }
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
        KillTimer(nullptr, timer);
        Stop();
        return 0;
    }

    void Unregister()
    {
        RegDeleteTreeW(HKEY_CURRENT_USER, ClassKey().c_str());
    }
}
