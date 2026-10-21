#include "DropDescription.h"
#include "DropTarget.h"
#include "FileActions.h"
#include "PanelLayout.h"
#include "Settings.h"
#include "TestUtil.h"

#include <shlobj.h>
#include <wrl/implements.h>

#include <cmath>
#include <cstring>
#include <map>
#include <numeric>

using namespace sd;
using namespace sd::test;
using Microsoft::WRL::ComPtr;

namespace
{
    // ---- The top panel's size -------------------------------------------------------------------

    // The wheel names and the hint as the panel shows them, measured generously (0.6 em a letter
    // for the semibold names, 0.55 em for the hint) so a real font is never wider.
    const std::vector<std::wstring> kNames{ L"Convert", L"Tools", L"Reduce size", L"File actions" };
    const std::wstring kHint = L"Scroll or Shift+1\u20134";

    std::vector<float> NameWidths(float scale)
    {
        std::vector<float> out;
        for (const auto& name : kNames)
            out.push_back(static_cast<float>(name.size()) * 0.6f * panel::kTabFontDip * scale);
        return out;
    }
    float HintWidth(float scale) { return static_cast<float>(kHint.size()) * 0.55f * panel::kHintFontDip * scale; }

    float PanelScale(WheelSize size, float dpi, float heightPx) { return panel::Scale(SizeScale(size, dpi, heightPx)); }

    // The switcher as Island lays it out on a monitor `workWidth` pixels wide.
    panel::Switcher Fit(float scale, float workWidth)
    {
        const float available = panel::MaxContentWidth(scale, workWidth) - 2.f * panel::kInsetDip * scale;
        return panel::FitSwitcher(NameWidths(scale), HintWidth(scale), scale, available);
    }

    constexpr WheelSize kSizes[] = { WheelSize::Auto, WheelSize::Small, WheelSize::Medium, WheelSize::Large };

    // ---- Drag picture over the wheel --------------------------------------------------------------

    // A drag's data object: formats held as HGLOBAL bytes, as Explorer's is.
    class FakeData final : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IDataObject>
    {
    public:
        void Put(CLIPFORMAT format, const void* bytes, size_t size)
        {
            m_formats[format].assign(static_cast<const BYTE*>(bytes), static_cast<const BYTE*>(bytes) + size);
        }
        bool Has(CLIPFORMAT format) const { return m_formats.contains(format); }
        const std::vector<BYTE>& Bytes(CLIPFORMAT format) { return m_formats[format]; }

        STDMETHODIMP GetData(FORMATETC* fmt, STGMEDIUM* medium) override
        {
            *medium = {};
            const auto it = m_formats.find(fmt->cfFormat);
            if (it == m_formats.end() || !(fmt->tymed & TYMED_HGLOBAL))
                return DV_E_FORMATETC;
            medium->tymed = TYMED_HGLOBAL;
            medium->hGlobal = GlobalAlloc(GMEM_MOVEABLE, std::max<size_t>(it->second.size(), 1));
            memcpy(GlobalLock(medium->hGlobal), it->second.data(), it->second.size());
            GlobalUnlock(medium->hGlobal);
            return S_OK;
        }
        STDMETHODIMP QueryGetData(FORMATETC* fmt) override { return m_formats.contains(fmt->cfFormat) ? S_OK : DV_E_FORMATETC; }
        STDMETHODIMP SetData(FORMATETC* fmt, STGMEDIUM* medium, BOOL release) override
        {
            if (medium->tymed != TYMED_HGLOBAL)
                return DV_E_TYMED;
            const void* bytes = GlobalLock(medium->hGlobal);
            Put(fmt->cfFormat, bytes, GlobalSize(medium->hGlobal));
            GlobalUnlock(medium->hGlobal);
            if (release)
                ReleaseStgMedium(medium);
            return S_OK;
        }
        STDMETHODIMP GetDataHere(FORMATETC*, STGMEDIUM*) override { return E_NOTIMPL; }
        STDMETHODIMP GetCanonicalFormatEtc(FORMATETC*, FORMATETC* out) override
        {
            out->ptd = nullptr;
            return E_NOTIMPL;
        }
        STDMETHODIMP EnumFormatEtc(DWORD, IEnumFORMATETC**) override { return E_NOTIMPL; }
        STDMETHODIMP DAdvise(FORMATETC*, DWORD, IAdviseSink*, DWORD*) override { return OLE_E_ADVISENOTSUPPORTED; }
        STDMETHODIMP DUnadvise(DWORD) override { return OLE_E_ADVISENOTSUPPORTED; }
        STDMETHODIMP EnumDAdvise(IEnumSTATDATA**) override { return OLE_E_ADVISENOTSUPPORTED; }

    private:
        std::map<CLIPFORMAT, std::vector<BYTE>> m_formats;
    };

    CLIPFORMAT Format(const wchar_t* name) { return static_cast<CLIPFORMAT>(RegisterClipboardFormatW(name)); }

    ComPtr<FakeData> FileDrag(const std::vector<std::wstring>& files)
    {
        auto data = Microsoft::WRL::Make<FakeData>();
        auto drop = MakeDropFiles(files);
        data->Put(CF_HDROP, GlobalLock(drop.get()), GlobalSize(drop.get()));
        GlobalUnlock(drop.get());
        return data;
    }

    // A picture dragged out of a browser: a name and contents, no path.
    ComPtr<FakeData> BrowserDrag(const std::wstring& name)
    {
        auto data = Microsoft::WRL::Make<FakeData>();
        FILEGROUPDESCRIPTORW group{};
        group.cItems = 1;
        group.fgd[0].dwFlags = FD_FILESIZE;
        group.fgd[0].nFileSizeLow = 4;
        wcsncpy_s(group.fgd[0].cFileName, MAX_PATH, name.c_str(), _TRUNCATE);
        data->Put(Format(CFSTR_FILEDESCRIPTORW), &group, sizeof(group));
        return data;
    }

    // What the drop target tells the shell's drag-image helper.
    class FakeHelper final : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IDropTargetHelper>
    {
    public:
        STDMETHODIMP DragEnter(HWND target, IDataObject* data, POINT* pt, DWORD effect) override
        {
            calls += L"enter ";
            hwnd = target;
            seen = data;
            last = *pt;
            lastEffect = effect;
            return S_OK;
        }
        STDMETHODIMP DragLeave() override
        {
            calls += L"leave ";
            return S_OK;
        }
        STDMETHODIMP DragOver(POINT* pt, DWORD effect) override
        {
            calls += L"over ";
            last = *pt;
            lastEffect = effect;
            return S_OK;
        }
        STDMETHODIMP Drop(IDataObject* data, POINT* pt, DWORD effect) override
        {
            calls += L"drop ";
            seen = data;
            last = *pt;
            lastEffect = effect;
            return S_OK;
        }
        STDMETHODIMP Show(BOOL) override { return S_OK; }

        std::wstring calls;
        HWND hwnd = nullptr;
        IDataObject* seen = nullptr;
        POINT last{};
        DWORD lastEffect = 0;
    };

    // The wheel's side: takes any files, answers COPY.
    struct FakeWheel final : IDropSink
    {
        bool OnDragEnter(const std::vector<std::wstring>& f, POINT) override
        {
            entered = f;
            return !f.empty();
        }
        DWORD OnDragOver(POINT, DWORD allowed) override { return SafeEffect(allowed); }
        void OnDragLeave() override { ++left; }
        void OnDrop(const std::vector<std::wstring>& f, POINT) override { dropped = f; }

        std::vector<std::wstring> entered, dropped;
        int left = 0;
    };

    // The badge the data object carries (CFSTR_DROPDESCRIPTION), or -1 for none.
    int Badge(FakeData& data, std::wstring* message = nullptr)
    {
        const auto format = Format(CFSTR_DROPDESCRIPTION);
        if (!data.Has(format) || data.Bytes(format).size() < sizeof(DROPDESCRIPTION))
            return -1;
        DROPDESCRIPTION d{};
        memcpy(&d, data.Bytes(format).data(), sizeof(d));
        if (message)
            *message = d.szMessage;
        return d.type;
    }

    const HWND kWheel = reinterpret_cast<HWND>(static_cast<ULONG_PTR>(0x1234));   // only passed on to the helper

    template <class T>
    constexpr bool kHasHideCursor = requires(T t) { t.hideCursor; };
}

// ---- Size setting and panel scale ------------------------------------------------------------------

TEST(Panel, SizeScaleFollowsTheSetting)
{
    EXPECT_FLOAT_EQ(SizeScale(WheelSize::Medium, 1.f, 1080.f), 1.f);
    EXPECT_FLOAT_EQ(SizeScale(WheelSize::Small, 1.f, 1080.f), 0.85f);
    EXPECT_FLOAT_EQ(SizeScale(WheelSize::Large, 1.5f, 1080.f), 1.8f);
    // Auto: 1080p at 100% is the reference, 1440p gets larger (1.33, kept to 1.3), small screens stay readable.
    EXPECT_FLOAT_EQ(SizeScale(WheelSize::Auto, 1.f, 1080.f), 1.f);
    EXPECT_FLOAT_EQ(SizeScale(WheelSize::Auto, 1.f, 1440.f), 1.3f);
    EXPECT_FLOAT_EQ(SizeScale(WheelSize::Auto, 1.f, 768.f), 0.9f);
    EXPECT_NEAR(SizeScale(WheelSize::Auto, 1.25f, 1440.f), 1.25f * 1152.f / 1080.f, 1e-4f);
}

TEST(Panel, IsLargerThanTheWheelAndFollowsTheSizeSetting)
{
    for (const auto size : kSizes)
        EXPECT_FLOAT_EQ(PanelScale(size, 1.25f, 1440.f), 1.4f * SizeScale(size, 1.25f, 1440.f));
    EXPECT_LT(PanelScale(WheelSize::Small, 1.f, 1080.f), PanelScale(WheelSize::Medium, 1.f, 1080.f));
    EXPECT_LT(PanelScale(WheelSize::Medium, 1.f, 1080.f), PanelScale(WheelSize::Large, 1.f, 1080.f));
    EXPECT_LT(PanelScale(WheelSize::Auto, 1.f, 1080.f), PanelScale(WheelSize::Auto, 1.f, 1440.f));
}

TEST(Panel, AutoIsReadableOn1440p)
{
    // 2560x1440 at 100% and 125%: names of about 24 px and a panel about 80 px tall (it was 17 and 57).
    for (const float dpi : { 1.f, 1.25f })
    {
        const float s = PanelScale(WheelSize::Auto, dpi, 1440.f);
        EXPECT_GE(panel::kTabFontDip * s, 22.f) << dpi;
        EXPECT_GE(panel::kHintFontDip * s, 19.f) << dpi;
        EXPECT_GE(panel::kSwitcherHeightDip * s, 75.f) << dpi;
        const auto fit = Fit(s, 2560.f);
        EXPECT_TRUE(fit.hint);
        EXPECT_FLOAT_EQ(fit.nameScale, 1.f);
        EXPECT_FLOAT_EQ(fit.pad, panel::kTabPadDip * s);   // nothing had to shrink
    }
    // 1080p at 100%: 18 px names.
    EXPECT_NEAR(panel::kTabFontDip * PanelScale(WheelSize::Auto, 1.f, 1080.f), 18.2f, 0.01f);
}

TEST(Panel, FitsA1366PixelScreen)
{
    // 1366x768 at 100%, every size: the whole switcher, hint and all, and the window fits on screen.
    for (const auto size : kSizes)
    {
        const float s = PanelScale(size, 1.f, 768.f);
        const auto fit = Fit(s, 1366.f);
        EXPECT_TRUE(fit.hint) << static_cast<int>(size);
        EXPECT_FLOAT_EQ(fit.nameScale, 1.f);
        EXPECT_LE(fit.width + 2.f * panel::kInsetDip * s, panel::MaxContentWidth(s, 1366.f));
        const float window = panel::MaxContentWidth(s, 1366.f) + 2.f * 24.f * s;   // Island's shadow room
        EXPECT_LE(window, 1366.f + 1.f);
    }
    // At 150% and Large the hint gives way before the tabs get any smaller.
    const float s = PanelScale(WheelSize::Large, 1.5f, 768.f);
    const auto fit = Fit(s, 1366.f);
    EXPECT_FALSE(fit.hint);
    EXPECT_FLOAT_EQ(fit.nameScale, 1.f);
    EXPECT_LE(fit.width, panel::MaxContentWidth(s, 1366.f) - 2.f * panel::kInsetDip * s + 0.01f);
}

TEST(Panel, NarrowScreenGivesUpGapsThenHintThenNames)
{
    const float s = 1.4f;
    const auto names = NameWidths(s);
    const float hint = HintWidth(s);
    const auto full = panel::FitSwitcher(names, hint, s, 100000.f);
    ASSERT_TRUE(full.hint);
    EXPECT_FLOAT_EQ(full.pad, panel::kTabPadDip * s);
    EXPECT_FLOAT_EQ(full.iconGap, panel::kIconGapDip * s);
    EXPECT_FLOAT_EQ(full.width, std::accumulate(full.tabs.begin(), full.tabs.end(), 0.f) + full.hintGap + hint + full.hintEnd);

    // A little too narrow: the gaps shrink, the hint and the names stay.
    const auto tighter = panel::FitSwitcher(names, hint, s, full.width - 20.f);
    EXPECT_TRUE(tighter.hint);
    EXPECT_FLOAT_EQ(tighter.nameScale, 1.f);
    EXPECT_LT(tighter.pad, full.pad);
    EXPECT_GE(tighter.pad, panel::kTabPadMinDip * s);
    EXPECT_NEAR(tighter.width, full.width - 20.f, 0.05f);

    // Narrower than the smallest gaps allow with the hint: the hint goes, the tabs get their room back.
    const float text = std::accumulate(names.begin(), names.end(), 0.f);
    const float bare = text + 4.f * (2.f * panel::kTabPadDip + panel::kIconDip + panel::kIconGapDip) * s;
    const auto noHint = panel::FitSwitcher(names, hint, s, bare);
    EXPECT_FALSE(noHint.hint);
    EXPECT_FLOAT_EQ(noHint.nameScale, 1.f);
    EXPECT_NEAR(noHint.pad, panel::kTabPadDip * s, 0.01f);
    EXPECT_NEAR(noHint.width, bare, 0.05f);

    // Narrower still: smallest gaps, and only then the names are shortened (with "...").
    const float least = text + 4.f * (2.f * panel::kTabPadMinDip + panel::kIconDip + panel::kIconGapMinDip) * s;
    const auto cut = panel::FitSwitcher(names, hint, s, least - 40.f);
    EXPECT_FALSE(cut.hint);
    EXPECT_FLOAT_EQ(cut.pad, panel::kTabPadMinDip * s);
    EXPECT_LT(cut.nameScale, 1.f);
    EXPECT_NEAR(cut.width, least - 40.f, 0.05f);
    for (size_t i = 0; i < names.size(); ++i)
        EXPECT_GT(cut.tabs[i], (2.f * panel::kTabPadMinDip + panel::kIconDip + panel::kIconGapMinDip) * s);

    // Whatever the room, down to the bare icons with the smallest gaps, the layout never overflows it.
    for (float available = full.width + 50.f; available >= least - text; available -= 37.f)
        EXPECT_LE(panel::FitSwitcher(names, hint, s, available).width, available + 0.05f) << available;
}

TEST(Panel, NoHintNoNames)
{
    const auto fit = panel::FitSwitcher({ 50.f, 60.f }, 0.f, 1.f, 1000.f);
    EXPECT_FALSE(fit.hint);
    EXPECT_FLOAT_EQ(fit.width, 2.f * (2.f * 12.f + 18.f + 7.f) + 110.f);
    EXPECT_FLOAT_EQ(panel::FitSwitcher({}, 0.f, 1.f, 1000.f).width, 0.f);
}

// ---- settings.ini [Appearance] without HideCursor ------------------------------------------------

TEST(Panel, ApproachEasesAndLands)
{
    // One time constant closes about 63% of the gap, whatever the frame rate.
    float v = 0.f;
    panel::Approach(v, 1.f, 60.f, 60.f, 0.002f);
    EXPECT_NEAR(v, 1.f - std::exp(-1.f), 1e-6f);
    float stepped = 0.f;
    for (int i = 0; i < 4; ++i)
        panel::Approach(stepped, 1.f, 15.f, 60.f, 0.002f);
    EXPECT_NEAR(stepped, v, 1e-5f) << "four 15 ms frames go as far as one 60 ms frame";
    // Downward too, and it lands exactly once within the snap distance (so the frame timer can stop).
    float h = 300.f;
    int frames = 0;
    while (h != 120.f && frames < 1000)
    {
        panel::Approach(h, 120.f, 16.f, 60.f, 0.3f);
        ++frames;
        EXPECT_GE(h, 120.f);
    }
    EXPECT_EQ(h, 120.f);
    EXPECT_LT(frames, 40);
    // Nothing moves in no time, and a value at its target stays there.
    float still = 5.f;
    panel::Approach(still, 9.f, 0.f, 60.f, 0.f);
    EXPECT_EQ(still, 5.f);
    panel::Approach(still, 5.f, 16.f, 60.f, 0.f);
    EXPECT_EQ(still, 5.f);
}

TEST(Panel, AppearanceHasNoHideCursorSetting)
{
    static_assert(!kHasHideCursor<Appearance>, "the pointer is never hidden over the wheel");
    TempDir dir;
    const auto ini = dir / L"settings.ini";

    // A file from an older version still has the key: it is ignored, and the next save removes it.
    WriteText(ini, "[Appearance]\r\nSize=large\r\nStyle=glass\r\nHideCursor=1\r\nTheme=dark\r\n");
    auto a = LoadAppearance(ini);
    EXPECT_EQ(a.size, WheelSize::Large);
    EXPECT_EQ(a.style, Style::Glass);
    a.size = WheelSize::Small;
    SaveAppearance(a, ini);
    EXPECT_EQ(GetPrivateProfileIntW(L"Appearance", L"HideCursor", -1, ini.c_str()), -1);
    wchar_t theme[16]{};
    GetPrivateProfileStringW(L"Appearance", L"Theme", L"", theme, ARRAYSIZE(theme), ini.c_str());
    EXPECT_STREQ(theme, L"dark");   // the rest of the section is left alone
    const auto b = LoadAppearance(ini);
    EXPECT_EQ(b.size, WheelSize::Small);
    EXPECT_EQ(b.style, Style::Glass);

    // No file: Auto and Solid.
    const auto c = LoadAppearance(dir / L"missing.ini");
    EXPECT_EQ(c.size, WheelSize::Auto);
    EXPECT_EQ(c.style, Style::Solid);
}

// ---- The dragged item's picture over the wheel --------------------------------------------------

TEST(DragPicture, HelperFollowsTheWheelWithNoCopyBadge)
{
    FakeWheel wheel;
    auto helper = Microsoft::WRL::Make<FakeHelper>();
    auto target = Microsoft::WRL::Make<DropTarget>(kWheel, &wheel, helper);
    auto data = FileDrag({ L"C:\\photos\\holiday.jpg" });

    DWORD effect = DROPEFFECT_COPY | DROPEFFECT_MOVE;
    ASSERT_EQ(target->DragEnter(data.Get(), 0, POINTL{ 300, 200 }, &effect), S_OK);
    EXPECT_EQ(effect, static_cast<DWORD>(DROPEFFECT_COPY));
    EXPECT_EQ(helper->hwnd, kWheel);   // the picture is drawn over the wheel's window
    EXPECT_EQ(helper->seen, data.Get());
    EXPECT_EQ(helper->last.x, 300);
    EXPECT_EQ(helper->lastEffect, static_cast<DWORD>(DROPEFFECT_COPY));
    std::wstring message = L"x";
    EXPECT_EQ(Badge(*data.Get(), &message), DROPIMAGE_NOIMAGE);   // picture yes, "+ Copy" no
    EXPECT_EQ(message, L"");

    effect = DROPEFFECT_COPY;
    target->DragOver(0, POINTL{ 320, 210 }, &effect);
    EXPECT_EQ(helper->last.x, 320);
    EXPECT_EQ(helper->last.y, 210);
    EXPECT_EQ(Badge(*data.Get()), DROPIMAGE_NOIMAGE);

    // Off the wheel: the helper lets go and other windows get their usual badge back.
    target->DragLeave();
    EXPECT_EQ(helper->calls, L"enter over leave ");
    EXPECT_EQ(Badge(*data.Get()), DROPIMAGE_INVALID);
    EXPECT_EQ(wheel.left, 1);
}

TEST(DragPicture, DropIsHandedToTheHelperToo)
{
    FakeWheel wheel;
    auto helper = Microsoft::WRL::Make<FakeHelper>();
    auto target = Microsoft::WRL::Make<DropTarget>(kWheel, &wheel, helper);
    auto data = FileDrag({ L"C:\\a.png", L"C:\\b.png" });
    DWORD effect = DROPEFFECT_COPY;
    target->DragEnter(data.Get(), 0, POINTL{ 10, 10 }, &effect);
    effect = DROPEFFECT_COPY;
    target->Drop(data.Get(), 0, POINTL{ 40, 50 }, &effect);
    EXPECT_EQ(helper->calls, L"enter drop ");
    EXPECT_EQ(helper->last.y, 50);
    EXPECT_EQ(wheel.dropped, (std::vector<std::wstring>{ L"C:\\a.png", L"C:\\b.png" }));
}

TEST(DragPicture, BrowserDragsGetTheirPictureToo)
{
    // A picture dragged off a web page has no path; the helper still gets the drag, so the
    // browser's own picture of it shows over the wheel.
    FakeWheel wheel;
    auto helper = Microsoft::WRL::Make<FakeHelper>();
    auto target = Microsoft::WRL::Make<DropTarget>(kWheel, &wheel, helper);
    auto data = BrowserDrag(L"cat.webp");
    DWORD effect = DROPEFFECT_COPY;
    target->DragEnter(data.Get(), 0, POINTL{ 5, 5 }, &effect);
    ASSERT_EQ(wheel.entered.size(), 1u);
    EXPECT_EQ(helper->seen, data.Get());
    EXPECT_EQ(helper->hwnd, kWheel);
    EXPECT_EQ(Badge(*data.Get()), DROPIMAGE_NOIMAGE);
    target->DragLeave();
    EXPECT_EQ(Badge(*data.Get()), DROPIMAGE_INVALID);
}

TEST(DragPicture, NoHelperStillWorks)
{
    FakeWheel wheel;
    auto target = Microsoft::WRL::Make<DropTarget>(kWheel, &wheel, nullptr);
    auto data = FileDrag({ L"C:\\a.png" });
    DWORD effect = DROPEFFECT_COPY;
    EXPECT_EQ(target->DragEnter(data.Get(), 0, POINTL{ 1, 1 }, &effect), S_OK);
    EXPECT_EQ(effect, static_cast<DWORD>(DROPEFFECT_COPY));
    target->DragLeave();
    EXPECT_EQ(Badge(*data.Get()), DROPIMAGE_INVALID);
}

TEST(DragPicture, ReadsTheDragWindow)
{
    const auto format = Format(L"DragWindow");
    auto data = Microsoft::WRL::Make<FakeData>();
    EXPECT_EQ(DragImageWindow(data.Get()), nullptr);   // no picture
    const DWORD desktop = static_cast<DWORD>(reinterpret_cast<ULONG_PTR>(GetDesktopWindow()));
    data->Put(format, &desktop, sizeof(desktop));
    EXPECT_EQ(DragImageWindow(data.Get()), GetDesktopWindow());
    const DWORD gone = 0x7FFFFFF0;   // not a window
    data->Put(format, &gone, sizeof(gone));
    EXPECT_EQ(DragImageWindow(data.Get()), nullptr);
    data->Put(format, "x", 1);   // too short
    EXPECT_EQ(DragImageWindow(data.Get()), nullptr);
    EXPECT_EQ(DragImageWindow(nullptr), nullptr);
}

TEST(DragPicture, OurWindowsGoBelowThePicture)
{
    // The picture's window exists first; the wheel opens later, topmost, above it.
    const auto make = [] {
        return CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"STATIC", L"", WS_POPUP, -200, -200, 10, 10, nullptr, nullptr,
                               GetModuleHandleW(nullptr), nullptr);
    };
    const HWND picture = make();
    ASSERT_NE(picture, nullptr);
    ShowWindow(picture, SW_SHOWNOACTIVATE);
    const HWND wheel = make();
    ASSERT_NE(wheel, nullptr);
    ShowWindow(wheel, SW_SHOWNOACTIVATE);
    SetWindowPos(wheel, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

    EXPECT_EQ(KeepDragImageOnTop(picture), 1);
    EXPECT_EQ(GetWindow(picture, GW_HWNDNEXT), wheel);   // just below the picture
    EXPECT_NE(GetWindowLongPtrW(wheel, GWL_EXSTYLE) & WS_EX_TOPMOST, 0);   // and still topmost
    EXPECT_EQ(KeepDragImageOnTop(picture), 0);   // nothing left to move
    EXPECT_EQ(KeepDragImageOnTop(nullptr), 0);

    DestroyWindow(wheel);
    DestroyWindow(picture);
}
