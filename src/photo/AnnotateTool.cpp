#include "photo/PhotoTools.h"
#include "Utf.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <format>

namespace sd::photo
{
    using namespace sd::canvas;

    std::unique_ptr<ICanvasTool> MakeAnnotateTool(const std::vector<std::wstring>& files, const AnnotateOptions& options)
    {
        return std::make_unique<AnnotateTool>(files, options);
    }

    namespace
    {
        using Mode = AnnotateTool::Mode;

        struct ModeInfo
        {
            Mode mode;
            const wchar_t* label;
            unsigned key;
            wchar_t icon;   // its tile in the panel (Segoe Fluent Icons)
        };
        const ModeInfo kModes[] = {
            { Mode::Select, L"Select", 'V', 0xE7C9 },       { Mode::Text, L"Text", 'T', 0xE8D2 },          { Mode::Date, L"Date", 'D', 0xE787 },
            { Mode::Check, L"Tick", 'K', 0xE73E },          { Mode::Cross, L"Cross", 'X', 0xE711 },        { Mode::Arrow, L"Arrow", 'A', 0xE72A },
            { Mode::Rect, L"Box", 'B', 0xE739 },            { Mode::Ellipse, L"Circle", 'O', 0xEA3A },     { Mode::Callout, L"Callout", 'C', 0xE8BD },
            { Mode::Number, L"Number", 'N', 0xE8FD },       { Mode::Magnifier, L"Magnifier", 'M', 0xE71E }, { Mode::Pen, L"Pen", 'P', 0xE76D },
            { Mode::Highlighter, L"Highlighter", 'H', 0xE7E6 },
        };

        // Six colours and a custom chip fill the panel's row; a document's page starts with inks.
        const uint32_t kPalette[] = { 0xE81123, 0xFFB900, 0x10893E, 0x0078D4, 0x111111, 0xFFFFFF };
        const uint32_t kInks[] = { 0x111111, 0x1F3A93, 0xE81123, 0x10893E, 0x0078D4, 0xFFFFFF };

        // The type buttons (Segoe Fluent Icons) and what they do.
        enum TypeButton { kBold, kItalic, kUnderline, kLeft, kCentre, kRight, kSmaller, kBigger };
        const wchar_t kTypeIcons[] = { 0xE8DD, 0xE8DB, 0xE8DC, 0xE8E4, 0xE8E3, 0xE8E2, 0xE8E7, 0xE8E8 };
        const wchar_t* const kTypeNames[] = { L"Bold (Ctrl+B)",           L"Italic (Ctrl+I)",          L"Underline (Ctrl+U)",
                                              L"Align left (Ctrl+L)",     L"Centre (Ctrl+E)",          L"Align right (Ctrl+R)",
                                              L"Smaller text (Ctrl+[)",   L"Bigger text (Ctrl+])" };

        // Type sizes the size buttons step through, in points (Word's list).
        const double kSizes[] = { 6, 7, 8, 9, 10, 11, 12, 14, 16, 18, 20, 22, 24, 26, 28, 32, 36, 40, 44, 48, 54, 60, 66, 72, 80, 88, 96 };

        double StepSize(double current, int steps)
        {
            double size = current;
            for (int i = 0; i < std::abs(steps); ++i)
            {
                if (steps > 0)
                {
                    const auto it = std::upper_bound(std::begin(kSizes), std::end(kSizes), size + 0.01);
                    size = it != std::end(kSizes) ? *it : std::round(size * 1.1);
                }
                else
                {
                    const auto it = std::lower_bound(std::begin(kSizes), std::end(kSizes), size - 0.01);
                    size = it != std::begin(kSizes) ? *(it - 1) : std::max(1.0, std::round(size * 0.9 * 10) / 10);
                    if (size > kSizes[std::size(kSizes) - 1] && it == std::end(kSizes))
                        size = std::max(kSizes[std::size(kSizes) - 1], std::round(size / 1.1));
                }
            }
            return size;
        }

        Mark MarkFor(Mode m)
        {
            switch (m)
            {
            case Mode::Text:
            case Mode::Date: return Mark::Text;
            case Mode::Check: return Mark::Check;
            case Mode::Cross: return Mark::Cross;
            case Mode::Rect: return Mark::Rect;
            case Mode::Ellipse: return Mark::Ellipse;
            case Mode::Callout: return Mark::Callout;
            case Mode::Number: return Mark::Number;
            case Mode::Magnifier: return Mark::Magnifier;
            case Mode::Pen: return Mark::Pen;
            case Mode::Highlighter: return Mark::Highlighter;
            case Mode::Place: return Mark::Image;
            default: return Mark::Arrow;
            }
        }

        bool Freehand(Mode m) { return m == Mode::Pen || m == Mode::Highlighter; }
        bool HasText(Mark k) { return k == Mark::Text || k == Mark::Callout; }
        double Dist(PointD a, PointD b) { return std::hypot(a.x - b.x, a.y - b.y); }

        // A callout's box for its text, centred on `centre`.
        void PlaceCallout(Annotation& m, PointD centre)
        {
            const double w = m.fontSize * 7, h = m.fontSize * 2.4;
            m.a = { centre.x - w / 2, centre.y - h / 2 };
            m.b = { centre.x + w / 2, centre.y + h / 2 };
        }

        // The text area of a text or callout mark: where a click places the caret.
        RectD TextArea(const Annotations& marks, const Annotation& m) { return m.kind == Mark::Text ? marks.TextBox(m) : FromCorners(m.a, m.b); }

        CursorShape GripCursor(Part part, PointD at, const RectD& box)
        {
            switch (part)
            {
            case Part::Left:
            case Part::Right: return CursorShape::SizeWE;
            case Part::Top:
            case Part::Bottom: return CursorShape::SizeNS;
            case Part::A:
            case Part::B:
            case Part::AB:
            case Part::BA:
            {
                const PointD c = box.Centre();
                return (at.x < c.x) == (at.y < c.y) ? CursorShape::SizeNWSE : CursorShape::SizeNESW;
            }
            default: return CursorShape::Cross;
            }
        }

        uint32_t InkOf(const Annotation& m) { return m.fill ? ContrastingText(m.color) : m.color; }
    }

    AnnotateTool::AnnotateTool(std::vector<std::wstring> files, AnnotateOptions options) : PhotoTool(std::move(files)), m_options(options)
    {
        m_marks.resize(std::max<size_t>(1, m_files.size()));
        m_seeded.assign(m_marks.size(), false);
        if (m_options.document)
            m_color = 0x111111;   // dark ink on paper
        if (m_options.sign)
            m_mode = Mode::Text;
        LoadSignatures();
    }

    std::wstring AnnotateTool::Title() const
    {
        if (m_options.sign)
            return L"Fill and sign";
        return m_options.document ? L"Annotate" : L"Annotate Photos";
    }

    std::wstring AnnotateTool::ToolName() const { return m_options.sign ? L"Fill and sign" : L"Annotate"; }

    std::wstring AnnotateTool::TodayText()
    {
        wchar_t text[80] = L"";
        if (!GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_SHORTDATE, nullptr, nullptr, text, static_cast<int>(std::size(text)), nullptr))
        {
            SYSTEMTIME t{};
            GetLocalTime(&t);
            return std::format(L"{:04}-{:02}-{:02}", t.wYear, t.wMonth, t.wDay);
        }
        return text;
    }

    void AnnotateTool::UseSignatures(SignatureStore store)
    {
        m_store = std::move(store);
        LoadSignatures();
    }

    void AnnotateTool::LoadSignatures() { m_signatures = m_store.List(); }

    void AnnotateTool::Seed(size_t file, Annotations marks)
    {
        if (file >= m_marks.size())
            return;
        m_marks[file] = std::move(marks);
        m_seeded[file] = true;
    }

    void AnnotateTool::ImageReady(size_t index, int width, int height)
    {
        StopEditing();   // on the picture being left
        PhotoTool::ImageReady(index, width, height);
    }

    void AnnotateTool::PictureReady(size_t)
    {
        m_selected = m_editing = m_dragId = 0;
        m_dpi = m_photo.raster ? m_photo.raster->dpiX : 0;
        RelayoutAll();
        // Pictures an earlier edit placed (a page edited again) are loaded for the preview.
        std::vector<std::wstring> missing;
        for (const auto& m : Marks().Items())
            if (m.kind == Mark::Image && !m_images.contains(m.image) && std::find(missing.begin(), missing.end(), m.image) == missing.end())
                missing.push_back(m.image);
        if (missing.empty() || !m_host)
            return;
        m_host->RunAsync([this, missing](std::stop_token) -> std::function<void()> {
            auto loaded = std::make_shared<std::map<std::wstring, Picture>>();
            for (const auto& path : missing)
            {
                auto r = std::make_shared<Raster>();
                std::wstring error;
                if (SUCCEEDED(LoadRaster(path, *r, error, 2048)))
                    (*loaded)[path] = Picture::Of(std::move(r));
            }
            return [this, loaded] {
                for (auto& [path, picture] : *loaded)
                    m_images.emplace(path, std::move(picture));
                Touch();
                m_host->Redraw();
            };
        });
    }

    void AnnotateTool::SetMode(Mode mode)
    {
        StopEditing();
        if (m_mode == Mode::Place && mode != Mode::Place)
        {
            m_placePath.clear();
            m_placePicture = {};
        }
        m_mode = mode;
        if (mode != Mode::Select)
            m_selected = 0;
    }

    double AnnotateTool::Tolerance(const Pointer& p) const { return std::max(0.5, p.tolerance); }

    double AnnotateTool::PixelsPerUnit() const
    {
        if (!m_options.document)
            return 1;
        // A page is edited as a picture of it (PageEdits.h: 200 dpi, less for huge pages).
        return (m_dpi > 1 ? m_dpi : 200.0) / 72.0;
    }

    double AnnotateTool::DefaultTextSize() const { return m_options.document ? 11 * PixelsPerUnit() : DefaultFont(m_width, m_height); }

    Annotation AnnotateTool::Style(Mark kind) const
    {
        Annotation a;
        a.kind = kind;
        a.color = m_color;
        const double stroke = m_stroke > 0 ? m_stroke
                              : m_options.document ? std::max(2.0, std::round(1.5 * PixelsPerUnit()))
                                                   : DefaultStroke(m_width, m_height);
        a.width = kind == Mark::Highlighter ? stroke * 5 : stroke;
        const double labels = m_options.document ? 14 * PixelsPerUnit() : DefaultFont(m_width, m_height);
        a.fontSize = m_font > 0 ? m_font : kind == Mark::Text ? DefaultTextSize() : labels;
        a.fill = m_fill && (kind == Mark::Rect || kind == Mark::Ellipse || kind == Mark::Text);
        a.zoom = m_zoom;
        a.radius = std::max(12.0, std::round(std::min(m_width, m_height) * 0.06));
        if (kind == Mark::Text)
        {
            a.font = m_family;
            a.weight = m_weight;
            a.italic = m_italic;
            a.underline = m_underline;
            a.align = m_align;
        }
        return a;
    }

    void AnnotateTool::Relayout(uint32_t id)
    {
        auto& marks = Marks();
        const auto* m = marks.Find(id);
        if (!m || !HasText(m->kind))
        {
            m_layouts.erase(id);
            return;
        }
        TextLayoutCache cache;
        SizeD measured;
        if (!LayOutMarkText(*m, marks, cache.info, cache.origin, &measured))
        {
            m_layouts.erase(id);
            return;
        }
        if (m->kind == Mark::Text)
            marks.SetMeasured(id, measured);
        m_layouts[id] = std::move(cache);
    }

    void AnnotateTool::RelayoutAll()
    {
        m_layouts.clear();
        for (const auto& m : Marks().Items())
            if (HasText(m.kind))
                Relayout(m.id);
    }

    const AnnotateTool::TextLayoutCache* AnnotateTool::LayoutOf(uint32_t id) const
    {
        const auto it = m_layouts.find(id);
        return it == m_layouts.end() ? nullptr : &it->second;
    }

    void AnnotateTool::StartEditing(uint32_t id, size_t caret)
    {
        const auto* m = Marks().Find(id);
        if (!m || !HasText(m->kind))
            return;
        m_editing = m_selected = id;
        m_cursor.Collapse(std::min(caret, m->text.size()));
        m_goalX = -1;
        Relayout(id);
        Touch();
    }

    void AnnotateTool::StopEditing()
    {
        if (!m_editing)
            return;
        const uint32_t id = std::exchange(m_editing, 0);
        m_selecting = false;
        m_cursor = {};
        auto& marks = Marks();
        if (const auto* m = marks.Find(id); m && m->kind == Mark::Text && m->text.empty())
        {
            marks.Remove(id);
            m_layouts.erase(id);
            if (m_selected == id)
                m_selected = 0;
        }
        Touch();
    }

    void AnnotateTool::ApplyStyle(const std::function<void(Annotation&)>& change)
    {
        if (auto* m = Marks().Find(m_selected))
        {
            change(*m);
            Relayout(m->id);
            Touch();
        }
    }

    uint32_t AnnotateTool::InsertImage(const std::wstring& path, Picture picture)
    {
        if (picture.Empty() || m_width <= 0)
            return 0;
        const double side = std::min(m_width, m_height) * 0.4;
        const double k = side / std::max(picture.width, picture.height);
        const double w = picture.width * k, h = picture.height * k;
        Annotation a = Style(Mark::Image);
        a.image = path;
        a.a = { (m_width - w) / 2, (m_height - h) / 2 };
        a.b = { a.a.x + w, a.a.y + h };
        m_images[path] = std::move(picture);
        StopEditing();
        m_mode = Mode::Select;
        m_selected = Marks().Add(std::move(a));
        Touch();
        return m_selected;
    }

    void AnnotateTool::BeginPlacing(const std::wstring& path, Picture picture, bool signature)
    {
        if (picture.Empty())
            return;
        StopEditing();
        m_selected = 0;
        m_placePath = path;
        m_placePicture = std::move(picture);
        m_placeSignature = signature;
        m_mode = Mode::Place;
        Touch();
    }

    SizeD AnnotateTool::PlacedSize(const Picture& picture) const
    {
        const double aspect = picture.width > 0 && picture.height > 0 ? double(picture.width) / picture.height : 3.0;
        // A signature about as wide as one written on a form: 2.4 inches on a page, a quarter of a photo.
        double w = m_options.document ? 2.4 * 72 * PixelsPerUnit() : m_width * 0.25;
        w = std::min(w, m_width * 0.6);
        double h = w / aspect;
        if (h > m_height * 0.25)
        {
            h = m_height * 0.25;
            w = h * aspect;
        }
        return { std::max(4.0, w), std::max(4.0, h) };
    }

    Response AnnotateTool::Place(PointD at)
    {
        if (m_placePicture.Empty())
        {
            m_mode = Mode::Select;
            return { true, false, true };
        }
        const SizeD size = PlacedSize(m_placePicture);
        Annotation a = Style(Mark::Image);
        a.image = m_placePath;
        a.signature = m_placeSignature;
        a.a = { at.x - size.w / 2, at.y - size.h / 2 };
        a.b = { a.a.x + size.w, a.a.y + size.h };
        m_images[m_placePath] = m_placePicture;
        m_selected = Marks().Add(std::move(a));
        m_placePath.clear();
        m_placePicture = {};
        m_mode = Mode::Select;
        Touch();
        return kChanged;
    }

    bool AnnotateTool::Render(ID2D1DeviceContext* dc, double scale, unsigned device)
    {
        UseDevice(device);
        if (m_photo.Empty())
            return false;
        return SUCCEEDED(DrawAnnotated(dc, device, scale, m_photo, Marks(), m_images, m_bitmaps));
    }

    void AnnotateTool::Draw(IPainter& p) const
    {
        const auto& marks = m_marks[m_index];
        const auto& theme = p.Theme();
        if (m_mode == Mode::Place && m_hovering && !m_placePicture.Empty())
        {
            // Where the picture lands.
            const SizeD size = PlacedSize(m_placePicture);
            const RectD r{ m_hover.x - size.w / 2, m_hover.y - size.h / 2, size.w, size.h };
            p.StrokeRect(r, theme.accent, 1.5f, true);
            p.Tag({ r.x, r.Bottom() + 6 / p.Zoom() }, L"Click to place · Esc cancels", theme.accent, theme.onAccent);
        }
        const auto* m = marks.Find(m_selected);
        if (!m)
            return;
        const RectD box = marks.Bounds(*m);
        const double pad = 4 / p.Zoom();
        p.StrokeRect({ box.x - pad, box.y - pad, box.w + 2 * pad, box.h + 2 * pad }, theme.accent, 1.5f, true);
        if (m_editing == m->id)
            if (const auto* cache = LayoutOf(m->id))
            {
                // The selection, then the caret, drawn over the text.
                const PointD o = cache->origin;
                for (const auto& r : SelectionRects(cache->info, m_cursor.Start(), m_cursor.End(), m->fontSize * 0.3))
                    p.FillRect({ o.x + r.x, o.y + r.y, r.w, r.h }, theme.accent.WithAlpha(0.35f));
                const CaretBox caret = CaretAt(cache->info, m_cursor.caret);
                const double half = 0.75 / p.Zoom();
                p.FillRect({ o.x + caret.x - half, o.y + caret.top, 2 * half, caret.height }, Color::Rgb(InkOf(*m)));
            }
        for (const auto& g : marks.Grips(*m))
            p.Grip(g.at, m_dragId == m->id && m_dragPart == g.part);
    }

    CursorShape AnnotateTool::CursorAt(const Pointer& p) const
    {
        const auto& marks = m_marks[m_index];
        const double tol = Tolerance(p);
        if (m_mode == Mode::Place)
            return CursorShape::Cross;
        if (const auto* m = marks.Find(m_selected))
        {
            const Part part = marks.GripAt(*m, p.at, tol);
            if (part != Part::None && part != Part::Body)
            {
                for (const auto& g : marks.Grips(*m))
                    if (g.part == part)
                        return m->kind == Mark::Text || m->kind == Mark::Image || m->kind == Mark::Check || m->kind == Mark::Cross
                                   ? GripCursor(part, g.at, marks.Bounds(*m))
                                   : CursorShape::Cross;
                return CursorShape::Cross;
            }
            if (part == Part::Body && !Freehand(m_mode))
            {
                if (HasText(m->kind) && TextArea(marks, *m).Contains(p.at) && (m_editing == m->id || m->kind == Mark::Text))
                    return CursorShape::Text;
                return CursorShape::Move;
            }
        }
        if (!Freehand(m_mode))
            if (const uint32_t id = marks.HitTest(p.at, tol))
            {
                const auto* m = marks.Find(id);
                return m && m->kind == Mark::Text ? CursorShape::Text : CursorShape::Move;
            }
        if (m_mode == Mode::Text)
            return CursorShape::Text;
        return m_mode == Mode::Select ? CursorShape::Arrow : CursorShape::Cross;
    }

    Response AnnotateTool::Hover(const Pointer& p)
    {
        m_hover = p.at;
        m_hovering = true;
        return m_mode == Mode::Place ? kRedraw : Response{};
    }

    Response AnnotateTool::PointerDown(const Pointer& p)
    {
        auto& marks = Marks();
        const double tol = Tolerance(p);
        m_from = p.at;
        m_moved = m_created = m_wasSelected = m_selecting = m_clickEdits = false;
        m_dragId = 0;
        m_dragPart = Part::None;
        if (m_mode == Mode::Place)
            return Place(p.at);

        // Typing: a press in the text moves the caret (Shift extends the selection, a double click
        // takes a word, a triple click all of it); the box's grips and frame still resize and move it.
        bool typed = false;
        if (auto* e = marks.Find(m_editing))
        {
            const Part part = marks.GripAt(*e, p.at, tol);
            const auto* cache = LayoutOf(e->id);
            if (part != Part::None && part != Part::Body)
            {
                m_dragId = e->id;
                m_dragPart = part;
                m_before = *e;
                m_wasSelected = true;
                return { true, false, true };
            }
            if (cache && TextArea(marks, *e).Contains(p.at))
            {
                const size_t pos = PositionAt(cache->info, { p.at.x - cache->origin.x, p.at.y - cache->origin.y });
                if (p.clicks == 2)
                    SelectWord(m_cursor, e->text, pos);
                else if (p.clicks >= 3)
                {
                    m_cursor.anchor = 0;
                    m_cursor.caret = e->text.size();
                }
                else if (p.shift)
                    m_cursor.caret = pos;
                else
                    m_cursor.Collapse(pos);
                m_goalX = -1;
                m_selecting = p.clicks == 1;
                return kRedraw;
            }
            if (part == Part::Body)
            {
                m_dragId = e->id;   // the frame around the text: moves the box
                m_dragPart = Part::Body;
                m_before = *e;
                m_wasSelected = true;
                return { true, false, true };
            }
            const std::wstring before = e->text;
            StopEditing();   // a press anywhere else ends the typing
            typed = !before.empty();
            if (typed && m_host)
                m_host->Changed();
        }

        // A grip (or the body) of the selected mark, then any other mark.
        if (auto* sel = marks.Find(m_selected))
        {
            const Part part = marks.GripAt(*sel, p.at, tol);
            if (part != Part::None && !(part == Part::Body && Freehand(m_mode)))
            {
                m_dragId = sel->id;
                m_dragPart = part;
                m_before = *sel;
                m_wasSelected = true;
                m_clickEdits = part == Part::Body && HasText(sel->kind) && TextArea(marks, *sel).Contains(p.at);
                if (m_clickEdits && p.clicks >= 2)
                {
                    StartEditing(sel->id, 0);
                    if (const auto* cache = LayoutOf(sel->id))
                        SelectWord(m_cursor, sel->text, PositionAt(cache->info, { p.at.x - cache->origin.x, p.at.y - cache->origin.y }));
                    m_dragId = 0;
                    return { true, false, true };
                }
                return { true, false, true };
            }
        }
        if (!Freehand(m_mode))
            if (const uint32_t id = marks.HitTest(p.at, tol))
            {
                m_selected = m_dragId = id;
                m_dragPart = Part::Body;
                m_before = *marks.Find(id);
                // A text box takes a click as "type here"; a callout once it is selected.
                m_clickEdits = m_before.kind == Mark::Text || (m_before.kind == Mark::Callout && m_mode == Mode::Text);
                return { true, false, true };
            }
        if (m_mode == Mode::Select)
        {
            const bool had = m_selected != 0;
            m_selected = 0;
            return had || typed ? Response{ true, false, true } : Response{};
        }

        // A new mark.
        Annotation a = Style(MarkFor(m_mode));
        a.a = a.b = a.tip = p.at;
        m_dragPart = Part::B;
        switch (a.kind)
        {
        case Mark::Text:
            a.a = { p.at.x, p.at.y - a.fontSize * 0.66 };
            a.b = a.a;
            if (m_mode == Mode::Date)
            {
                a.text = TodayText();
                m_dragPart = Part::Body;
            }
            else
                m_dragPart = Part::None;   // a drag draws the box (with Auto-wrap at its width)
            break;
        case Mark::Check:
        case Mark::Cross:
        {
            const double side = std::max(8.0, a.fontSize * 0.9);
            a.a = { p.at.x - side / 2, p.at.y - side / 2 };
            a.b = { p.at.x + side / 2, p.at.y + side / 2 };
            m_dragPart = Part::Body;
            break;
        }
        case Mark::Number: m_dragPart = Part::Body; break;
        case Mark::Pen:
        case Mark::Highlighter: a.points = { p.at }; m_dragPart = Part::None; break;
        case Mark::Callout: PlaceCallout(a, p.at); break;
        default: break;
        }
        m_dragId = m_selected = marks.Add(a);
        m_before = *marks.Find(m_dragId);
        m_created = true;
        if (a.kind == Mark::Text)
        {
            Relayout(m_dragId);
            if (m_mode == Mode::Text)
                StartEditing(m_dragId, 0);
        }
        Touch();
        return { true, false, true };
    }

    Response AnnotateTool::PointerMove(const Pointer& p)
    {
        auto& marks = Marks();
        if (m_selecting)
        {
            const auto* e = marks.Find(m_editing);
            const auto* cache = LayoutOf(m_editing);
            if (!e || !cache)
                return {};
            m_cursor.caret = PositionAt(cache->info, { p.at.x - cache->origin.x, p.at.y - cache->origin.y });
            return kRedraw;
        }
        auto* m = marks.Find(m_dragId);
        if (!m)
            return {};
        if (!m_moved && Dist(p.at, m_from) <= Tolerance(p) / 2)
            return {};
        m_moved = true;
        m_clickEdits = false;
        if (m_created && (m->kind == Mark::Pen || m->kind == Mark::Highlighter))
        {
            if (m->points.empty() || Dist(m->points.back(), p.at) >= Tolerance(p) / 3)
                m->points.push_back(p.at);
        }
        else if (m_created && m->kind == Mark::Callout)
            PlaceCallout(*m, p.at);   // pressed where it points, dragged to where the bubble goes
        else if (m_created && m->kind == Mark::Text && m_dragPart == Part::None)
        {
            // Drawing a text box: it wraps at the width drawn.
            const RectD r = FromCorners(m_from, p.at);
            m->wrap = true;
            m->a = { r.x, r.y };
            m->b = { r.Right(), r.Bottom() };
            Relayout(m->id);
        }
        else if (m_created && (m->kind == Mark::Check || m->kind == Mark::Cross))
        {
            // Pressed on one corner, dragged to the other: a square.
            const double d = std::max({ std::abs(p.at.x - m_from.x), std::abs(p.at.y - m_from.y), 4.0 });
            m->a = { p.at.x >= m_from.x ? m_from.x : m_from.x - d, p.at.y >= m_from.y ? m_from.y : m_from.y - d };
            m->b = { m->a.x + d, m->a.y + d };
        }
        else
        {
            PointD to = p.at;
            if (p.shift && m_created && (m->kind == Mark::Rect || m->kind == Mark::Ellipse))
            {
                // A square or a circle.
                const double d = std::max(std::abs(to.x - m_from.x), std::abs(to.y - m_from.y));
                to = { m_from.x + (to.x >= m_from.x ? d : -d), m_from.y + (to.y >= m_from.y ? d : -d) };
            }
            else if (p.shift && m->kind == Mark::Arrow && (m_dragPart == Part::A || m_dragPart == Part::B))
            {
                // Snapped to 45 degrees about the other end.
                const PointD fixed = m_dragPart == Part::B ? m_before.a : m_before.b;
                const PointD moving = m_dragPart == Part::B ? m_before.b : m_before.a;
                const PointD end{ moving.x + to.x - m_from.x, moving.y + to.y - m_from.y };
                const double len = Dist(end, fixed), step = 3.14159265358979 / 4;
                const double angle = std::round(std::atan2(end.y - fixed.y, end.x - fixed.x) / step) * step;
                const PointD snapped{ fixed.x + std::cos(angle) * len, fixed.y + std::sin(angle) * len };
                to = { m_from.x + snapped.x - moving.x, m_from.y + snapped.y - moving.y };
            }
            marks.Drag(*m, m_before, m_dragPart, m_from, to, p.shift);
            if (HasText(m->kind))
                Relayout(m->id);
        }
        Touch();
        return kRedraw;
    }

    Response AnnotateTool::PointerUp(const Pointer& p)
    {
        auto& marks = Marks();
        if (m_selecting)
        {
            m_selecting = false;
            return kRedraw;
        }
        auto* m = marks.Find(m_dragId);
        const uint32_t id = std::exchange(m_dragId, 0);
        if (!m)
            return {};
        Touch();
        const double tol = Tolerance(p);
        if (m_created)
        {
            bool keep = true;
            switch (m->kind)
            {
            case Mark::Arrow: keep = Dist(m->a, m->b) >= tol * 2; break;
            case Mark::Rect:
            case Mark::Ellipse: keep = std::abs(m->b.x - m->a.x) >= tol * 2 && std::abs(m->b.y - m->a.y) >= tol * 2; break;
            case Mark::Text:
                if (m->wrap && m->b.x - m->a.x < MinTextWidth(*m))
                {
                    m->wrap = false;   // too narrow to be meant as a box: a plain text that fits its words
                    m->b = m->a;
                }
                Relayout(id);
                if (m_editing == id)
                    return { true, false, true };   // the undo step comes when the typing ends
                return kChanged;
            case Mark::Callout:
                if (!m_moved)
                    PlaceCallout(*m, { m->tip.x + m->fontSize * 4.5, m->tip.y - m->fontSize * 3 });
                StartEditing(id, m->text.size());   // straight to typing its text
                break;
            case Mark::Magnifier:
                if (!m_moved || Dist(m->a, m->b) < m->radius)
                {
                    // A click: the loupe beside the spot, inside the picture where it fits.
                    const double R = m->radius * m->zoom, off = (m->radius + R) * 0.9;
                    const double x = m->a.x + off + R <= m_width ? m->a.x + off : m->a.x - off;
                    const double y = m->a.y - off - R >= 0 ? m->a.y - off : m->a.y + off;
                    m->b = { x, y };
                }
                break;
            default: break;
            }
            if (!keep)
            {
                marks.Remove(id);
                m_selected = 0;
                return { true, false, true };
            }
            if (m->kind == Mark::Callout)
                return { true, false, true };
            return kChanged;
        }
        if (m_moved)
        {
            if (HasText(m->kind))
                Relayout(id);
            return kChanged;
        }
        // A click on a text box types where it landed; a callout already selected (or in the Text
        // tool) is typed in from its end.
        if (m_editing == id)
            return { true, false, true };
        if (m_clickEdits && m->kind == Mark::Text)
        {
            Relayout(id);
            size_t pos = m->text.size();
            if (const auto* cache = LayoutOf(id))
                pos = PositionAt(cache->info, { p.at.x - cache->origin.x, p.at.y - cache->origin.y });
            StartEditing(id, pos);
            return { true, false, true };
        }
        if (HasText(m->kind) && (m_wasSelected || m_mode == Mode::Text))
            StartEditing(id, m->text.size());
        return { true, false, true };
    }

    Response AnnotateTool::PointerCancel()
    {
        auto& marks = Marks();
        m_selecting = false;
        const uint32_t id = std::exchange(m_dragId, 0);
        if (auto* m = marks.Find(id))
        {
            if (m_created)
            {
                if (m_editing == id)
                    m_editing = 0;
                marks.Remove(id);
                m_layouts.erase(id);
                m_selected = 0;
            }
            else
            {
                *m = m_before;
                Relayout(id);
            }
        }
        Touch();
        return { true, false, true };
    }

    bool AnnotateTool::Escape()
    {
        if (m_mode == Mode::Place)
        {
            SetMode(Mode::Select);
            if (m_host)
                m_host->SetStatus({});
            return true;
        }
        if (m_selected)
        {
            m_selected = 0;
            Touch();
            return true;
        }
        return false;
    }

    Response AnnotateTool::Char(wchar_t ch)
    {
        auto* m = Marks().Find(m_editing);
        if (!m)
            return {};
        // An undo step at the end of each word.
        const bool wordEnd = (ch == L' ' || ch == L'\t') && m_cursor.caret > 0 && m_cursor.caret <= m->text.size() && m->text[m_cursor.caret - 1] != L' ';
        if (!InsertText(m->text, m_cursor, std::wstring_view(&ch, 1), 2000))
            return {};
        m_goalX = -1;
        Relayout(m->id);
        Touch();
        return wordEnd ? kChanged : kRedraw;
    }

    Response AnnotateTool::ChangeFontSize(int steps)
    {
        const double unit = PixelsPerUnit();
        auto* sel = Marks().Find(m_selected);
        const bool sized = sel && (HasText(sel->kind) || sel->kind == Mark::Number);
        const double current = (sized ? sel->fontSize : m_font > 0 ? m_font : DefaultTextSize()) / unit;
        const double next = std::clamp(StepSize(current, steps), 1.0, 4000 / unit) * unit;
        m_font = next;
        if (!sized)
            return { true, false, true };
        ApplyStyle([next](Annotation& a) {
            if (a.kind == Mark::Callout)
            {
                // The bubble grows with its text, about its centre.
                const PointD c = FromCorners(a.a, a.b).Centre();
                const double k = next / std::max(1.0, a.fontSize);
                a.a = { c.x + (a.a.x - c.x) * k, c.y + (a.a.y - c.y) * k };
                a.b = { c.x + (a.b.x - c.x) * k, c.y + (a.b.y - c.y) * k };
            }
            a.fontSize = next;
        });
        return kChanged;
    }

    Response AnnotateTool::StyleKey(unsigned vk, bool shift)
    {
        auto* sel = Marks().Find(m_selected);
        const bool text = sel && sel->kind == Mark::Text;
        switch (vk)
        {
        case VK_OEM_4:   // [
            return ChangeFontSize(-1);
        case VK_OEM_6:   // ]
            return ChangeFontSize(1);
        case VK_OEM_COMMA:   // Ctrl+Shift+<
            return shift ? ChangeFontSize(-1) : Response{};
        case VK_OEM_PERIOD:   // Ctrl+Shift+>
            return shift ? ChangeFontSize(1) : Response{};
        case 'B':
        case 'I':
        case 'U':
        case 'L':
        case 'E':
        case 'R':
        {
            if (!text)
                return {};
            const int button = vk == 'B' ? kBold : vk == 'I' ? kItalic : vk == 'U' ? kUnderline : vk == 'L' ? kLeft : vk == 'E' ? kCentre : kRight;
            ControlEvent e{ "type", button };
            return OnControl(e);
        }
        case 'D':
        {
            if (!sel || m_editing)
                return {};
            // A copy just below and to the right, selected.
            Annotation copy = *sel;
            const double step = std::max(8.0, sel->fontSize * 0.5);
            Marks().Drag(copy, *sel, Part::Body, {}, { step, step });
            m_selected = Marks().Add(std::move(copy));
            Relayout(m_selected);
            Touch();
            return kChanged;
        }
        default: return {};
        }
    }

    Response AnnotateTool::TextKey(Annotation& m, unsigned vk, bool shift, bool ctrl)
    {
        const auto edited = [&](bool step) {
            m_goalX = -1;
            Relayout(m.id);
            Touch();
            return step ? kChanged : kRedraw;
        };
        const auto move = [&](CaretMove how) {
            Relayout(m.id);
            if (const auto* cache = LayoutOf(m.id))
                MoveCaret(m_cursor, m.text, cache->info, how, shift, m_goalX);
            return kRedraw;
        };
        switch (vk)
        {
        case VK_ESCAPE:
            StopEditing();
            return kChanged;
        case VK_RETURN:
            if (ctrl)
            {
                StopEditing();
                return kChanged;
            }
            return InsertText(m.text, m_cursor, L"\n", 2000) ? edited(true) : Response{};
        case VK_BACK: return DeleteBackward(m.text, m_cursor, ctrl) ? edited(false) : Response{};
        case VK_DELETE: return DeleteForward(m.text, m_cursor, ctrl) ? edited(false) : Response{};
        case VK_LEFT: return move(ctrl ? CaretMove::WordLeft : CaretMove::Left);
        case VK_RIGHT: return move(ctrl ? CaretMove::WordRight : CaretMove::Right);
        case VK_UP: return move(CaretMove::Up);
        case VK_DOWN: return move(CaretMove::Down);
        case VK_HOME: return move(ctrl ? CaretMove::TextStart : CaretMove::Home);
        case VK_END: return move(ctrl ? CaretMove::TextEnd : CaretMove::End);
        default: break;
        }
        if (!ctrl)
            return {};
        switch (vk)
        {
        case 'A':
            m_cursor.anchor = 0;
            m_cursor.caret = m.text.size();
            return kRedraw;
        case 'C':
        case 'X':
        {
            const std::wstring selected = SelectedText(m.text, m_cursor);
            if (selected.empty())
                return {};
            if (!m_host || !m_host->CopyText(selected))
                m_clipboard = selected;
            if (vk == 'C')
                return {};
            return DeleteBackward(m.text, m_cursor, false) ? edited(true) : Response{};
        }
        case 'V':
        {
            std::wstring pasted;
            if (!m_host || !m_host->PasteText(pasted))
                pasted = m_clipboard;
            pasted = CleanPasted(pasted);
            return !pasted.empty() && InsertText(m.text, m_cursor, pasted, 2000) ? edited(true) : Response{};
        }
        default: return StyleKey(vk, shift);
        }
    }

    Response AnnotateTool::Key(unsigned vk, bool shift, bool ctrl)
    {
        auto& marks = Marks();
        if (m_editing)
        {
            auto* m = marks.Find(m_editing);
            if (!m)
            {
                m_editing = 0;
                return kRedraw;
            }
            return TextKey(*m, vk, shift, ctrl);
        }
        if (!ctrl)
            for (const auto& info : kModes)
                if (vk == info.key)
                {
                    SetMode(info.mode);
                    return { true, false, true };
                }
        if (ctrl)
            return StyleKey(vk, shift);
        auto* m = marks.Find(m_selected);
        if (!m)
            return {};
        if (vk == VK_DELETE || vk == VK_BACK)
        {
            marks.Remove(m_selected);
            m_layouts.erase(m_selected);
            m_selected = 0;
            Touch();
            return kChanged;
        }
        if (vk == VK_F2 && HasText(m->kind))
        {
            StartEditing(m->id, m->text.size());
            return { true, false, true };
        }
        if (vk == VK_LEFT || vk == VK_RIGHT || vk == VK_UP || vk == VK_DOWN)
        {
            const double step = shift ? 10 : 1;
            const PointD d{ vk == VK_LEFT ? -step : vk == VK_RIGHT ? step : 0, vk == VK_UP ? -step : vk == VK_DOWN ? step : 0 };
            const Annotation before = *m;
            marks.Drag(*m, before, Part::Body, {}, d);
            Relayout(m->id);
            Touch();
            return kChanged;
        }
        return {};
    }

    // ---- The panel -------------------------------------------------------------------------------------

    void AnnotateTool::BuildTextPanel(std::vector<Control>& out, const Annotation* sel) const
    {
        const auto& source = sel && sel->kind == Mark::Text ? *sel : Style(Mark::Text);
        Control family;
        family.id = "family";
        family.kind = ControlKind::Dropdown;
        family.label = L"Font";
        family.fonts = true;
        family.text = FontOf(source);
        family.options = SystemFontFamilies();
        for (size_t i = 0; i < family.options.size(); ++i)
            if (_wcsicmp(family.options[i].c_str(), family.text.c_str()) == 0)
                family.selected = static_cast<int>(i);
        out.push_back(std::move(family));

        Control size;
        size.id = "textSize";
        size.kind = ControlKind::Number;
        size.label = L"Size";
        size.text = m_options.document ? L"pt" : L"px";
        size.value = std::round(source.fontSize / PixelsPerUnit() * 2) / 2;
        size.min = 1;
        size.max = std::round(4000 / PixelsPerUnit());
        size.decimals = std::abs(size.value - std::round(size.value)) > 0.01 ? 1 : 0;
        out.push_back(size);

        Control type;
        type.id = "type";
        type.kind = ControlKind::Icons;
        for (size_t i = 0; i < std::size(kTypeIcons); ++i)
        {
            type.icons.push_back(kTypeIcons[i]);
            type.options.emplace_back(kTypeNames[i]);
        }
        type.flags = { source.weight >= 600, source.italic, source.underline, source.align == Align::Left, source.align == Align::Centre,
                       source.align == Align::Right, false, false };
        out.push_back(std::move(type));

        Control wrap;
        wrap.id = "wrap";
        wrap.kind = ControlKind::Toggle;
        wrap.label = L"Auto-wrap";
        wrap.on = sel && sel->kind == Mark::Text ? sel->wrap : m_wrap;
        out.push_back(wrap);
    }

    void AnnotateTool::BuildPanel(std::vector<Control>& out) const
    {
        const auto& marks = m_marks[m_index];
        const auto* sel = marks.Find(m_selected);

        Control section;
        section.kind = ControlKind::Section;
        section.label = L"Add";
        out.push_back(section);
        Control modes;
        modes.id = "mode";
        modes.kind = ControlKind::Choice;
        for (size_t i = 0; i < std::size(kModes); ++i)
        {
            modes.options.emplace_back(kModes[i].label);
            modes.icons.push_back(kModes[i].icon);
            if (kModes[i].mode == m_mode)
                modes.selected = static_cast<int>(i);
        }
        out.push_back(modes);
        Control insert;
        insert.id = "image";
        insert.kind = ControlKind::Button;
        insert.label = L"Insert picture";
        insert.icon = 0xEB9F;   // Photo
        out.push_back(insert);

        // Signatures: the saved ones to pick, and two ways to add one.
        section.label = L"Signature";
        out.push_back(section);
        if (!m_signatures.empty())
        {
            Control saved;
            saved.id = "signature";
            saved.kind = ControlKind::Pictures;
            saved.pictures = m_signatures;
            for (size_t i = 0; i < m_signatures.size(); ++i)
                saved.options.push_back(std::format(L"Saved signature {}", i + 1));
            out.push_back(std::move(saved));
        }
        Control draw;
        draw.id = "drawSignature";
        draw.kind = ControlKind::Button;
        draw.label = L"Draw…";
        draw.icon = 0xEE56;   // Signature
        draw.half = true;
        out.push_back(draw);
        Control photo = draw;
        photo.id = "pictureSignature";
        photo.label = L"From a photo…";
        photo.icon = 0xEB9F;   // Photo
        out.push_back(photo);
        Control signNote;
        signNote.kind = ControlKind::Note;
        signNote.text = m_signatures.empty() ? L"Draw your signature, or take it from a photo or scan of it. Saved signatures stay on this PC."
                                             : L"Click one to place it. Saved signatures stay on this PC; × removes one.";
        out.push_back(signNote);

        const Mark kind = sel ? sel->kind : MarkFor(m_mode);
        section.label = sel ? std::format(L"{} (selected)", sel->kind == Mark::Image && sel->signature ? L"Signature" : MarkName(sel->kind))
                            : std::wstring(L"Style");
        out.push_back(section);
        if (kind == Mark::Text)
            BuildTextPanel(out, sel);
        if (kind != Mark::Image)
        {
            Control colors;
            colors.id = "color";
            colors.kind = ControlKind::Swatches;
            colors.custom = true;
            const uint32_t color = sel ? sel->color : m_color;
            const uint32_t* palette = m_options.document ? kInks : kPalette;
            for (size_t i = 0; i < std::size(kPalette); ++i)
            {
                colors.swatches.push_back(Color::Rgb(palette[i]));
                if (palette[i] == color)
                    colors.selected = static_cast<int>(i);
            }
            colors.swatches.push_back(m_custom ? Color::Rgb(m_custom) : Color{ 0, 0, 0, 0 });
            if (colors.selected < 0 && m_custom == color)
                colors.selected = static_cast<int>(std::size(kPalette));
            out.push_back(colors);
        }

        const double longSide = std::max({ 1, m_width, m_height });
        const bool stroked = kind != Mark::Text && kind != Mark::Number && kind != Mark::Callout && kind != Mark::Image && kind != Mark::Check &&
                             kind != Mark::Cross;
        if (stroked && !(sel && (kind == Mark::Rect || kind == Mark::Ellipse) && sel->fill))
        {
            Control width;
            width.id = "width";
            width.kind = ControlKind::Slider;
            width.label = kind == Mark::Highlighter ? L"Marker width" : L"Line width";
            width.text = L" px";
            const Annotation style = Style(kind);
            width.value = sel ? sel->width : style.width;
            width.min = 1;
            width.max = std::max(32.0, std::round(longSide / (kind == Mark::Highlighter ? 20 : 60)));
            width.value = std::min(width.value, width.max);
            out.push_back(width);
        }
        if (kind == Mark::Callout || kind == Mark::Number)
        {
            Control font;
            font.id = "font";
            font.kind = ControlKind::Slider;
            font.label = kind == Mark::Number ? L"Size" : L"Text size";
            font.text = L" px";
            font.value = sel ? sel->fontSize : Style(kind).fontSize;
            font.min = 8;
            font.max = std::max(96.0, std::round(longSide / 6));
            font.value = std::min(font.value, font.max);
            out.push_back(font);
        }
        if (kind == Mark::Rect || kind == Mark::Ellipse || kind == Mark::Text)
        {
            Control fill;
            fill.id = "fill";
            fill.kind = ControlKind::Toggle;
            fill.label = kind == Mark::Text ? L"On a coloured label" : L"Filled";
            fill.on = sel ? sel->fill : m_fill;
            out.push_back(fill);
        }
        if (kind == Mark::Magnifier)
        {
            Control zoom;
            zoom.id = "zoom";
            zoom.kind = ControlKind::Slider;
            zoom.label = L"Magnification";
            zoom.text = L"×";
            zoom.value = sel ? sel->zoom : m_zoom;
            zoom.min = 1.5;
            zoom.max = 6;
            zoom.decimals = 1;
            out.push_back(zoom);
        }
        if (sel)
        {
            if (HasText(sel->kind))
            {
                Control edit;
                edit.id = "editText";
                edit.kind = ControlKind::Button;
                edit.label = m_editing == sel->id ? L"Done typing" : L"Edit text";
                edit.icon = 0xE8D2;   // Font
                out.push_back(edit);
            }
            Control front;
            front.id = "front";
            front.kind = ControlKind::Button;
            front.label = L"To front";
            front.half = true;
            out.push_back(front);
            Control del = front;
            del.id = "delete";
            del.label = L"Delete";
            del.icon = 0xE74D;   // Delete
            out.push_back(del);
        }

        Control note;
        note.kind = ControlKind::Note;
        if (m_editing)
            note.text = L"Type · Enter for a new line · Ctrl+B, I, U for bold, italic, underline · Ctrl+[ and ] for the size · Esc when done";
        else if (sel && sel->kind == Mark::Text)
            note.text = sel->wrap ? L"Click in it to type. Drag the frame to move it; the handles resize the box and the lines wrap to it."
                                  : L"Click in it to type. Drag the frame to move it; a corner scales the text, a side sets a width to wrap at.";
        else if (sel && sel->kind == Mark::Image)
            note.text = L"Drag it into place; the corners resize it and keep its shape (Shift stretches it).";
        else
        {
            switch (m_mode)
            {
            case Mode::Select: note.text = L"Click a mark to change it; drag its grips to reshape it."; break;
            case Mode::Text: note.text = L"Click where the text goes and type, or drag a box for the text to wrap in."; break;
            case Mode::Date: note.text = L"Click where today's date goes."; break;
            case Mode::Check:
            case Mode::Cross: note.text = L"Click a box on the form to mark it; drag to make the mark bigger."; break;
            case Mode::Arrow: note.text = L"Drag from the tail to the point. Shift snaps to 45°."; break;
            case Mode::Rect:
            case Mode::Ellipse: note.text = L"Drag across what to mark. Shift keeps it even."; break;
            case Mode::Callout: note.text = L"Press on what it's about, drag to where the bubble goes, then type."; break;
            case Mode::Number: note.text = L"Click to place 1, 2, 3… Numbers follow the order they were placed in."; break;
            case Mode::Magnifier: note.text = L"Press on the detail, drag to where the loupe goes."; break;
            case Mode::Pen:
            case Mode::Highlighter: note.text = L"Draw on the picture."; break;
            case Mode::Place: note.text = L"Click where it goes. Esc cancels."; break;
            }
            if (m_mode != Mode::Place)
                note.text += L" Delete removes the selected mark; arrows nudge it.";
        }
        out.push_back(note);
        if (m_files.size() > 1)
        {
            Control files;
            files.kind = ControlKind::Note;
            size_t marked = 0;
            for (const auto& set : m_marks)
                marked += set.Items().empty() ? 0 : 1;
            files.text = std::format(L"Each {} has its own marks ({} of {} marked); {} without any are skipped.", m_options.document ? L"page" : L"picture",
                                     marked, m_files.size(), m_options.document ? L"pages" : L"pictures");
            out.push_back(files);
        }
    }

    void AnnotateTool::PickSignature(size_t index)
    {
        if (index >= m_signatures.size())
            return;
        const std::wstring path = m_signatures[index];
        if (const auto it = m_images.find(path); it != m_images.end())
        {
            BeginPlacing(path, it->second, true);
            return;
        }
        if (!m_host)
            return;
        m_host->SetStatus(L"Opening the signature…");
        m_host->RunAsync([this, path](std::stop_token) -> std::function<void()> {
            auto r = std::make_shared<Raster>();
            std::wstring error;
            const bool ok = SUCCEEDED(LoadRaster(path, *r, error, 2048));
            return [this, path, r, ok, error] {
                m_host->SetStatus(ok ? std::wstring(L"Click where the signature goes.") : L"Can't open that signature: " + error);
                if (ok)
                    BeginPlacing(path, Picture::Of(r), true);
                m_host->Redraw();
            };
        });
    }

    void AnnotateTool::AddSignature(bool fromPicture)
    {
        if (!m_host)
            return;
        StopEditing();
        m_host->NewSignature(fromPicture, [this](std::shared_ptr<const Raster> ink, bool remember) {
            if (!ink || ink->Empty())
                return;
            m_host->SetStatus(L"Saving the signature…");
            m_host->RunAsync([this, ink, remember, store = m_store](std::stop_token) -> std::function<void()> {
                std::wstring error;
                const std::wstring path = store.Add(*ink, error, !remember);
                return [this, ink, path, error] {
                    if (path.empty())
                    {
                        m_host->SetStatus(L"The signature could not be saved: " + error);
                        return;
                    }
                    LoadSignatures();
                    BeginPlacing(path, Picture::Of(ink), true);
                    m_host->SetStatus(L"Click where the signature goes.");
                    m_host->Redraw();
                };
            });
        });
    }

    Response AnnotateTool::OnControl(const ControlEvent& e)
    {
        if (e.id == "mode")
        {
            if (e.index < 0 || e.index >= static_cast<int>(std::size(kModes)))
                return {};
            SetMode(kModes[e.index].mode);
            return { true, false, true };
        }
        if (e.id == "image")
        {
            std::wstring path;
            if (!m_host || !m_host->PickImage(path))
                return {};
            if (const auto it = m_images.find(path); it != m_images.end())
            {
                InsertImage(path, it->second);
                return kChanged;
            }
            m_host->SetStatus(L"Opening the picture…");
            m_host->RunAsync([this, path](std::stop_token) -> std::function<void()> {
                auto r = std::make_shared<Raster>();
                std::wstring error;
                const bool ok = SUCCEEDED(LoadRaster(path, *r, error, 2048));
                return [this, path, r, ok, error] {
                    m_host->SetStatus(ok ? std::wstring() : L"Can't open that picture: " + error);
                    if (ok && InsertImage(path, Picture::Of(r)))
                        m_host->Changed();
                    m_host->Redraw();
                };
            });
            return {};
        }
        if (e.id == "signature")
        {
            if (e.index < 0 || e.index >= static_cast<int>(m_signatures.size()))
                return {};
            if (e.on)
            {
                const bool removed = m_store.Remove(m_signatures[static_cast<size_t>(e.index)]);
                LoadSignatures();
                if (m_host)
                    m_host->SetStatus(removed ? L"The saved signature was removed from this PC." : L"That signature could not be removed.");
                return { true, false, true };
            }
            PickSignature(static_cast<size_t>(e.index));
            return { true, false, true };
        }
        if (e.id == "drawSignature" || e.id == "pictureSignature")
        {
            AddSignature(e.id == "pictureSignature");
            return {};
        }
        auto* sel = Marks().Find(m_selected);
        const bool textSelected = sel && sel->kind == Mark::Text;
        if (e.id == "color")
        {
            uint32_t rgb = 0;
            const uint32_t* palette = m_options.document ? kInks : kPalette;
            if (e.index >= 0 && e.index < static_cast<int>(std::size(kPalette)))
                rgb = palette[e.index];
            else if (e.index == static_cast<int>(std::size(kPalette)))
            {
                if (e.rgb)
                    m_custom = e.rgb;
                rgb = m_custom;
            }
            if (!rgb && !(e.index >= 0 && e.index < static_cast<int>(std::size(kPalette))))
                return {};
            m_color = rgb;
            ApplyStyle([rgb](Annotation& a) { a.color = rgb; });
            return sel ? kChanged : Response{ true, false, true };
        }
        if (e.id == "family")
        {
            const auto& families = SystemFontFamilies();
            if (e.index < 0 || e.index >= static_cast<int>(families.size()))
                return {};
            m_family = families[static_cast<size_t>(e.index)];
            if (!textSelected)
                return { true, false, true };
            ApplyStyle([family = m_family](Annotation& a) { a.font = family; });
            return kChanged;
        }
        if (e.id == "textSize")
        {
            const double v = std::clamp(e.value, 1.0, 4000.0) * PixelsPerUnit();
            m_font = v;
            if (!textSelected)
                return { true, false, true };
            ApplyStyle([v](Annotation& a) { a.fontSize = v; });
            return kChanged;
        }
        if (e.id == "type")
        {
            if (e.index == kSmaller || e.index == kBigger)
                return ChangeFontSize(e.index == kBigger ? 1 : -1);
            const Annotation current = textSelected ? *sel : Style(Mark::Text);
            switch (e.index)
            {
            case kBold: m_weight = current.weight >= 600 ? 400 : 700; break;
            case kItalic: m_italic = !current.italic; break;
            case kUnderline: m_underline = !current.underline; break;
            case kLeft: m_align = Align::Left; break;
            case kCentre: m_align = Align::Centre; break;
            case kRight: m_align = Align::Right; break;
            default: return {};
            }
            if (!textSelected)
                return { true, false, true };
            ApplyStyle([&, index = e.index](Annotation& a) {
                switch (index)
                {
                case kBold: a.weight = m_weight; break;
                case kItalic: a.italic = m_italic; break;
                case kUnderline: a.underline = m_underline; break;
                default: a.align = m_align; break;
                }
            });
            return kChanged;
        }
        if (e.id == "wrap")
        {
            m_wrap = e.on;
            if (!textSelected)
                return { true, false, true };
            const RectD box = Marks().TextBox(*sel);
            ApplyStyle([on = e.on, box](Annotation& a) {
                a.wrap = on;
                // Wrapping starts at the width the text has now; without it the box fits the text.
                a.b = on ? PointD{ box.Right(), box.Bottom() } : a.a;
            });
            return kChanged;
        }
        if (e.id == "width")
        {
            const double v = std::max(1.0, std::round(e.value));
            const bool marker = sel ? sel->kind == Mark::Highlighter : m_mode == Mode::Highlighter;
            m_stroke = marker ? v / 5 : v;
            ApplyStyle([v](Annotation& a) { a.width = v; });
            return sel && e.final ? kChanged : Response{ true, false, true };
        }
        if (e.id == "font")
        {
            const double v = std::max(4.0, std::round(e.value));
            m_font = v;
            ApplyStyle([v](Annotation& a) {
                if (a.kind == Mark::Callout)
                {
                    // The bubble grows with its text, about its centre.
                    const PointD c = FromCorners(a.a, a.b).Centre();
                    const double k = v / std::max(1.0, a.fontSize);
                    a.a = { c.x + (a.a.x - c.x) * k, c.y + (a.a.y - c.y) * k };
                    a.b = { c.x + (a.b.x - c.x) * k, c.y + (a.b.y - c.y) * k };
                }
                a.fontSize = v;
            });
            return sel && e.final ? kChanged : Response{ true, false, true };
        }
        if (e.id == "fill")
        {
            m_fill = e.on;
            ApplyStyle([on = e.on](Annotation& a) { a.fill = on; });
            return sel ? kChanged : Response{ true, false, true };
        }
        if (e.id == "zoom")
        {
            const double v = std::clamp(std::round(e.value * 10) / 10, 1.2, 8.0);
            m_zoom = v;
            ApplyStyle([v](Annotation& a) { a.zoom = v; });
            return sel && e.final ? kChanged : Response{ true, false, true };
        }
        if (!sel)
            return {};
        if (e.id == "editText")
        {
            if (m_editing == sel->id)
            {
                StopEditing();
                return kChanged;
            }
            StartEditing(sel->id, sel->text.size());
            return { true, false, true };
        }
        if (e.id == "front")
        {
            Marks().BringToFront(sel->id);
            Touch();
            return kChanged;
        }
        if (e.id == "delete")
        {
            StopEditing();
            Marks().Remove(m_selected);
            m_layouts.erase(m_selected);
            m_selected = 0;
            Touch();
            return kChanged;
        }
        return {};
    }

    nlohmann::json AnnotateTool::State() const
    {
        auto files = nlohmann::json::array();
        for (const auto& set : m_marks)
            files.push_back(set.ToJson());
        return { { "files", std::move(files) } };
    }

    void AnnotateTool::Restore(const nlohmann::json& state)
    {
        const auto& files = state.contains("files") && state["files"].is_array() ? state["files"] : nlohmann::json::array();
        for (size_t i = 0; i < m_marks.size(); ++i)
            m_marks[i] = i < files.size() ? Annotations::FromJson(files[i]) : Annotations{};
        m_dragId = 0;
        m_selecting = false;
        // Marks keep their ids, so the selection and the text being typed carry on where they still exist.
        if (!Marks().Find(m_selected))
            m_selected = 0;
        if (const auto* e = Marks().Find(m_editing))
            m_cursor.Clamp(e->text);
        else
        {
            m_editing = 0;
            m_cursor = {};
        }
        RelayoutAll();
        Touch();
    }

    bool AnnotateTool::CanApply(std::wstring* why) const
    {
        for (size_t i = 0; i < m_marks.size(); ++i)
            if (!m_marks[i].Items().empty() || m_seeded[i])
                return true;
        if (why)
            *why = m_options.sign ? L"Add a text, a signature or a tick first" : L"Add an arrow, a text or a shape first";
        return false;
    }

    nlohmann::json AnnotateTool::Params() const
    {
        // {"perFile": {path: {"items": [...]}}}: each picture its own marks (engine: ParamsForFile).
        auto perFile = nlohmann::json::object();
        for (size_t i = 0; i < m_marks.size() && i < m_files.size(); ++i)
            perFile[Narrow(m_files[i])] = m_marks[i].ToJson();
        return { { "perFile", std::move(perFile) } };
    }
}
