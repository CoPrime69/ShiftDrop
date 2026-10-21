#pragma once
#include "Overlay.h"
#include "Settings.h"
#include "keyboard/Hotkey.h"
#include "keyboard/KeyNav.h"

#include <windows.h>
#include <wil/resource.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace sd
{
    // Everything keyboard about the wheel, kept out of main.cpp:
    //
    //  - During a drag (PollDrag, from the main window's 30 ms drag poll): arrows move the
    //    highlight, Enter runs the highlighted slice on the dragged files. The drag then ends the way
    //    Esc ends it: an Esc is sent, so the drag source cancels (nothing is dropped anywhere). Keys
    //    are read with GetAsyncKeyState like the wheel modifiers; the overlay never takes focus, and
    //    nothing here installs a keyboard hook or touches the Win key.
    //
    //  - Without a drag: the global hotkey (default Ctrl+Shift+Space, [Keyboard] in settings.ini)
    //    opens the wheel on File Explorer's selection, and "ShiftDrop.exe --wheel <paths>" (the
    //    right-click entry) arrives here by WM_COPYDATA. The wheel opens in click mode. While it is
    //    open, arrows / Enter / Esc are taken with RegisterHotKey (so they do not also move Explorer's
    //    selection) and given back the moment it closes.
    //
    // Owns a message-only window (class kWindowClass) that receives WM_HOTKEY and WM_COPYDATA.
    class KeyboardController
    {
    public:
        static constexpr wchar_t kWindowClass[] = L"ShiftDrop.Keys";

        struct Events
        {
            std::function<void(const std::wstring& title, const std::wstring& text)> notice;   // a tray balloon
        };

        KeyboardController(HINSTANCE instance, Overlay& overlay, std::array<UINT, kRingCount> ringKeys, Events events);
        ~KeyboardController();
        KeyboardController(const KeyboardController&) = delete;
        KeyboardController& operator=(const KeyboardController&) = delete;

        bool Create();
        void Destroy();

        // Which key switches to which wheel section, by the section's place on the wheel (0 = off).
        void SetRingKeys(std::array<UINT, kRingCount> keys) { m_ringKeys = keys; }

        // From the drag poll while the wheel is visible.
        void PollDrag();

        // Opens the wheel in click mode on `files` at the cursor.
        void OpenFor(std::vector<std::wstring> files);
        // Paths from --wheel or WM_COPYDATA: collected for a moment (Explorer starts one process per
        // selected item), then one wheel opens for all of them.
        void Queue(const std::vector<std::wstring>& paths);

        // Tray menu.
        bool HotkeyOn() const { return m_hotkeyOn; }
        void ToggleHotkey();
        std::wstring HotkeyLabel() const;   // "Ctrl+Shift+Space"
        static bool ShellMenuOn();
        void ToggleShellMenu();

        // The second process ("--wheel" while a copy is running): hands `paths` to the running copy.
        // Waits up to `timeoutMs` for its window (it may still be starting). False if none answered.
        static bool Forward(const std::vector<std::wstring>& paths, DWORD timeoutMs = 10000);

    private:
        static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
        LRESULT Handle(UINT msg, WPARAM wParam, LPARAM lParam);
        void LoadSettings();
        bool RegisterExplorerHotkey();
        void OnHotkey();
        void PassThrough();            // the hotkey outside Explorer: give the keystroke to the app
        void OnSelection(std::unique_ptr<std::vector<std::wstring>> paths);
        void GrabKeys();
        void ReleaseKeys();
        void OnClickPoll();
        void OnGrabbedKey(UINT vk);

        HINSTANCE m_instance;
        Overlay& m_overlay;
        std::array<UINT, kRingCount> m_ringKeys;
        Events m_events;
        wil::unique_hwnd m_hwnd;

        Hotkey m_hotkey = kDefaultExplorerHotkey;
        bool m_hotkeyOn = true;
        bool m_hotkeyRegistered = false;
        ULONGLONG m_passedAt = 0;      // when a keystroke was last passed through
        std::shared_ptr<std::atomic<bool>> m_querying = std::make_shared<std::atomic<bool>>(false);

        std::vector<std::wstring> m_queued;

        // Drag mode.
        bool m_dragShowing = false;
        bool m_enterDown = false;
        keynav::Repeat m_repeat[4];   // Left, Right, Up, Down

        // Click mode.
        bool m_grabbed = false;
        std::vector<int> m_grabIds;
        HWND m_foreground = nullptr;
        std::array<bool, kRingCount> m_ringDown{};
        bool m_buttonDown = false;
    };
}
