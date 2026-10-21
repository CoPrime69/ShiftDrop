#include "photo/PhotoTools.h"
#include "Utf.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <format>

namespace sd::photo
{
    using namespace sd::canvas;

    std::unique_ptr<ICanvasTool> MakeAnnotateTool(const std::vector<std::wstring>& files) { return std::make_unique<AnnotateTool>(files); }

    namespace
    {
        using Mode = AnnotateTool::Mode;

        struct ModeInfo
        {
            Mode mode;
            const wchar_t* label;
            unsigned key;
        };
        const ModeInfo kModes[] = {
            { Mode::Select, L"Select", 'V' },   { Mode::Arrow, L"Arrow", 'A' },   { Mode::Text, L"Text", 'T' },
            { Mode::Rect, L"Box", 'B' },        { Mode::Ellipse, L"Circle", 'O' }, { Mode::Callout, L"Callout", 'C' },
            { Mode::Number, L"Number", 'N' },   { Mode::Magnifier, L"Magnifier", 'M' }, { Mode::Pen, L"Pen", 'P' },
            { Mode::Highlighter, L"Highlighter", 'H' },
        };

        const uint32_t kPalette[] = { 0xE81123, 0xFFB900, 0x10893E, 0x0078D4, 0x111111, 0xFFFFFF };   // six and a custom chip fill the panel

        Mark MarkFor(Mode m)
        {
            switch (m)
            {
            case Mode::Text: return Mark::Text;
            case Mode::Rect: return Mark::Rect;
            case Mode::Ellipse: return Mark::Ellipse;
            case Mode::Callout: return Mark::Callout;
            case Mode::Number: return Mark::Number;
            case Mode::Magnifier: return Mark::Magnifier;
            case Mode::Pen: return Mark::Pen;
            case Mode::Highlighter: return Mark::Highlighter;
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
    }

    AnnotateTool::AnnotateTool(std::vector<std::wstring> files) : PhotoTool(std::move(files)) { m_marks.resize(std::max<size_t>(1, m_files.size())); }

    void AnnotateTool::ImageReady(size_t index, int width, int height)
    {
        StopEditing();   // on the picture being left
        PhotoTool::ImageReady(index, width, height);
    }

    void AnnotateTool::PictureReady(size_t)
    {
        m_selected = m_editing = m_dragId = 0;
    }

    void AnnotateTool::SetMode(Mode mode)
    {
        StopEditing();
        m_mode = mode;
        if (mode != Mode::Select)
            m_selected = 0;
    }

    double AnnotateTool::Tolerance(const Pointer& p) const { return std::max(0.5, p.tolerance); }

    Annotation AnnotateTool::Style(Mark kind) const
    {
        Annotation a;
        a.kind = kind;
        a.color = m_color;
        const double stroke = m_stroke > 0 ? m_stroke : DefaultStroke(m_width, m_height);
        a.width = kind == Mark::Highlighter ? stroke * 5 : stroke;
        a.fontSize = m_font > 0 ? m_font : DefaultFont(m_width, m_height);
        a.fill = m_fill && (kind == Mark::Rect || kind == Mark::Ellipse || kind == Mark::Text);
        a.zoom = m_zoom;
        a.radius = std::max(12.0, std::round(std::min(m_width, m_height) * 0.06));
        return a;
    }

    void AnnotateTool::StopEditing()
    {
        if (!m_editing)
            return;
        const uint32_t id = std::exchange(m_editing, 0);
        auto& marks = Marks();
        if (const auto* m = marks.Find(id); m && m->kind == Mark::Text && m->text.empty())
        {
            marks.Remove(id);
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

    bool AnnotateTool::Render(ID2D1DeviceContext* dc, double scale, unsigned device)
    {
        UseDevice(device);
        if (m_photo.Empty())
            return false;
        return SUCCEEDED(DrawAnnotated(dc, device, scale, m_photo, Marks(), m_images, m_bitmaps, m_editing));
    }

    void AnnotateTool::Draw(IPainter& p) const
    {
        const auto& marks = m_marks[m_index];
        const auto* m = marks.Find(m_selected);
        if (!m)
            return;
        const auto& theme = p.Theme();
        const RectD box = marks.Bounds(*m);
        const double pad = 4 / p.Zoom();
        p.StrokeRect({ box.x - pad, box.y - pad, box.w + 2 * pad, box.h + 2 * pad }, theme.accent, 1.5f, true);
        if (m_editing == m->id)
            return;
        for (const auto& g : marks.Grips(*m))
            p.Grip(g.at, m_dragId == m->id && m_dragPart == g.part);
    }

    CursorShape AnnotateTool::CursorAt(const Pointer& p) const
    {
        const auto& marks = m_marks[m_index];
        const double tol = Tolerance(p);
        if (const auto* m = marks.Find(m_selected))
            switch (marks.GripAt(*m, p.at, tol))
            {
            case Part::None: break;
            case Part::Body:
                if (!Freehand(m_mode))
                    return m_editing == m->id ? CursorShape::Arrow : CursorShape::Move;
                break;
            default: return CursorShape::Cross;
            }
        if (!Freehand(m_mode) && marks.HitTest(p.at, tol))
            return CursorShape::Move;
        return m_mode == Mode::Select ? CursorShape::Arrow : CursorShape::Cross;
    }

    Response AnnotateTool::PointerDown(const Pointer& p)
    {
        StopEditing();
        auto& marks = Marks();
        const double tol = Tolerance(p);
        m_from = p.at;
        m_moved = m_created = m_wasSelected = false;
        m_dragId = 0;
        m_dragPart = Part::None;

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
                return { true, false, true };
            }
        }
        if (!Freehand(m_mode))
            if (const uint32_t id = marks.HitTest(p.at, tol))
            {
                m_selected = m_dragId = id;
                m_dragPart = Part::Body;
                m_before = *marks.Find(id);
                return { true, false, true };
            }
        if (m_mode == Mode::Select)
        {
            const bool had = m_selected != 0;
            m_selected = 0;
            return had ? Response{ true, false, true } : Response{};
        }

        // A new mark.
        Annotation a = Style(MarkFor(m_mode));
        a.a = a.b = a.tip = p.at;
        m_dragPart = Part::B;
        switch (a.kind)
        {
        case Mark::Text:
            a.a = { p.at.x, p.at.y - a.fontSize * 0.66 };
            m_dragPart = Part::Body;
            break;
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
            m_editing = m_dragId;
        Touch();
        return { true, false, true };
    }

    Response AnnotateTool::PointerMove(const Pointer& p)
    {
        auto& marks = Marks();
        auto* m = marks.Find(m_dragId);
        if (!m)
            return {};
        if (!m_moved && Dist(p.at, m_from) <= Tolerance(p) / 2)
            return {};
        m_moved = true;
        if (m_created && (m->kind == Mark::Pen || m->kind == Mark::Highlighter))
        {
            if (m->points.empty() || Dist(m->points.back(), p.at) >= Tolerance(p) / 3)
                m->points.push_back(p.at);
        }
        else if (m_created && m->kind == Mark::Callout)
            PlaceCallout(*m, p.at);   // pressed where it points, dragged to where the bubble goes
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
            marks.Drag(*m, m_before, m_dragPart, m_from, to);
        }
        Touch();
        return kRedraw;
    }

    Response AnnotateTool::PointerUp(const Pointer& p)
    {
        auto& marks = Marks();
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
            case Mark::Callout:
                if (!m_moved)
                    PlaceCallout(*m, { m->tip.x + m->fontSize * 4.5, m->tip.y - m->fontSize * 3 });
                m_editing = id;   // straight to typing its text
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
            if (m->kind == Mark::Text)
                return { true, false, true };   // the undo step comes when the typing ends
            return kChanged;
        }
        if (m_moved)
            return kChanged;
        // A click on a text that was already selected (or in the Text tool) edits it.
        if (HasText(m->kind) && (m_wasSelected || m_mode == Mode::Text))
            m_editing = id;
        return { true, false, true };
    }

    Response AnnotateTool::PointerCancel()
    {
        auto& marks = Marks();
        const uint32_t id = std::exchange(m_dragId, 0);
        if (auto* m = marks.Find(id))
        {
            if (m_created)
            {
                if (m_editing == id)
                    m_editing = 0;
                marks.Remove(id);
                m_selected = 0;
            }
            else
                *m = m_before;
        }
        Touch();
        return { true, false, true };
    }

    Response AnnotateTool::Char(wchar_t ch)
    {
        auto* m = Marks().Find(m_editing);
        if (!m)
            return {};
        if (m->text.size() < 2000)
            m->text.push_back(ch);
        Touch();
        return kRedraw;
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
            if (vk == VK_RETURN && shift)
            {
                m->text.push_back(L'\n');
                Touch();
                return kRedraw;
            }
            if (vk == VK_RETURN || vk == VK_ESCAPE)
            {
                StopEditing();
                return kChanged;
            }
            if (vk == VK_BACK)
            {
                if (!m->text.empty())
                    m->text.pop_back();
                Touch();
                return kRedraw;
            }
            return {};
        }
        if (!ctrl)
            for (const auto& info : kModes)
                if (vk == info.key)
                {
                    SetMode(info.mode);
                    return { true, false, true };
                }
        auto* m = marks.Find(m_selected);
        if (!m)
            return {};
        if (vk == VK_DELETE || vk == VK_BACK)
        {
            marks.Remove(m_selected);
            m_selected = 0;
            Touch();
            return kChanged;
        }
        if (vk == VK_F2 && HasText(m->kind))
        {
            m_editing = m->id;
            Touch();
            return kRedraw;
        }
        if (vk == VK_LEFT || vk == VK_RIGHT || vk == VK_UP || vk == VK_DOWN)
        {
            const double step = shift ? 10 : 1;
            const PointD d{ vk == VK_LEFT ? -step : vk == VK_RIGHT ? step : 0, vk == VK_UP ? -step : vk == VK_DOWN ? step : 0 };
            const Annotation before = *m;
            marks.Drag(*m, before, Part::Body, {}, d);
            Touch();
            return kChanged;
        }
        return {};
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

        section.label = sel ? std::format(L"{} (selected)", MarkName(sel->kind)) : std::wstring(L"Style");
        out.push_back(section);
        Control colors;
        colors.id = "color";
        colors.kind = ControlKind::Swatches;
        colors.custom = true;
        const uint32_t color = sel ? sel->color : m_color;
        for (size_t i = 0; i < std::size(kPalette); ++i)
        {
            colors.swatches.push_back(Color::Rgb(kPalette[i]));
            if (kPalette[i] == color)
                colors.selected = static_cast<int>(i);
        }
        colors.swatches.push_back(m_custom ? Color::Rgb(m_custom) : Color{ 0, 0, 0, 0 });
        if (colors.selected < 0 && m_custom == color)
            colors.selected = static_cast<int>(std::size(kPalette));
        out.push_back(colors);

        const Mark kind = sel ? sel->kind : MarkFor(m_mode);
        const double longSide = std::max({ 1, m_width, m_height });
        const bool stroked = kind != Mark::Text && kind != Mark::Number && kind != Mark::Callout && kind != Mark::Image;
        if (stroked && !(sel && (kind == Mark::Rect || kind == Mark::Ellipse) && sel->fill))
        {
            Control width;
            width.id = "width";
            width.kind = ControlKind::Slider;
            width.label = kind == Mark::Highlighter ? L"Marker width" : L"Line width";
            width.text = L" px";
            const double stroke = m_stroke > 0 ? m_stroke : DefaultStroke(m_width, m_height);
            width.value = sel ? sel->width : kind == Mark::Highlighter ? stroke * 5 : stroke;
            width.min = 1;
            width.max = std::max(32.0, std::round(longSide / (kind == Mark::Highlighter ? 20 : 60)));
            width.value = std::min(width.value, width.max);
            out.push_back(width);
        }
        if (HasText(kind) || kind == Mark::Number)
        {
            Control font;
            font.id = "font";
            font.kind = ControlKind::Slider;
            font.label = kind == Mark::Number ? L"Size" : L"Text size";
            font.text = L" px";
            font.value = sel ? sel->fontSize : m_font > 0 ? m_font : DefaultFont(m_width, m_height);
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
            note.text = L"Type the text · Shift+Enter for a new line · Enter when done";
        else
        {
            switch (m_mode)
            {
            case Mode::Select: note.text = L"Click a mark to change it; drag its grips to reshape it."; break;
            case Mode::Arrow: note.text = L"Drag from the tail to the point. Shift snaps to 45°."; break;
            case Mode::Text: note.text = L"Click where the text goes, then type."; break;
            case Mode::Rect:
            case Mode::Ellipse: note.text = L"Drag across what to mark. Shift keeps it even."; break;
            case Mode::Callout: note.text = L"Press on what it's about, drag to where the bubble goes, then click it to type."; break;
            case Mode::Number: note.text = L"Click to place 1, 2, 3… Numbers follow the order they were placed in."; break;
            case Mode::Magnifier: note.text = L"Press on the detail, drag to where the loupe goes."; break;
            case Mode::Pen:
            case Mode::Highlighter: note.text = L"Draw on the picture."; break;
            }
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
            files.text = std::format(L"Each picture has its own marks ({} of {} marked); pictures without any are skipped.", marked, m_files.size());
            out.push_back(files);
        }
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
        auto* sel = Marks().Find(m_selected);
        if (e.id == "color")
        {
            uint32_t rgb = 0;
            if (e.index >= 0 && e.index < static_cast<int>(std::size(kPalette)))
                rgb = kPalette[e.index];
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
            m_editing = sel->id;
            Touch();
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
        m_selected = m_editing = m_dragId = 0;
        Touch();
    }

    bool AnnotateTool::CanApply(std::wstring* why) const
    {
        for (const auto& set : m_marks)
            if (!set.Items().empty())
                return true;
        if (why)
            *why = L"Add an arrow, a text or a shape first";
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
