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

        bool Same(const std::wstring& a, const std::wstring& b)
        {
            return a.size() == b.size() &&
                   std::equal(a.begin(), a.end(), b.begin(), [](wchar_t x, wchar_t y) { return std::towlower(x) == std::towlower(y); });
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

        // Step 3's first try: "Under 10 MB" reads as "≤10 MB".
        std::wstring Abbreviate(const std::wstring& text)
        {
            std::wstring out = text;
            constexpr std::wstring_view kUnder = L"Under ";
            for (size_t at = out.find(kUnder); at != std::wstring::npos; at = out.find(kUnder, at + 1))
                if ((at == 0 || out[at - 1] == L' ') && at + kUnder.size() < out.size() && std::iswdigit(out[at + kUnder.size()]))
                    out.replace(at, kUnder.size(), L"\u2264");
            return out;
        }

        float IconSize(float labelSize)
        {
            return kIconFontDip * labelSize / kIconLabelDip;
        }

        class Fitter
        {
        public:
            Fitter(const Layout& layout, const MeasureLabel& measure) : m_layout(layout), m_measure(measure) {}

            // `text` on one line or two, from `from` down to `to` DIPs, as steps 1 and 2 say.
            std::optional<FittedLabel> Fit(int slot, const std::wstring& text, LabelStyle style, float from, float to)
            {
                const auto words = Words(text);
                std::vector<std::pair<std::wstring, std::wstring>> splits;
                for (size_t i = 1; i < words.size(); ++i)
                    if (BreakAllowed(words, i))
                        splits.emplace_back(Join(words, 0, i), Join(words, i, words.size()));
                // The most even split first: it needs the least width.
                std::stable_sort(splits.begin(), splits.end(), [&](const auto& a, const auto& b) {
                    return std::max(Width(a.first, style), Width(a.second, style)) < std::max(Width(b.first, style), Width(b.second, style));
                });
                const int steps = static_cast<int>(std::floor((from - to) / kLabelStepDip + 1e-3f));
                for (int k = 0; k <= steps; ++k)
                {
                    const float size = from - kLabelStepDip * static_cast<float>(k);
                    if (auto one = Try(slot, style, { text }, size))
                        return one;
                    for (const auto& [a, b] : splits)
                        if (auto two = Try(slot, style, { a, b }, size))
                            return two;
                }
                return std::nullopt;
            }

            // Where the block would sit when nothing fits: the middle of the tile.
            FittedLabel Place(LabelStyle style, std::vector<std::wstring> lines, float size, float radius)
            {
                FittedLabel f;
                f.lines = std::move(lines);
                f.size = size;
                f.radius = radius;
                f.lineHeight = size * kLineHeight;
                const auto rows = Rows(style, f.lines, size);
                float height = 0.f;
                for (const auto& r : rows)
                    height += r.h;
                float y = -height / 2.f;
                if (style == LabelStyle::Icon)
                {
                    f.iconSize = IconSize(size);
                    f.iconDy = y + f.iconSize / 2.f;
                    y += f.iconSize + kIconGapDip;
                }
                for (size_t i = 0; i < f.lines.size(); ++i)
                {
                    f.lineDy.push_back(y + f.lineHeight / 2.f);
                    y += f.lineHeight;
                }
                return f;
            }

        private:
            float Width(const std::wstring& text, LabelStyle style)
            {
                const auto key = std::make_pair(static_cast<int>(style), text);
                if (auto it = m_widths.find(key); it != m_widths.end())
                    return it->second;
                return m_widths[key] = (m_measure ? m_measure(text, style) : 0.f) + kMeasureSlackDip;
            }

            std::vector<Row> Rows(LabelStyle style, const std::vector<std::wstring>& lines, float size)
            {
                std::vector<Row> rows;
                if (style == LabelStyle::Icon)
                {
                    rows.push_back(Row{ IconSize(size), IconSize(size) });
                    rows.push_back(Row{ 0.f, kIconGapDip });
                }
                for (const auto& line : lines)
                    rows.push_back(Row{ Width(line, style) * size / FullSize(style), size * kLineHeight });
                return rows;
            }

            std::optional<FittedLabel> Try(int slot, LabelStyle style, const std::vector<std::wstring>& lines, float size)
            {
                const auto rows = Rows(style, lines, size);
                const auto band = m_layout.BandOf(slot);
                const float mid = (band.inner + band.outer) / 2.f, reach = (band.outer - band.inner) / 2.f;
                // The middle of the tile first, then a DIP at a time out and in: toward the rim a tile is
                // wider, toward the centre its corners are further from the text.
                for (float off = 0.f; off <= reach; off += 1.f)
                    for (const float r : { mid + off, mid - off })
                    {
                        if (RowsFit(m_layout, slot, rows, r))
                            return Place(style, lines, size, r);
                        if (off == 0.f)
                            break;
                    }
                return std::nullopt;
            }

            const Layout& m_layout;
            const MeasureLabel& m_measure;
            std::map<std::pair<int, std::wstring>, float> m_widths;
        };
    }

    float FullSize(LabelStyle style) { return style == LabelStyle::Name ? kNameLabelDip : kIconLabelDip; }
    float MinSize(LabelStyle style) { return style == LabelStyle::Name ? kNameLabelMinDip : kIconLabelMinDip; }

    std::wstring FittedLabel::Text() const
    {
        return Join(lines, 0, lines.size());
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
        std::vector<FittedLabel> out(n);
        std::vector<bool> done(n, false);
        Fitter fitter(layout, measure);
        for (size_t i = 0; i < n; ++i)
        {
            const auto& l = labels[i];
            if (auto f = fitter.Fit(static_cast<int>(i), l.text, l.style, FullSize(l.style), MinSize(l.style)))
            {
                out[i] = std::move(*f);
                done[i] = true;
            }
        }

        // A shortening may not read like another slice: not another label, nor what another slice shows.
        const auto distinct = [&](const std::wstring& candidate, size_t i) {
            for (size_t j = 0; j < n; ++j)
            {
                if (j == i || labels[j].text == labels[i].text)
                    continue;   // the same label twice (Compress for a photo and a video) may look the same
                if (Same(candidate, labels[j].text) || (done[j] && Same(candidate, out[j].Text())))
                    return false;
            }
            return true;
        };
        for (size_t i = 0; i < n; ++i)
        {
            if (done[i])
                continue;
            const auto& l = labels[i];
            const int slot = static_cast<int>(i);
            std::vector<std::wstring> candidates;
            const auto add = [&](const std::wstring& c) {
                if (!c.empty() && c != l.text && std::find(candidates.begin(), candidates.end(), c) == candidates.end())
                    candidates.push_back(c);
            };
            const std::wstring abbreviated = Abbreviate(l.text);
            add(abbreviated);
            const auto words = Words(abbreviated);
            // Words another label has too say the least about this one ("Under", "MB").
            std::vector<std::wstring> kept = words;
            for (const auto& w : words)
            {
                const bool shared = std::any_of(labels.begin(), labels.begin() + static_cast<ptrdiff_t>(n), [&](const LabelSpec& o) {
                    if (o.text == l.text)
                        return false;
                    const auto ow = Words(o.text);
                    return std::any_of(ow.begin(), ow.end(), [&](const std::wstring& x) { return Same(x, w); });
                });
                const auto at = std::find(kept.begin(), kept.end(), w);
                const size_t index = static_cast<size_t>(at - kept.begin());
                // A unit stays with its number ("10 MB" never becomes "10").
                if (shared && kept.size() > 1 && (index == 0 || BreakAllowed(kept, index)))
                {
                    kept.erase(at);
                    add(Join(kept, 0, kept.size()));
                }
            }
            for (size_t k = words.size(); k-- > 1;)
                if (BreakAllowed(words, k))
                    add(Join(words, 0, k));

            for (const auto& c : candidates)
            {
                if (!distinct(c, i))
                    continue;
                if (auto f = fitter.Fit(slot, c, l.style, FullSize(l.style), MinSize(l.style)))
                {
                    out[i] = std::move(*f);
                    out[i].shortened = true;
                    done[i] = true;
                    break;
                }
            }
            if (done[i])
                continue;
            // Step 4: the whole label, smaller than the minimum.
            if (auto f = fitter.Fit(slot, l.text, l.style, MinSize(l.style) - kLabelStepDip, kLabelFloorDip))
            {
                out[i] = std::move(*f);
                done[i] = true;
                continue;
            }
            const auto band = layout.BandOf(slot);
            out[i] = fitter.Place(l.style, { l.text }, kLabelFloorDip, (band.inner + band.outer) / 2.f);
            out[i].fits = false;
            done[i] = true;
        }
        return out;
    }

    float ShrinkToFit(float fullWidth, float full, float min, float room)
    {
        if (fullWidth <= room || fullWidth <= 0.f)
            return full;
        return std::max(min, full * room / fullWidth);
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
