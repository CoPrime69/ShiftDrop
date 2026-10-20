#include "MouseHook.h"
#include "Messages.h"

#include <wil/resource.h>

#include <cstdlib>

namespace sd::hook
{
    namespace
    {
        // Plain POD state: written by the hook callback, read by UI code, both on the same thread.
        struct State
        {
            HWND notify = nullptr;
            UINT keys[3] = { VK_SHIFT, 0, 0 };
            int dragCx = 4, dragCy = 4;
            UINT downMsg = WM_LBUTTONDOWN, upMsg = WM_LBUTTONUP;
            bool down = false;
            bool dragging = false;
            bool armed = false;
            POINT downPt{};
            POINT lastPt{};
            DWORD lastTime = 0;
        };
        State s;
        wil::unique_hhook g_hook;

        LRESULT CALLBACK Proc(int code, WPARAM wParam, LPARAM lParam)
        {
            if (code == HC_ACTION)
            {
                const auto* m = reinterpret_cast<const MSLLHOOKSTRUCT*>(lParam);
                s.lastPt = m->pt;   // physical screen pixels, because the process is per-monitor-v2 aware
                s.lastTime = m->time;

                const auto msgId = static_cast<UINT>(wParam);
                if (msgId == s.downMsg)
                {
                    s.down = true;
                    s.dragging = false;
                    s.armed = false;
                    s.downPt = m->pt;
                }
                else if (msgId == WM_MOUSEMOVE && s.down)
                {
                    if (!s.dragging && (std::abs(m->pt.x - s.downPt.x) > s.dragCx || std::abs(m->pt.y - s.downPt.y) > s.dragCy))
                    {
                        s.dragging = true;
                        PostMessageW(s.notify, msg::DragBegin, 0, msg::PackPoint(m->pt));
                    }
                    // GetAsyncKeyState reads the physical key state right now; it is cheap and lock-free.
                    if (s.dragging && !s.armed)
                    {
                        for (const UINT vk : s.keys)
                        {
                            if (vk && (GetAsyncKeyState(static_cast<int>(vk)) & 0x8000))
                            {
                                s.armed = true;
                                PostMessageW(s.notify, msg::DragArm, vk, msg::PackPoint(m->pt));
                                break;
                            }
                        }
                    }
                }
                else if (msgId == s.upMsg)
                {
                    if (s.dragging)
                        PostMessageW(s.notify, msg::DragEnd, 0, msg::PackPoint(m->pt));
                    s.down = s.dragging = s.armed = false;
                }
            }
            // Always pass the event on. We only observe; swallowing input here would break the drag itself.
            return CallNextHookEx(nullptr, code, wParam, lParam);
        }
    }

    bool Install(HWND notify, const UINT (&keys)[3])
    {
        s.notify = notify;
        for (int i = 0; i < 3; ++i)
            s.keys[i] = keys[i];
        // Start the watchdog's clock now; otherwise its first check sees "no events since boot".
        s.lastTime = GetTickCount();
        GetCursorPos(&s.lastPt);
        // Read once here so the callback never calls into user32 for them.
        // (These are system-DPI values; a few pixels of difference on other monitors does not matter.)
        s.dragCx = GetSystemMetrics(SM_CXDRAG);
        s.dragCy = GetSystemMetrics(SM_CYDRAG);
        // Left-handed setups swap the buttons. Low-level hooks report the physical button, so the
        // primary (drag) button is then the right one. Not yet verified on hardware; see TESTING.md.
        const bool swapped = GetSystemMetrics(SM_SWAPBUTTON) != 0;
        s.downMsg = swapped ? WM_RBUTTONDOWN : WM_LBUTTONDOWN;
        s.upMsg = swapped ? WM_RBUTTONUP : WM_LBUTTONUP;

        // For WH_MOUSE_LL the module handle only has to be non-null; no DLL injection happens.
        g_hook.reset(SetWindowsHookExW(WH_MOUSE_LL, Proc, GetModuleHandleW(nullptr), 0));
        return static_cast<bool>(g_hook);
    }

    void Uninstall() { g_hook.reset(); }

    bool Reinstall()
    {
        g_hook.reset();
        s.down = s.dragging = s.armed = false;
        g_hook.reset(SetWindowsHookExW(WH_MOUSE_LL, Proc, GetModuleHandleW(nullptr), 0));
        return static_cast<bool>(g_hook);
    }

    bool IsInstalled() { return static_cast<bool>(g_hook); }
    bool IsDragging() { return s.dragging; }
    bool IsArmed() { return s.armed; }
    void SetArmed(bool armed) { s.armed = armed; }
    DWORD LastEventTime() { return s.lastTime; }
    POINT LastPoint() { return s.lastPt; }
}
