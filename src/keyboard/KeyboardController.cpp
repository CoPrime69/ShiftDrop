#include "keyboard/KeyboardController.h"
#include "Brand.h"
#include "Log.h"
#include "keyboard/ExplorerSelection.h"
#include "keyboard/ShellVerb.h"
#include "keyboard/WheelRequest.h"

#include <objbase.h>

#include <format>
#include <thread>

namespace sd
{
    namespace
    {
        constexpr int kHotkeyExplorer = 1;
        constexpr int kGrabBase = 0x100;   // ids of the keys taken while a click-mode wheel is open

        constexpr UINT_PTR kTimerQueue = 1;        // paths from --wheel: wait for the rest
        constexpr UINT_PTR kTimerClick = 2;        // click mode: modifiers, clicks outside, focus changes
        constexpr UINT_PTR kTimerReregister = 3;   // after passing the hotkey through to another app
        constexpr UINT kQueueMs = 350;
        constexpr UINT kClickPollMs = 30;

        constexpr UINT kMsgSelection = WM_APP + 1;   // lParam: std::vector<std::wstring>*, owned by the receiver

        constexpr wchar_t kSection[] = L"Keyboard";

        bool Down(UINT vk) { return vk && (GetAsyncKeyState(static_cast<int>(vk)) & 0x8000) != 0; }

        bool AnyButtonDown() { return Down(VK_LBUTTON) || Down(VK_RBUTTON) || Down(VK_MBUTTON); }

        void SendKey(UINT vk)
        {
            INPUT in[2]{};
            in[0].type = in[1].type = INPUT_KEYBOARD;
            in[0].ki.wVk = in[1].ki.wVk = static_cast<WORD>(vk);
            in[1].ki.dwFlags = KEYEVENTF_KEYUP;
            SendInput(2, in, sizeof(INPUT));
        }

        std::vector<std::wstring> Existing(std::vector<std::wstring> paths)
        {
            std::erase_if(paths, [](const std::wstring& p) { return GetFileAttributesW(p.c_str()) == INVALID_FILE_ATTRIBUTES; });
            return paths;
        }
    }

    KeyboardController::KeyboardController(HINSTANCE instance, Overlay& overlay, std::array<UINT, kRingCount> ringKeys, Events events)
        : m_instance(instance), m_overlay(overlay), m_ringKeys(ringKeys), m_events(std::move(events))
    {
    }

    KeyboardController::~KeyboardController() { Destroy(); }

    bool KeyboardController::Create()
    {
        WNDCLASSEXW wc{ sizeof(wc) };
        wc.lpfnWndProc = WndProc;
        wc.hInstance = m_instance;
        wc.lpszClassName = kWindowClass;
        RegisterClassExW(&wc);
        // Message-only: never shown, never activated; a second process finds it with FindWindowEx(HWND_MESSAGE).
        m_hwnd.reset(CreateWindowExW(0, kWindowClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, m_instance, this));
        if (!m_hwnd)
        {
            log::Error(L"keys: CreateWindowExW failed ({})", GetLastError());
            return false;
        }
        // Explorer runs at medium integrity like us; this only matters if one of us is elevated.
        ChangeWindowMessageFilterEx(m_hwnd.get(), WM_COPYDATA, MSGFLT_ALLOW, nullptr);
        LoadSettings();
        RegisterExplorerHotkey();
        log::Info(L"keys: ready (Explorer hotkey {} {}, right-click entry {})", HotkeyText(m_hotkey),
                  m_hotkeyOn ? (m_hotkeyRegistered ? L"on" : L"unavailable") : L"off", ShellMenuOn() ? L"on" : L"off");
        return true;
    }

    void KeyboardController::Destroy()
    {
        if (!m_hwnd)
            return;
        ReleaseKeys();
        if (m_hotkeyRegistered)
            UnregisterHotKey(m_hwnd.get(), kHotkeyExplorer);
        m_hotkeyRegistered = false;
        m_hwnd.reset();
    }

    // ---- Settings --------------------------------------------------------------------------------

    void KeyboardController::LoadSettings()
    {
        const auto path = SettingsPath();
        wchar_t probe[8]{};
        if (!GetPrivateProfileStringW(kSection, nullptr, L"", probe, ARRAYSIZE(probe), path.c_str()))
        {
            // First run with this version: write the defaults, so the file shows what can be set.
            WritePrivateProfileStringW(kSection, L"ExplorerHotkey", HotkeyText(kDefaultExplorerHotkey).c_str(), path.c_str());
            WritePrivateProfileStringW(kSection, L"ExplorerHotkeyOn", L"1", path.c_str());
            WritePrivateProfileStringW(kSection, L"; ExplorerHotkey",
                                       L"Ctrl, Alt and Shift plus a key, e.g. Ctrl+Alt+W or Alt+F2 (no Win key); restart to apply",
                                       path.c_str());
        }
        wchar_t text[64]{};
        GetPrivateProfileStringW(kSection, L"ExplorerHotkey", L"", text, ARRAYSIZE(text), path.c_str());
        std::wstring error;
        Hotkey parsed;
        if (text[0] && ParseHotkey(text, parsed, &error))
            m_hotkey = parsed;
        else if (text[0])
            log::Warn(L"keys: ExplorerHotkey '{}' ignored ({}); using {}", text, error, HotkeyText(kDefaultExplorerHotkey));
        m_hotkeyOn = GetPrivateProfileIntW(kSection, L"ExplorerHotkeyOn", 1, path.c_str()) != 0;
    }

    bool KeyboardController::RegisterExplorerHotkey()
    {
        if (!m_hwnd || !m_hotkeyOn || m_hotkeyRegistered)
            return m_hotkeyRegistered;
        m_hotkeyRegistered = RegisterHotKey(m_hwnd.get(), kHotkeyExplorer, m_hotkey.modifiers | MOD_NOREPEAT, m_hotkey.vk) != FALSE;
        if (!m_hotkeyRegistered)
        {
            log::Warn(L"keys: RegisterHotKey({}) failed ({}): another app has it", HotkeyText(m_hotkey), GetLastError());
            if (m_events.notice)
                m_events.notice(std::format(L"{} is taken", HotkeyText(m_hotkey)),
                                L"Another app uses this shortcut. Pick another in settings.ini, [Keyboard] ExplorerHotkey.");
        }
        return m_hotkeyRegistered;
    }

    std::wstring KeyboardController::HotkeyLabel() const { return HotkeyText(m_hotkey); }

    void KeyboardController::ToggleHotkey()
    {
        m_hotkeyOn = !m_hotkeyOn;
        WritePrivateProfileStringW(kSection, L"ExplorerHotkeyOn", m_hotkeyOn ? L"1" : L"0", SettingsPath().c_str());
        if (m_hotkeyOn)
            RegisterExplorerHotkey();
        else if (m_hotkeyRegistered)
        {
            UnregisterHotKey(m_hwnd.get(), kHotkeyExplorer);
            m_hotkeyRegistered = false;
        }
        log::Info(L"keys: Explorer hotkey {}", m_hotkeyOn ? L"on" : L"off");
    }

    bool KeyboardController::ShellMenuOn() { return shellverb::IsRegistered(); }

    void KeyboardController::ToggleShellMenu()
    {
        const bool on = !ShellMenuOn();
        const bool ok = on ? shellverb::Register(shellverb::CurrentExe()) : shellverb::Unregister();
        log::Info(L"keys: right-click entry {} ({})", on ? L"added" : L"removed", ok ? L"ok" : L"failed");
        if (!ok && m_events.notice)
            m_events.notice(SD_NAME, on ? L"Could not add the right-click entry. See the log." : L"Could not remove the right-click entry. See the log.");
    }

    // ---- Messages --------------------------------------------------------------------------------

    LRESULT CALLBACK KeyboardController::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        if (msg == WM_NCCREATE)
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams));
        auto* self = reinterpret_cast<KeyboardController*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        // m_hwnd is still empty until CreateWindowExW returns, and Handle passes m_hwnd to
        // DefWindowProcW: WM_NCCREATE sent to a null window returns FALSE and the creation fails
        // with ERROR_INVALID_WINDOW_HANDLE (1400). Messages before then go straight to the default.
        if (!self || self->m_hwnd.get() != hwnd)
        {
            if (msg == kMsgSelection)
                delete reinterpret_cast<std::vector<std::wstring>*>(lParam);
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        }
        return self->Handle(msg, wParam, lParam);
    }

    LRESULT KeyboardController::Handle(UINT msg, WPARAM wParam, LPARAM lParam)
    {
        switch (msg)
        {
        case WM_HOTKEY:
            if (wParam == kHotkeyExplorer)
                OnHotkey();
            else if (static_cast<int>(wParam) >= kGrabBase)
                OnGrabbedKey(HIWORD(lParam));
            return 0;
        case WM_COPYDATA:
        {
            const auto* cds = reinterpret_cast<const COPYDATASTRUCT*>(lParam);
            if (!cds || cds->dwData != wheelrequest::kCopyDataId || !cds->lpData)
                return FALSE;
            Queue(wheelrequest::Decode(static_cast<const wchar_t*>(cds->lpData), cds->cbData / sizeof(wchar_t)));
            return TRUE;
        }
        case kMsgSelection:
            OnSelection(std::unique_ptr<std::vector<std::wstring>>(reinterpret_cast<std::vector<std::wstring>*>(lParam)));
            return 0;
        case WM_TIMER:
            if (wParam == kTimerQueue)
            {
                KillTimer(m_hwnd.get(), kTimerQueue);
                auto files = wheelrequest::CleanSelection(m_queued);
                m_queued.clear();
                log::Info(L"keys: wheel requested for {} item(s)", files.size());
                OpenFor(std::move(files));
            }
            else if (wParam == kTimerClick)
                OnClickPoll();
            else if (wParam == kTimerReregister)
            {
                KillTimer(m_hwnd.get(), kTimerReregister);
                RegisterExplorerHotkey();
            }
            return 0;
        default:
            return DefWindowProcW(m_hwnd.get(), msg, wParam, lParam);
        }
    }

    // ---- The wheel on Explorer's selection -------------------------------------------------------

    void KeyboardController::OnHotkey()
    {
        HWND foreground = GetForegroundWindow();
        HWND root = foreground ? GetAncestor(foreground, GA_ROOT) : nullptr;
        if (!root || !explorer::IsExplorerWindow(root))
        {
            PassThrough();
            return;
        }
        if (m_overlay.IsVisible())
        {
            // The same keys again close a click-mode wheel; a drag owns its wheel.
            if (m_overlay.InClickMode())
                m_overlay.Cancel();
            return;
        }
        if (m_querying->exchange(true))
            return;   // the last press is still being answered
        // Explorer's automation objects are cross-process COM: asked from a thread of their own, so a
        // busy Explorer never stalls the wheel, the hook or the tray.
        std::thread([hwnd = m_hwnd.get(), root, busy = m_querying] {
            const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
            std::unique_ptr<std::vector<std::wstring>> paths;
            try
            {
                paths = std::make_unique<std::vector<std::wstring>>(explorer::SelectionOf(root));
            }
            catch (...)
            {
                paths = std::make_unique<std::vector<std::wstring>>();
            }
            if (SUCCEEDED(init))
                CoUninitialize();
            busy->store(false);
            if (PostMessageW(hwnd, kMsgSelection, 0, reinterpret_cast<LPARAM>(paths.get())))
                paths.release();   // the window owns it now
        }).detach();
    }

    // RegisterHotKey takes the keys from every app. Outside Explorer the press is handed back: the
    // hotkey is dropped for a moment and the key sent again, so Ctrl+Shift+Space still reaches the
    // spreadsheet or editor in front (the modifiers are still held).
    void KeyboardController::PassThrough()
    {
        const ULONGLONG now = GetTickCount64();
        if (now - m_passedAt < 300)
            return;   // our own key came back before the hotkey was dropped: let it go
        m_passedAt = now;
        if (m_hotkeyRegistered)
        {
            UnregisterHotKey(m_hwnd.get(), kHotkeyExplorer);
            m_hotkeyRegistered = false;
        }
        SendKey(m_hotkey.vk);
        SetTimer(m_hwnd.get(), kTimerReregister, 150, nullptr);
    }

    void KeyboardController::OnSelection(std::unique_ptr<std::vector<std::wstring>> paths)
    {
        if (!paths || paths->empty())
        {
            log::Info(L"keys: hotkey pressed but nothing with a path is selected");
            if (m_events.notice)
                m_events.notice(L"Nothing selected", std::format(L"Select files in File Explorer, then press {}.", HotkeyText(m_hotkey)));
            return;
        }
        log::Info(L"keys: Explorer selection of {} item(s)", paths->size());
        OpenFor(std::move(*paths));
    }

    void KeyboardController::Queue(const std::vector<std::wstring>& paths)
    {
        m_queued.insert(m_queued.end(), paths.begin(), paths.end());
        if (m_hwnd)
            SetTimer(m_hwnd.get(), kTimerQueue, kQueueMs, nullptr);   // restarts with each process that reports in
    }

    void KeyboardController::OpenFor(std::vector<std::wstring> files)
    {
        files = Existing(wheelrequest::CleanSelection(files));
        if (files.empty() || m_overlay.InDrag())
            return;
        if (m_overlay.IsVisible())
            m_overlay.Hide();
        POINT pt{};
        GetCursorPos(&pt);
        m_foreground = GetForegroundWindow();
        // Modifiers already down (the hotkey's own Ctrl and Shift) only switch wheels once pressed again.
        for (size_t i = 0; i < m_ringKeys.size(); ++i)
            m_ringDown[i] = Down(m_ringKeys[i]);
        m_buttonDown = AnyButtonDown();
        m_overlay.ShowForFiles(pt, 0, std::move(files));
        if (!m_overlay.InClickMode())
            return;
        GrabKeys();
        SetTimer(m_hwnd.get(), kTimerClick, kClickPollMs, nullptr);
    }

    // ---- Click mode ------------------------------------------------------------------------------

    // Arrows, Enter and Esc go to the wheel while it is open, with or without the wheel modifiers
    // held, instead of moving Explorer's selection. Never Ctrl+Esc (the Start menu) or anything with
    // the Win key.
    void KeyboardController::GrabKeys()
    {
        if (m_grabbed)
            return;
        m_grabbed = true;
        constexpr UINT kMods[] = { 0, MOD_SHIFT, MOD_CONTROL, MOD_ALT, MOD_CONTROL | MOD_SHIFT };
        constexpr UINT kKeys[] = { VK_LEFT, VK_RIGHT, VK_UP, VK_DOWN, VK_RETURN };
        int id = kGrabBase;
        for (const UINT vk : kKeys)
            for (const UINT mods : kMods)
            {
                if (RegisterHotKey(m_hwnd.get(), id, mods, vk))
                    m_grabIds.push_back(id);
                ++id;
            }
        for (const UINT mods : { 0u, static_cast<UINT>(MOD_SHIFT) })
        {
            if (RegisterHotKey(m_hwnd.get(), id, mods, VK_ESCAPE))
                m_grabIds.push_back(id);
            ++id;
        }
    }

    void KeyboardController::ReleaseKeys()
    {
        if (m_hwnd)
        {
            KillTimer(m_hwnd.get(), kTimerClick);
            for (const int id : m_grabIds)
                UnregisterHotKey(m_hwnd.get(), id);
        }
        m_grabIds.clear();
        m_grabbed = false;
    }

    void KeyboardController::OnGrabbedKey(UINT vk)
    {
        if (!m_overlay.InClickMode())
        {
            ReleaseKeys();
            return;
        }
        switch (vk)
        {
        case VK_LEFT: m_overlay.Navigate(keynav::Key::Left); break;
        case VK_RIGHT: m_overlay.Navigate(keynav::Key::Right); break;
        case VK_UP: m_overlay.Navigate(keynav::Key::Up); break;
        case VK_DOWN: m_overlay.Navigate(keynav::Key::Down); break;
        case VK_RETURN: m_overlay.Activate(); break;
        case VK_ESCAPE: m_overlay.Cancel(); break;
        default: break;
        }
        if (!m_overlay.InClickMode())
            ReleaseKeys();
    }

    void KeyboardController::OnClickPoll()
    {
        if (!m_overlay.InClickMode())
        {
            ReleaseKeys();
            return;
        }
        // A wheel key pressed again switches wheels, as during a drag.
        for (size_t i = 0; i < m_ringKeys.size(); ++i)
        {
            const bool down = Down(m_ringKeys[i]);
            if (down && !m_ringDown[i])
                m_overlay.SetRing(static_cast<int>(i));
            m_ringDown[i] = down;
        }
        // A click anywhere else, or another window coming to the front, closes it like a menu.
        const bool button = AnyButtonDown();
        if (button && !m_buttonDown)
        {
            POINT pt{};
            GetCursorPos(&pt);
            if (!m_overlay.Contains(pt))
                m_overlay.Cancel();
        }
        m_buttonDown = button;
        if (m_overlay.InClickMode() && GetForegroundWindow() != m_foreground)
            m_overlay.Cancel();
        if (!m_overlay.InClickMode())
            ReleaseKeys();
    }

    // ---- During a drag ---------------------------------------------------------------------------

    void KeyboardController::PollDrag()
    {
        if (!m_overlay.ShowsWheel() || !m_overlay.InDrag())
        {
            m_dragShowing = false;
            return;
        }
        // Keys already down when the wheel appeared do not count until pressed again.
        const bool first = !m_dragShowing;
        m_dragShowing = true;
        const ULONGLONG now = GetTickCount64();
        constexpr UINT kVks[] = { VK_LEFT, VK_RIGHT, VK_UP, VK_DOWN };
        constexpr keynav::Key kKeys[] = { keynav::Key::Left, keynav::Key::Right, keynav::Key::Up, keynav::Key::Down };
        for (size_t i = 0; i < 4; ++i)
        {
            const bool fire = m_repeat[i].Update(Down(kVks[i]), now);
            if (fire && !first)
                m_overlay.Navigate(kKeys[i]);
        }
        const bool enter = Down(VK_RETURN);
        const bool pressed = enter && !m_enterDown && !first;
        m_enterDown = enter;
        if (pressed && m_overlay.Activate() == Overlay::Activation::Ran)
        {
            // The slice has run on the dragged files; now end the drag the way Esc does, so the
            // source drops nothing anywhere. The wheel keeps catching a drop until then.
            log::Info(L"keys: slice run by Enter during the drag; cancelling the drag");
            SendKey(VK_ESCAPE);
        }
    }

    // ---- Second process --------------------------------------------------------------------------

    bool KeyboardController::Forward(const std::vector<std::wstring>& paths, DWORD timeoutMs)
    {
        if (paths.empty())
            return true;
        const std::wstring data = wheelrequest::Encode(paths);
        COPYDATASTRUCT cds{};
        cds.dwData = wheelrequest::kCopyDataId;
        cds.cbData = static_cast<DWORD>(data.size() * sizeof(wchar_t));
        cds.lpData = const_cast<wchar_t*>(data.data());
        const ULONGLONG until = GetTickCount64() + timeoutMs;
        for (;;)
        {
            // The running copy may still be starting (Explorer launches one process per selected item
            // at once, and the first becomes the running copy).
            if (HWND target = FindWindowExW(HWND_MESSAGE, nullptr, kWindowClass, nullptr))
            {
                DWORD pid = 0;
                GetWindowThreadProcessId(target, &pid);
                AllowSetForegroundWindow(pid);
                DWORD_PTR result = 0;
                if (SendMessageTimeoutW(target, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&cds), SMTO_ABORTIFHUNG, 5000, &result) && result)
                    return true;
            }
            if (GetTickCount64() >= until)
                return false;
            Sleep(100);
        }
    }
}
