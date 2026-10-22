#pragma once
#include "canvas/CanvasTool.h"
#include "photo/Compose.h"
#include "photo/Signatures.h"
#include "photo/TextBox.h"

#include <map>
#include <memory>
#include <string>
#include <vector>

// The photo tools' editors on the image canvas: each one is an ICanvasTool that also draws its own
// result (IPreview), with the same Compose.cpp functions the export uses. The window renders that
// result on the GPU at the screen's resolution, so sliders and drags show the real output live.
namespace sd::photo
{
    std::unique_ptr<canvas::ICanvasTool> MakeEditTool(const std::vector<std::wstring>& files, const nlohmann::json& params);
    // How Annotate opens: on photos, or on the pages of a PDF (`document`: sizes in points, dark ink),
    // as Fill and sign (`sign`: the text box first, the signatures in view).
    struct AnnotateOptions
    {
        bool document = false;
        bool sign = false;
    };
    std::unique_ptr<canvas::ICanvasTool> MakeAnnotateTool(const std::vector<std::wstring>& files, const AnnotateOptions& options = {});
    std::unique_ptr<canvas::ICanvasTool> MakeBackgroundTool(const std::vector<std::wstring>& files, const nlohmann::json& params);
    std::unique_ptr<canvas::ICanvasTool> MakeCollageTool(const std::vector<std::wstring>& files, const nlohmann::json& params);

    // What the four editors share: the shown picture (a copy of at most 4096 pixels for the preview,
    // with the full size kept), the revision the window redraws on, and the device's bitmaps.
    class PhotoTool : public canvas::ICanvasTool, public canvas::IPreview
    {
    public:
        static constexpr unsigned kPreviewEdge = 4096;

        explicit PhotoTool(std::vector<std::wstring> files) : m_files(std::move(files)) {}

        void ImageReady(size_t index, int width, int height) override;
        canvas::IPreview* Preview() override { return this; }
        uint64_t Revision() const override { return m_revision; }

        void Draw(canvas::IPainter&) const override {}
        canvas::CursorShape CursorAt(const canvas::Pointer&) const override { return canvas::CursorShape::Arrow; }
        canvas::Response PointerDown(const canvas::Pointer&) override { return {}; }
        canvas::Response PointerMove(const canvas::Pointer&) override { return {}; }
        canvas::Response PointerUp(const canvas::Pointer&) override { return {}; }
        canvas::Response PointerCancel() override { return {}; }
        canvas::Response Key(unsigned, bool, bool) override { return {}; }

        // For tests (no window): the current file's picture, as ImageReady would set it.
        void SetPicture(size_t index, std::shared_ptr<const canvas::Raster> full);
        const Picture& Photo() const { return m_photo; }

    protected:
        // The current file's picture is ready (m_photo); `index` is the file.
        virtual void PictureReady(size_t) {}
        // The window's Direct2D device was made again: drop what belongs to the old one.
        virtual void DeviceChanged() {}
        // Call from Render: notices a new device.
        void UseDevice(unsigned device);
        void Touch() { ++m_revision; }

        std::vector<std::wstring> m_files;
        size_t m_index = 0;
        int m_width = 0, m_height = 0;   // the current file's full, oriented size
        Picture m_photo;
        Bitmaps m_bitmaps;
        uint64_t m_revision = 1;
        unsigned m_device = ~0u;
    };

    // Tool 3: light, colour, detail and effects sliders with presets and Auto; press and hold on the
    // picture to see the original. Every file gets the same settings.
    class EditTool final : public PhotoTool
    {
    public:
        EditTool(std::vector<std::wstring> files, const nlohmann::json& params);

        std::wstring Title() const override { return L"Edit Photos"; }
        std::wstring ToolName() const override { return L"Edit"; }
        wchar_t Icon() const override { return 0xE706; }   // Brightness
        std::wstring ApplyLabel() const override { return m_files.size() > 1 ? L"Save all" : L"Save"; }
        canvas::SizeD Size() const override { return { double(m_width), double(m_height) }; }
        bool Render(ID2D1DeviceContext* dc, double scale, unsigned device) override;

        canvas::CursorShape CursorAt(const canvas::Pointer&) const override { return canvas::CursorShape::Hand; }
        canvas::Response PointerDown(const canvas::Pointer& p) override;
        canvas::Response PointerUp(const canvas::Pointer& p) override;
        canvas::Response PointerCancel() override;
        canvas::Response Key(unsigned vk, bool shift, bool ctrl) override;
        void Draw(canvas::IPainter& painter) const override;
        void BuildPanel(std::vector<canvas::Control>& out) const override;
        canvas::Response OnControl(const canvas::ControlEvent& e) override;

        nlohmann::json State() const override;
        void Restore(const nlohmann::json& state) override;
        bool CanApply(std::wstring* why) const override;
        nlohmann::json Params() const override;

        const Adjustments& Current() const { return m_adjust; }
        bool Comparing() const { return m_compare; }

    protected:
        void PictureReady(size_t index) override;
        void DeviceChanged() override;

    private:
        Adjustments m_adjust;
        std::wstring m_group = L"Light";
        bool m_compare = false;
        std::shared_ptr<const HazeModel> m_haze;
        fx::AdjustGraph m_graph;
        CpuCopy m_cpu;
    };

    // Tool 4: arrows, text boxes, shapes, callouts, numbered labels, magnifiers, inserted pictures,
    // signatures, ticks and crosses, the date, pen and highlighter. Each picture gets its own marks.
    //
    // A text box is typed in place: a caret, a selection (Shift+arrows, the mouse, a double click for
    // a word), the clipboard, and its font, size, weight, slant, underline, colour and alignment for
    // the whole box. Its handles move and resize it: with Auto-wrap the lines break at the box's
    // width, without it the box fits the text and a corner scales the type.
    class AnnotateTool final : public PhotoTool
    {
    public:
        enum class Mode { Select, Text, Date, Check, Cross, Arrow, Rect, Ellipse, Callout, Number, Magnifier, Pen, Highlighter, Place };

        explicit AnnotateTool(std::vector<std::wstring> files, AnnotateOptions options = {});

        std::wstring Title() const override;
        std::wstring ToolName() const override;
        wchar_t Icon() const override { return 0xE70F; }   // Edit
        std::wstring ApplyLabel() const override { return m_files.size() > 1 ? L"Save all" : L"Save"; }
        bool PerFile() const override { return true; }
        void ImageReady(size_t index, int width, int height) override;
        canvas::SizeD Size() const override { return { double(m_width), double(m_height) }; }
        bool Render(ID2D1DeviceContext* dc, double scale, unsigned device) override;

        void Draw(canvas::IPainter& painter) const override;
        canvas::CursorShape CursorAt(const canvas::Pointer& p) const override;
        canvas::Response PointerDown(const canvas::Pointer& p) override;
        canvas::Response PointerMove(const canvas::Pointer& p) override;
        canvas::Response PointerUp(const canvas::Pointer& p) override;
        canvas::Response PointerCancel() override;
        canvas::Response Hover(const canvas::Pointer& p) override;
        canvas::Response Key(unsigned vk, bool shift, bool ctrl) override;
        bool Escape() override;
        bool TextInput() const override { return m_editing != 0; }
        canvas::Response Char(wchar_t ch) override;
        void BuildPanel(std::vector<canvas::Control>& out) const override;
        canvas::Response OnControl(const canvas::ControlEvent& e) override;

        nlohmann::json State() const override;
        void Restore(const nlohmann::json& state) override;
        bool CanApply(std::wstring* why) const override;
        nlohmann::json Params() const override;

        // For tests and the panel.
        Mode CurrentMode() const { return m_mode; }
        void SetMode(Mode mode);
        Annotations& Marks() { return m_marks[m_index]; }
        uint32_t Selected() const { return m_selected; }
        uint32_t Editing() const { return m_editing; }
        const TextCursor& Cursor() const { return m_cursor; }
        // Places a picture (already loaded) as an Image mark, centred, and selects it.
        uint32_t InsertImage(const std::wstring& path, Picture picture);
        // A picture (a signature) waiting to be placed where the next click lands.
        void BeginPlacing(const std::wstring& path, Picture picture, bool signature);
        // The signatures offered in the panel (tests use a folder of their own).
        void UseSignatures(SignatureStore store);
        // Marks the file had when it was edited before (a PDF page edited again), kept as its starting point.
        void Seed(size_t file, Annotations marks);
        // Today's date as the date mark writes it.
        static std::wstring TodayText();

    protected:
        void PictureReady(size_t index) override;

    private:
        struct TextLayoutCache
        {
            TextLayoutInfo info;
            canvas::PointD origin;
        };

        Annotation Style(Mark kind) const;   // a new mark with the current colour, sizes and type
        void StopEditing();
        void StartEditing(uint32_t id, size_t caret);
        // Changes the selected mark (and remembers the choice for new marks).
        void ApplyStyle(const std::function<void(Annotation&)>& change);
        double Tolerance(const canvas::Pointer& p) const;
        // The editing box's text was changed: lays it out again.
        void Relayout(uint32_t id);
        void RelayoutAll();
        const TextLayoutCache* LayoutOf(uint32_t id) const;
        // Text sizes as the panel shows them: points on a PDF page, pixels on a photo.
        double PixelsPerUnit() const;
        double DefaultTextSize() const;
        canvas::Response ChangeFontSize(int steps);
        canvas::Response TextKey(Annotation& m, unsigned vk, bool shift, bool ctrl);
        canvas::Response StyleKey(unsigned vk, bool shift);
        canvas::Response Place(canvas::PointD at);
        canvas::SizeD PlacedSize(const Picture& picture) const;
        void LoadSignatures();
        void PickSignature(size_t index);
        void AddSignature(bool fromPicture);
        void BuildTextPanel(std::vector<canvas::Control>& out, const Annotation* sel) const;

        AnnotateOptions m_options;
        std::vector<Annotations> m_marks;   // per file
        std::vector<bool> m_seeded;         // per file: started from an earlier edit
        std::map<std::wstring, Picture> m_images;
        std::map<uint32_t, TextLayoutCache> m_layouts;   // the current file's text marks
        Mode m_mode = Mode::Arrow;
        uint32_t m_color = 0xE81123;
        uint32_t m_custom = 0;
        double m_stroke = 0;   // 0 = the picture's default
        double m_font = 0;
        bool m_fill = false;
        double m_zoom = 2.5;
        // The type new text boxes get.
        std::wstring m_family = L"Segoe UI";
        int m_weight = 400;
        bool m_italic = false, m_underline = false, m_wrap = false;
        Align m_align = Align::Left;
        double m_dpi = 0;   // the picture's resolution (PDF pages: points from pixels)

        uint32_t m_selected = 0;
        uint32_t m_editing = 0;
        TextCursor m_cursor;
        double m_goalX = -1;
        std::wstring m_clipboard;   // without a window (tests)

        // The picture waiting to be placed (Mode::Place) and where the pointer is.
        std::wstring m_placePath;
        Picture m_placePicture;
        bool m_placeSignature = false;
        canvas::PointD m_hover;
        bool m_hovering = false;

        SignatureStore m_store = SignatureStore::Default();
        std::vector<std::wstring> m_signatures;

        // The drag in progress.
        uint32_t m_dragId = 0;
        Part m_dragPart = Part::None;
        Annotation m_before;
        canvas::PointD m_from;
        bool m_created = false, m_moved = false, m_wasSelected = false;
        bool m_selecting = false;      // dragging a selection through the text being typed
        bool m_clickEdits = false;     // a click (no drag) on this text box starts typing in it
    };

    // Tool 5: the picture on a solid colour, a gradient, its own blur or another picture, with an
    // output shape, spacing, rounded corners and a shadow. Every file gets the same settings.
    class BackgroundTool final : public PhotoTool
    {
    public:
        BackgroundTool(std::vector<std::wstring> files, const nlohmann::json& params);

        std::wstring Title() const override { return L"Add a Background"; }
        std::wstring ToolName() const override { return L"Background"; }
        wchar_t Icon() const override { return 0xE790; }   // Color
        std::wstring ApplyLabel() const override { return m_files.size() > 1 ? L"Save all" : L"Save"; }
        canvas::SizeD Size() const override;
        bool Render(ID2D1DeviceContext* dc, double scale, unsigned device) override;

        void BuildPanel(std::vector<canvas::Control>& out) const override;
        canvas::Response OnControl(const canvas::ControlEvent& e) override;

        nlohmann::json State() const override;
        void Restore(const nlohmann::json& state) override;
        nlohmann::json Params() const override;

        const BackgroundParams& Current() const { return m_params; }
        // For tests: the background picture, as the picker would load it.
        void SetBackgroundImage(const std::wstring& path, Picture picture);

    protected:
        void PictureReady(size_t) override;
        void DeviceChanged() override { m_cache = {}; }

    private:
        void LoadBackgroundImage(const std::wstring& path);

        BackgroundParams m_params;
        uint32_t m_custom1 = 0, m_custom2 = 0;
        Picture m_backImage;
        std::wstring m_backPath;
        BackgroundCache m_cache;
    };

    // Tool 8: every dropped picture in one, as a grid, a row, a column or one featured with the rest
    // beside it. Drag a photo onto another to swap them.
    class CollageTool final : public PhotoTool
    {
    public:
        CollageTool(std::vector<std::wstring> files, const nlohmann::json& params);

        std::wstring Title() const override { return L"Create a Collage"; }
        std::wstring ToolName() const override { return L"Collage"; }
        wchar_t Icon() const override { return 0xE80A; }   // Tiles
        std::wstring ApplyLabel() const override { return L"Save"; }
        canvas::SizeD Size() const override;
        bool Render(ID2D1DeviceContext* dc, double scale, unsigned device) override;

        void Draw(canvas::IPainter& painter) const override;
        canvas::CursorShape CursorAt(const canvas::Pointer& p) const override;
        canvas::Response PointerDown(const canvas::Pointer& p) override;
        canvas::Response PointerMove(const canvas::Pointer& p) override;
        canvas::Response PointerUp(const canvas::Pointer& p) override;
        canvas::Response PointerCancel() override;
        void BuildPanel(std::vector<canvas::Control>& out) const override;
        canvas::Response OnControl(const canvas::ControlEvent& e) override;

        nlohmann::json State() const override;
        void Restore(const nlohmann::json& state) override;
        bool CanApply(std::wstring* why) const override;
        nlohmann::json Params() const override;

        const CollageParams& Current() const { return m_params; }
        const CollageLayout& Layout() const { return m_layout; }
        // For tests: the pictures, as the loader would set them (file order).
        void SetPictures(std::vector<Picture> pictures);

    protected:
        void PictureReady(size_t index) override;

    private:
        void Relayout();
        bool Loaded() const { return m_pictures.size() == m_files.size() && !m_pictures.empty(); }

        CollageParams m_params;
        uint32_t m_custom = 0;
        std::vector<Picture> m_pictures;
        bool m_loading = false;
        CollageLayout m_layout;
        int m_dragCell = -1, m_overCell = -1;
    };
}
