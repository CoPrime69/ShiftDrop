#include "photo/Annotations.h"
#include "Recipe.h"
#include "Utf.h"
#include "canvas/Regions.h"

#include <algorithm>
#include <cmath>
#include <initializer_list>

namespace sd::photo
{
    namespace
    {
        double Dist(PointD a, PointD b) { return std::hypot(a.x - b.x, a.y - b.y); }

        RectD Inflate(const RectD& r, double d) { return { r.x - d, r.y - d, r.w + 2 * d, r.h + 2 * d }; }

        PointD Moved(PointD p, double dx, double dy) { return { p.x + dx, p.y + dy }; }

        const char* KindId(Mark k)
        {
            switch (k)
            {
            case Mark::Arrow: return "arrow";
            case Mark::Text: return "text";
            case Mark::Rect: return "rect";
            case Mark::Ellipse: return "ellipse";
            case Mark::Callout: return "callout";
            case Mark::Number: return "number";
            case Mark::Magnifier: return "magnifier";
            case Mark::Image: return "image";
            case Mark::Pen: return "pen";
            case Mark::Highlighter: return "highlighter";
            case Mark::Check: return "check";
            case Mark::Cross: return "cross";
            }
            return "arrow";
        }

        bool KindOf(const std::string& id, Mark& out)
        {
            for (Mark k : { Mark::Arrow, Mark::Text, Mark::Rect, Mark::Ellipse, Mark::Callout, Mark::Number, Mark::Magnifier, Mark::Image, Mark::Pen,
                            Mark::Highlighter, Mark::Check, Mark::Cross })
                if (id == KindId(k))
                {
                    out = k;
                    return true;
                }
            return false;
        }

        nlohmann::json Pt(PointD p) { return nlohmann::json::array({ p.x, p.y }); }

        PointD PtOf(const nlohmann::json& j, PointD fallback = {})
        {
            if (j.is_array() && j.size() == 2 && j[0].is_number() && j[1].is_number())
            {
                const double x = j[0].get<double>(), y = j[1].get<double>();
                if (std::isfinite(x) && std::isfinite(y))
                    return { x, y };
            }
            return fallback;
        }
    }

    const wchar_t* MarkName(Mark kind)
    {
        switch (kind)
        {
        case Mark::Arrow: return L"Arrow";
        case Mark::Text: return L"Text";
        case Mark::Rect: return L"Box";
        case Mark::Ellipse: return L"Circle";
        case Mark::Callout: return L"Callout";
        case Mark::Number: return L"Number";
        case Mark::Magnifier: return L"Magnifier";
        case Mark::Image: return L"Image";
        case Mark::Pen: return L"Pen";
        case Mark::Highlighter: return L"Highlighter";
        case Mark::Check: return L"Tick";
        case Mark::Cross: return L"Cross";
        }
        return L"";
    }

    const wchar_t* FontOf(const Annotation& a) { return a.font.empty() ? L"Segoe UI" : a.font.c_str(); }

    double MinTextWidth(const Annotation& a) { return std::max(8.0, a.fontSize * 1.2); }

    double DistanceToSegment(PointD p, PointD a, PointD b)
    {
        const double dx = b.x - a.x, dy = b.y - a.y;
        const double len2 = dx * dx + dy * dy;
        if (len2 <= 1e-12)
            return Dist(p, a);
        const double t = std::clamp(((p.x - a.x) * dx + (p.y - a.y) * dy) / len2, 0.0, 1.0);
        return Dist(p, { a.x + t * dx, a.y + t * dy });
    }

    double DefaultStroke(double w, double h) { return std::max(2.0, std::round(std::max(w, h) / 250)); }
    double DefaultFont(double w, double h) { return std::max(14.0, std::round(std::max(w, h) / 22)); }

    uint32_t ContrastingText(uint32_t rgb)
    {
        const double l = (0.2126 * ((rgb >> 16) & 0xFF) + 0.7152 * ((rgb >> 8) & 0xFF) + 0.0722 * (rgb & 0xFF)) / 255;
        return l > 0.6 ? 0x111111 : 0xFFFFFF;
    }

    Annotation* Annotations::Find(uint32_t id)
    {
        for (auto& a : m_items)
            if (a.id == id)
                return &a;
        return nullptr;
    }

    const Annotation* Annotations::Find(uint32_t id) const { return const_cast<Annotations*>(this)->Find(id); }

    uint32_t Annotations::Add(Annotation a)
    {
        a.id = m_next++;
        m_items.push_back(std::move(a));
        return m_items.back().id;
    }

    bool Annotations::Remove(uint32_t id)
    {
        const auto it = std::find_if(m_items.begin(), m_items.end(), [id](const Annotation& a) { return a.id == id; });
        if (it == m_items.end())
            return false;
        m_items.erase(it);
        m_measured.erase(id);
        return true;
    }

    void Annotations::BringToFront(uint32_t id)
    {
        const auto it = std::find_if(m_items.begin(), m_items.end(), [id](const Annotation& a) { return a.id == id; });
        if (it != m_items.end())
            std::rotate(it, it + 1, m_items.end());
    }

    int Annotations::NumberOf(uint32_t id) const
    {
        int n = 0;
        for (const auto& a : m_items)
            if (a.kind == Mark::Number)
            {
                ++n;
                if (a.id == id)
                    return n;
            }
        return 0;
    }

    SizeD Annotations::TextSize(const Annotation& a) const
    {
        if (const auto it = m_measured.find(a.id); it != m_measured.end() && it->second.w > 0)
            return it->second;
        size_t lines = 1, longest = 0, run = 0;
        for (wchar_t c : a.text)
        {
            if (c == L'\n')
            {
                ++lines;
                run = 0;
                continue;
            }
            longest = std::max(longest, ++run);
        }
        SizeD size{ a.fontSize * 0.56 * std::max<size_t>(1, longest), a.fontSize * 1.33 * lines };
        if (a.wrap)
        {
            // Lines broken at the box's width.
            const double width = std::max(MinTextWidth(a), a.b.x - a.a.x);
            size.h *= std::max(1.0, std::ceil(size.w / width));
            size.w = width;
        }
        return size;
    }

    RectD Annotations::TextBox(const Annotation& a) const
    {
        const SizeD s = TextSize(a);
        if (!a.wrap)
            return { a.a.x, a.a.y, s.w, s.h };
        return { a.a.x, a.a.y, std::max(MinTextWidth(a), a.b.x - a.a.x), std::max(s.h, a.b.y - a.a.y) };
    }

    RectD Annotations::Bounds(const Annotation& a) const
    {
        switch (a.kind)
        {
        case Mark::Arrow: return Inflate(canvas::FromCorners(a.a, a.b), a.width * 1.5);
        case Mark::Rect:
        case Mark::Ellipse:
        case Mark::Callout:
        case Mark::Image:
        case Mark::Check:
        case Mark::Cross: return canvas::FromCorners(a.a, a.b);
        case Mark::Text:
        {
            const RectD r = TextBox(a);
            return a.fill ? Inflate(r, a.fontSize * 0.3) : r;
        }
        case Mark::Number:
        {
            const double r = a.fontSize * 0.75;
            return { a.a.x - r, a.a.y - r, 2 * r, 2 * r };
        }
        case Mark::Magnifier:
        {
            const double r = a.radius * a.zoom;
            return { a.b.x - r, a.b.y - r, 2 * r, 2 * r };
        }
        case Mark::Pen:
        case Mark::Highlighter:
        {
            if (a.points.empty())
                return {};
            double l = a.points[0].x, t = a.points[0].y, r = l, b = t;
            for (const auto& p : a.points)
            {
                l = std::min(l, p.x), t = std::min(t, p.y), r = std::max(r, p.x), b = std::max(b, p.y);
            }
            return Inflate({ l, t, r - l, b - t }, a.width / 2);
        }
        }
        return {};
    }

    std::vector<Grip> Annotations::Grips(const Annotation& a) const
    {
        switch (a.kind)
        {
        case Mark::Arrow: return { { Part::A, a.a }, { Part::B, a.b } };
        case Mark::Rect:
        case Mark::Ellipse:
        case Mark::Image:
        case Mark::Callout:
        case Mark::Check:
        case Mark::Cross:
        {
            std::vector<Grip> g = { { Part::A, a.a }, { Part::B, a.b }, { Part::AB, { a.a.x, a.b.y } }, { Part::BA, { a.b.x, a.a.y } } };
            if (a.kind == Mark::Callout)
                g.push_back({ Part::Tip, a.tip });
            if (a.kind == Mark::Image)
            {
                // Sides too, for stretching a picture (Shift) or trimming a signature's box.
                const RectD r = canvas::FromCorners(a.a, a.b);
                const PointD c = r.Centre();
                g.insert(g.end(), { { Part::Left, { r.x, c.y } }, { Part::Right, { r.Right(), c.y } }, { Part::Top, { c.x, r.y } },
                                    { Part::Bottom, { c.x, r.Bottom() } } });
            }
            return g;
        }
        case Mark::Text:
        {
            // The corners, and the sides that set the width; a wrapping box also has a top and a bottom.
            const RectD r = Bounds(a);
            const PointD c = r.Centre();
            std::vector<Grip> g = { { Part::A, { r.x, r.y } },           { Part::BA, { r.Right(), r.y } }, { Part::B, { r.Right(), r.Bottom() } },
                                    { Part::AB, { r.x, r.Bottom() } },   { Part::Left, { r.x, c.y } },     { Part::Right, { r.Right(), c.y } } };
            if (a.wrap)
                g.insert(g.end(), { { Part::Top, { c.x, r.y } }, { Part::Bottom, { c.x, r.Bottom() } } });
            return g;
        }
        case Mark::Number: return { { Part::Size, { a.a.x + a.fontSize * 0.75, a.a.y } } };
        case Mark::Magnifier: return { { Part::Tip, { a.a.x + a.radius, a.a.y } }, { Part::Size, { a.b.x + a.radius * a.zoom, a.b.y } } };
        case Mark::Pen:
        case Mark::Highlighter: return {};
        }
        return {};
    }

    namespace
    {
        bool InEllipse(PointD p, const RectD& r)
        {
            if (r.w <= 0 || r.h <= 0)
                return false;
            const double nx = (p.x - r.Centre().x) / (r.w / 2), ny = (p.y - r.Centre().y) / (r.h / 2);
            return nx * nx + ny * ny <= 1;
        }

        bool Hits(const Annotations& set, const Annotation& a, PointD p, double tol)
        {
            switch (a.kind)
            {
            case Mark::Arrow: return DistanceToSegment(p, a.a, a.b) <= a.width / 2 + tol;
            case Mark::Pen:
            case Mark::Highlighter:
                for (size_t i = 0; i + 1 < a.points.size(); ++i)
                    if (DistanceToSegment(p, a.points[i], a.points[i + 1]) <= a.width / 2 + tol)
                        return true;
                return a.points.size() == 1 && Dist(p, a.points[0]) <= a.width / 2 + tol;
            case Mark::Rect:
            {
                const RectD r = canvas::FromCorners(a.a, a.b);
                if (a.fill)
                    return Inflate(r, tol).Contains(p);
                // Only the outline: the picture inside stays clickable for the marks under it.
                const double band = a.width / 2 + tol;
                return Inflate(r, band).Contains(p) && !(r.w > 2 * band && r.h > 2 * band && Inflate(r, -band).Contains(p));
            }
            case Mark::Ellipse:
            {
                const RectD r = canvas::FromCorners(a.a, a.b);
                const double band = a.width / 2 + tol;
                if (a.fill)
                    return InEllipse(p, Inflate(r, tol));
                return InEllipse(p, Inflate(r, band)) && !(r.w > 2 * band && r.h > 2 * band && InEllipse(p, Inflate(r, -band)));
            }
            case Mark::Callout:
                return Inflate(set.Bounds(a), tol).Contains(p) || DistanceToSegment(p, canvas::FromCorners(a.a, a.b).Centre(), a.tip) <= tol + a.width;
            case Mark::Text:
            case Mark::Image:
            case Mark::Check:
            case Mark::Cross: return Inflate(set.Bounds(a), tol).Contains(p);
            case Mark::Number: return Dist(p, a.a) <= a.fontSize * 0.75 + tol;
            case Mark::Magnifier: return Dist(p, a.a) <= a.radius + tol || Dist(p, a.b) <= a.radius * a.zoom + tol;
            }
            return false;
        }
    }

    uint32_t Annotations::HitTest(PointD at, double tolerance) const
    {
        for (auto it = m_items.rbegin(); it != m_items.rend(); ++it)
            if (Hits(*this, *it, at, tolerance))
                return it->id;
        return 0;
    }

    Part Annotations::GripAt(const Annotation& a, PointD at, double tolerance) const
    {
        for (const auto& g : Grips(a))
            if (Dist(g.at, at) <= tolerance * 1.5)
                return g.part;
        if (a.kind == Mark::Magnifier)
        {
            // The loupe is on top of the spot when they overlap.
            if (Dist(at, a.b) <= a.radius * a.zoom + tolerance)
                return Part::B;
            if (Dist(at, a.a) <= a.radius + tolerance)
                return Part::A;
            return Part::None;
        }
        return Hits(*this, a, at, tolerance) ? Part::Body : Part::None;
    }

    namespace
    {
        bool BoxLike(Mark k) { return k == Mark::Image || k == Mark::Check || k == Mark::Cross; }

        // The resize grip of rectangle `r` that a part held at `at` stands for.
        canvas::Grip GripFor(Part part, PointD at, const RectD& r)
        {
            switch (part)
            {
            case Part::Left: return canvas::Grip::W;
            case Part::Right: return canvas::Grip::E;
            case Part::Top: return canvas::Grip::N;
            case Part::Bottom: return canvas::Grip::S;
            default: break;
            }
            const PointD c = r.Centre();
            const bool left = at.x < c.x, top = at.y < c.y;
            return top ? (left ? canvas::Grip::NW : canvas::Grip::NE) : (left ? canvas::Grip::SW : canvas::Grip::SE);
        }

        PointD GripPoint(const RectD& r, canvas::Grip g)
        {
            const PointD c = r.Centre();
            switch (g)
            {
            case canvas::Grip::N: return { c.x, r.y };
            case canvas::Grip::NE: return { r.Right(), r.y };
            case canvas::Grip::E: return { r.Right(), c.y };
            case canvas::Grip::SE: return { r.Right(), r.Bottom() };
            case canvas::Grip::S: return { c.x, r.Bottom() };
            case canvas::Grip::SW: return { r.x, r.Bottom() };
            case canvas::Grip::W: return { r.x, c.y };
            case canvas::Grip::NW: return { r.x, r.y };
            default: return c;
            }
        }

        // `r` resized by dragging grip `g` by (dx, dy), the way the region tools resize (the opposite
        // side or corner stays; `aspect` > 0 keeps the shape). Marks may sit past the picture's edge.
        RectD ResizeBox(const RectD& r, canvas::Grip g, double dx, double dy, double aspect, double minSize)
        {
            canvas::RegionSet unbounded;
            unbounded.SetBounds({ -1e7, -1e7, 2e7, 2e7 });
            unbounded.SetMinSize(minSize);
            const PointD at = GripPoint(r, g);
            return unbounded.Resized(r, g, { at.x + dx, at.y + dy }, aspect, false);
        }

        RectD Deflate(const RectD& r, double d) { return { r.x + d, r.y + d, std::max(0.0, r.w - 2 * d), std::max(0.0, r.h - 2 * d) }; }

        bool IsCorner(Part p) { return p == Part::A || p == Part::B || p == Part::AB || p == Part::BA; }
        bool IsSide(Part p) { return p == Part::Left || p == Part::Right || p == Part::Top || p == Part::Bottom; }
    }

    void Annotations::Drag(Annotation& a, const Annotation& before, Part part, PointD from, PointD to, bool free) const
    {
        const double dx = to.x - from.x, dy = to.y - from.y;
        if (a.kind == Mark::Text && (IsCorner(part) || IsSide(part)))
        {
            const RectD outer = Bounds(before);
            const double pad = before.fill ? before.fontSize * 0.3 : 0;
            const PointD held = part == Part::A ? PointD{ outer.x, outer.y } : part == Part::B ? PointD{ outer.Right(), outer.Bottom() }
                              : part == Part::AB ? PointD{ outer.x, outer.Bottom() } : PointD{ outer.Right(), outer.y };
            const canvas::Grip grip = GripFor(part, held, outer);
            if (before.wrap || part == Part::Left || part == Part::Right)
            {
                // The box itself: its width (Auto-wrap from now on) and, when it wraps, its height.
                if (!before.wrap && (part == Part::Top || part == Part::Bottom))
                    return;
                const RectD box = Deflate(ResizeBox(outer, grip, dx, dy, 0, MinTextWidth(before) + 2 * pad), pad);
                a.wrap = true;
                a.a = { box.x, box.y };
                a.b = { box.Right(), before.wrap ? box.Bottom() : box.y + TextBox(before).h };
                return;
            }
            // The type: a corner scales the text, the opposite corner staying put.
            const RectD scaled = ResizeBox(outer, grip, dx, dy, outer.w / std::max(1e-6, outer.h), 4);
            const double k = std::clamp(scaled.w / std::max(1e-6, outer.w), 0.05, 50.0);
            a.fontSize = std::clamp(before.fontSize * k, 4.0, 4000.0);
            const double newPad = before.fill ? a.fontSize * 0.3 : 0;
            a.a = { scaled.x + newPad, scaled.y + newPad };
            a.b = a.a;
            return;
        }
        if (BoxLike(a.kind) && (IsCorner(part) || IsSide(part)))
        {
            const RectD outer = canvas::FromCorners(before.a, before.b);
            const PointD held = part == Part::A ? before.a : part == Part::B ? before.b : part == Part::AB ? PointD{ before.a.x, before.b.y }
                                                                                                             : PointD{ before.b.x, before.a.y };
            const double aspect = free || outer.h <= 0 ? 0 : outer.w / outer.h;
            const RectD box = ResizeBox(outer, GripFor(part, held, outer), dx, dy, aspect, 4);
            a.a = { box.x, box.y };
            a.b = { box.Right(), box.Bottom() };
            return;
        }
        switch (part)
        {
        case Part::Body:
            a.a = Moved(before.a, dx, dy);
            a.b = Moved(before.b, dx, dy);
            a.tip = Moved(before.tip, dx, dy);
            for (size_t i = 0; i < a.points.size() && i < before.points.size(); ++i)
                a.points[i] = Moved(before.points[i], dx, dy);
            break;
        case Part::A: a.a = Moved(before.a, dx, dy); break;
        case Part::B: a.b = Moved(before.b, dx, dy); break;
        case Part::AB:
            a.a.x = before.a.x + dx;
            a.b.y = before.b.y + dy;
            break;
        case Part::BA:
            a.b.x = before.b.x + dx;
            a.a.y = before.a.y + dy;
            break;
        case Part::Tip:
            if (a.kind == Mark::Magnifier)
                a.radius = std::max(4.0, Dist(to, a.a));
            else
                a.tip = Moved(before.tip, dx, dy);
            break;
        case Part::Size:
            if (a.kind == Mark::Number)
                a.fontSize = std::clamp(Dist(to, a.a) / 0.75, 8.0, 2000.0);
            else if (a.kind == Mark::Magnifier)
                a.zoom = std::clamp(Dist(to, a.b) / std::max(1.0, a.radius), 1.2, 8.0);
            break;
        default: break;
        }
    }

    // ---- Shapes shared by the screen and the PDF writer ---------------------------------------------

    ArrowShape ArrowGeometry(const Annotation& m)
    {
        ArrowShape s;
        const double dx = m.b.x - m.a.x, dy = m.b.y - m.a.y, len = std::hypot(dx, dy);
        s.lineEnd = m.b;
        s.head[0] = s.head[1] = s.head[2] = m.b;
        if (len < 0.5)
            return s;
        const double ux = dx / len, uy = dy / len;
        const double head = std::min(len * 0.6, std::max(m.width * 3.6, 10.0)), half = head * 0.55;
        s.lineEnd = { m.b.x - ux * head * 0.85, m.b.y - uy * head * 0.85 };
        const PointD back{ m.b.x - ux * head, m.b.y - uy * head };
        s.head[1] = { back.x - uy * half, back.y + ux * half };
        s.head[2] = { back.x + uy * half, back.y - ux * half };
        s.headStroke = std::max(1.0, m.width * 0.25);
        return s;
    }

    void CalloutTail(const Annotation& m, PointD out[3])
    {
        const RectD box = canvas::FromCorners(m.a, m.b);
        const PointD c = box.Centre();
        const double dx = m.tip.x - c.x, dy = m.tip.y - c.y, len = std::max(1.0, std::hypot(dx, dy));
        const double half = std::min(box.w, box.h) * 0.16;
        out[0] = { c.x - dy / len * half, c.y + dx / len * half };
        out[1] = m.tip;
        out[2] = { c.x + dy / len * half, c.y - dx / len * half };
    }

    double CalloutRadius(const Annotation& m)
    {
        const RectD box = canvas::FromCorners(m.a, m.b);
        return std::min({ box.w, box.h, m.fontSize * 1.2 }) * 0.35;
    }

    std::vector<std::vector<PointD>> CheckStrokes(const Annotation& m, double& width)
    {
        const RectD r = canvas::FromCorners(m.a, m.b);
        const auto at = [&](double u, double v) { return PointD{ r.x + u * r.w, r.y + v * r.h }; };
        width = std::max(1.0, std::min(r.w, r.h) * 0.12);
        if (m.kind == Mark::Check)
            return { { at(0.14, 0.54), at(0.40, 0.80), at(0.86, 0.22) } };
        return { { at(0.2, 0.2), at(0.8, 0.8) }, { at(0.8, 0.2), at(0.2, 0.8) } };
    }

    std::vector<QuadSegment> SmoothStroke(const std::vector<PointD>& pts)
    {
        std::vector<QuadSegment> out;
        if (pts.empty())
            return out;
        if (pts.size() == 1)
        {
            const PointD dot{ pts[0].x + 0.01, pts[0].y };
            out.push_back({ dot, dot });
            return out;
        }
        for (size_t i = 1; i + 1 < pts.size(); ++i)
            out.push_back({ pts[i], { (pts[i].x + pts[i + 1].x) / 2, (pts[i].y + pts[i + 1].y) / 2 } });
        out.push_back({ pts.back(), pts.back() });   // a straight end
        return out;
    }

    nlohmann::json Annotations::ToJson() const
    {
        auto items = nlohmann::json::array();
        for (const auto& a : m_items)
        {
            nlohmann::json j = { { "id", a.id },         { "kind", KindId(a.kind) }, { "a", Pt(a.a) },       { "b", Pt(a.b) },
                                 { "color", a.color }, { "width", a.width },       { "fontSize", a.fontSize }, { "fill", a.fill } };
            if (a.kind == Mark::Callout)
                j["tip"] = Pt(a.tip);
            if (a.kind == Mark::Magnifier)
            {
                j["radius"] = a.radius;
                j["zoom"] = a.zoom;
            }
            if (a.kind == Mark::Text || a.kind == Mark::Callout)
                j["text"] = Narrow(a.text);
            if (a.kind == Mark::Text)
            {
                if (!a.font.empty())
                    j["font"] = Narrow(a.font);
                j["weight"] = a.weight;
                j["italic"] = a.italic;
                j["underline"] = a.underline;
                j["align"] = a.align == Align::Centre ? "centre" : a.align == Align::Right ? "right" : "left";
                j["wrap"] = a.wrap;
            }
            if (a.kind == Mark::Image)
            {
                j["image"] = Narrow(a.image);
                if (a.signature)
                    j["signature"] = true;
            }
            if (a.kind == Mark::Pen || a.kind == Mark::Highlighter)
            {
                auto pts = nlohmann::json::array();
                for (const auto& p : a.points)
                    pts.push_back(Pt(p));
                j["points"] = std::move(pts);
            }
            items.push_back(std::move(j));
        }
        return { { "items", std::move(items) } };
    }

    Annotations Annotations::FromJson(const nlohmann::json& j)
    {
        Annotations set;
        if (!j.is_object() || !j.contains("items") || !j["items"].is_array())
            return set;
        for (const auto& item : j["items"])
        {
            if (!item.is_object())
                continue;
            Annotation a;
            if (!KindOf(item.value("kind", std::string()), a.kind))
                continue;
            a.a = PtOf(item.value("a", nlohmann::json()));
            a.b = PtOf(item.value("b", nlohmann::json()), a.a);
            a.tip = PtOf(item.value("tip", nlohmann::json()), a.b);
            a.radius = JsonNumber(item, "radius", a.radius, 2, 1e5);
            a.zoom = JsonNumber(item, "zoom", a.zoom, 1, 16);
            if (const auto c = item.find("color"); c != item.end() && c->is_number_unsigned())
                a.color = c->get<uint32_t>() & 0xFFFFFF;
            a.width = JsonNumber(item, "width", a.width, 0.5, 2000);
            a.fontSize = JsonNumber(item, "fontSize", a.fontSize, 4, 4000);
            a.fill = item.value("fill", false);
            a.text = Widen(item.value("text", std::string()));
            a.image = Widen(item.value("image", std::string()));
            a.font = Widen(item.value("font", std::string()));
            a.weight = static_cast<int>(JsonNumber(item, "weight", a.weight, 100, 950));
            a.italic = item.value("italic", false);
            a.underline = item.value("underline", false);
            const std::string align = item.value("align", std::string());
            a.align = align == "centre" ? Align::Centre : align == "right" ? Align::Right : Align::Left;
            a.wrap = item.value("wrap", false);
            a.signature = item.value("signature", false);
            if (const auto p = item.find("points"); p != item.end() && p->is_array())
                for (const auto& pt : *p)
                    a.points.push_back(PtOf(pt));
            // The same ids as before, so undo keeps the selection and the text being typed.
            const auto id = item.find("id");
            if (id != item.end() && id->is_number_unsigned() && id->get<uint64_t>() > 0 && id->get<uint64_t>() < 0xFFFFFFFF &&
                !set.Find(id->get<uint32_t>()))
            {
                a.id = id->get<uint32_t>();
                set.m_next = std::max(set.m_next, a.id + 1);
                set.m_items.push_back(std::move(a));
            }
            else
                set.Add(std::move(a));
        }
        return set;
    }
}
