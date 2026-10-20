#pragma once
#include <windows.h>

namespace sd
{
    // Hides the mouse pointer while a file is dragged over the wheel, and brings it back.
    //
    // During a drag the pointer is not ours: the app the file is dragged from holds the mouse capture,
    // so its thread decides what the pointer looks like and our own ShowCursor would change nothing.
    // AttachThreadInput makes our thread and that thread share one input state, which includes the
    // pointer's display count, so ShowCursor(FALSE) hides it for both.
    //
    // Windows also keeps each thread's own part of that count. Detaching takes our -1 with it (and
    // detaching is automatic if our thread ends), so the other app is never left without a pointer.
    class HiddenCursor
    {
    public:
        HiddenCursor() = default;
        ~HiddenCursor() { Restore(); }
        HiddenCursor(const HiddenCursor&) = delete;
        HiddenCursor& operator=(const HiddenCursor&) = delete;

        bool Hide();      // false if no drag source could be found (the pointer stays)
        void Restore();   // safe to call when not hidden

    private:
        DWORD m_attached = 0;   // the drag source's thread, while attached
        bool m_hidden = false;
    };
}
