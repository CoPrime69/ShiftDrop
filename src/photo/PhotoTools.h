#pragma once
#include "canvas/CanvasTool.h"
#include "photo/Compose.h"

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
    std::unique_ptr<canvas::ICanvasTool> MakeAnnotateTool(const std::vector<std::wstring>& files);
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

    // Tool 4: arrows, text, shapes, callouts, numbered labels, magnifiers, inserted pictures, pen and
    // highlighter. Each picture gets its own marks.
    class AnnotateTool final : public PhotoTool
    {
    public:
        enum class Mode { Select, Arrow, Text, Rect, Ellipse, Callout, Number, Magnifier, Pen, Highlighter };

        explicit AnnotateTool(std::vector<std::wstring> files);

        std::wstring Title() const override { return L"Annotate Photos"; }
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
        canvas::Response Key(unsigned vk, bool shift, bool ctrl) override;
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
        // Places a picture (already loaded) as an Image mark, centred, and selects it.
        uint32_t InsertImage(const std::wstring& path, Picture picture);

    protected:
        void PictureReady(size_t index) override;

    private:
        Annotation Style(Mark kind) const;   // a new mark with the current colour and sizes
        void StopEditing();
        void ApplyStyle(const std::function<void(Annotation&)>& change);
        double Tolerance(const canvas::Pointer& p) const;

        std::vector<Annotations> m_marks;   // per file
        std::map<std::wstring, Picture> m_images;
        Mode m_mode = Mode::Arrow;
        uint32_t m_color = 0xE81123;
        uint32_t m_custom = 0;
        double m_stroke = 0;   // 0 = the picture's default
        double m_font = 0;
        bool m_fill = false;
        double m_zoom = 2.5;
        uint32_t m_selected = 0;
        uint32_t m_editing = 0;
        // The drag in progress.
        uint32_t m_dragId = 0;
        Part m_dragPart = Part::None;
        Annotation m_before;
        PointD m_from;
        bool m_created = false, m_moved = false, m_wasSelected = false;
    };

    // Tool 5: the picture on a solid colour, a gradient, its own blur or another picture, with an
    // output shape, spacing, rounded corners and a shadow. Every file gets the same settings.
    class BackgroundTool final : public PhotoTool
    {
    public:
        BackgroundTool(std::vector<std::wstring> files, const nlohmann::json& params);

        std::wstring Title() const override { return L"Add a Background"; }
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
