#include "photo/Annotations.h"
#include "Utf.h"

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
            }
            return "arrow";
        }

        bool KindOf(const std::string& id, Mark& out)
        {
            for (Mark k : { Mark::Arrow, Mark::Text, Mark::Rect, Mark::Ellipse, Mark::Callout, Mark::Number, Mark::Magnifier, Mark::Image, Mark::Pen,
                            Mark::Highlighter })
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

        double Num(const nlohmann::json& j, const char* key, double fallback, double lo, double hi)
        {
            const auto it = j.find(key);
            if (it == j.end() || !it->is_number())
                return fallback;
            const double v = it->get<double>();
            return std::isfinite(v) ? std::clamp(v, lo, hi) : fallback;
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
        }
        return L"";
    }

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
        return { a.fontSize * 0.56 * std::max<size_t>(1, longest), a.fontSize * 1.33 * lines };
    }

    RectD Annotations::Bounds(const Annotation& a) const
    {
        switch (a.kind)
        {
        case Mark::Arrow: return Inflate(canvas::FromCorners(a.a, a.b), a.width * 1.5);
        case Mark::Rect:
        case Mark::Ellipse:
        case Mark::Callout:
        case Mark::Image: return canvas::FromCorners(a.a, a.b);
        case Mark::Text:
        {
            const SizeD s = TextSize(a);
            const RectD r{ a.a.x, a.a.y, s.w, s.h };
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
        {
            std::vector<Grip> g = { { Part::A, a.a }, { Part::B, a.b }, { Part::AB, { a.a.x, a.b.y } }, { Part::BA, { a.b.x, a.a.y } } };
            if (a.kind == Mark::Callout)
                g.push_back({ Part::Tip, a.tip });
            return g;
        }
        case Mark::Text:
        {
            const RectD r = Bounds(a);
            return { { Part::Size, { r.Right(), r.Bottom() } } };
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
            case Mark::Image: return Inflate(set.Bounds(a), tol).Contains(p);
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

    void Annotations::Drag(Annotation& a, const Annotation& before, Part part, PointD from, PointD to) const
    {
        const double dx = to.x - from.x, dy = to.y - from.y;
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
            if (a.kind == Mark::Text)
            {
                const RectD r = Bounds(before);
                const double k = std::clamp((r.w + dx) / std::max(1.0, r.w), 0.1, 20.0);
                a.fontSize = std::clamp(before.fontSize * k, 6.0, 2000.0);
            }
            else if (a.kind == Mark::Number)
                a.fontSize = std::clamp(Dist(to, a.a) / 0.75, 8.0, 2000.0);
            else if (a.kind == Mark::Magnifier)
                a.zoom = std::clamp(Dist(to, a.b) / std::max(1.0, a.radius), 1.2, 8.0);
            break;
        case Part::None: break;
        }
    }

    nlohmann::json Annotations::ToJson() const
    {
        auto items = nlohmann::json::array();
        for (const auto& a : m_items)
        {
            nlohmann::json j = { { "kind", KindId(a.kind) }, { "a", Pt(a.a) }, { "b", Pt(a.b) }, { "color", a.color }, { "width", a.width },
                                 { "fontSize", a.fontSize }, { "fill", a.fill } };
            if (a.kind == Mark::Callout)
                j["tip"] = Pt(a.tip);
            if (a.kind == Mark::Magnifier)
            {
                j["radius"] = a.radius;
                j["zoom"] = a.zoom;
            }
            if (a.kind == Mark::Text || a.kind == Mark::Callout)
                j["text"] = Narrow(a.text);
            if (a.kind == Mark::Image)
                j["image"] = Narrow(a.image);
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
            a.radius = Num(item, "radius", a.radius, 2, 1e5);
            a.zoom = Num(item, "zoom", a.zoom, 1, 16);
            if (const auto c = item.find("color"); c != item.end() && c->is_number_unsigned())
                a.color = c->get<uint32_t>() & 0xFFFFFF;
            a.width = Num(item, "width", a.width, 0.5, 2000);
            a.fontSize = Num(item, "fontSize", a.fontSize, 4, 4000);
            a.fill = item.value("fill", false);
            a.text = Widen(item.value("text", std::string()));
            a.image = Widen(item.value("image", std::string()));
            if (const auto p = item.find("points"); p != item.end() && p->is_array())
                for (const auto& pt : *p)
                    a.points.push_back(PtOf(pt));
            set.Add(std::move(a));
        }
        return set;
    }
}
