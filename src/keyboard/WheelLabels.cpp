#include "keyboard/WheelLabels.h"

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <map>
#include <numbers>

namespace sd::wheel
{
    namespace
    {
        constexpr float kDeg = std::numbers::pi_v<float> / 180.f;
        constexpr float kMeasureSlackDip = 1.f;   // rounding, and the glyphs' overhang past their advance

        std::vector<std::wstring> Words(const std::wstring& text)
        {
            std::vector<std::wstring> words;
            size_t at = 0;
            while (at < text.size())
            {
                const size_t space = text.find(L' ', at);
                const size_t end = space == std::wstring::npos ? text.size() : space;
                if (end > at)
                    words.push_back(text.substr(at, end - at));
                at = end + 1;
            }
            return words;
        }

        std::wstring Join(const std::vector<std::wstring>& words, size_t from, size_t to)
        {
            std::wstring out;
            for (size_t i = from; i < to; ++i)
                out += (i > from ? L" " : L"") + words[i];
            return out;
        }

        // "10 MB", "1920 px": a number and its unit stay on one line.
        bool BreakAllowed(const std::vector<std::wstring>& words, size_t before)
        {
            static const wchar_t* const kUnits[] = { L"B", L"KB", L"MB", L"GB", L"TB", L"px", L"%", L"fps", L"x" };
            const auto& prev = words[before - 1];
            if (prev.empty() || !std::iswdigit(prev.back()))
                return true;
            return std::none_of(std::begin(kUnits), std::end(kUnits), [&](const wchar_t* u) { return words[before] == u; });
        }

        class Fitter
        {
        public:
            Fitter(const Layout& layout, const MeasureLabel& measure) : m_layout(layout), m_measure(measure) {}

            // `text` on one line, else on two: the first way that fits, or the most even two lines
            // (one line for a single word) with fits = false.
            FittedLabel Fit(int slot, const std::wstring& text, bool icon)
            {
                const auto words = Words(text);
                std::vector<std::vector<std::wstring>> ways{ { text } };
                std::vector<std::pair<std::wstring, std::wstring>> splits;
                for (size_t i = 1; i < words.size(); ++i)
                    if (BreakAllowed(words, i))
                        splits.emplace_back(Join(words, 0, i), Join(words, i, words.size()));
                // The most even split first: it needs the least width.
                std::stable_sort(splits.begin(), splits.end(), [&](const auto& a, const auto& b) {
                    return std::max(Width(a.first), Width(a.second)) < std::max(Width(b.first), Width(b.second));
                });
                for (const auto& [x, y] : splits)
                    ways.push_back({ x, y });
                for (const auto& lines : ways)
                    if (auto fitted = Try(slot, lines, icon))
                        return *fitted;
                const auto band = m_layout.BandOf(slot);
                auto best = Place(ways.size() > 1 ? ways[1] : ways[0], icon, (band.inner + band.outer) / 2.f);
                best.fits = false;
                return best;
            }

        private:
            float Width(const std::wstring& text)
            {
                if (auto it = m_widths.find(text); it != m_widths.end())
                    return it->second;
                return m_widths[text] = (m_measure ? m_measure(text) : 0.f) + kMeasureSlackDip;
            }

            FittedLabel Place(const std::vector<std::wstring>& lines, bool icon, float radius)
            {
                FittedLabel f;
                f.lines = lines;
                f.icon = icon;
                f.radius = radius;
                for (const auto& line : lines)
                    f.widths.push_back(Width(line));
                float height = 0.f;
                for (const auto& r : RowsOf(f))
                    height += r.h;
                float y = -height / 2.f;
                if (icon)
                {
                    f.iconDy = y + kIconDip / 2.f;
                    y += kIconDip + kIconGapDip;
                }
                for (size_t i = 0; i < lines.size(); ++i)
                {
                    f.lineDy.push_back(y + kLabelDip * kLineHeight / 2.f);
                    y += kLabelDip * kLineHeight;
                }
                return f;
            }

            std::optional<FittedLabel> Try(int slot, const std::vector<std::wstring>& lines, bool icon)
            {
                auto f = Place(lines, icon, 0.f);
                const auto rows = RowsOf(f);
                const auto band = m_layout.BandOf(slot);
                const float mid = (band.inner + band.outer) / 2.f, reach = (band.outer - band.inner) / 2.f;
                // The middle of the tile first, then a DIP at a time out and in: toward the rim a tile is
                // wider, toward the centre its corners are further from the text.
                for (float off = 0.f; off <= reach; off += 1.f)
                    for (const float r : { mid + off, mid - off })
                    {
                        if (RowsFit(m_layout, slot, rows, r))
                        {
                            f.radius = r;
                            return f;
                        }
                        if (off == 0.f)
                            break;
                    }
                return std::nullopt;
            }

            const Layout& m_layout;
            const MeasureLabel& m_measure;
            std::map<std::wstring, float> m_widths;
        };

        // The label's box, DIPs from the wheel's centre, at `radius` on `slot`.
        Rect BoxOf(const Layout& layout, int slot, const FittedLabel& label, float radius)
        {
            const float mid = layout.MidDeg(slot) * kDeg;
            const float px = radius * std::sin(mid), py = -radius * std::cos(mid);
            float w = label.icon ? kIconDip : 0.f, h = 0.f;
            for (const auto& r : RowsOf(label))
            {
                w = std::max(w, r.w);
                h += r.h;
            }
            return Rect{ px - w / 2.f, py - h / 2.f, px + w / 2.f, py + h / 2.f };
        }
    }

    std::wstring FittedLabel::Text() const
    {
        return Join(lines, 0, lines.size());
    }

    std::vector<Row> RowsOf(const FittedLabel& f)
    {
        std::vector<Row> rows;
        if (f.icon)
        {
            rows.push_back(Row{ kIconDip, kIconDip });
            rows.push_back(Row{ 0.f, kIconGapDip });
        }
        for (const float w : f.widths)
            rows.push_back(Row{ w, kLabelDip * kLineHeight });
        return rows;
    }

    bool RowsFit(const Layout& layout, int slot, const std::vector<Row>& rows, float radius, float pad)
    {
        if (slot < 0 || slot >= static_cast<int>(layout.slots.size()))
            return false;
        const auto& s = layout.slots[static_cast<size_t>(slot)];
        const auto band = layout.BandOf(slot);
        const float mid = layout.MidDeg(slot) * kDeg, half = layout.SpanDeg(slot) / 2.f * kDeg;
        const float px = radius * std::sin(mid), py = -radius * std::cos(mid);   // 0° = 12 o'clock, clockwise, y down
        float height = 0.f;
        for (const auto& r : rows)
            height += r.h;

        // A tile's straight side runs kTileGapDip / 2 from the radial line at its edge angle. Inside
        // the tile, a point is that far (plus the padding) on the tile's side of both lines.
        const float side = kTileGapDip / 2.f + pad;
        const float n0x = std::cos(mid - half), n0y = std::sin(mid - half);
        const float n1x = std::cos(mid + half), n1y = std::sin(mid + half);
        const float outer = band.outer - pad, inner = band.inner + pad;

        float top = py - height / 2.f;
        for (const auto& r : rows)
        {
            const float x0 = px - r.w / 2.f, x1 = px + r.w / 2.f, y0 = top, y1 = top + r.h;
            top += r.h;
            if (r.w <= 0.f)
                continue;
            for (const float x : { x0, x1 })
                for (const float y : { y0, y1 })
                {
                    if (x * x + y * y > outer * outer)
                        return false;
                    if (s.count > 1 && (x * n0x + y * n0y < side || x * n1x + y * n1y > -side))
                        return false;
                }
            // The inner arc bulges into the box, so its nearest point counts, not only the corners.
            const float dx = x0 > 0.f ? x0 : (x1 < 0.f ? -x1 : 0.f);
            const float dy = y0 > 0.f ? y0 : (y1 < 0.f ? -y1 : 0.f);
            if (dx * dx + dy * dy < inner * inner)
                return false;
        }
        return true;
    }

    std::vector<FittedLabel> FitLabels(const Layout& layout, const std::vector<LabelSpec>& labels, const MeasureLabel& measure)
    {
        const size_t n = std::min(labels.size(), layout.slots.size());
        std::vector<FittedLabel> out;
        out.reserve(n);
        Fitter fitter(layout, measure);
        for (size_t i = 0; i < n; ++i)
            out.push_back(fitter.Fit(static_cast<int>(i), labels[i].text, labels[i].style == LabelStyle::Icon));
        return out;
    }

    float CoveredArea(const Layout& layout, int slot, const FittedLabel& label, float radius, const Rect& p)
    {
        const auto b = BoxOf(layout, slot, label, radius);
        const float w = std::min(b.r, p.r) - std::max(b.l, p.l), h = std::min(b.b, p.b) - std::max(b.t, p.t);
        return w > 0.f && h > 0.f ? w * h : 0.f;
    }

    float ClearOfPicture(const Layout& layout, int slot, const FittedLabel& label, const Rect& picture)
    {
        float best = label.radius, least = CoveredArea(layout, slot, label, label.radius, picture);
        if (least <= 0.f || slot < 0 || slot >= static_cast<int>(layout.slots.size()))
            return best;
        const auto band = layout.BandOf(slot);
        const auto rows = RowsOf(label);
        // Every spot the label fits at, nearest its own first, so a tie keeps it where it was.
        for (float off = 1.f; off <= band.outer - band.inner; off += 1.f)
            for (const float r : { label.radius - off, label.radius + off })
            {
                if (!RowsFit(layout, slot, rows, r))
                    continue;
                const float covered = CoveredArea(layout, slot, label, r, picture);
                if (covered < least)
                {
                    least = covered;
                    best = r;
                    if (covered <= 0.f)
                        return best;
                }
            }
        return best;
    }

    CentrePlan PlanCentre(const Rect& text, const std::optional<Rect>& picture, float badgeW, float badgeH, bool wasBadge)
    {
        CentrePlan plan;
        if (!picture)
            return plan;
        const Rect& p = *picture;
        // Entering takes a clear overlap margin; leaving a clear gap, so the edge does not flicker.
        const float pad = wasBadge ? 4.f : 1.f;
        const bool covers = p.l < text.r + pad && p.r > text.l - pad && p.t < text.b + pad && p.b > text.t - pad;
        if (!covers)
            return plan;
        plan.badge = true;
        constexpr float kGap = 3.f;
        const float roomBelow = kCentreDip - p.b, roomAbove = p.t + kCentreDip;
        plan.y = roomBelow >= roomAbove ? p.b + kGap + badgeH / 2.f : p.t - kGap - badgeH / 2.f;
        // Under the picture's middle, but not further out than the centre disc's edge.
        const float limit = std::max(0.f, kCentreDip - badgeW / 4.f);
        plan.x = std::clamp((p.l + p.r) / 2.f, -limit, limit);
        return plan;
    }
}
