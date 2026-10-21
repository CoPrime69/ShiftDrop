#include "Recipe.h"
#include "Settings.h"
#include "TestUtil.h"
#include "keyboard/WheelLabels.h"
#include "keyboard/WheelLayout.h"

#include <dwrite.h>
#include <wrl/client.h>

#include <algorithm>
#include <format>
#include <map>
#include <set>

using namespace sd;
using namespace sd::test;
using namespace sd::wheel;
using Microsoft::WRL::ComPtr;

namespace
{
    // A stand-in font: every character `em` of the font size wide.
    MeasureLabel Even(float em = 0.6f)
    {
        return [em](const std::wstring& text, LabelStyle style) { return static_cast<float>(text.size()) * em * FullSize(style); };
    }

    // The rows FitLabels checked for a fitted label, rebuilt from what it reports.
    std::vector<Row> RowsOf(const FittedLabel& f, LabelStyle style, const MeasureLabel& measure)
    {
        std::vector<Row> rows;
        if (style == LabelStyle::Icon)
        {
            rows.push_back(Row{ f.iconSize, f.iconSize });
            rows.push_back(Row{ 0.f, kIconGapDip });
        }
        for (const auto& line : f.lines)
            rows.push_back(Row{ (measure(line, style) + 1.f) * f.size / FullSize(style), f.size * kLineHeight });
        return rows;
    }

    // The same label on every slot of a wheel of `n` slices: each position on the wheel at once.
    std::vector<FittedLabel> Everywhere(size_t n, const std::wstring& text, LabelStyle style, const MeasureLabel& measure)
    {
        return FitLabels(Arrange(n), std::vector<LabelSpec>(n, LabelSpec{ text, style }), measure);
    }

    // DirectWrite, as the wheel measures (Overlay::FitShown): the family at the label's full size at
    // `scale` px per DIP, in DIPs.
    class DWriteMeasure
    {
    public:
        DWriteMeasure(IDWriteFactory* dwrite, std::wstring family, float scale) : m_dwrite(dwrite), m_family(std::move(family)), m_scale(scale) {}

        float operator()(const std::wstring& text, LabelStyle style)
        {
            const auto key = std::make_pair(static_cast<int>(style), text);
            if (auto it = m_widths.find(key); it != m_widths.end())
                return it->second;
            auto& format = m_formats[static_cast<int>(style)];
            if (!format)
                m_dwrite->CreateTextFormat(m_family.c_str(), nullptr, style == LabelStyle::Name ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_SEMI_BOLD,
                                           DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, FullSize(style) * m_scale, L"", &format);
            ComPtr<IDWriteTextLayout> layout;
            DWRITE_TEXT_METRICS m{};
            float width = 0.f;
            if (format && SUCCEEDED(m_dwrite->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), format.Get(), 10000.f, 100.f, &layout)) &&
                SUCCEEDED(layout->GetMetrics(&m)))
                width = m.widthIncludingTrailingWhitespace / m_scale;
            return m_widths[key] = width;
        }

    private:
        IDWriteFactory* m_dwrite;
        std::wstring m_family;
        float m_scale;
        std::map<int, ComPtr<IDWriteTextFormat>> m_formats;
        std::map<std::pair<int, std::wstring>, float> m_widths;
    };

    // For messages: the label's letters (non-ASCII ones as '?').
    std::string Narrow(const std::wstring& text)
    {
        std::string out;
        for (const wchar_t c : text)
            out += c < 0x80 ? static_cast<char>(c) : '?';
        return out;
    }

    bool HasFamily(IDWriteFactory* dwrite, const wchar_t* name)
    {
        ComPtr<IDWriteFontCollection> fonts;
        UINT32 index = 0;
        BOOL exists = FALSE;
        return SUCCEEDED(dwrite->GetSystemFontCollection(&fonts)) && SUCCEEDED(fonts->FindFamilyName(name, &index, &exists)) && exists;
    }
}

// ---- The tile's shape ---------------------------------------------------------------------------

TEST(WheelLabels, RowsStayInsideTheTile)
{
    const auto four = Arrange(4);   // the top tile spans 90°, 58 to 128 DIPs out
    EXPECT_TRUE(RowsFit(four, 0, { Row{ 40.f, 16.f } }, 93.f));
    EXPECT_FALSE(RowsFit(four, 0, { Row{ 200.f, 16.f } }, 93.f));   // past the straight sides
    // Clear of the centre by the padding: the box's lower edge may not come within it of the inner arc.
    EXPECT_FALSE(RowsFit(four, 0, { Row{ 20.f, 10.f } }, kInnerInDip + kLabelPadDip));
    EXPECT_TRUE(RowsFit(four, 0, { Row{ 20.f, 10.f } }, kInnerInDip + kLabelPadDip + 6.f));
    // ...and of the rim.
    EXPECT_FALSE(RowsFit(four, 0, { Row{ 20.f, 10.f } }, kInnerOutDip - kLabelPadDip));
    // The same box is too wide for a tile of an eight-slice ring, which is half as wide.
    EXPECT_TRUE(RowsFit(four, 0, { Row{ 70.f, 16.f } }, 100.f));
    EXPECT_FALSE(RowsFit(Arrange(8), 0, { Row{ 70.f, 16.f } }, 100.f));
    // A box on its side (3 o'clock) is checked against the tile there, not the top one.
    EXPECT_TRUE(RowsFit(four, 1, { Row{ 50.f, 16.f } }, 93.f));
    EXPECT_FALSE(RowsFit(four, 1, { Row{ 16.f, 200.f } }, 93.f));
    // The inner arc bulges into a wide box: its middle, not only its corners, must stay clear.
    EXPECT_FALSE(RowsFit(four, 0, { Row{ 60.f, 8.f } }, kInnerInDip + kLabelPadDip + 3.f));
}

// ---- Fitting a label ----------------------------------------------------------------------------

TEST(WheelLabels, ShortLabelsKeepOneLineAtFullSize)
{
    for (const auto& f : Everywhere(8, L"PNG", LabelStyle::Name, Even()))
    {
        EXPECT_EQ(f.lines, (std::vector<std::wstring>{ L"PNG" }));
        EXPECT_FLOAT_EQ(f.size, kNameLabelDip);
        EXPECT_FALSE(f.shortened);
        EXPECT_TRUE(f.fits);
    }
}

TEST(WheelLabels, WrapsBeforeItShrinks)
{
    // Too wide for one line at the top of a six-slice ring, but two lines fit at full size.
    const auto fits = Everywhere(6, L"Under 10 MB", LabelStyle::Name, Even());
    EXPECT_EQ(fits[0].lines, (std::vector<std::wstring>{ L"Under", L"10 MB" }));
    EXPECT_FLOAT_EQ(fits[0].size, kNameLabelDip);
    EXPECT_FALSE(fits[0].shortened);
    // Each line's centre is below the one before, the block centred on its radius.
    ASSERT_EQ(fits[0].lineDy.size(), 2u);
    EXPECT_LT(fits[0].lineDy[0], 0.f);
    EXPECT_GT(fits[0].lineDy[1], 0.f);
}

TEST(WheelLabels, NeverBreaksANumberFromItsUnit)
{
    for (size_t n = 1; n <= kTwoRingCapacity; ++n)
        for (const auto* text : { L"Under 10 MB", L"Fit in 1920 px", L"Under 1 MB" })
            for (const auto& f : Everywhere(n, text, LabelStyle::Name, Even(0.7f)))
                for (const auto& line : f.lines)
                {
                    EXPECT_NE(line, L"MB") << text;
                    EXPECT_NE(line, L"px") << text;
                    EXPECT_FALSE(line.ends_with(L"10") || line.ends_with(L"1920") || line.ends_with(L" 1")) << line;
                }
}

TEST(WheelLabels, ALongWordShrinksAndIsNeverCut)
{
    const auto fits = Everywhere(16, L"Visualizer", LabelStyle::Icon, Even(0.62f));
    for (const auto& f : fits)
    {
        EXPECT_EQ(f.Text(), L"Visualizer");
        EXPECT_EQ(f.lines.size(), 1u);
        EXPECT_LE(f.size, kIconLabelDip);
        EXPECT_TRUE(f.fits);
    }
    // Somewhere on that crowded wheel it had to be drawn smaller than its full size.
    EXPECT_TRUE(std::any_of(fits.begin(), fits.end(), [](const FittedLabel& f) { return f.size < kIconLabelDip; }));
}

TEST(WheelLabels, EveryFittedLabelIsInsideItsTile)
{
    const auto measure = Even(0.6f);
    const std::vector<std::wstring> texts{ L"Rotate right", L"Remove metadata", L"Share-safe", L"Under 25 MB", L"Make a black and white poster" };
    for (size_t n = 1; n <= kTwoRingCapacity; ++n)
        for (const auto style : { LabelStyle::Name, LabelStyle::Icon })
            for (const auto& text : texts)
            {
                const auto fits = Everywhere(n, text, style, measure);
                for (size_t i = 0; i < fits.size(); ++i)
                {
                    if (!fits[i].fits)
                        continue;
                    EXPECT_TRUE(RowsFit(Arrange(n), static_cast<int>(i), RowsOf(fits[i], style, measure), fits[i].radius)) << text << " on slot " << i << " of " << n;
                    for (const auto& line : fits[i].lines)
                        EXPECT_EQ(line.find(L'\u2026'), std::wstring::npos);
                }
            }
}

TEST(WheelLabels, ShortensOnlyAsALastResortAndKeepsLabelsApart)
{
    // A font in which "Under" can never fit: the sizes are shortened to what tells them apart.
    const MeasureLabel wideUnder = [](const std::wstring& text, LabelStyle style) {
        return text.find(L"Under") != std::wstring::npos ? 500.f : static_cast<float>(text.size()) * 0.6f * FullSize(style);
    };
    const std::vector<LabelSpec> labels{ { L"Compress", LabelStyle::Name }, { L"Under 10 MB", LabelStyle::Name }, { L"Under 25 MB", LabelStyle::Name } };
    const auto fits = FitLabels(Arrange(3), labels, wideUnder);
    EXPECT_FALSE(fits[0].shortened);
    EXPECT_EQ(fits[0].Text(), L"Compress");
    EXPECT_TRUE(fits[1].shortened);
    EXPECT_EQ(fits[1].Text(), L"\u226410 MB");
    EXPECT_EQ(fits[2].Text(), L"\u226425 MB");
    EXPECT_GE(fits[1].size, kNameLabelMinDip);
}

TEST(WheelLabels, AShorteningNeverReadsLikeAnotherSlice)
{
    // Only four letters ever fit, and "Blur" is another slice: neither longer label may be cut to it.
    const MeasureLabel four = [](const std::wstring& text, LabelStyle style) {
        return text.size() > 4 ? 900.f : static_cast<float>(text.size()) * 0.6f * FullSize(style);
    };
    const std::vector<LabelSpec> labels{ { L"Blur faces", LabelStyle::Icon }, { L"Blur plates", LabelStyle::Icon }, { L"Blur", LabelStyle::Icon } };
    const auto fits = FitLabels(Arrange(3), labels, four);
    std::set<std::wstring> seen;
    for (const auto& f : fits)
        EXPECT_TRUE(seen.insert(f.Text()).second) << "two slices read " << Narrow(f.Text());
    EXPECT_EQ(fits[2].Text(), L"Blur");
    // The same label twice (Compress for a photo and for a video) is not a clash.
    const auto twins = FitLabels(Arrange(2), { { L"Compress", LabelStyle::Name }, { L"Compress", LabelStyle::Name } }, Even());
    EXPECT_EQ(twins[0].Text(), twins[1].Text());
}

// ---- Every recipe, every Size, every DPI --------------------------------------------------------

TEST(WheelLabels, EveryRecipeLabelFitsEverySliceAtEverySize)
{
    TempDir dir;
    Recipes recipes(dir);
    std::set<std::pair<std::wstring, LabelStyle>> labels{ { L"More", LabelStyle::Icon } };   // the paging slice
    for (const auto& r : recipes.store.All())
        labels.insert({ r.label, r.NameOnlySlice() ? LabelStyle::Name : LabelStyle::Icon });
    ASSERT_GT(labels.size(), 60u);

    // The scales the wheel is drawn at: each Size setting at 100% to 200%, on 1080p and 1440p screens.
    std::set<float> scales;
    for (const auto size : { WheelSize::Auto, WheelSize::Small, WheelSize::Medium, WheelSize::Large })
        for (const float dpi : { 1.f, 1.25f, 1.5f, 2.f })
            for (const float height : { 1080.f, 1440.f })
                scales.insert(SizeScale(size, dpi, height * dpi));

    ComPtr<IDWriteFactory> dwrite;
    ASSERT_HRESULT_SUCCEEDED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(dwrite.GetAddressOf())));
    // The wheel's text font (theme::TextFamily): Segoe UI Variable on Windows 11, else Segoe UI.
    std::vector<std::wstring> families;
    for (const auto* family : { L"Segoe UI Variable Text", L"Segoe UI" })
        if (HasFamily(dwrite.Get(), family))
            families.push_back(family);
    if (families.empty())
        families.push_back(L"Segoe UI");   // DirectWrite stands another font in

    // One line per label that goes wrong anywhere, with the place it does worst.
    std::map<std::string, std::pair<float, std::string>> problems;
    for (const auto& family : families)
        for (const float scale : scales)
        {
            DWriteMeasure dw(dwrite.Get(), family, scale);
            const MeasureLabel measure = [&dw](const std::wstring& text, LabelStyle style) { return dw(text, style); };
            for (const auto& [text, style] : labels)
            {
                std::string worst;
                float smallest = 1e9f;
                const std::string key = Narrow(family) + " " + Narrow(text) + (style == LabelStyle::Icon ? " (icon)" : " (name)");
                for (size_t n = 1; n <= kTwoRingCapacity; ++n)
                {
                    const auto fits = Everywhere(n, text, style, measure);
                    for (size_t i = 0; i < fits.size(); ++i)
                    {
                        const auto& f = fits[i];
                        const bool inside = RowsFit(Arrange(n), static_cast<int>(i), RowsOf(f, style, measure), f.radius);
                        const bool whole = !f.shortened && f.Text() == text;
                        if (!f.fits || !whole || !inside || f.size < smallest)
                        {
                            smallest = std::min(smallest, f.size);
                            if (!f.fits || !whole || !inside || f.size < MinSize(style))
                                worst = std::format("as '{}' at {:.1f} DIP on slot {} of {} at scale {:.2f}{}{}", Narrow(f.Text()), f.size, i, n, scale,
                                                    inside ? "" : ", outside its tile", f.fits ? "" : ", does not fit");
                        }
                    }
                }
                if (!worst.empty() && (!problems.contains(key) || smallest < problems[key].first))
                    problems[key] = { smallest, worst };
            }
        }
    std::string report;
    for (const auto& [key, problem] : problems)
        report += std::format("  {} {}\n", key, problem.second);
    EXPECT_TRUE(problems.empty()) << "labels below the readable minimum, shortened or outside their tile:\n" << report;
}

// ---- The centre and the drag picture ------------------------------------------------------------

TEST(WheelLabels, TheCountMovesBesideAPictureOverIt)
{
    const Rect text{ -45.f, -22.f, 45.f, 20.f };
    // No picture (click mode), or one clear of the text: the count stays in the middle.
    EXPECT_FALSE(PlanCentre(text, std::nullopt, 60.f, 24.f, false).badge);
    EXPECT_FALSE(PlanCentre(text, Rect{ 60.f, -20.f, 140.f, 60.f }, 60.f, 24.f, false).badge);

    // Explorer's thumbnail, held a little above its middle: the badge goes below it, clear of it.
    const Rect picture{ -40.f, -45.f, 40.f, 35.f };
    const auto below = PlanCentre(text, picture, 60.f, 24.f, false);
    ASSERT_TRUE(below.badge);
    EXPECT_GE(below.y - 12.f, picture.b);
    EXPECT_NEAR(below.x, 0.f, 0.01f);

    // Held near its top, so it hangs below the cursor: more of the disc is free above it.
    const Rect low{ -40.f, -10.f, 40.f, 70.f };
    const auto above = PlanCentre(text, low, 60.f, 24.f, false);
    ASSERT_TRUE(above.badge);
    EXPECT_LE(above.y + 12.f, low.t);

    // A badge stays until the picture is clearly off the text, so it does not flicker at the edge.
    const Rect edge{ 46.f, -20.f, 120.f, 60.f };
    EXPECT_FALSE(PlanCentre(text, edge, 60.f, 24.f, false).badge);
    EXPECT_TRUE(PlanCentre(text, edge, 60.f, 24.f, true).badge);
}

TEST(WheelLabels, ShrinkToFitKeepsToItsLimits)
{
    EXPECT_FLOAT_EQ(ShrinkToFit(50.f, 19.f, 13.f, 90.f), 19.f);
    EXPECT_FLOAT_EQ(ShrinkToFit(100.f, 19.f, 13.f, 90.f), 17.1f);
    EXPECT_FLOAT_EQ(ShrinkToFit(400.f, 19.f, 13.f, 90.f), 13.f);
}
