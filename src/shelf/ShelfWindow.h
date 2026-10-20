#pragma once
#include "Canvas.h"
#include "Island.h"
#include "JobQueue.h"
#include "Settings.h"
#include "Theme.h"
#include "shelf/ShelfGeometry.h"
#include "shelf/ShelfStore.h"

#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <oleidl.h>
#include <wil/resource.h>
#include <wrl/client.h>

#include <memory>
#include <string>
#include <vector>

namespace sd
{
    // The Float shelf: a small always-on-top panel that holds files until you drag them somewhere.
    //
    // Files arrive from the Float slice on the File-actions wheel, from a job's result in the top
    // panel, or by dropping them on the shelf itself. Each shows its thumbnail and name; drag one, a
    // selection or all of them out into any app. Drop it near a screen edge and it pins there, folding
    // into a slim tab until the mouse (or a drag) comes back. It remembers its files and place across
    // restarts (shelf.json next to settings.ini), drops files that disappear, and hides when empty.
    //
    // Like the wheel and the top panel it is a layered window drawn with Direct2D (Canvas): pixels
    // with alpha 0 let clicks through, so the folded tab takes no more room than it shows. Unlike
    // them it can be activated by a click, for the keyboard (arrows, Delete, Ctrl+A, Ctrl+C, Enter).
    // Thumbnails, existence checks and clipboard pictures are made on its own worker thread.
    //
    // Created, used and destroyed on the UI thread, before OleUninitialize.
    class Shelf
    {
    public:
        Shelf(HINSTANCE instance, std::wstring storePath);
        ~Shelf();
        Shelf(const Shelf&) = delete;
        Shelf& operator=(const Shelf&) = delete;

        // Registers the "file.float" action and loads the saved shelf (shown if it has files).
        bool Create();
        void SetAppearance(const Appearance& appearance);

        // Puts the files at the top of the shelf and shows it.
        void Add(const std::vector<std::wstring>& files);

        // Convert-and-carry for a finished job's outputs in the top panel: drag them out, put them on
        // the shelf, copy them. The actions do nothing once the shelf is gone.
        Island::ResultActions CarryActions(std::vector<std::wstring> outputs);

        // Puts the files on the clipboard; a single image also as a picture, a moment later.
        // Returns the line to show ("Copied ...").
        std::wstring Copy(const std::vector<std::wstring>& files);

        // Called by the shelf's drop target (OLE, UI thread).
        bool OnDragEnter(const std::vector<std::wstring>& files);
        void OnDragLeave();
        void OnDrop(const std::vector<std::wstring>& files);

    private:
        struct Item
        {
            std::wstring path;
            std::wstring name, detail;
            bool folder = false;
            bool requested = false;        // a thumbnail is on its way (or arrived)
            theme::Pixels thumb;           // CPU copy, so a lost device can rebuild the bitmap
            Microsoft::WRL::ComPtr<ID2D1Bitmap> bitmap;
            unsigned bitmapGeneration = 0;
        };
        struct ThumbResult;
        struct CheckResult;
        struct ClipboardJob;

        static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
        LRESULT Handle(UINT msg, WPARAM wParam, LPARAM lParam);

        // Items and saving.
        void Restore();
        void Save() const;
        void RemoveAt(std::vector<size_t> indices);
        void Clear();
        std::vector<std::wstring> PathsOf(const std::vector<size_t>& indices) const;
        void RequestThumbnails();
        void CheckFiles();
        void OnThumb(std::unique_ptr<ThumbResult> result);
        void OnCheck(std::unique_ptr<CheckResult> result);

        // Showing, hiding, folding.
        void Show(bool peek);
        void Hide();
        void UseMonitor(HMONITOR monitor);
        void Refresh();                         // monitor work area, DPI scale and theme
        shelf::Box Expanded() const;            // the open panel at rest
        shelf::Box Shown() const;               // what is drawn now (folding, sliding, appearing)
        shelf::Box Content() const;             // where the open panel's content is drawn now
        void Appearing(float& dx, float& dy) const;   // offset while showing or hiding
        bool Folded() const { return m_edge != shelf::Edge::None && m_open < 0.98f; }
        void WantOpen(bool open);
        void ScheduleFold(UINT delayMs);
        void Animate();
        void OnFrame();

        // Mouse and keyboard.
        POINT ToScreen(LPARAM lParam) const;
        shelf::Hit HitAt(POINT screen) const;
        void OnMouseMove(POINT pt);
        void OnButtonDown(POINT pt, WPARAM keys);
        void OnButtonUp(POINT pt, WPARAM keys);
        void OnDoubleClick(POINT pt);
        void OnContextMenu(POINT pt, bool fromKeyboard);
        void OnKey(UINT vk);
        void OnWheel(int delta);
        void StartDragOut(std::vector<size_t> indices);
        void EndMove();
        void TogglePin();
        void Open(const std::vector<size_t>& indices);
        void MoveFocus(int delta, bool extend);
        void ScrollIntoView(size_t index);

        // Drawing.
        Microsoft::WRL::ComPtr<IDWriteTextFormat> MakeFormat(const std::wstring& family, float dip, DWRITE_FONT_WEIGHT weight,
                                                             DWRITE_TEXT_ALIGNMENT align) const;
        bool EnsureTextFormats();
        float TextWidth(const std::wstring& text, IDWriteTextFormat* format) const;
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> Brush(const D2D1_COLOR_F& color, float alpha = 1.f);
        void Text(const std::wstring& text, IDWriteTextFormat* format, const shelf::Box& box, const D2D1_COLOR_F& color, float alpha = 1.f);
        void Glyph(wchar_t glyph, IDWriteTextFormat* format, const shelf::Box& box, const D2D1_COLOR_F& color, float alpha = 1.f);
        void RoundButton(const shelf::Box& box, wchar_t glyph, bool hover, bool danger, float alpha);
        void DrawThumb(Item& item, const shelf::Box& box, float alpha);
        void DrawPanel(const shelf::Layout& layout, float alpha);
        void DrawTab(const shelf::Box& tab, float alpha);
        void Render();

        HINSTANCE m_instance;
        std::wstring m_storePath;
        Appearance m_appearance;
        std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);   // carry actions check it

        wil::unique_hwnd m_hwnd;
        Microsoft::WRL::ComPtr<IDropTarget> m_dropTarget;
        bool m_registered = false;
        Microsoft::WRL::ComPtr<ID2D1Factory> m_d2d;
        Microsoft::WRL::ComPtr<IDWriteFactory> m_dwrite;
        std::unique_ptr<Canvas> m_canvas;
        ID2D1RenderTarget* m_rt = nullptr;   // valid during Render
        std::wstring m_textFamily, m_iconFamily;
        Microsoft::WRL::ComPtr<IDWriteTextFormat> m_titleFont, m_nameFont, m_detailFont, m_iconFont, m_smallIconFont, m_countFont, m_footerFont;
        float m_fontScale = 0.f;

        // Content.
        std::vector<Item> m_items;   // newest first
        shelf::Selection m_selection;
        size_t m_focus = 0;          // the keyboard's current item
        unsigned m_thumbPx = 0;      // thumbnail size requested, in pixels
        bool m_checking = false;     // an existence check is running
        std::wstring m_note;         // shown in the footer for a moment ("Copied")
        ULONGLONG m_noteUntil = 0;

        // Place.
        HMONITOR m_monitor = nullptr;
        shelf::Box m_work, m_screen;   // the monitor's work area and whole area, pixels
        float m_scale = 1.f;
        shelf::Metrics m_metrics{ 1.f };
        theme::Palette m_palette{};
        shelf::Edge m_edge = shelf::Edge::Right;
        float m_x = 0.f, m_y = 0.f;    // the open panel's top-left corner at rest
        bool m_placed = false;         // m_x/m_y mean something (saved, or shown before)
        POINT m_origin{};              // window position, pixels
        SIZE m_size{};

        // Animation.
        bool m_visible = false, m_hiding = false, m_animating = false, m_motion = true;
        ULONGLONG m_lastFrame = 0;
        float m_presence = 0.f;        // 0 hidden .. 1 shown
        float m_open = 1.f;            // 0 folded into the tab .. 1 open
        bool m_wantOpen = true;
        float m_h = 0.f;               // panel height, easing toward the content's
        float m_slideX = 0.f, m_slideY = 0.f;   // snapping to an edge glides there
        float m_scroll = 0.f, m_scrollTarget = 0.f;

        // Interaction.
        shelf::Hit m_hover, m_press;
        bool m_pressed = false, m_moving = false, m_tracking = false, m_mouseInside = false;
        bool m_active = false;         // has the keyboard
        bool m_menu = false;           // a context menu is open
        bool m_dropHover = false;      // files are being dragged over the shelf
        bool m_draggingOut = false;    // our own drag: never accept it back
        POINT m_pressPt{};
        float m_grabX = 0.f, m_grabY = 0.f;   // where in the panel the move started

        std::unique_ptr<JobQueue> m_worker;   // last: stopped and joined first
    };
}
