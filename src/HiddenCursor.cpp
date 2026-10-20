#include "HiddenCursor.h"
#include "Log.h"

namespace sd
{
    bool HiddenCursor::Hide()
    {
        if (m_hidden)
            return true;
        // The drag source is the foreground thread (you just pressed the mouse in its window) and owns
        // the capture window OLE's drag loop uses.
        GUITHREADINFO info{};
        info.cbSize = sizeof(info);
        if (!GetGUIThreadInfo(0, &info) || !info.hwndCapture)
        {
            log::Info(L"cursor: no drag source with mouse capture; leaving the pointer visible");
            return false;
        }
        const DWORD source = GetWindowThreadProcessId(info.hwndCapture, nullptr);
        const DWORD self = GetCurrentThreadId();
        if (source != self && !AttachThreadInput(self, source, TRUE))
        {
            log::Warn(L"cursor: AttachThreadInput failed ({}); leaving the pointer visible", GetLastError());
            return false;
        }
        m_attached = source != self ? source : 0;
        ShowCursor(FALSE);
        m_hidden = true;
        return true;
    }

    void HiddenCursor::Restore()
    {
        if (!m_hidden)
            return;
        ShowCursor(TRUE);
        if (m_attached)
            AttachThreadInput(GetCurrentThreadId(), m_attached, FALSE);
        m_attached = 0;
        m_hidden = false;
    }
}
