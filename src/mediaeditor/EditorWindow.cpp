#include "mediaeditor/EditorWindow.h"
#include "Brand.h"
#include "Log.h"
#include "Utf.h"
#include "videotools/VideoTools.h"

#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <format>

using Microsoft::WRL::ComPtr;

namespace sd::mediatools
{
    using theme::WithAlpha;

    namespace
    {
        constexpr UINT kLoadedMsg = WM_APP + 41;
        constexpr UINT kCloseMsg = WM_APP + 42;   // closing is posted, so no handler runs on a deleted editor
        constexpr UINT_PTR kTimerThumbs = 2, kTimerPlay = 3;

        // Layout, in DIPs.
        constexpr float kHeaderH = 56, kFooterH = 64, kPanelW = 316, kPad = 16;
        constexpr float kTransportH = 40, kRulerH = 24, kFilmH = 64, kScrollH = 10;

        // Control ids shared by every tool (tool controls use 100 and up).
        enum : int { kIdPlay = 1, kIdTime, kIdBack, kIdForward, kIdZoomOut, kIdZoomIn, kIdFit, kIdSkip, kIdCancel, kIdPrimary, kIdTools = 50 };

        std::wstring FileName(const std::wstring& path)
        {
            const auto slash = path.find_last_of(L"\\/");
            return slash == std::wstring::npos ? path : path.substr(slash + 1);
        }

        void OpenEditorWindow(HWND owner, EditorRequest request, EditorDone done) { EditorWindow::Open(owner, std::move(request), std::move(done)); }

        // The app links this file in; the engines in shiftdrop_core find the editor through this.
        const bool kRegistered = (SetEditorOpener(&OpenEditorWindow), true);
    }

    // ---- Lifetime -------------------------------------------------------------------------------------

    void EditorWindow::Open(HWND, EditorRequest request, EditorDone done)
    {
        (void)kRegistered;
        auto window = std::make_unique<EditorWindow>(std::move(request), std::move(done));
        if (!window->Start())
        {
            log::Error(L"media editor: could not open the window for '{}'", window->m_req.title);
            return;
        }
        window.release();   // owns itself now: deleted when its window goes
    }

    // An opaque target: text in ClearType like the rest of Windows, and frames shown without alpha.
    EditorWindow::EditorWindow(EditorRequest request, EditorDone done) : EditorFrame(true), m_req(std::move(request)), m_done(std::move(done))
    {
        m_startTool = m_req.tool;
        for (const auto& path : m_req.files)
        {
            FileState f;
            f.path = path;
            f.name = FileName(path);
            m_files.push_back(std::move(f));
        }
        if (m_files.empty())
            m_files.emplace_back();
        for (int i = 0; i < static_cast<int>(m_files.size()); ++i)
            m_order.push_back(i);
        // Shared choices start from the recipe's parameters (a user recipe can preset them).
        const auto& p = m_req.params;
        m_fast = p.value("mode", std::string("exact")) == "fast";
        m_aspect = p.value("aspect", std::string("free"));
        m_factor = std::clamp(p.value("factor", 2.0), 0.25, 4.0);
        m_parts = std::clamp(p.value("parts", 0), 0, 100);
        m_equalParts = m_parts >= 2;
        m_joinFill = p.value("fit", std::string("fit")) == "fill";
        if (m_parts < 2)
            m_parts = 2;
        m_blurStyle = p.value("style", std::string("blur")) != "solid";
        m_tone = p.value("tone", std::string("tone")) != "silence";
        m_silence = p.value("silence", false);
        m_mono = p.value("mode", std::string("stereo")) == "mono";
        m_swap = p.value("swap", false);
        m_left = std::clamp(p.value("left", 1.0), 0.0, 2.0);
        m_right = std::clamp(p.value("right", 1.0), 0.0, 2.0);
        m_imageStyle = p.value("style", std::string("waveform")) == "image";
        m_shape = p.value("shape", std::string("landscape"));
        m_image = Widen(p.value("image", std::string()));
    }

    EditorWindow::~EditorWindow()
    {
        // Threads first (they use the temp folder), then the folder goes with m_temp.
        m_playback.reset();
        m_loader.reset();
    }

    bool EditorWindow::Start()
    {
        if (!m_painter.Init(DWrite(), [this](float size, DWRITE_FONT_WEIGHT weight, bool icon) { return icon ? IconFormat(size) : TextFormat(size, weight); }))
            return false;
        const float dashes[] = { 4.f, 3.f };
        Factory()->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT, D2D1_LINE_JOIN_MITER, 10.f,
                                                                 D2D1_DASH_STYLE_CUSTOM, 0.f),
                                     dashes, 2, &m_dash);

        // Sized for the tool, within 94% x 92% of the work area of the monitor it opens on.
        const auto monitor = CursorMonitor();
        const float scale = static_cast<float>(monitor.dpi) / 96.f;
        float wantW = Video() ? 1200.f : 1080.f, wantH = Video() ? 780.f : 660.f;
        if (m_req.tool == EditorTool::JoinVideos)
            wantW = 960, wantH = 640;
        if (m_req.tool == EditorTool::AudioVisualizer)
            wantW = 1040, wantH = 660;
        const auto& work = monitor.work;
        const int w = static_cast<int>(std::min(wantW, static_cast<float>(work.right - work.left) * 0.94f / scale));
        const int h = static_cast<int>(std::min(wantH, static_cast<float>(work.bottom - work.top) * 0.92f / scale));
        if (!Create(m_req.title + L" \u2013 " SD_NAME, w, h, 820, 540, false))
            return false;

        m_loader = std::make_unique<MediaLoader>(m_req.ffmpeg, &m_temp, Hwnd(), kLoadedMsg);
        if (HasTimeline())
            m_playback = std::make_unique<Playback>(m_req.ffmpeg, &m_temp);
        // Probe the first file first: it is the one shown.
        for (int i = 0; i < static_cast<int>(m_files.size()); ++i)
            if (!m_files[static_cast<size_t>(i)].path.empty())
                m_loader->Probe(i, m_files[static_cast<size_t>(i)].path);

        Layout(ClientDip());
        Present();
        log::Info(L"media editor: {} with {} file(s)", m_req.title, m_req.files.size());
        return true;
    }

    void EditorWindow::RequestClose()
    {
        if (Hwnd())
            PostMessageW(Hwnd(), kCloseMsg, 0, 0);
    }

    void EditorWindow::Layout(D2D1_SIZE_F size)
    {
        m_w = size.width;
        m_h = size.height;
        Layout();
        Rebuild();
    }

    void EditorWindow::OnDpiChanged()
    {
        m_thumbs.clear();   // thumbnails were sized for the old DPI
        if (Cur().ready)
            FileReady();
    }

    bool EditorWindow::OnMessage(UINT msg, WPARAM wParam, LPARAM lParam, LRESULT& result)
    {
        const HWND hwnd = Hwnd();
        result = 0;
        switch (msg)
        {
        case WM_LBUTTONDOWN:
        case WM_LBUTTONDBLCLK:
            SetFocus(hwnd);
            SetCapture(hwnd);
            OnMouseDown(Dip(GET_X_LPARAM(lParam)), Dip(GET_Y_LPARAM(lParam)), msg == WM_LBUTTONDBLCLK);
            return true;
        case WM_MOUSEMOVE:
        {
            TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hwnd, 0 };
            TrackMouseEvent(&tme);
            OnMouseMove(Dip(GET_X_LPARAM(lParam)), Dip(GET_Y_LPARAM(lParam)));
            return true;
        }
        case WM_LBUTTONUP:
            OnMouseUp(Dip(GET_X_LPARAM(lParam)), Dip(GET_Y_LPARAM(lParam)));
            if (GetCapture() == hwnd)
                ReleaseCapture();
            return true;
        case WM_CAPTURECHANGED:
            if (m_drag != Drag::None && reinterpret_cast<HWND>(lParam) != hwnd)
                EndDrag();
            return true;
        case WM_MOUSELEAVE:
            if (m_hover || m_hoverX >= 0)
            {
                m_hover = 0;
                m_hoverX = -1;
                Invalidate();
            }
            return true;
        case WM_MOUSEWHEEL:
        case WM_MOUSEHWHEEL:
        {
            POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            ScreenToClient(hwnd, &pt);
            OnWheel(Dip(pt.x), Dip(pt.y), GET_WHEEL_DELTA_WPARAM(wParam), msg == WM_MOUSEHWHEEL);
            return true;
        }
        case WM_SETCURSOR:
            if (LOWORD(lParam) == HTCLIENT)
            {
                POINT pt{};
                GetCursorPos(&pt);
                ScreenToClient(hwnd, &pt);
                SetCursor(LoadCursorW(nullptr, CursorAt(Dip(pt.x), Dip(pt.y))));
                result = TRUE;
                return true;
            }
            break;
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            if (OnKey(static_cast<UINT>(wParam)))
                return true;
            break;
        case WM_CHAR:
            OnChar(static_cast<wchar_t>(wParam));
            return true;
        case WM_TIMER:
            if (wParam == kTimerThumbs)
            {
                KillTimer(hwnd, kTimerThumbs);
                RequestThumbs();
            }
            else if (wParam == kTimerPlay)
                OnPlayTick();
            return true;
        case kLoadedMsg:
            OnLoaded();
            return true;
        case kCloseMsg:
            Close();   // deletes this editor
            return true;
        case WM_ACTIVATE:
            if (LOWORD(wParam) == WA_INACTIVE && m_drag != Drag::None)
                EndDrag();
            break;
        case WM_DESTROY:
            StopPlay();
            m_playback.reset();
            m_loader.reset();   // stops ffmpeg and waits for the threads
            return true;
        }
        return false;
    }

    // ---- Tool traits ----------------------------------------------------------------------------------

    bool EditorWindow::HasTimeline() const { return m_req.tool != EditorTool::JoinVideos && m_req.tool != EditorTool::AudioVisualizer; }

    bool EditorWindow::HasStage() const { return (Video() && HasTimeline()) || m_req.tool == EditorTool::AudioVisualizer; }

    bool EditorWindow::UsesWaveform() const { return !Video(); }

    bool EditorWindow::CanPlay() const { return HasTimeline() && m_playback && Cur().ready; }

    // ---- Loading ----------------------------------------------------------------------------------------

    void EditorWindow::OnLoaded()
    {
        if (!m_loader)
            return;
        bool rebuild = false;
        for (auto& r : m_loader->Take())
        {
            if (r.file < 0 || r.file >= static_cast<int>(m_files.size()))
                continue;
            auto& f = m_files[static_cast<size_t>(r.file)];
            switch (r.kind)
            {
            case LoadResult::Kind::Probe:
                f.probed = true;
                f.info = std::move(r.info);
                if (Video() && !f.info.HasVideo())
                    f.error = L"This file has no video to edit.";
                else if (!Video() && !f.info.HasAudio())
                    f.error = L"This file has no sound to edit.";
                else if (f.info.DurationUs() <= 0)
                    f.error = L"Cannot tell how long this file is.";
                else
                {
                    f.ready = true;
                    InitChoices(f);
                }
                if (!f.error.empty())
                    log::Warn(L"media editor: {}: {}", f.path, f.error);
                if (r.file == m_current)
                    FileReady();
                if (m_req.tool == EditorTool::JoinVideos && f.ready && !f.joinThumbRequested)
                {
                    f.joinThumbRequested = true;
                    const int h = MulDiv(54, static_cast<int>(Dpi()), 96);
                    m_loader->Thumbs(r.file, f.path, f.info, h * 2, h, { { std::min<int64_t>(500'000, f.info.DurationUs() / 3), 0, true } }, false);
                }
                if (m_req.tool == EditorTool::AudioVisualizer && r.file == 0 && f.ready)
                    RequestWaveform(0);
                rebuild = true;
                break;
            case LoadResult::Kind::Thumb:
                if (r.frame.w <= 0)
                    break;
                if (m_req.tool == EditorTool::JoinVideos)
                    f.joinThumb = { std::move(r.frame), nullptr, r.us };
                else if (r.file == m_current)
                    m_thumbs[r.key] = { std::move(r.frame), nullptr, r.us };
                break;
            case LoadResult::Kind::Preview:
                if (r.file == m_current)
                {
                    m_previewInFlight = false;
                    if (r.frame.w > 0 && r.key >= m_previewShown)
                    {
                        m_previewShown = r.key;
                        m_preview = { std::move(r.frame), nullptr, r.us };
                        if (m_req.tool == EditorTool::RedactVideo)
                            m_blur = { Downscale(m_preview.frame, 24), nullptr, r.us };
                    }
                    if (m_previewWanted)
                        RequestPreview();
                }
                break;
            case LoadResult::Kind::Waveform:
                f.peaks = std::move(r.peaks);
                f.waveFailed = !f.peaks;
                break;
            case LoadResult::Kind::WaveformProgress:
                f.waveProgress = r.progress;
                break;
            }
        }
        if (rebuild)
            Rebuild();
        Invalidate();
    }

    Frame EditorWindow::Downscale(const Frame& frame, int target)
    {
        // Box-average to about `target` pixels across: drawn back up with linear filtering it reads as
        // a strong blur, close enough to what the redaction will look like.
        Frame out;
        if (frame.w <= 0 || frame.h <= 0)
            return out;
        const int step = std::max(1, frame.w / std::max(8, target));
        out.w = std::max(1, frame.w / step);
        out.h = std::max(1, frame.h / step);
        out.bgra.resize(static_cast<size_t>(out.w) * out.h);
        for (int y = 0; y < out.h; ++y)
            for (int x = 0; x < out.w; ++x)
            {
                uint32_t sum[3]{};
                for (int dy = 0; dy < step; ++dy)
                    for (int dx = 0; dx < step; ++dx)
                    {
                        const uint32_t p = frame.bgra[static_cast<size_t>(y * step + dy) * frame.w + static_cast<size_t>(x * step + dx)];
                        sum[0] += p & 0xFF;
                        sum[1] += (p >> 8) & 0xFF;
                        sum[2] += (p >> 16) & 0xFF;
                    }
                const uint32_t n = static_cast<uint32_t>(step * step);
                out.bgra[static_cast<size_t>(y) * out.w + static_cast<size_t>(x)] = 0xFF000000u | ((sum[2] / n) << 16) | ((sum[1] / n) << 8) | (sum[0] / n);
            }
        return out;
    }

    void EditorWindow::FileReady()
    {
        auto& f = Cur();
        m_thumbs.clear();
        m_preview = {};
        m_blur = {};
        m_previewShown = 0;
        m_previewWanted = false;
        m_selected = -1;
        m_view = {};
        if (!f.ready)
        {
            Rebuild();
            Invalidate();
            return;
        }
        m_view.durationUs = f.info.DurationUs();
        m_view.widthPx = std::max(1.f, m_rcTrack.w);
        m_view.Fit();
        m_playhead = std::clamp<int64_t>(f.playhead, 0, f.info.DurationUs());
        if (Video())
        {
            const double aspect = f.info.height > 0 ? f.info.width * f.info.sar / f.info.height : 16.0 / 9;
            m_thumbH = MulDiv(static_cast<int>(kFilmH), static_cast<int>(Dpi()), 96);
            m_thumbW = std::clamp(static_cast<int>(std::lround(m_thumbH * aspect)), m_thumbH / 2, m_thumbH * 3);
            RequestThumbs();
            RequestPreview();
        }
        if (UsesWaveform())
            RequestWaveform(m_current);
        if (m_req.tool == EditorTool::BleepAudio || m_req.tool == EditorTool::RedactVideo || m_req.tool == EditorTool::SaveFrames)
            m_selected = -1;
        Rebuild();
        Invalidate();
    }

    void EditorWindow::ShowFile(int index)
    {
        StopPlay();
        CancelEdit();
        m_current = std::clamp(index, 0, static_cast<int>(m_files.size()) - 1);
        FileReady();
    }

    void EditorWindow::RequestWaveform(int index)
    {
        auto& f = m_files[static_cast<size_t>(index)];
        if (!f.ready || f.waveRequested || !m_loader)
            return;
        f.waveRequested = true;
        m_loader->Waveform(index, f.path, f.info.DurationUs());
    }

    void EditorWindow::RequestPreview()
    {
        if (!Video() || !HasTimeline() || !Cur().ready || !m_loader)
            return;
        // One request at a time: while a frame is being read, remember that a newer one is wanted.
        if (m_previewInFlight)
        {
            m_previewWanted = true;
            return;
        }
        m_previewWanted = false;
        m_previewInFlight = true;
        const float scale = static_cast<float>(Dpi()) / 96.f;
        const int maxW = std::clamp(static_cast<int>(m_rcStage.w * scale), 64, 2560);
        const int maxH = std::clamp(static_cast<int>(m_rcStage.h * scale), 64, 1600);
        m_loader->Preview(m_current, Cur().path, Cur().info, m_playhead, maxW, maxH, ++m_previewSerial);
    }

    EditorWindow::Strip EditorWindow::StripNow() const
    {
        Strip s;
        const int64_t dur = Duration();
        if (dur <= 0 || m_thumbW <= 0 || m_rcTrack.w <= 0)
            return s;
        const float thumbDip = std::max(24.f, static_cast<float>(m_thumbW) * 96.f / static_cast<float>(Dpi()));
        const int fitSlots = FilmstripSlots(m_rcTrack.w, thumbDip);
        const double zoom = m_view.usPerPx > 0 ? m_view.FitUsPerPx() / m_view.usPerPx : 1.0;
        s.level = std::clamp(static_cast<int>(std::floor(std::log2(std::max(1.0, zoom)) + 1e-9)), 0, 16);
        s.slotUs = static_cast<double>(dur) / fitSlots / static_cast<double>(1 << s.level);
        const int64_t total = static_cast<int64_t>(fitSlots) << s.level;
        s.first = std::clamp<int64_t>(static_cast<int64_t>(std::floor(static_cast<double>(m_view.startUs) / s.slotUs)), 0, total - 1);
        s.last = std::clamp<int64_t>(static_cast<int64_t>(std::ceil(static_cast<double>(m_view.EndUs()) / s.slotUs)), 0, total - 1);
        s.valid = true;
        return s;
    }

    int64_t EditorWindow::SlotTime(const Strip& s, int64_t slot) const
    {
        return std::min(LastFrameTime(Fps(), Duration()), static_cast<int64_t>((static_cast<double>(slot) + 0.5) * s.slotUs));
    }

    void EditorWindow::RequestThumbs()
    {
        if (!Video() || !HasTimeline() || !Cur().ready || !m_loader)
            return;
        const auto s = StripNow();
        if (!s.valid)
            return;
        std::vector<MediaLoader::ThumbJob> jobs;
        // From the playhead outwards, so what the user looks at fills in first.
        std::vector<int64_t> slots;
        for (int64_t k = s.first; k <= s.last; ++k)
            slots.push_back(k);
        const auto centre = static_cast<int64_t>(static_cast<double>(m_playhead) / s.slotUs);
        std::stable_sort(slots.begin(), slots.end(), [centre](int64_t a, int64_t b) { return std::llabs(a - centre) < std::llabs(b - centre); });
        for (const int64_t k : slots)
        {
            const uint64_t key = (static_cast<uint64_t>(s.level) << 32) | static_cast<uint64_t>(k);
            if (!m_thumbs.contains(key))
                jobs.push_back({ SlotTime(s, k), key, s.level == 0 });
        }
        if (!jobs.empty())
            m_loader->Thumbs(m_current, Cur().path, Cur().info, m_thumbW * 2, m_thumbH, std::move(jobs));
    }

    // ---- Moving through the files and finishing --------------------------------------------------------

    void EditorWindow::Advance(bool skip)
    {
        StopPlay();
        CancelEdit();
        if (!PerFile())
        {
            if (skip)
                RequestClose();
            else
                Finish();
            return;
        }
        Cur().status = skip ? Status::Skipped : Status::Done;
        const int n = static_cast<int>(m_files.size());
        for (int step = 1; step <= n; ++step)
        {
            const int i = (m_current + step) % n;
            if (m_files[static_cast<size_t>(i)].status == Status::Open)
            {
                ShowFile(i);
                return;
            }
        }
        Finish();
    }

    void EditorWindow::Finish()
    {
        // The recipe's own params hold for its own tool; another tool from the tool bar starts clean.
        nlohmann::json params = m_req.params.is_object() && m_req.tool == m_startTool ? m_req.params : nlohmann::json::object();
        const nlohmann::json shared = SharedParams();   // kept alive: items() only refers to it
        for (const auto& [key, value] : shared.items())
            params[key] = value;
        params["tool"] = ToolAction(m_req.tool);
        std::vector<std::wstring> files;
        if (PerFile())
        {
            nlohmann::json perFile = nlohmann::json::object();
            for (const auto& f : m_files)
                if (f.status == Status::Done && f.ready)
                {
                    files.push_back(f.path);
                    perFile[Narrow(f.path)] = FileParams(f);
                }
            params["perFile"] = std::move(perFile);
        }
        else if (m_req.tool == EditorTool::JoinVideos)
        {
            for (const int i : m_order)
                files.push_back(m_files[static_cast<size_t>(i)].path);
        }
        else
        {
            for (const auto& f : m_files)
                if (!f.path.empty() && (!f.probed || f.ready))
                    files.push_back(f.path);
        }
        auto done = std::move(m_done);
        ShowWindow(Hwnd(), SW_HIDE);
        if (!files.empty() && done)
        {
            log::Info(L"media editor: {} confirmed for {} file(s)", m_req.title, files.size());
            done(std::move(params), std::move(files));
        }
        RequestClose();
    }

    // ---- Layout and controls ------------------------------------------------------------------------------

    RectF EditorWindow::TimelineRect() const { return { m_rcRuler.x, m_rcRuler.y, m_rcRuler.w, m_rcTrack.Bottom() - m_rcRuler.y }; }

    void EditorWindow::Layout()
    {
        const float W = std::max(m_w, 400.f), H = std::max(m_h, 300.f);
        m_rcHeader = { 0, 0, W, kHeaderH };
        m_rcFooter = { 0, H - kFooterH, W, kFooterH };
        m_rcPanel = { W - kPanelW, kHeaderH, kPanelW, H - kHeaderH - kFooterH };
        const RectF content{ kPad, kHeaderH + 4, W - kPanelW - 2 * kPad, H - kHeaderH - kFooterH - kPad - 4 };
        m_rcStage = m_rcTransport = m_rcRuler = m_rcTrack = m_rcScroll = m_rcList = {};
        if (m_req.tool == EditorTool::JoinVideos)
            m_rcList = content;
        else if (m_req.tool == EditorTool::AudioVisualizer)
            m_rcStage = content;
        else if (Video())
        {
            m_rcScroll = { content.x, content.Bottom() - kScrollH, content.w, kScrollH };
            m_rcTrack = { content.x, m_rcScroll.y - 6 - kFilmH, content.w, kFilmH };
            m_rcRuler = { content.x, m_rcTrack.y - kRulerH, content.w, kRulerH };
            m_rcTransport = { content.x, m_rcRuler.y - 8 - kTransportH, content.w, kTransportH };
            m_rcStage = { content.x, content.y, content.w, std::max(80.f, m_rcTransport.y - 8 - content.y) };
        }
        else
        {
            m_rcTransport = { content.x, content.Bottom() - kTransportH, content.w, kTransportH };
            m_rcScroll = { content.x, m_rcTransport.y - 10 - kScrollH, content.w, kScrollH };
            m_rcRuler = { content.x, content.y, content.w, kRulerH };
            m_rcTrack = { content.x, m_rcRuler.Bottom(), content.w, std::max(60.f, m_rcScroll.y - 6 - m_rcRuler.Bottom()) };
        }
        if (HasTimeline() && m_view.durationUs > 0)
        {
            const bool zoomed = m_view.Zoomed();
            m_view.widthPx = std::max(1.f, m_rcTrack.w);
            if (zoomed)
                m_view.Clamp();
            else
                m_view.Fit();
            ViewChanged();
        }
    }

    void EditorWindow::Rebuild()
    {
        m_controls.clear();
        m_texts.clear();
        // Tab goes through them in this order, after the timeline.
        BuildToolBar();
        if (HasTimeline())
            BuildTransport();
        BuildPanel();
        BuildFooter();
        // A focused control that went away hands the keys back to the timeline.
        if (m_focus && !ControlById(m_focus))
            m_focus = 0;
        if (m_editId && !ControlById(m_editId))
            m_editId = 0;
        Invalidate();
    }

    void EditorWindow::BuildToolBar()
    {
        const auto tools = SiblingTools(m_req.tool);
        const RectF& h = m_rcHeader;
        float x = kPad;
        for (size_t i = 0; i < tools.size(); ++i)
        {
            const EditorTool tool = tools[i];
            ui::Control c;
            c.id = kIdTools + static_cast<int>(i);
            c.kind = ui::Control::Kind::Segment;
            c.text = ToolName(tool);
            const float w = std::max(64.f, m_painter.TextWidth(c.text, ui::Font::Body) + 28);
            c.rect = { x, h.y + (h.h - 32) / 2, w, 32 };
            c.on = tool == m_req.tool;
            c.tip = std::format(L"{} (Ctrl+{})", ToolTitle(tool), i + 1);
            c.click = [this, tool] { SwitchTool(tool); };
            c.step = [this, tools, i](int d) {
                const auto next = static_cast<int>(i) + d;
                if (next >= 0 && next < static_cast<int>(tools.size()))
                    SwitchTool(tools[static_cast<size_t>(next)]);
            };
            m_controls.push_back(std::move(c));
            x += w + 4;
        }
        m_toolBarRight = x + 12;
    }

    void EditorWindow::SwitchTool(EditorTool tool)
    {
        if (tool == m_req.tool || IsVideoTool(tool) != Video())
            return;
        StopPlay();
        CancelEdit();
        if (m_drag != Drag::None)
            EndDrag();
        const bool tabFocused = m_focus >= kIdTools && m_focus < kIdTools + 16;
        const bool wasPerFile = PerFile();
        m_req.tool = tool;
        m_req.title = ToolTitle(tool);
        SetWindowTextW(Hwnd(), (m_req.title + L" \u2013 " SD_NAME).c_str());
        if (HasTimeline() && !m_playback)
            m_playback = std::make_unique<Playback>(m_req.ffmpeg, &m_temp);
        // A fresh pass through the files; choices made so far stay (a trim range is still there on
        // the way back). Marks are frames for one tool and cut points for the other.
        for (auto& f : m_files)
        {
            f.status = Status::Open;
            if (!f.ready)
                continue;
            if (tool == EditorTool::SaveFrames)
                f.marks = videotools::NormalizeFrameTimes(f.marks, f.info.media.fps, f.info.DurationUs());
            else if (tool == EditorTool::SplitVideo)
                f.marks = NormalizePoints(f.marks, f.info.DurationUs());
        }
        for (int i = 0; i < static_cast<int>(m_files.size()); ++i)
        {
            auto& f = m_files[static_cast<size_t>(i)];
            if (tool == EditorTool::JoinVideos && f.ready && !f.joinThumbRequested)
            {
                f.joinThumbRequested = true;
                const int th = MulDiv(54, static_cast<int>(Dpi()), 96);
                m_loader->Thumbs(i, f.path, f.info, th * 2, th, { { std::min<int64_t>(500'000, f.info.DurationUs() / 3), 0, true } }, false);
            }
        }
        if (!PerFile() || !wasPerFile)
            m_current = 0;
        if (tool == EditorTool::AudioVisualizer)
            RequestWaveform(0);
        m_focus = 0;
        m_listScroll = 0;
        Layout();
        FileReady();   // view, thumbnails, preview and selection for the new layout; rebuilds
        // A focused tab keeps the keyboard focus, so arrow keys go on through the tools.
        const auto tools = SiblingTools(tool);
        for (size_t i = 0; i < tools.size(); ++i)
            if (tools[i] == tool && tabFocused)
                m_focus = kIdTools + static_cast<int>(i);
        log::Info(L"media editor: switched to {}", m_req.title);
    }

    void EditorWindow::BuildTransport()
    {
        using K = ui::Control::Kind;
        const RectF& t = m_rcTransport;
        const float cy = t.y + (t.h - 32) / 2;
        float x = t.x;
        const bool ready = Cur().ready;
        auto icon = [&](int id, const wchar_t* glyph, std::wstring tip, std::function<void()> click, bool enabled = true) {
            ui::Control c;
            c.id = id;
            c.kind = K::Icon;
            c.rect = { x, cy, 32, 32 };
            c.glyph = glyph;
            c.tip = std::move(tip);
            c.click = std::move(click);
            c.enabled = enabled && ready;
            m_controls.push_back(std::move(c));
            x += 36;
        };
        icon(kIdPlay, m_playing ? ui::icon::Pause : ui::icon::Play,
             Cur().info.HasAudio() ? L"Play from the playhead (Space); Shift+Space plays the selection" : L"Play (Space): this file has no sound",
             [this] { TogglePlay(false); });
        ui::Control time;
        time.id = kIdTime;
        time.kind = K::Field;
        time.rect = { x + 4, cy, 124, 32 };
        time.text = FormatTimecode(m_playhead);
        time.tip = L"Type a time to jump there, like 1:23.5";
        time.enabled = ready;
        time.commit = [this](const std::wstring& text) {
            int64_t us = 0;
            if (!ParseTimecode(text, us) || us > Duration())
                return false;
            SetPlayhead(us);
            return true;
        };
        m_controls.push_back(std::move(time));
        x += 132;
        m_texts.push_back({ { x, t.y, 120, t.h }, L"/ " + FormatTimecode(Duration()), ui::Font::Number, 2 });
        x += 124;
        icon(kIdBack, ui::icon::Back, Video() ? L"One frame back (\u2190); Shift+\u2190 one second" : L"Back 10 ms (\u2190); Shift+\u2190 one second",
             [this] { SetPlayhead(StepFrames(m_playhead, Fps(), -1, Duration())); });
        icon(kIdForward, ui::icon::Forward, Video() ? L"One frame forward (\u2192); Shift+\u2192 one second" : L"Forward 10 ms (\u2192); Shift+\u2192 one second",
             [this] { SetPlayhead(StepFrames(m_playhead, Fps(), 1, Duration())); });
        x = t.Right() - 3 * 36 + 4;
        icon(kIdZoomOut, ui::icon::ZoomOut, L"Zoom out (\u2212)", [this] { ZoomBy(1 / 1.6, m_rcTrack.x + m_view.XOf(m_playhead)); }, m_view.Zoomed());
        icon(kIdZoomIn, ui::icon::ZoomIn, L"Zoom in (+), or turn the mouse wheel over the timeline",
             [this] { ZoomBy(1.6, m_rcTrack.x + m_view.XOf(m_playhead)); }, m_view.usPerPx > m_view.MinUsPerPx() * 1.01);
        icon(kIdFit, ui::icon::Fit, L"Show the whole clip (0)", [this] { m_view.Fit(); ViewChanged(); Rebuild(); }, m_view.Zoomed());
    }

    void EditorWindow::BuildFooter()
    {
        using K = ui::Control::Kind;
        const RectF& f = m_rcFooter;
        const float y = f.y + (f.h - 34) / 2;
        float right = f.Right() - kPad;
        ui::Control primary;
        primary.id = kIdPrimary;
        primary.kind = K::Primary;
        primary.text = PrimaryLabel();
        const float pw = std::max(112.f, m_painter.TextWidth(primary.text, ui::Font::BodyStrong) + 40);
        primary.rect = { right - pw, y, pw, 34 };
        primary.enabled = PrimaryEnabled();
        primary.tip = L"Enter";
        primary.click = [this] { Advance(false); };
        right -= pw + 8;
        ui::Control cancel;
        cancel.id = kIdCancel;
        cancel.kind = K::Button;
        cancel.text = L"Cancel";
        cancel.rect = { right - 96, y, 96, 34 };
        cancel.tip = L"Close without changing anything (Esc)";
        cancel.click = [this] { RequestClose(); };
        right -= 104;
        m_controls.push_back(std::move(cancel));
        m_controls.push_back(std::move(primary));
        int open = 0;
        for (const auto& file : m_files)
            open += file.status == Status::Open ? 1 : 0;
        if (PerFile() && m_files.size() > 1 && open > 1)
        {
            ui::Control skip;
            skip.id = kIdSkip;
            skip.kind = K::Button;
            skip.text = L"Skip this file";
            skip.rect = { right - 128, y, 128, 34 };
            skip.tip = L"Leave this file as it is and go to the next";
            skip.click = [this] { Advance(true); };
            m_controls.push_back(std::move(skip));
        }
    }

    const ui::Control* EditorWindow::ControlById(int id) const
    {
        for (const auto& c : m_controls)
            if (c.id == id)
                return &c;
        return nullptr;
    }

    int EditorWindow::ControlAt(float x, float y) const
    {
        for (auto it = m_controls.rbegin(); it != m_controls.rend(); ++it)
        {
            // Sliders are easier to grab a little outside their row.
            const RectF r = it->kind == ui::Control::Kind::Slider ? RectF{ it->rect.x - 6, it->rect.y, it->rect.w + 12, it->rect.h + 4 } : it->rect;
            if (r.Contains(x, y))
                return it->id;
        }
        return 0;
    }

    // ---- Painting -----------------------------------------------------------------------------------------

    void EditorWindow::DropBitmaps()
    {
        m_preview.bitmap.Reset();
        m_blur.bitmap.Reset();
        m_imagePicture.bitmap.Reset();
        for (auto& [key, t] : m_thumbs)
            t.bitmap.Reset();
        for (auto& f : m_files)
            f.joinThumb.bitmap.Reset();
    }

    ID2D1Bitmap* EditorWindow::BitmapOf(Picture& picture)
    {
        if (!picture.bitmap && picture.frame.w > 0 && Target())
            Target()->CreateBitmap(D2D1::SizeU(static_cast<UINT32>(picture.frame.w), static_cast<UINT32>(picture.frame.h)), picture.frame.bgra.data(),
                               static_cast<UINT32>(picture.frame.w) * 4,
                               D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE)), &picture.bitmap);
        return picture.bitmap.Get();
    }

    void EditorWindow::DrawCover(ui::Painter& p, ID2D1Bitmap* bitmap, const RectF& slot, float opacity)
    {
        if (!bitmap || slot.w <= 0 || slot.h <= 0)
            return;
        const auto size = bitmap->GetPixelSize();
        const float bw = static_cast<float>(size.width), bh = static_cast<float>(size.height);
        const float scale = std::max(slot.w / bw, slot.h / bh);
        const float sw = slot.w / scale, sh = slot.h / scale;
        const D2D1_RECT_F src{ (bw - sw) / 2, (bh - sh) / 2, (bw + sw) / 2, (bh + sh) / 2 };
        p.Rt()->DrawBitmap(bitmap, ui::ToD2D(slot), opacity, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, src);
    }

    void EditorWindow::Paint(ID2D1RenderTarget* rt, D2D1_SIZE_F)
    {
        if (m_device != DeviceGeneration())
        {
            DropBitmaps();
            m_device = DeviceGeneration();
        }
        auto& p = m_painter;
        p.Begin(rt, Theme(), m_device);
        PaintHeader(p);
        p.Fill(m_rcPanel, Theme().panel);
        p.Line(m_rcPanel.x + 0.5f, m_rcPanel.y, m_rcPanel.x + 0.5f, m_rcPanel.Bottom(), Theme().line);
        if (m_req.tool == EditorTool::JoinVideos)
            PaintJoinList(p);
        else if (m_req.tool == EditorTool::AudioVisualizer)
            PaintVisualizer(p);
        else
        {
            if (HasStage())
                PaintStage(p);
            PaintTimeline(p);
        }
        for (const auto& t : m_texts)
        {
            const D2D1_COLOR_F color = t.tone == 1 ? Theme().textDim : t.tone == 2 ? Theme().textFaint : t.tone == 3 ? Theme().danger : Theme().text;
            if (t.wrap)
                p.Paragraph(t.text, t.rect, t.font, color);
            else
                p.Text(t.text, t.rect, t.font, color, t.align);
        }
        PaintFooter(p);
        for (const auto& c : m_controls)
        {
            ui::DrawState s;
            s.hover = m_hover == c.id && c.enabled;
            s.pressed = m_pressed == c.id && m_hover == c.id;
            s.focused = m_focus == c.id && GetFocus() == Hwnd();
            s.invalid = m_invalidId == c.id;
            if (m_editId == c.id)
            {
                s.editText = &m_editText;
                s.caret = m_editCaret;
                s.selectAll = m_editAll;
            }
            ui::DrawControl(p, c, s);
        }
    }

    void EditorWindow::PaintHeader(ui::Painter& p)
    {
        const auto& f = Cur();
        const RectF& h = m_rcHeader;
        // The tool bar's tabs are on the left (controls); the file's details follow them.
        std::wstring sub;
        if (m_req.tool == EditorTool::JoinVideos)
            sub = std::format(L"{} clips", m_order.size());
        else
        {
            sub = f.name;
            if (f.ready && Video())
                sub += std::format(L"   {}\u00D7{}  \u00B7  {} fps  \u00B7  {}", f.info.width, f.info.height, Num(std::round(f.info.media.fps * 100) / 100),
                                   FormatTimecode(f.info.DurationUs()));
            else if (f.ready)
                sub += std::format(L"   {}  \u00B7  {} Hz  \u00B7  {}", f.info.channels == 1 ? L"Mono" : f.info.channels == 2 ? L"Stereo" : std::format(L"{} channels", f.info.channels),
                                   f.info.sampleRate, FormatTimecode(f.info.DurationUs()));
            if (!PerFile() && m_files.size() > 1)
                sub += std::format(L"   (and {} more)", m_files.size() - 1);
        }
        float right = h.Right() - kPad;
        if (PerFile() && m_files.size() > 1)
        {
            const auto counter = std::format(L"File {} of {}", m_current + 1, m_files.size());
            const float cw = p.TextWidth(counter, ui::Font::BodyStrong) + 24;
            const RectF chip{ right - cw, h.y + (h.h - 26) / 2, cw, 26 };
            p.Fill(chip, Theme().accentSoft, 13);
            p.Text(counter, chip, ui::Font::BodyStrong, Theme().accent, 1);
            right -= cw + 12;
        }
        p.Text(sub, { m_toolBarRight, h.y, std::max(0.f, right - m_toolBarRight), h.h }, ui::Font::Body, Theme().textDim);
        p.Line(0, h.Bottom() - 0.5f, h.Right(), h.Bottom() - 0.5f, Theme().line);
    }

    PreviewMap EditorWindow::StageMap() const
    {
        const auto& f = Cur();
        PreviewMap map;
        if (!f.ready || f.info.width <= 0 || f.info.height <= 0)
            return map;
        map.frameW = f.info.width;
        map.frameH = f.info.height;
        map.shown = FitInside(static_cast<float>(f.info.width * f.info.sar), static_cast<float>(f.info.height), m_rcStage.Inset(10));
        return map;
    }

    void EditorWindow::PaintStage(ui::Painter& p)
    {
        const auto& f = Cur();
        p.Fill(m_rcStage, Theme().stage, 8);
        if (!f.error.empty() || !f.ready)
        {
            const RectF mid{ m_rcStage.x, m_rcStage.y + m_rcStage.h / 2 - 40, m_rcStage.w, 80 };
            if (!f.error.empty())
            {
                p.Text(ui::icon::Warning, { mid.x, mid.y, mid.w, 32 }, ui::Font::Icon, Theme().danger, 1);
                p.Text(f.error, { mid.x, mid.y + 36, mid.w, 24 }, ui::Font::Body, Theme().text, 1);
                if (PerFile() && m_files.size() > 1)
                    p.Text(L"Skip it to go on with the other files.", { mid.x, mid.y + 60, mid.w, 22 }, ui::Font::Small, Theme().textDim, 1);
            }
            else
                p.Text(L"Opening\u2026", mid, ui::Font::Body, Theme().textFaint, 1);
            return;
        }
        const auto map = StageMap();
        const D2D1_RECT_F shown = ui::ToD2D(map.shown);
        if (ID2D1Bitmap* bmp = BitmapOf(m_preview))
            Target()->DrawBitmap(bmp, shown, 1.f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        else
        {
            p.Fill(map.shown, D2D1::ColorF(0, 0, 0, 1));
            // Until the exact frame arrives, the nearest thumbnail stands in.
            Picture* best = nullptr;
            for (auto& [key, t] : m_thumbs)
                if (!best || std::llabs(t.us - m_playhead) < std::llabs(best->us - m_playhead))
                    best = &t;
            if (best)
                if (ID2D1Bitmap* bmp = BitmapOf(*best))
                    Target()->DrawBitmap(bmp, shown, 1.f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        }
        if (m_req.tool == EditorTool::CropVideo)
            PaintCropOverlay(p, map);
        else if (m_req.tool == EditorTool::RedactVideo)
            PaintAreas(p, map);
    }

    void EditorWindow::Handles(ui::Painter& p, const RectF& r, bool edges)
    {
        const float s = 9;
        const D2D1_POINT_2F pts[8] = { { r.x, r.y }, { r.Right(), r.y }, { r.Right(), r.Bottom() }, { r.x, r.Bottom() },
                                       { r.x + r.w / 2, r.y }, { r.Right(), r.y + r.h / 2 }, { r.x + r.w / 2, r.Bottom() }, { r.x, r.y + r.h / 2 } };
        for (int i = 0; i < (edges ? 8 : 4); ++i)
        {
            const RectF h{ pts[i].x - s / 2, pts[i].y - s / 2, s, s };
            p.Fill(h, D2D1::ColorF(1, 1, 1, 1), 2);
            p.Stroke(h, D2D1::ColorF(0, 0, 0, 0.55f), 1.f, 2);
        }
    }

    // ---- Timeline -------------------------------------------------------------------------------------------

    void EditorWindow::PaintTimeline(ui::Painter& p)
    {
        const auto& f = Cur();
        const RectF& ruler = m_rcRuler;
        const RectF& track = m_rcTrack;
        p.Fill(track, Theme().track, 6);
        if (!f.ready)
        {
            if (!HasStage())
                p.Text(f.error.empty() ? L"Opening\u2026" : f.error, track, ui::Font::Body, f.error.empty() ? Theme().textFaint : Theme().danger, 1);
            return;
        }
        // Ruler: labelled ticks at a round step, minor ticks between.
        const int64_t step = TickStepUs(m_view.usPerPx, 92);
        const int64_t minor = step / 5 >= 1000 ? step / 5 : step;
        Target()->PushAxisAlignedClip(ui::ToD2D(ruler), D2D1_ANTIALIAS_MODE_ALIASED);
        for (int64_t t = (m_view.startUs / minor) * minor; t <= m_view.EndUs(); t += minor)
        {
            const float x = TX(t);
            const bool major = t % step == 0;
            p.Line(x, ruler.Bottom() - (major ? 8.f : 4.f), x, ruler.Bottom(), major ? Theme().textDim : Theme().textFaint);
            if (major)
                p.Text(FormatRulerLabel(t, step), { x + 4, ruler.y, 90, ruler.h - 6 }, ui::Font::Small, Theme().textDim);
        }
        Target()->PopAxisAlignedClip();

        Target()->PushAxisAlignedClip(ui::ToD2D(track), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        if (UsesWaveform())
            PaintWaveform(p);
        else
            PaintFilmstrip(p);
        PaintTrackOverlay(p);
        Target()->PopAxisAlignedClip();

        // Hover line and playhead.
        if (m_hoverX >= track.x && m_hoverX <= track.Right() && m_drag == Drag::None)
            p.Line(m_hoverX, ruler.y + 4, m_hoverX, track.Bottom(), WithAlpha(Theme().text, 0.25f), 1.f);
        const float px = TX(m_playhead);
        if (px >= track.x - 1 && px <= track.Right() + 1)
        {
            p.Line(px, ruler.y + 6, px, track.Bottom(), Theme().text, 1.5f);
            const RectF knob{ px - 6, ruler.y + 2, 12, 12 };
            p.Fill(knob, Theme().text, 3);
        }
        // Scroll bar while zoomed in.
        if (m_view.Zoomed() && m_view.durationUs > 0)
        {
            p.Fill(m_rcScroll, Theme().track, kScrollH / 2);
            const float x0 = m_rcScroll.x + static_cast<float>(static_cast<double>(m_view.startUs) / m_view.durationUs) * m_rcScroll.w;
            const float x1 = m_rcScroll.x + static_cast<float>(static_cast<double>(m_view.EndUs()) / m_view.durationUs) * m_rcScroll.w;
            const float w = std::max(24.f, x1 - x0);
            p.Fill({ std::min(x0, m_rcScroll.Right() - w), m_rcScroll.y + 1, w, kScrollH - 2 }, m_drag == Drag::Scroll ? Theme().textDim : Theme().textFaint,
                   (kScrollH - 2) / 2);
            const float ph = m_rcScroll.x + static_cast<float>(static_cast<double>(m_playhead) / m_view.durationUs) * m_rcScroll.w;
            p.Line(ph, m_rcScroll.y, ph, m_rcScroll.Bottom(), Theme().text, 1.f);
        }
    }

    void EditorWindow::PaintFilmstrip(ui::Painter& p)
    {
        const auto s = StripNow();
        if (!s.valid)
            return;
        const RectF& track = m_rcTrack;
        for (int64_t k = s.first; k <= s.last; ++k)
        {
            const float x0 = TX(static_cast<int64_t>(static_cast<double>(k) * s.slotUs));
            const float x1 = TX(static_cast<int64_t>(static_cast<double>(k + 1) * s.slotUs));
            const RectF slot{ x0, track.y, x1 - x0 - 1, track.h };
            // This level's thumbnail, else the coarser one covering it, while it loads.
            Picture* pic = nullptr;
            for (int level = s.level; level >= 0 && !pic; --level)
            {
                const uint64_t key = (static_cast<uint64_t>(level) << 32) | static_cast<uint64_t>(k >> (s.level - level));
                if (const auto it = m_thumbs.find(key); it != m_thumbs.end())
                    pic = &it->second;
            }
            if (pic)
                DrawCover(p, BitmapOf(*pic), slot);
            else
                p.Fill(slot.Inset(1), WithAlpha(Theme().text, 0.04f));
        }
    }

    void EditorWindow::PaintWaveform(ui::Painter& p)
    {
        const auto& f = Cur();
        const RectF& track = m_rcTrack;
        if (!f.peaks)
        {
            const std::wstring text = f.waveFailed ? L"Could not read the sound of this file." : std::format(L"Reading the sound\u2026 {}%", static_cast<int>(f.waveProgress * 100));
            p.Text(text, track, ui::Font::Body, f.waveFailed ? Theme().danger : Theme().textFaint, 1);
            if (!f.waveFailed)
                p.Fill({ track.x + track.w * 0.3f, track.y + track.h / 2 + 18, track.w * 0.4f * static_cast<float>(f.waveProgress), 3 }, Theme().accent, 1.5f);
            return;
        }
        const float mid = track.y + track.h / 2, amp = track.h / 2 - 8;
        const float gain = std::clamp(0.92f / std::max(0.01f, f.peaks->Peak()), 1.f, 8.f);
        p.Line(track.x, mid, track.Right(), mid, WithAlpha(Theme().text, 0.12f));
        ComPtr<ID2D1PathGeometry> path;
        ComPtr<ID2D1GeometrySink> sink;
        if (FAILED(Factory()->CreatePathGeometry(&path)) || FAILED(path->Open(&sink)))
            return;
        const int cols = static_cast<int>(std::ceil(track.w));
        std::vector<std::pair<float, float>> env(static_cast<size_t>(cols));
        for (int i = 0; i < cols; ++i)
        {
            const auto [lo, hi] = f.peaks->Span(m_view.TimeAt(i), m_view.TimeAt(i + 1));
            env[static_cast<size_t>(i)] = { std::max(0.6f, hi * gain * amp), std::max(0.6f, -lo * gain * amp) };
        }
        sink->BeginFigure({ track.x, mid }, D2D1_FIGURE_BEGIN_FILLED);
        for (int i = 0; i < cols; ++i)
            sink->AddLine({ track.x + i + 0.5f, mid - std::min(env[static_cast<size_t>(i)].first, amp + 6) });
        for (int i = cols - 1; i >= 0; --i)
            sink->AddLine({ track.x + i + 0.5f, mid + std::min(env[static_cast<size_t>(i)].second, amp + 6) });
        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
        sink->Close();
        // Tools colour the waveform by range: the kept part of a trim, the bleeped parts.
        const auto ranges = EditableRanges();
        const bool keep = m_req.tool == EditorTool::TrimAudio;
        Target()->FillGeometry(path.Get(), p.Brush(keep ? WithAlpha(Theme().text, 0.28f) : m_req.tool == EditorTool::BleepAudio ? Theme().textDim : Theme().accent));
        for (const auto& r : ranges)
        {
            const float x0 = std::max(track.x, TX(r.start)), x1 = std::min(track.Right(), TX(r.end));
            if (x1 <= x0)
                continue;
            Target()->PushAxisAlignedClip(D2D1::RectF(x0, track.y, x1, track.Bottom()), D2D1_ANTIALIAS_MODE_ALIASED);
            Target()->FillGeometry(path.Get(), p.Brush(keep ? Theme().accent : Theme().danger));
            Target()->PopAxisAlignedClip();
        }
    }

    void EditorWindow::SetPlayhead(int64_t us, bool follow)
    {
        if (!Cur().ready)
            return;
        us = std::clamp<int64_t>(us, 0, Duration());
        if (Fps() > 0)
            us = std::min(SnapToFrame(us, Fps()), LastFrameTime(Fps(), Duration()));
        const bool changed = us != m_playhead;
        m_playhead = us;
        Cur().playhead = us;
        if (follow)
        {
            const int64_t before = m_view.startUs;
            m_view.EnsureVisible(us);
            if (m_view.startUs != before)
                ViewChanged();
        }
        if (changed)
        {
            RequestPreview();
            Rebuild();
        }
        Invalidate();
    }

    void EditorWindow::ZoomBy(double factor, double x)
    {
        if (!Cur().ready)
            return;
        m_view.ZoomAt(factor, x - m_rcTrack.x);
        ViewChanged();
        Rebuild();
    }

    void EditorWindow::ViewChanged()
    {
        if (Hwnd() && Video())
            SetTimer(Hwnd(), kTimerThumbs, 120, nullptr);
        Invalidate();
    }

    // ---- Input --------------------------------------------------------------------------------------------

    void EditorWindow::OnMouseDown(float x, float y, bool doubleClick)
    {
        if (m_editId)
        {
            const auto* c = ControlById(m_editId);
            if (c && c->rect.Contains(x, y))
            {
                m_editAll = doubleClick;
                const float textX = c->rect.x + (c->detail.empty() ? 0.f : 64.f) + 10;
                m_editCaret = m_painter.CharAt(m_editText, x - textX, ui::Font::Number);
                Invalidate();
                return;
            }
            if (!CommitEdit(false))
                CancelEdit();
        }
        m_downX = x;
        m_downY = y;
        m_dragMoved = false;
        if (const int id = ControlAt(x, y))
        {
            const auto* c = ControlById(id);
            if (!c->enabled)
                return;
            if (c->kind == ui::Control::Kind::Field)
            {
                StartEdit(*c);
                return;
            }
            m_pressed = id;
            m_drag = Drag::Pressed;
            if (c->kind == ui::Control::Kind::Slider && c->slide)
            {
                m_drag = Drag::Slider;
                const auto slide = c->slide;   // sliding rebuilds the controls
                slide(std::clamp((x - c->rect.x - 7) / std::max(1.f, c->rect.w - 14), 0.f, 1.f));
            }
            Invalidate();
            return;
        }
        m_focus = 0;   // a click on the timeline or the picture gives the keys back to them
        if (HasTimeline() && Cur().ready)
        {
            if (m_view.Zoomed() && RectF{ m_rcScroll.x, m_rcScroll.y - 3, m_rcScroll.w, m_rcScroll.h + 6 }.Contains(x, y))
            {
                const float x0 = m_rcScroll.x + static_cast<float>(static_cast<double>(m_view.startUs) / m_view.durationUs) * m_rcScroll.w;
                const float x1 = m_rcScroll.x + static_cast<float>(static_cast<double>(m_view.EndUs()) / m_view.durationUs) * m_rcScroll.w;
                if (x < x0 || x > x1)
                {
                    // Clicking beside the thumb centres the view there.
                    const int64_t visible = m_view.EndUs() - m_view.startUs;
                    m_view.startUs = static_cast<int64_t>((x - m_rcScroll.x) / m_rcScroll.w * m_view.durationUs) - visible / 2;
                    m_view.Clamp();
                    ViewChanged();
                }
                m_drag = Drag::Scroll;
                m_downTime = m_view.startUs;
                return;
            }
            if (m_rcRuler.Contains(x, y))
            {
                StopPlay();
                m_drag = Drag::Scrub;
                SetPlayhead(TT(x), false);
                return;
            }
            if (m_rcTrack.Contains(x, y) && TrackMouseDown(x, y, doubleClick))
                return;
        }
        if ((m_rcStage.Contains(x, y) || m_rcList.Contains(x, y)) && StageMouseDown(x, y))
            return;
        Invalidate();
    }

    void EditorWindow::OnMouseMove(float x, float y)
    {
        if (m_drag == Drag::None || m_drag == Drag::Pressed)
        {
            const int hover = ControlAt(x, y);
            const float hoverX = HasTimeline() && TimelineRect().Contains(x, y) ? x : -1;
            if (hover != m_hover || hoverX != m_hoverX)
            {
                m_hover = hover;
                m_hoverX = hoverX;
                Invalidate();
            }
            return;
        }
        if (std::abs(x - m_downX) > 3 || std::abs(y - m_downY) > 3)
            m_dragMoved = true;
        SetCursor(LoadCursorW(nullptr, CursorAt(x, y)));
        switch (m_drag)
        {
        case Drag::Slider:
            if (const auto* c = ControlById(m_pressed); c && c->slide)
            {
                const auto slide = c->slide;
                slide(std::clamp((x - c->rect.x - 7) / std::max(1.f, c->rect.w - 14), 0.f, 1.f));
            }
            break;
        case Drag::Scroll:
            m_view.startUs = m_downTime + static_cast<int64_t>((x - m_downX) / std::max(1.f, m_rcScroll.w) * m_view.durationUs);
            m_view.Clamp();
            ViewChanged();
            break;
        case Drag::Scrub:
            // Dragging past either end scrolls the zoomed view along.
            if (x < m_rcTrack.x || x > m_rcTrack.Right())
            {
                m_view.ScrollBy(x < m_rcTrack.x ? x - m_rcTrack.x : x - m_rcTrack.Right());
                ViewChanged();
            }
            SetPlayhead(TT(std::clamp(x, m_rcTrack.x, m_rcTrack.Right())), false);
            break;
        default:
            DragTo(x, y);
            break;
        }
        Invalidate();
    }

    void EditorWindow::OnMouseUp(float x, float y)
    {
        if (m_drag == Drag::Pressed)
        {
            const int id = m_pressed;
            m_pressed = 0;
            m_drag = Drag::None;
            if (id && ControlAt(x, y) == id)
                if (const auto* c = ControlById(id); c && c->enabled && c->click)
                {
                    const auto click = c->click;   // the click may rebuild the controls
                    click();
                }
            Invalidate();
            return;
        }
        if (m_drag != Drag::None)
            EndDrag();
        m_pressed = 0;
    }

    void EditorWindow::OnWheel(float x, float y, int delta, bool horizontal)
    {
        const bool shift = GetKeyState(VK_SHIFT) < 0;
        if (HasTimeline() && Cur().ready && (TimelineRect().Contains(x, y) || m_rcScroll.Contains(x, y)))
        {
            if (horizontal || shift)
            {
                m_view.ScrollBy((horizontal ? delta : -delta) / 120.0 * 80);
                ViewChanged();
            }
            else
                ZoomBy(std::pow(1.25, delta / 120.0), x);
            return;
        }
        if (m_rcList.Contains(x, y))
        {
            m_listScroll = std::max(0.f, m_listScroll - delta / 120.f * 60);
            Invalidate();
            return;
        }
        if (const int id = ControlAt(x, y))
            if (const auto* c = ControlById(id); c && c->kind == ui::Control::Kind::Slider && c->step && c->enabled)
            {
                const auto step = c->step;
                step(delta > 0 ? 1 : -1);
            }
    }

    void EditorWindow::Focus(int id)
    {
        m_focus = id;
        if (const auto* c = ControlById(id); c && c->kind == ui::Control::Kind::Field)
            StartEdit(*c);
        Invalidate();
    }

    void EditorWindow::MoveFocus(bool back)
    {
        // The timeline (id 0) is the first stop, then every enabled control in layout order.
        std::vector<int> order{ 0 };
        for (const auto& c : m_controls)
            if (c.enabled)
                order.push_back(c.id);
        auto it = std::find(order.begin(), order.end(), m_focus);
        size_t i = it == order.end() ? 0 : static_cast<size_t>(it - order.begin());
        i = back ? (i + order.size() - 1) % order.size() : (i + 1) % order.size();
        Focus(order[i]);
    }

    void EditorWindow::StartEdit(const ui::Control& c)
    {
        m_editId = c.id;
        m_focus = c.id;
        m_editText = c.text;
        m_editCaret = m_editText.size();
        m_editAll = true;
        m_invalidId = 0;
        Invalidate();
    }

    bool EditorWindow::CommitEdit(bool keepOnError)
    {
        if (!m_editId)
            return true;
        const auto* c = ControlById(m_editId);
        const auto commit = c ? c->commit : nullptr;
        const int id = m_editId;
        const std::wstring text = m_editText;
        m_editId = 0;
        if (commit && !commit(text))
        {
            MessageBeep(MB_OK);
            if (keepOnError && ControlById(id))
            {
                m_editId = id;
                m_editText = text;
                m_editAll = true;
                m_invalidId = id;
                Invalidate();
            }
            return false;
        }
        m_invalidId = 0;
        Rebuild();
        return true;
    }

    void EditorWindow::CancelEdit()
    {
        if (!m_editId)
            return;
        m_editId = 0;
        m_invalidId = 0;
        Invalidate();
    }

    void EditorWindow::OnChar(wchar_t ch)
    {
        if (!m_editId || ch < 32 || ch == 127)
            return;
        if (m_editAll)
        {
            m_editText.clear();
            m_editCaret = 0;
            m_editAll = false;
        }
        if (m_editText.size() < 32)
            m_editText.insert(m_editCaret++, 1, ch);
        m_invalidId = 0;
        Invalidate();
    }

    bool EditorWindow::OnKey(UINT vk)
    {
        const bool shift = GetKeyState(VK_SHIFT) < 0, ctrl = GetKeyState(VK_CONTROL) < 0;
        if (m_editId)
        {
            switch (vk)
            {
            case VK_LEFT:
                m_editCaret = m_editAll ? 0 : m_editCaret > 0 ? m_editCaret - 1 : 0;
                m_editAll = false;
                break;
            case VK_RIGHT:
                m_editCaret = m_editAll ? m_editText.size() : std::min(m_editText.size(), m_editCaret + 1);
                m_editAll = false;
                break;
            case VK_HOME:
                m_editCaret = 0;
                m_editAll = false;
                break;
            case VK_END:
                m_editCaret = m_editText.size();
                m_editAll = false;
                break;
            case VK_BACK:
            case VK_DELETE:
                if (m_editAll)
                {
                    m_editText.clear();
                    m_editCaret = 0;
                    m_editAll = false;
                }
                else if (vk == VK_BACK && m_editCaret > 0)
                    m_editText.erase(--m_editCaret, 1);
                else if (vk == VK_DELETE && m_editCaret < m_editText.size())
                    m_editText.erase(m_editCaret, 1);
                break;
            case VK_RETURN:
                if (CommitEdit(true))
                    m_focus = 0;
                break;
            case VK_ESCAPE:
                CancelEdit();
                m_focus = 0;
                break;
            case VK_TAB:
                if (!CommitEdit(false))
                    CancelEdit();
                MoveFocus(shift);
                break;
            case 'A':
                if (!ctrl)
                    return false;
                m_editAll = true;
                break;
            case 'V':
                if (!ctrl)
                    return false;
                if (OpenClipboard(Hwnd()))
                {
                    if (HANDLE data = GetClipboardData(CF_UNICODETEXT))
                        if (const auto* text = static_cast<const wchar_t*>(GlobalLock(data)))
                        {
                            std::wstring pasted(text);
                            GlobalUnlock(data);
                            std::erase_if(pasted, [](wchar_t c) { return c < 32; });
                            if (m_editAll)
                            {
                                m_editText.clear();
                                m_editCaret = 0;
                                m_editAll = false;
                            }
                            pasted = pasted.substr(0, 32 - std::min<size_t>(32, m_editText.size()));
                            m_editText.insert(m_editCaret, pasted);
                            m_editCaret += pasted.size();
                        }
                    CloseClipboard();
                }
                break;
            default:
                return false;   // characters arrive as WM_CHAR
            }
            Invalidate();
            return true;
        }

        // Ctrl+1, Ctrl+2...: the tool bar's tabs.
        if (ctrl && vk >= '1' && vk <= '9')
        {
            const auto tools = SiblingTools(m_req.tool);
            if (const size_t i = vk - '1'; i < tools.size())
                SwitchTool(tools[i]);
            return true;
        }
        const ui::Control* focused = m_focus ? ControlById(m_focus) : nullptr;
        switch (vk)
        {
        case VK_TAB:
            MoveFocus(shift);
            return true;
        case VK_ESCAPE:
            if (m_drag != Drag::None)
                EndDrag();
            else if (m_playing)
                StopPlay();
            else if (m_focus)
                Focus(0);
            else
                RequestClose();
            return true;
        case VK_RETURN:
            if (focused && focused->click && focused->enabled)
            {
                const auto click = focused->click;
                click();
            }
            else if (PrimaryEnabled())
                Advance(false);
            return true;
        case VK_SPACE:
            if (focused && focused->click && focused->enabled && focused->kind != ui::Control::Kind::Slider)
            {
                const auto click = focused->click;
                click();
            }
            else if (CanPlay())
                TogglePlay(shift);
            return true;
        case VK_LEFT:
        case VK_RIGHT:
        case VK_UP:
        case VK_DOWN:
            if (focused && focused->step && focused->enabled)
            {
                const auto step = focused->step;
                step(vk == VK_LEFT || vk == VK_DOWN ? -1 : 1);
                return true;
            }
            break;
        default:
            break;
        }
        if (ToolKey(vk, shift, ctrl))
            return true;
        if (!HasTimeline() || !Cur().ready)
            return false;
        switch (vk)
        {
        case VK_LEFT:
        case VK_RIGHT:
        {
            const int dir = vk == VK_LEFT ? -1 : 1;
            if (shift)
                SetPlayhead(m_playhead + dir * 1'000'000);
            else
                SetPlayhead(StepFrames(m_playhead, Fps(), dir * (ctrl ? 10 : 1), Duration()));
            return true;
        }
        case VK_HOME:
            SetPlayhead(0);
            return true;
        case VK_END:
            SetPlayhead(Duration());
            return true;
        case VK_OEM_PLUS:
        case VK_ADD:
            ZoomBy(1.6, m_rcTrack.x + m_view.XOf(m_playhead));
            return true;
        case VK_OEM_MINUS:
        case VK_SUBTRACT:
            ZoomBy(1 / 1.6, m_rcTrack.x + m_view.XOf(m_playhead));
            return true;
        case '0':
        case VK_NUMPAD0:
            m_view.Fit();
            ViewChanged();
            Rebuild();
            return true;
        default:
            return false;
        }
    }

    void EditorWindow::EndDrag()
    {
        const Drag was = m_drag;
        m_drag = Drag::None;
        m_dragIndex = -1;
        m_dragHandle = -1;
        m_pressed = 0;
        m_joinDropAt = -1;
        if (GetCapture() == Hwnd())
            ReleaseCapture();
        ToolDragEnd(was);
        // Changes heard while playing: restart from where we are so they apply.
        if (m_playing && m_playback && (was == Drag::Edge || was == Drag::Body || was == Drag::NewRange || was == Drag::Slider))
            StartPlay(m_playback->PositionUs(), m_playEnd);
        Rebuild();
    }

    // ---- Playback ------------------------------------------------------------------------------------------

    void EditorWindow::TogglePlay(bool selectionOnly)
    {
        if (m_playing)
        {
            StopPlay();
            return;
        }
        if (!CanPlay())
            return;
        int64_t from = m_playhead, to = Duration();
        if (selectionOnly)
        {
            const auto ranges = EditableRanges();
            const int sel = SelectedRange();
            if (sel >= 0 && sel < static_cast<int>(ranges.size()))
            {
                // A little before a bleep, so it is heard in context.
                from = ranges[static_cast<size_t>(sel)].start;
                to = ranges[static_cast<size_t>(sel)].end;
                if (m_req.tool == EditorTool::BleepAudio)
                {
                    from = std::max<int64_t>(0, from - 1'000'000);
                    to = std::min(Duration(), to + 1'000'000);
                }
            }
        }
        if (from >= to - 20'000)
            from = selectionOnly ? from : 0;
        StartPlay(from, to);
    }

    void EditorWindow::StartPlay(int64_t from, int64_t to)
    {
        if (!m_playback || !Cur().ready)
            return;
        from = std::clamp<int64_t>(from, 0, Duration());
        to = std::clamp<int64_t>(to, from, Duration());
        m_playback->Play(Cur().path, { from, to }, PlayFilter(), PlayRate(), Cur().info.HasAudio());
        if (!m_playing)
        {
            m_playing = true;
            SetTimer(Hwnd(), kTimerPlay, 15, nullptr);
        }
        m_playEnd = to;
        Rebuild();
    }

    void EditorWindow::StopPlay()
    {
        if (!m_playing)
            return;
        m_playing = false;
        if (m_playback)
            m_playback->Stop();
        if (Hwnd())
            KillTimer(Hwnd(), kTimerPlay);
        Rebuild();
    }

    void EditorWindow::OnPlayTick()
    {
        if (!m_playing || !m_playback)
            return;
        const int64_t at = m_playback->PositionUs();
        // Follow the playhead a page at a time when zoomed in.
        if (at < m_view.startUs || at > m_view.EndUs())
        {
            m_view.startUs = at;
            m_view.Clamp();
            ViewChanged();
        }
        SetPlayhead(at, false);
        if (m_playback->AtEnd())
            StopPlay();
    }

    void EditorWindow::PaintFooter(ui::Painter& p)
    {
        p.Line(0, m_rcFooter.y + 0.5f, m_rcFooter.Right(), m_rcFooter.y + 0.5f, Theme().line);
        // The hovered control's hint, else the keys for this tool.
        std::wstring hint;
        if (const auto* c = m_hover ? ControlById(m_hover) : nullptr)
            hint = c->tip;
        if (hint.empty() && m_hoverX >= 0 && Cur().ready)
            hint = FormatTimecode(TT(std::clamp(m_hoverX, m_rcTrack.x, m_rcTrack.Right())));
        if (hint.empty())
            hint = Hint();
        float right = m_rcFooter.Right();
        for (const auto& c : m_controls)
            if (c.rect.y >= m_rcFooter.y)
                right = std::min(right, c.rect.x);
        p.Text(hint, { kPad, m_rcFooter.y, right - kPad - 16, m_rcFooter.h }, ui::Font::Small, Theme().textDim);
    }
}
