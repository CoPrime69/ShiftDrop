#pragma once
#include "canvas/Geometry.h"
#include "canvas/Raster.h"
#include "canvas/Regions.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <stop_token>
#include <string>
#include <vector>

struct ID2D1DeviceContext;   // Direct2D; only IPreview's implementations (src/photo) include its header

// The image canvas's plug-in interface. The editor window (EditorWindow, Direct2D) owns the image,
// zoom, pan, the side panel, Apply/Cancel and undo; a tool (Crop, Redact, later Edit Photos,
// Annotate, Add a Background, Collage) is a small class that draws an overlay, answers the pointer
// and keyboard, describes its options as panel controls, and turns its state into recipe params.
// Tools see no Win32 or Direct2D types, so their logic is unit-tested without a window.
namespace sd::canvas
{
    struct Color
    {
        float r = 0, g = 0, b = 0, a = 1;
        static Color Rgb(uint32_t rgb, float alpha = 1.f)
        {
            return { ((rgb >> 16) & 0xFF) / 255.f, ((rgb >> 8) & 0xFF) / 255.f, (rgb & 0xFF) / 255.f, alpha };
        }
        Color WithAlpha(float alpha) const { return { r, g, b, alpha }; }
    };

    // The theme's colours, so overlays match the app in light and dark mode.
    struct Colors
    {
        bool dark = true;
        Color accent, onAccent, text, textDim, shade, line, handle;
    };

    enum class CursorShape { Arrow, Move, SizeNS, SizeWE, SizeNWSE, SizeNESW, Cross, Hand, Busy };

    enum class Effect { Blur, Pixelate };

    // What a tool draws with. Positions are image coordinates; line widths and handle sizes are DIPs,
    // so they look the same at every zoom.
    class IPainter
    {
    public:
        virtual ~IPainter() = default;
        virtual const Colors& Theme() const = 0;
        virtual double Zoom() const = 0;   // DIPs per image pixel
        virtual void FillRect(const RectD& r, Color c) = 0;
        virtual void StrokeRect(const RectD& r, Color c, float widthDip, bool dashed = false) = 0;
        virtual void Line(PointD a, PointD b, Color c, float widthDip) = 0;
        // Darkens everything but `keep` (the area a crop throws away).
        virtual void ShadeOutside(const RectD& keep, Color c) = 0;
        // The picture under `r` as the effect will make it (from a preview layer the window keeps).
        virtual void DrawEffect(const RectD& r, Effect effect) = 0;
        // A square resize grip; `hot` when the pointer is on it.
        virtual void Grip(PointD at, bool hot) = 0;
        // A small rounded label whose top-left corner is at `at` (moved inside the view if needed).
        virtual void Tag(PointD at, const std::wstring& text, Color back, Color fore) = 0;
    };

    // Side-panel controls. A tool lists them (BuildPanel) every time the panel is drawn; the window
    // lays them out, handles focus and typing, and reports changes (OnControl).
    enum class ControlKind
    {
        Section,    // a heading
        Note,       // a line of secondary text (`text`)
        Button,     // `label`, optional `icon`; `primary` draws it in the accent colour
        Toggle,     // a switch: `label`, `on`
        Choice,     // chips: `options`, `selected`
        Number,     // a typed number: `label`, `value`, `min`..`max`, `decimals`, `text` = unit ("px")
        Slider,     // `label`, `value`, `min`..`max`, `decimals`, `text` = unit ("°")
        Swatches,   // colour chips: `swatches`, `selected`; the last chip may be "custom" (`custom`)
    };

    struct Control
    {
        std::string id;
        ControlKind kind = ControlKind::Note;
        std::wstring label;
        std::wstring text;
        wchar_t icon = 0;           // Segoe Fluent Icons code point
        bool enabled = true;
        bool primary = false;
        bool on = false;
        bool half = false;          // half the panel's width: two in a row (W and H fields)
        bool custom = false;        // Swatches: the last chip opens a colour picker
        std::vector<std::wstring> options;
        std::vector<Color> swatches;
        int selected = -1;
        double value = 0, min = 0, max = 0;
        int decimals = 0;
    };

    struct ControlEvent
    {
        std::string id;
        int index = -1;        // Choice / Swatches
        double value = 0;      // Number / Slider
        bool on = false;       // Toggle
        uint32_t rgb = 0;      // Swatches: a custom colour picked
        bool final = true;     // Slider: false while the thumb is still being dragged
    };

    struct Pointer
    {
        PointD at;                 // image coordinates
        double tolerance = 6;      // image pixels within which a grip counts as hit (a few DIPs on screen)
        bool shift = false, ctrl = false, alt = false;
    };

    // What a tool's handler changed. `changed` records an undo step (after a drag, at its end).
    struct Response
    {
        bool redraw = false;
        bool changed = false;
        bool panel = false;   // the panel's controls changed shape (rebuilt on the next frame anyway)
        Response& operator|=(const Response& o)
        {
            redraw |= o.redraw;
            changed |= o.changed;
            panel |= o.panel;
            return *this;
        }
    };
    inline constexpr Response kRedraw{ true, false, false };
    inline constexpr Response kChanged{ true, true, true };

    // A quarter-turn rotation and a small extra angle the window shows the picture with (Crop's
    // rotate and straighten). Tool coordinates are in the rotated picture.
    struct ViewRotation
    {
        int quarterTurns = 0;    // clockwise, 0..3
        double degrees = 0;      // extra clockwise rotation about the centre
    };

    // What the window offers a tool.
    class ICanvasHost
    {
    public:
        virtual ~ICanvasHost() = default;
        virtual void Redraw() = 0;
        virtual void Changed() = 0;                        // record an undo step now
        virtual void SetStatus(const std::wstring& text) = 0;
        // Runs `work` on a worker thread (COM initialised, MTA; may block, e.g. on OCR) and the
        // function it returns back on the UI thread, unless the window has closed by then. The stop
        // token is requested when the window closes.
        virtual void RunAsync(std::function<std::function<void()>(std::stop_token)> work) = 0;
        virtual bool Busy() const = 0;
        // The current file's full-resolution picture (oriented as shown, before the tool's rotation),
        // or null while it loads.
        virtual std::shared_ptr<const Raster> Picture() const = 0;
        virtual size_t FileIndex() const = 0;
        virtual void ShowFile(size_t index) = 0;
        // The Open dialog for one picture (Annotate's inserted images, a background picture). False
        // when cancelled or when there is no window (tests).
        virtual bool PickImage(std::wstring& path)
        {
            (void)path;
            return false;
        }
    };

    // A pre-pass a tool can ask for before its window appears (Share-safe looks for personal details
    // first and only opens the review when it finds some).
    struct Prepass
    {
        std::wstring message;   // "Checking 3 photos for personal details"
        // Runs on a worker; `progress` takes 0..1. Returns true when the editor should open, false
        // to apply at once with the tool's Params().
        std::function<bool(std::stop_token, const std::function<void(double)>& progress)> run;
    };

    // A tool that shows its result instead of the plain picture (the photo tools: an edited photo,
    // a picture on a background, a collage). The window renders it into a bitmap whenever Revision()
    // or the zoom changes and shows that bitmap, so panning and the overlay stay cheap. Tool
    // coordinates are the result's pixels; the view fits Size() instead of the picture.
    class IPreview
    {
    public:
        virtual ~IPreview() = default;
        virtual SizeD Size() const = 0;
        virtual uint64_t Revision() const = 0;
        // Draws the result at `scale` result pixels per tool unit into `dc` (UI thread; its target is
        // set and cleared, transform identity, 96 DPI). `device` changes when the window's Direct2D
        // device is made again, so cached bitmaps must be too. False draws the plain picture instead.
        virtual bool Render(ID2D1DeviceContext* dc, double scale, unsigned device) = 0;
    };

    class ICanvasTool
    {
    public:
        virtual ~ICanvasTool() = default;

        virtual void Attach(ICanvasHost* host) { m_host = host; }

        virtual std::wstring Title() const = 0;                         // "Crop", in the title bar
        virtual std::wstring ApplyLabel() const { return L"Apply"; }
        // Tools that edit every file on its own (Redact) get a file switcher; the others edit the
        // first file and their params apply to all.
        virtual bool PerFile() const { return false; }
        // The file at `index` is decoded; `width` x `height` is its oriented size.
        virtual void ImageReady(size_t index, int width, int height) = 0;
        virtual ViewRotation Rotation() const { return {}; }
        virtual std::unique_ptr<Prepass> MakePrepass() { return nullptr; }

        virtual void Draw(IPainter& painter) const = 0;
        virtual CursorShape CursorAt(const Pointer& p) const = 0;
        virtual Response PointerDown(const Pointer& p) = 0;
        virtual Response PointerMove(const Pointer& p) = 0;     // while the button is down
        virtual Response PointerUp(const Pointer& p) = 0;
        virtual Response PointerCancel() = 0;                   // Esc during a drag, or capture lost
        virtual Response Hover(const Pointer&) { return {}; }
        // Keys the window doesn't use itself (it keeps Enter, Esc, Ctrl+Z/Y, zoom and space).
        virtual Response Key(unsigned vk, bool shift, bool ctrl) = 0;
        // While true (a text annotation being typed), typed characters go to Char() and every key but
        // Ctrl+Z / Ctrl+Y / zoom goes to Key() first, Enter and Esc included.
        virtual bool TextInput() const { return false; }
        virtual Response Char(wchar_t) { return {}; }
        // Non-null for tools that draw their own result (IPreview above).
        virtual IPreview* Preview() { return nullptr; }

        virtual void BuildPanel(std::vector<Control>& out) const = 0;
        virtual Response OnControl(const ControlEvent& e) = 0;

        // Undo and redo: the tool's whole editable state.
        virtual nlohmann::json State() const = 0;
        virtual void Restore(const nlohmann::json& state) = 0;

        virtual bool CanApply(std::wstring* why) const
        {
            (void)why;
            return true;
        }
        // The choices, merged into the recipe step's params for the engine.
        virtual nlohmann::json Params() const = 0;

    protected:
        ICanvasHost* m_host = nullptr;
    };

    // The resize cursor for a grip, the move cursor for a body.
    CursorShape CursorForGrip(Grip grip);
}
