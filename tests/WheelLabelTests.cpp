#include "ImagePipeline.h"
#include "PipelineRunner.h"
#include "Recipe.h"
#include "Settings.h"
#include "TestUtil.h"
#include "keyboard/WheelLabels.h"
#include "keyboard/WheelLayout.h"

#include <dwrite.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstring>
#include <format>
#include <map>
#include <set>

using namespace sd;
using namespace sd::test;
using namespace sd::wheel;
using Microsoft::WRL::ComPtr;

namespace
{
    // A stand-in font: every character `em` of the label size wide.
    MeasureLabel Even(float em = 0.6f)
    {
        return [em](const std::wstring& text) { return static_cast<float>(text.size()) * em * kLabelDip; };
    }

    // The same label on every slot of a wheel of `n` slices: each position on the wheel at once.
    std::vector<FittedLabel> Everywhere(size_t n, const std::wstring& text, LabelStyle style, const MeasureLabel& measure)
    {
        return FitLabels(Arrange(n), std::vector<LabelSpec>(n, LabelSpec{ text, style }), measure);
    }

    // DirectWrite, as the wheel measures (Overlay::FitShown): the family at kLabelDip Semibold at
    // `scale` px per DIP, in DIPs.
    class DWriteMeasure
    {
    public:
        DWriteMeasure(IDWriteFactory* dwrite, const std::wstring& family, float scale) : m_dwrite(dwrite), m_scale(scale)
        {
            m_dwrite->CreateTextFormat(family.c_str(), nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                       kLabelDip * m_scale, L"", &m_format);
        }

        float operator()(const std::wstring& text)
        {
            if (auto it = m_widths.find(text); it != m_widths.end())
                return it->second;
            ComPtr<IDWriteTextLayout> layout;
            DWRITE_TEXT_METRICS m{};
            float width = 0.f;
            if (m_format && SUCCEEDED(m_dwrite->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), m_format.Get(), 10000.f, 100.f, &layout)) &&
                SUCCEEDED(layout->GetMetrics(&m)))
                width = m.widthIncludingTrailingWhitespace / m_scale;
            return m_widths[text] = width;
        }

    private:
        IDWriteFactory* m_dwrite;
        float m_scale;
        ComPtr<IDWriteTextFormat> m_format;
        std::map<std::wstring, float> m_widths;
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

TEST(WheelLabels, ShortLabelsKeepOneLine)
{
    for (const auto style : { LabelStyle::Name, LabelStyle::Icon })
        for (const auto& f : Everywhere(8, L"PNG", style, Even()))
        {
            EXPECT_EQ(f.lines, (std::vector<std::wstring>{ L"PNG" }));
            EXPECT_EQ(f.icon, style == LabelStyle::Icon);
            EXPECT_TRUE(f.fits);
        }
}

TEST(WheelLabels, OneSizeForEveryLabel)
{
    // A text-only tile and an icon tile, short and long, on one ring or two: the same type, and the
    // same icon size, wherever they are (the drawing takes kLabelDip and kIconDip for every tile).
    const std::vector<LabelSpec> labels{ { L"90%", LabelStyle::Name }, { L"Under 10 MB", LabelStyle::Name }, { L"Copy", LabelStyle::Icon },
                                         { L"Remove metadata", LabelStyle::Icon }, { L"Zip", LabelStyle::Icon } };
    for (const size_t n : { size_t(5), size_t(10), size_t(16) })
    {
        std::vector<LabelSpec> many;
        for (size_t i = 0; i < n; ++i)
            many.push_back(labels[i % labels.size()]);
        for (const auto& f : FitLabels(Arrange(n), many, Even(0.55f)))
        {
            ASSERT_FALSE(f.lines.empty());
            for (size_t k = 0; k < f.lines.size(); ++k)
                EXPECT_NEAR(f.widths[k], Even(0.55f)(f.lines[k]) + 1.f, 1e-3f);   // measured at kLabelDip, never scaled down
            if (f.icon && f.fits)
                EXPECT_LT(f.iconDy, f.lineDy.front());   // the icon above the label
        }
    }
}

TEST(WheelLabels, WrapsToTwoLinesInsteadOfShrinking)
{
    // Too wide for one line at the top of a six-slice ring, so it goes on two.
    const auto fits = Everywhere(6, L"Under 10 MB", LabelStyle::Name, Even(0.85f));
    EXPECT_EQ(fits[0].lines, (std::vector<std::wstring>{ L"Under", L"10 MB" }));
    EXPECT_TRUE(fits[0].fits);
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
                    EXPECT_FALSE(line.ends_with(L"10") || line.ends_with(L"1920") || line.ends_with(L" 1")) << Narrow(line);
                }
}

TEST(WheelLabels, ALabelThatDoesNotFitIsReportedNeverShrunkOrCut)
{
    // A word far too wide for any tile: the whole label is kept, on one line, and flagged.
    for (const auto& f : Everywhere(16, L"Supercalifragilistic", LabelStyle::Icon, Even(0.62f)))
    {
        EXPECT_EQ(f.Text(), L"Supercalifragilistic");
        EXPECT_EQ(f.lines.size(), 1u);
        EXPECT_FALSE(f.fits);
    }
    // Two long words: flagged, on the most even two lines.
    const auto two = Everywhere(16, L"Extraordinarily longwinded", LabelStyle::Icon, Even(0.62f));
    EXPECT_FALSE(two[0].fits);
    EXPECT_EQ(two[0].lines.size(), 2u);
    EXPECT_EQ(two[0].Text(), L"Extraordinarily longwinded");
}

TEST(WheelLabels, EveryFittedLabelIsInsideItsTile)
{
    const auto measure = Even(0.6f);
    const std::vector<std::wstring> texts{ L"Rotate right", L"Remove metadata", L"Share-safe", L"Under 25 MB", L"Copy path" };
    for (size_t n = 1; n <= kTwoRingCapacity; ++n)
        for (const auto style : { LabelStyle::Name, LabelStyle::Icon })
            for (const auto& text : texts)
            {
                const auto fits = Everywhere(n, text, style, measure);
                for (size_t i = 0; i < fits.size(); ++i)
                {
                    EXPECT_EQ(fits[i].Text(), text);
                    for (const auto& line : fits[i].lines)
                        EXPECT_EQ(line.find(L'\u2026'), std::wstring::npos);
                    if (fits[i].fits)
                        EXPECT_TRUE(RowsFit(Arrange(n), static_cast<int>(i), RowsOf(fits[i]), fits[i].radius)) << Narrow(text) << " on slot " << i << " of " << n;
                }
            }
}

// ---- The drag picture over a tile ---------------------------------------------------------------

TEST(WheelLabels, TheHoveredLabelMovesOutFromUnderThePicture)
{
    const auto layout = Arrange(8);
    const auto fits = FitLabels(layout, { { L"WebP", LabelStyle::Icon } }, Even());
    ASSERT_FALSE(fits.empty());
    const auto& f = fits[0];   // the top tile, 58 to 128 DIPs up
    // A picture well away from the label leaves it where it is.
    EXPECT_FLOAT_EQ(ClearOfPicture(layout, 0, f, Rect{ 150.f, 150.f, 200.f, 200.f }), f.radius);
    // A picture over the label's outer edge pushes it inward, clear of the picture, still in the tile.
    const Rect outer{ -30.f, -130.f, 30.f, -(f.radius + 11.f) };
    ASSERT_GT(CoveredArea(layout, 0, f, f.radius, outer), 0.f);
    const float in = ClearOfPicture(layout, 0, f, outer);
    EXPECT_LT(in, f.radius);
    EXPECT_FLOAT_EQ(CoveredArea(layout, 0, f, in, outer), 0.f);
    EXPECT_TRUE(RowsFit(layout, 0, RowsOf(f), in));
    // ...and one over its inner edge pushes it outward.
    const Rect inner{ -30.f, -(f.radius - 11.f), 30.f, -50.f };
    const float out = ClearOfPicture(layout, 0, f, inner);
    EXPECT_GT(out, f.radius);
    EXPECT_FLOAT_EQ(CoveredArea(layout, 0, f, out, inner), 0.f);
    // A picture over the whole tile: as little of the label under it as the tile allows (here, all).
    const Rect all{ -100.f, -140.f, 100.f, 0.f };
    EXPECT_TRUE(RowsFit(layout, 0, RowsOf(f), ClearOfPicture(layout, 0, f, all)));
}

// ---- Every wheel the app shows ------------------------------------------------------------------

TEST(WheelLabels, EveryWheelTheAppShowsFitsItsLabels)
{
    TempDir dir;
    Recipes recipes(dir);
    const PipelineRunner runner(std::make_shared<const ImageCodecs>(), nullptr, nullptr);

    // The wheels main.cpp builds (ContentFor): for each kind of file, alone or two at once, and a photo
    // with a video, each section shows the recipes meant for all the files, else for any, else all of
    // them dimmed; past 16 the wheel pages with a "More" slice.
    const std::vector<std::vector<std::wstring>> drops = [] {
        std::vector<std::vector<std::wstring>> out;
        for (const auto* name : { L"photo.jpg", L"a.png", L"a.heic", L"a.gif", L"a.webp", L"a.tif", L"a.bmp", L"a.avif", L"a.svg", L"a.ico",
                                  L"clip.mp4", L"clip.mov", L"clip.mkv", L"song.mp3", L"song.flac", L"song.wav", L"doc.pdf", L"a.docx",
                                  L"a.xlsx", L"a.pptx", L"a.odt", L"a.txt", L"a.md", L"a.html", L"a.csv", L"a.json", L"a.srt", L"a.vtt",
                                  L"a.zip", L"a.7z", L"a.tar.gz", L"a.rar", L"a.epub", L"a.xyz" })
        {
            const std::wstring path = L"C:\\files\\" + std::wstring(name);
            out.push_back({ path });
            out.push_back({ path, L"C:\\files\\b" + std::wstring(name).substr(std::wstring(name).find(L'.')) });
        }
        out.push_back({ L"C:\\files\\photo.jpg", L"C:\\files\\clip.mp4" });
        return out;
    }();
    struct Wheel
    {
        std::vector<LabelSpec> labels;
        std::string what;
    };
    std::vector<Wheel> wheels;
    std::set<std::vector<std::wstring>> seen;
    for (const char* ring : { "convert", "tools", "reduce", "files" })
        for (const auto& files : drops)
        {
            std::vector<const Recipe*> forAll, forAny, onRing;
            for (const auto& recipe : recipes.store.All())
            {
                if (recipe.ring != ring || !runner.AppliesToDrop(recipe, files.size()))
                    continue;
                onRing.push_back(&recipe);
                const auto applies = [&](const std::wstring& f) { return runner.Applies(recipe, f); };
                if (std::all_of(files.begin(), files.end(), applies))
                    forAll.push_back(&recipe);
                else if (std::any_of(files.begin(), files.end(), applies))
                    forAny.push_back(&recipe);
            }
            if (forAll.empty())
                forAll = forAny;
            const auto& shown = forAll.empty() ? onRing : forAll;
            std::vector<LabelSpec> content;
            for (const auto* r : shown)
                content.push_back({ r->label, r->NameOnlySlice() ? LabelStyle::Name : LabelStyle::Icon });
            const auto first = Arrange(content.size());
            for (int page = 0; page < first.pages; ++page)
            {
                const auto layout = Arrange(content.size(), page);
                std::vector<LabelSpec> labels;
                for (int i = 0; i < static_cast<int>(layout.slots.size()); ++i)
                    labels.push_back(layout.IsMore(i) ? LabelSpec{ L"More", LabelStyle::Icon } : content[static_cast<size_t>(layout.ContentIndex(i))]);
                std::vector<std::wstring> key{ std::wstring(ring, ring + std::strlen(ring)) };
                for (const auto& l : labels)
                    key.push_back(l.text);
                if (seen.insert(key).second)
                    wheels.push_back({ labels, std::format("{} wheel for {}{} page {}", ring, Narrow(files.front()), files.size() > 1 ? " and more" : "", page + 1) });
            }
        }
    ASSERT_GT(wheels.size(), 20u);

    ComPtr<IDWriteFactory> dwrite;
    ASSERT_HRESULT_SUCCEEDED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(dwrite.GetAddressOf())));
    // The wheel's text font (theme::TextFamily): Segoe UI Variable Text on Windows 11, else Segoe UI.
    std::vector<std::wstring> families;
    for (const auto* family : { L"Segoe UI Variable Text", L"Segoe UI" })
        if (HasFamily(dwrite.Get(), family))
            families.push_back(family);
    if (families.empty())
        families.push_back(L"Segoe UI");   // DirectWrite stands another font in

    // Labels are laid out in DIPs; the scale (DPI times the Size setting) only changes rounding.
    std::map<std::string, std::string> problems;
    for (const auto& family : families)
        for (const float scale : { 1.f, 1.25f, 1.5f, 2.f })
        {
            DWriteMeasure dw(dwrite.Get(), family, scale);
            const MeasureLabel measure = [&dw](const std::wstring& text) { return dw(text); };
            for (const auto& wheel : wheels)
            {
                const auto layout = Arrange(wheel.labels.size());
                const auto fits = FitLabels(layout, wheel.labels, measure);
                for (size_t i = 0; i < fits.size(); ++i)
                    if (!fits[i].fits || fits[i].Text() != wheel.labels[i].text)
                    {
                        // How wide a one-line label may be on that tile, for choosing a shorter one.
                        float room = 0.f;
                        for (float w = 10.f; w < 200.f; w += 1.f)
                        {
                            auto one = wheel.labels;
                            one[i].text = L"x";
                            if (!FitLabels(layout, one, [&](const std::wstring& t) { return t == L"x" ? w : measure(t); })[i].fits)
                                break;
                            room = w;
                        }
                        std::string widths;
                        for (const auto& line : fits[i].lines)
                            widths += std::format(" '{}' {:.0f}", Narrow(line), measure(line));
                        problems.try_emplace(Narrow(wheel.labels[i].text), std::format("{} slot {} of {} ({}, {:.2f}); lines{}; a line may be {:.0f} DIPs there",
                                                                                       wheel.what, i, fits.size(), Narrow(family), scale, widths, room));
                    }
            }
        }
    std::string report;
    for (const auto& [label, where] : problems)
        report += std::format("  '{}': {}\n", label, where);
    EXPECT_TRUE(problems.empty()) << "labels that do not fit their tile on two lines at the one label size (give the recipe a shorter label):\n" << report;
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
