#pragma once
#include "mediaeditor/EditorApi.h"
#include "mediaeditor/EditorLayout.h"
#include "mediaeditor/EditorUi.h"
#include "mediaeditor/MediaLoader.h"
#include "mediaeditor/MediaProbe.h"
#include "mediaeditor/MediaTools.h"
#include "mediaeditor/Playback.h"
#include "mediaeditor/Timeline.h"

#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wil/resource.h>
#include <wrl/client.h>

#include <map>
#include <memory>
#include <string>
#include <vector>

// The timeline editor: one Direct2D window for every video and audio tool that needs a choice first.
// A preview (the frame at the playhead, with crop and redaction boxes drawn on it) or a waveform, a
// zoomable timeline with a filmstrip, range handles and markers, and the tool's options on the right.
// Everything slow (probing, frames, the waveform, decoding sound) happens on MediaLoader / Playback
// threads; the window only draws what has arrived.
namespace sd::mediatools
{
    class EditorWindow
    {
    public:
        static void Open(HWND owner, EditorRequest request, EditorDone done);
        EditorWindow(EditorRequest request, EditorDone done);   // use Open
        ~EditorWindow();
        EditorWindow(const EditorWindow&) = delete;
        EditorWindow& operator=(const EditorWindow&) = delete;

    private:
        using Bitmap = Microsoft::WRL::ComPtr<ID2D1Bitmap>;

        struct Picture   // CPU pixels kept so bitmaps can be rebuilt after a device loss
        {
            Frame frame;
            Bitmap bitmap;
            int64_t us = 0;
        };
        struct Area      // one redaction box
        {
            PixelRect box;
            TimeRange range;
            bool blur = true;
        };
        enum class Status { Open, Done, Skipped };
        struct FileState
        {
            std::wstring path, name;
            bool probed = false;
            std::wstring error;            // why this file cannot be edited (no video stream...)
            ProbeInfo info;
            bool ready = false;            // probed, usable, defaults set
            Status status = Status::Open;
            // The user's choices for this file.
            TimeRange keep;                // trim
            PixelRect crop;
            int outW = 0, outH = 0;        // crop output size (0 = the box's own)
            std::vector<int64_t> marks;    // frames to save, cut points
            std::vector<Area> areas;       // redaction
            std::vector<TimeRange> ranges; // bleeps
            // Loaded media.
            std::shared_ptr<const PeakPyramid> peaks;
            double waveProgress = 0;
            bool waveRequested = false, waveFailed = false;
            Picture joinThumb;
            bool joinThumbRequested = false;
            int64_t playhead = 0;
        };

        enum class Drag
        {
            None, Scrub, Edge, Body, NewRange, Marker, Scroll, Slider, CropHandle, CropNew, AreaHandle, AreaNew, JoinRow, Pressed
        };

        struct TextItem
        {
            RectF rect;
            std::wstring text;
            ui::Font font = ui::Font::Body;
            int tone = 0;      // 0 text, 1 dim, 2 faint, 3 danger
            int align = 0;
            bool wrap = false;
        };

        // The filmstrip at the current zoom: thumbnail slots of `slotUs` on a grid from 0, a finer grid
        // (level) each time the zoom doubles, so thumbnails are reused while zooming.
        struct Strip
        {
            bool valid = false;
            int level = 0;
            double slotUs = 0;
            int64_t first = 0, last = 0;
        };

        bool Create();
        static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
        LRESULT Handle(UINT msg, WPARAM wParam, LPARAM lParam);

        // ---- Tool traits ----
        bool Video() const { return IsVideoTool(m_req.tool); }
        bool PerFile() const { return IsPerFileTool(m_req.tool); }
        bool HasTimeline() const;
        bool HasStage() const;              // the big picture area (video preview, visualizer)
        bool UsesWaveform() const;
        bool CanPlay() const;
        FileState& Cur() { return m_files[static_cast<size_t>(m_current)]; }
        const FileState& Cur() const { return m_files[static_cast<size_t>(m_current)]; }
        int64_t Duration() const { return Cur().ready ? Cur().info.DurationUs() : 0; }
        double Fps() const { return Video() && Cur().ready ? Cur().info.media.fps : 0; }

        // ---- Files and loading (EditorWindow.cpp) ----
        void OnLoaded();
        void InitChoices(FileState& file);  // defaults once a file is probed (EditorTools.cpp)
        void ShowFile(int index);
        void FileReady();
        void RequestPreview();
        void RequestThumbs();
        Strip StripNow() const;
        int64_t SlotTime(const Strip& s, int64_t slot) const;
        static Frame Downscale(const Frame& frame, int target);
        void RequestWaveform(int index);
        void Advance(bool skip);
        void Finish();
        void Close();

        // ---- Layout, controls and painting ----
        void Layout();
        void Rebuild();                     // controls and texts from the current state
        void BuildToolBar();                // the header's tabs: every tool of this kind
        void SwitchTool(EditorTool tool);
        void BuildTransport();
        void BuildFooter();
        void BuildPanel();                  // the tool's options (EditorTools.cpp)
        std::wstring PrimaryLabel() const;  // EditorTools.cpp
        bool PrimaryEnabled() const;        // EditorTools.cpp
        std::wstring Hint() const;          // keyboard help for the footer (EditorTools.cpp)
        void Invalidate();

        bool EnsureTarget();
        void DiscardDevice();
        void Paint();
        void PaintHeader(ui::Painter& p);
        void PaintStage(ui::Painter& p);
        void PaintCropOverlay(ui::Painter& p, const PreviewMap& map);
        void PaintAreas(ui::Painter& p, const PreviewMap& map);
        void PaintVisualizer(ui::Painter& p);
        void PaintJoinList(ui::Painter& p);
        void PaintTimeline(ui::Painter& p);
        void PaintFilmstrip(ui::Painter& p);
        void PaintWaveform(ui::Painter& p);
        void PaintTrackOverlay(ui::Painter& p);
        void PaintFooter(ui::Painter& p);
        ID2D1Bitmap* BitmapOf(Picture& picture);
        void DrawCover(ui::Painter& p, ID2D1Bitmap* bitmap, const RectF& slot, float opacity = 1.f);
        void Handles(ui::Painter& p, const RectF& r, bool edges);

        // ---- Input (EditorWindow.cpp; tool-specific parts in EditorTools.cpp) ----
        float Dip(int px) const { return static_cast<float>(px) * 96.f / static_cast<float>(m_dpi); }
        void OnMouseDown(float x, float y, bool doubleClick);
        void OnMouseMove(float x, float y);
        void OnMouseUp(float x, float y);
        void OnWheel(float x, float y, int delta, bool horizontal);
        bool OnKey(UINT vk);
        void OnChar(wchar_t ch);
        LPCWSTR CursorAt(float x, float y) const;
        int ControlAt(float x, float y) const;
        const ui::Control* ControlById(int id) const;
        void Focus(int id);
        void MoveFocus(bool back);
        void StartEdit(const ui::Control& c);
        bool CommitEdit(bool keepOnError);
        void CancelEdit();
        bool TrackMouseDown(float x, float y, bool doubleClick);   // EditorTools.cpp
        bool StageMouseDown(float x, float y);                     // EditorTools.cpp
        void DragTo(float x, float y);                             // EditorTools.cpp
        void EndDrag();
        void ToolDragEnd(Drag was);                                // EditorTools.cpp
        bool ToolKey(UINT vk, bool shift, bool ctrl);              // EditorTools.cpp
        PreviewMap StageMap() const;

        // ---- Timeline helpers ----
        float TX(int64_t us) const { return m_rcTrack.x + static_cast<float>(m_view.XOf(us)); }
        int64_t TT(float x) const { return m_view.TimeAt(x - m_rcTrack.x); }
        void SetPlayhead(int64_t us, bool follow = true);
        void ZoomBy(double factor, double x);
        void ViewChanged();
        RectF TimelineRect() const;   // ruler + track
        // Ranges shown with handles on the track for this tool (trim: the kept part).
        std::vector<TimeRange> EditableRanges() const;            // EditorTools.cpp
        void SetEditableRange(size_t index, TimeRange r);         // EditorTools.cpp
        int SelectedRange() const;                                // EditorTools.cpp

        // ---- Playback ----
        void TogglePlay(bool selectionOnly);
        void StartPlay(int64_t from, int64_t to);
        void StopPlay();
        Playback::FilterFn PlayFilter() const;                    // EditorTools.cpp
        double PlayRate() const;                                  // EditorTools.cpp
        void OnPlayTick();

        // ---- Results ----
        nlohmann::json SharedParams() const;                      // EditorTools.cpp
        nlohmann::json FileParams(const FileState& file) const;   // EditorTools.cpp
        void ChoosePicture();                                     // EditorTools.cpp

        EditorRequest m_req;                // m_req.tool and title follow the tool bar
        EditorTool m_startTool{};          // the one the recipe asked for: its params apply to it only
        float m_toolBarRight = 0;           // where the header's file details start (DIPs)
        EditorDone m_done;
        std::vector<FileState> m_files;
        int m_current = 0;

        wil::unique_hwnd m_hwnd;
        UINT m_dpi = 96;
        float m_w = 0, m_h = 0;   // client size in DIPs
        Microsoft::WRL::ComPtr<ID2D1Factory> m_d2d;
        Microsoft::WRL::ComPtr<IDWriteFactory> m_dwrite;
        Microsoft::WRL::ComPtr<ID2D1HwndRenderTarget> m_rt;
        Microsoft::WRL::ComPtr<ID2D1StrokeStyle> m_dash;
        ui::Painter m_painter;
        ui::Colors m_colors;

        TempFolder m_temp;
        std::unique_ptr<MediaLoader> m_loader;
        std::unique_ptr<Playback> m_playback;

        // Layout (DIPs).
        RectF m_rcHeader, m_rcFooter, m_rcPanel, m_rcStage, m_rcTransport, m_rcRuler, m_rcTrack, m_rcScroll, m_rcList;
        std::vector<ui::Control> m_controls;
        std::vector<TextItem> m_texts;

        // Timeline.
        TimelineView m_view;
        int64_t m_playhead = 0;
        float m_hoverX = -1;   // over the timeline, for the hover line

        // Preview frame and thumbnails of the current file.
        Picture m_preview;
        Picture m_blur;              // a tiny copy of the preview for blur boxes
        uint64_t m_previewSerial = 0, m_previewShown = 0;
        bool m_previewWanted = false, m_previewInFlight = false;
        std::map<uint64_t, Picture> m_thumbs;   // key: level << 32 | slot
        int m_thumbW = 0, m_thumbH = 0;         // pixels

        // Selection and dragging.
        int m_selected = -1;     // range, marker, area or join row
        Drag m_drag = Drag::None;
        int m_dragIndex = -1;
        int m_dragHandle = -1;
        bool m_dragStartEdge = false;
        float m_downX = 0, m_downY = 0;
        int64_t m_downTime = 0;
        TimeRange m_dragRange;
        PixelRect m_dragBox;
        bool m_dragMoved = false;
        int m_joinDropAt = -1;

        // Controls.
        int m_hover = 0, m_pressed = 0, m_focus = 0;   // control ids; 0 = none / the timeline
        int m_editId = 0;
        std::wstring m_editText;
        size_t m_editCaret = 0;
        bool m_editAll = false;
        int m_invalidId = 0;
        float m_listScroll = 0;      // join list / side list, DIPs

        // Shared choices (all files).
        bool m_fast = false;         // trim / split by stream copy
        std::string m_aspect = "free";
        double m_factor = 2.0;       // speed
        bool m_equalParts = false;
        bool m_joinFill = false;     // join: crop other shapes to fill the frame (else black bars)
        int m_parts = 2;
        bool m_blurStyle = true;     // redact default
        bool m_tone = true;          // bleep with a tone (else silence)
        bool m_silence = false;      // audio trim: also cut silence at the ends
        bool m_mono = false, m_swap = false;
        double m_left = 1.0, m_right = 1.0;
        bool m_imageStyle = false;
        std::string m_shape = "landscape";
        std::wstring m_image;
        Picture m_imagePicture;
        bool m_listenProcessed = true;   // A/B: hear the result (else the original)
        std::vector<int> m_order;        // join: clip order (indices into m_files)

        // Playback.
        bool m_playing = false;
        int64_t m_playEnd = 0;
    };
}
