#pragma once
#include <windows.h>
#include <wrl/client.h>

#include <string>

// The UI Automation headers stay in Accessibility.cpp: they need the COM headers that
// WIN32_LEAN_AND_MEAN leaves out of windows.h, and every file that includes Overlay.h would pull them in.
struct IRawElementProviderSimple;

// Screen reader support for the windows ShiftDrop draws itself (the wheel and the top panel), through
// UI Automation. Everything built from standard controls (Settings, History, Rename, the editors'
// buttons) is already readable; these windows draw with Direct2D, so they describe themselves.
//
// What gets said goes out as UIA notifications (UiaRaiseNotificationEvent): the wheel never takes the
// keyboard focus (it would end the drag), and screen readers speak notifications whatever has the
// focus. The element tree is there too, for inspection tools and for reading the wheel by mouse.
// UI thread only: the providers ask for COM threading, so UIA calls them on the thread that made them.
namespace sd::a11y
{
    // Speaks `text` as a notification from `source`. `activity` groups announcements: a newer one with
    // the same activity replaces one not yet spoken (the slice under a moving cursor). `done`: the end
    // of something (a job's result) rather than news along the way.
    void Announce(IRawElementProviderSimple* source, const std::wstring& text, const wchar_t* activity, bool done = false);

    // A window that only has something to say, such as the top panel: one element whose name follows
    // what it shows.
    class TextElement
    {
    public:
        explicit TextElement(HWND hwnd);
        ~TextElement();
        TextElement(const TextElement&) = delete;
        TextElement& operator=(const TextElement&) = delete;

        // WM_GETOBJECT: true (with `result`) when it was for this element.
        bool OnGetObject(WPARAM wParam, LPARAM lParam, LRESULT& result);
        void SetName(std::wstring name);
        void Announce(const std::wstring& text, const wchar_t* activity, bool done = false);

    private:
        class Provider;
        Microsoft::WRL::ComPtr<Provider> m_provider;
        HWND m_hwnd;
    };

    // What the wheel shows, as the wheel's element asks for it (Overlay implements it).
    class WheelView
    {
    public:
        struct Slice
        {
            std::wstring name;   // "PNG, PNG, lossless", or "TIFF, not available: needs ..."
            bool enabled = true;
            RECT bounds{};       // screen pixels
        };
        virtual bool ViewOpen() const = 0;              // the wheel is up (not the probe, not closing)
        virtual std::wstring ViewTitle() const = 0;     // its section: "Convert"
        virtual size_t ViewCount() const = 0;
        virtual Slice ViewSlice(size_t index) const = 0;
        virtual int ViewHighlighted() const = 0;        // the slice under the cursor or picked by key, -1 for none
        virtual int ViewSliceAt(POINT screen) const = 0;   // -1 for none

    protected:
        ~WheelView() = default;
    };

    // The wheel as UIA sees it: a list (named after its section) with one item per slice, the
    // highlighted one selected. Moving to a slice says its name; opening the wheel or switching
    // sections says the section and how many slices it has.
    class WheelElement
    {
    public:
        WheelElement(HWND hwnd, WheelView& view);
        ~WheelElement();
        WheelElement(const WheelElement&) = delete;
        WheelElement& operator=(const WheelElement&) = delete;

        bool OnGetObject(WPARAM wParam, LPARAM lParam, LRESULT& result);
        void SlicesChanged(bool announceSection);   // opened, switched sections or turned the page
        void HighlightChanged();
        void Closed();

    private:
        class Root;
        class Item;
        Microsoft::WRL::ComPtr<Root> m_root;
        HWND m_hwnd;
        int m_spoken = -1;   // the slice last announced, so a cursor moving within it says nothing new
    };
}
