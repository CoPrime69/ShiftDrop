#include "FileActions.h"
#include "TestUtil.h"
#include "Utf.h"
#include "shelf/Carry.h"
#include "shelf/ShelfGeometry.h"
#include "shelf/ShelfStore.h"

#include <shellapi.h>
#include <shlobj.h>

using namespace sd;
using namespace sd::shelf;
using namespace sd::test;

namespace
{
    std::vector<std::wstring> FilesIn(HGLOBAL block)
    {
        std::vector<std::wstring> files;
        auto* locked = GlobalLock(block);
        const auto drop = static_cast<HDROP>(locked);
        const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        for (UINT i = 0; i < count; ++i)
        {
            std::wstring path(DragQueryFileW(drop, i, nullptr, 0), L'\0');
            DragQueryFileW(drop, i, path.data(), static_cast<UINT>(path.size() + 1));
            files.push_back(path);
        }
        GlobalUnlock(block);
        return files;
    }

    const Box kWork{ 0, 0, 1920, 1040 };   // a 1080p screen with the taskbar at the bottom
}

// ---- Saved shelf ---------------------------------------------------------------------------------

TEST(Shelf, JsonRoundTrip)
{
    SavedShelf a;
    a.items = { L"C:\\Users\\me\\Pictures\\b\u00e4r (converted).png", L"D:\\clips\\\u65e5\u672c.mp4", L"C:\\folder" };
    a.edge = Edge::Left;
    a.x = -1200;   // a monitor left of the primary one
    a.y = 340;
    SavedShelf b;
    ASSERT_TRUE(FromJson(ToJson(a), b));
    EXPECT_EQ(b.items, a.items);
    EXPECT_EQ(b.edge, Edge::Left);
    ASSERT_TRUE(b.x && b.y);
    EXPECT_EQ(*b.x, -1200);
    EXPECT_EQ(*b.y, 340);

    // No position yet (never shown): none comes back.
    SavedShelf c;
    c.items = { L"C:\\a.txt" };
    SavedShelf d;
    ASSERT_TRUE(FromJson(ToJson(c), d));
    EXPECT_FALSE(d.x.has_value());
    EXPECT_EQ(d.edge, Edge::Right);
}

TEST(Shelf, JsonIsForgiving)
{
    SavedShelf s;
    // Unknown keys, entries that are not paths and an unknown edge: the rest still loads.
    ASSERT_TRUE(FromJson(R"({ "version": 7, "items": [ "C:\\a.png", 3, "", null, "C:\\b.png" ], "edge": "middle", "future": true })", s));
    EXPECT_EQ(s.items, (std::vector<std::wstring>{ L"C:\\a.png", L"C:\\b.png" }));
    EXPECT_EQ(s.edge, Edge::None);
    // Not a shelf file at all: refused, and the shelf passed in is left alone.
    SavedShelf keep;
    keep.items = { L"C:\\x" };
    EXPECT_FALSE(FromJson("not json", keep));
    EXPECT_FALSE(FromJson("[1, 2]", keep));
    EXPECT_EQ(keep.items.size(), 1u);
}

TEST(Shelf, SaveAndLoad)
{
    TempDir dir;
    const auto path = dir / L"state\\shelf.json";   // the folder is created on demand
    EXPECT_TRUE(Load(path).items.empty());          // first run: nothing yet

    SavedShelf a;
    a.items = { dir / L"one.png", dir / L"two.txt" };
    a.edge = Edge::Bottom;
    a.x = 10;
    a.y = 20;
    ASSERT_TRUE(Save(path, a));
    a.items.pop_back();
    ASSERT_TRUE(Save(path, a));   // replaces our own state file
    const auto b = Load(path);
    EXPECT_EQ(b.items, a.items);
    EXPECT_EQ(b.edge, Edge::Bottom);
    EXPECT_FALSE(std::filesystem::exists(path + L".tmp"));

    WriteText(path, "{ broken");
    EXPECT_TRUE(Load(path).items.empty());
}

// ---- Items and selection ---------------------------------------------------------------------------

TEST(Shelf, AddPutsNewestOnTopWithoutDuplicates)
{
    std::vector<std::wstring> items = { L"C:\\a.png", L"C:\\b.png" };
    // c is new; B.PNG (other case) and c:/a.png (other slash) are already there and move up.
    EXPECT_EQ(AddToTop(items, { L"C:\\c.png", L"C:\\B.PNG", L"c:/a.png", L"C:\\c.png" }), 1u);
    ASSERT_EQ(items.size(), 3u);
    EXPECT_EQ(items[0], L"C:\\c.png");
    EXPECT_EQ(items[1], L"C:\\B.PNG");
    EXPECT_EQ(items[2], L"c:/a.png");

    std::vector<std::wstring> many;
    for (int i = 0; i < 10; ++i)
        AddToTop(many, { std::format(L"C:\\{}.txt", i) }, 4);
    ASSERT_EQ(many.size(), 4u);   // the oldest fell off the end
    EXPECT_EQ(many.front(), L"C:\\9.txt");
    EXPECT_EQ(many.back(), L"C:\\6.txt");

    EXPECT_TRUE(SamePath(L"C:\\Dir\\File.TXT", L"c:/dir/file.txt"));
    EXPECT_FALSE(SamePath(L"C:\\dir\\file.txt", L"C:\\dir\\file.txt2"));
}

TEST(Shelf, RemoveDropsWhatIsGone)
{
    std::vector<std::wstring> items = { L"keep1", L"gone1", L"keep2", L"gone2" };
    EXPECT_EQ(RemoveIf(items, [](const std::wstring& p) { return p.starts_with(L"gone"); }), 2u);
    EXPECT_EQ(items, (std::vector<std::wstring>{ L"keep1", L"keep2" }));
}

TEST(Shelf, SelectionFollowsExplorerClicks)
{
    Selection s;
    s.Resize(6);
    EXPECT_EQ(s.HandleSet().size(), 6u);   // nothing selected: the handle carries everything
    s.Click(1, false, false);
    s.Click(3, false, true);                // Shift: 1..3
    EXPECT_EQ(s.Indices(), (std::vector<size_t>{ 1, 2, 3 }));
    s.Click(5, true, false);                // Ctrl adds 5 and moves the anchor there
    EXPECT_EQ(s.Count(), 4u);
    s.Click(4, true, true);                 // Ctrl+Shift adds 4..5 to what is there
    EXPECT_EQ(s.Indices(), (std::vector<size_t>{ 1, 2, 3, 4, 5 }));
    s.Click(2, true, false);                // Ctrl toggles 2 off
    EXPECT_FALSE(s.IsSelected(2));
    s.Click(0, false, false);               // a plain click: only that one
    EXPECT_EQ(s.Indices(), (std::vector<size_t>{ 0 }));

    // A drag from a selected item carries the selection; from another item, just that item.
    s.Click(2, true, false);
    EXPECT_EQ(s.DragSet(2), (std::vector<size_t>{ 0, 2 }));
    EXPECT_EQ(s.DragSet(4), (std::vector<size_t>{ 4 }));
    EXPECT_EQ(s.HandleSet(), (std::vector<size_t>{ 0, 2 }));

    // Removing item 1 shifts the flags after it.
    s.Erase(1);
    EXPECT_EQ(s.Indices(), (std::vector<size_t>{ 0, 1 }));
    s.SelectAll();
    EXPECT_EQ(s.Count(), 5u);
    s.Clear();
    EXPECT_EQ(s.Count(), 0u);
}

TEST(Shelf, Texts)
{
    EXPECT_EQ(SizeText(12), L"12 bytes");
    EXPECT_EQ(SizeText(820 * 1024), L"820 KB");
    EXPECT_EQ(SizeText(1468006), L"1.4 MB");
    EXPECT_EQ(SizeText(250ull * 1024 * 1024), L"250 MB");
    EXPECT_EQ(SizeText(3ull * 1024 * 1024 * 1024), L"3.0 GB");
    EXPECT_EQ(CountText(1), L"1 item");
    EXPECT_EQ(CountText(3), L"3 items");
}

// ---- Edges -----------------------------------------------------------------------------------------

TEST(Shelf, SnapsToTheNearestEdgeWithinReach)
{
    const float reach = 32;
    EXPECT_EQ(SnapEdge(Box{ 20, 300, 292, 600 }, kWork, reach), Edge::Left);
    EXPECT_EQ(SnapEdge(Box{ -50, 300, 222, 600 }, kWork, reach), Edge::Left);      // past the edge counts
    EXPECT_EQ(SnapEdge(Box{ 1630, 300, 1902, 600 }, kWork, reach), Edge::Right);
    EXPECT_EQ(SnapEdge(Box{ 800, 10, 1072, 300 }, kWork, reach), Edge::Top);
    EXPECT_EQ(SnapEdge(Box{ 800, 700, 1072, 1030 }, kWork, reach), Edge::Bottom);
    EXPECT_EQ(SnapEdge(Box{ 800, 300, 1072, 600 }, kWork, reach), Edge::None);      // the middle: floats
    EXPECT_EQ(SnapEdge(Box{ 40, 300, 312, 600 }, kWork, reach), Edge::None);        // just out of reach
    // A corner: the closer edge wins; on a tie, the side.
    EXPECT_EQ(SnapEdge(Box{ 5, 20, 277, 300 }, kWork, reach), Edge::Left);
    EXPECT_EQ(SnapEdge(Box{ 25, 3, 297, 300 }, kWork, reach), Edge::Top);
    EXPECT_EQ(SnapEdge(Box{ 0, 0, 272, 300 }, kWork, reach), Edge::Left);
    // A monitor to the left of the primary one has negative coordinates.
    const Box left{ -1920, 0, 0, 1080 };
    EXPECT_EQ(SnapEdge(Box{ -280, 200, -8, 500 }, left, reach), Edge::Right);
}

TEST(Shelf, DocksFlushAndStaysOnScreen)
{
    const Box panel{ 1700, 900, 1972, 1200 };   // off the bottom right
    const Box right = Dock(panel, kWork, Edge::Right);
    EXPECT_EQ(right.right, kWork.right);
    EXPECT_EQ(right.bottom, kWork.bottom);       // pulled back up into the work area
    EXPECT_EQ(right.Width(), panel.Width());
    EXPECT_EQ(right.Height(), panel.Height());

    const Box left = Dock(Box{ 300, 200, 572, 500 }, kWork, Edge::Left);
    EXPECT_EQ(left, (Box{ 0, 200, 272, 500 }));  // keeps its height on the screen
    const Box bottom = Dock(Box{ 300, 200, 572, 500 }, kWork, Edge::Bottom);
    EXPECT_EQ(bottom, (Box{ 300, 740, 572, 1040 }));
    const Box floating = Dock(Box{ -40, 200, 232, 500 }, kWork, Edge::None);
    EXPECT_EQ(floating.left, 0.f);

    // Larger than the screen: the top-left corner stays visible.
    const Box huge = ClampInto(Box{ 100, 100, 2100, 1200 }, kWork);
    EXPECT_EQ(huge.left, 0.f);
    EXPECT_EQ(huge.top, 0.f);
}

TEST(Shelf, FoldsIntoATabOnItsEdge)
{
    const Box panel{ 1648, 300, 1920, 600 };
    const Box tab = Tab(panel, kWork, Edge::Right, 22, 84);
    EXPECT_EQ(tab, (Box{ 1898, 408, 1920, 492 }));   // flush right, centred on the panel
    const Box top = Tab(Box{ 400, 0, 672, 300 }, kWork, Edge::Top, 22, 84);
    EXPECT_EQ(top, (Box{ 494, 0, 578, 22 }));
    // Near a corner the tab is kept on the screen.
    const Box low = Tab(Box{ 0, 1000, 272, 1060 }, kWork, Edge::Left, 22, 84);
    EXPECT_EQ(low.bottom, kWork.bottom);
    EXPECT_EQ(low.left, 0.f);
    EXPECT_EQ(Tab(panel, kWork, Edge::None, 22, 84), panel);

    float dx = 0, dy = 0;
    Outward(Edge::Left, dx, dy);
    EXPECT_EQ(dx, -1.f);
    Outward(Edge::Bottom, dx, dy);
    EXPECT_EQ(dy, 1.f);
    EXPECT_EQ(EdgeFromName(EdgeName(Edge::Top)), Edge::Top);
}

// ---- Layout and hit-testing ------------------------------------------------------------------------

TEST(Shelf, LayoutShowsAtMostFiveRowsAndScrolls)
{
    const Metrics m(1.f);
    EXPECT_EQ(PanelHeight(m, 0), PanelHeight(m, 1));   // never shorter than one row
    EXPECT_EQ(PanelHeight(m, 5), PanelHeight(m, 9));   // more than five scroll
    EXPECT_EQ(PanelHeight(m, 2) - PanelHeight(m, 1), m.row);
    EXPECT_EQ(MaxScroll(m, 5), 0.f);
    EXPECT_EQ(MaxScroll(m, 8), 3 * m.row);

    const Box panel{ 100, 100, 100 + m.width, 100 + PanelHeight(m, 8) };
    const auto top = LayOut(panel, m, 8, 0);
    ASSERT_EQ(top.rows.size(), 5u);
    EXPECT_EQ(top.rows.front().index, 0u);
    // Halfway down a row: six rows are partly visible, the first starting above the list.
    const auto mid = LayOut(panel, m, 8, m.row * 1.5f);
    ASSERT_EQ(mid.rows.size(), 6u);
    EXPECT_EQ(mid.rows.front().index, 1u);
    EXPECT_LT(mid.rows.front().row.top, mid.list.top);
    // Scrolling past the end is clamped.
    const auto end = LayOut(panel, m, 8, 1e6f);
    EXPECT_EQ(end.rows.back().index, 7u);
    EXPECT_EQ(end.rows.back().row.bottom, end.list.bottom);

    // Twice the DPI, twice the size.
    const Metrics m2(2.f);
    EXPECT_EQ(m2.width, 2 * m.width);
    EXPECT_EQ(PanelHeight(m2, 3), 2 * PanelHeight(m, 3));
}

TEST(Shelf, HitTestFindsEachPart)
{
    const Metrics m(1.5f);   // 150%
    const Box panel{ 0, 0, m.width, PanelHeight(m, 8) };
    const auto l = LayOut(panel, m, 8, m.row * 0.5f);
    const auto centre = [](const Box& b) { return std::pair{ (b.left + b.right) / 2, (b.top + b.bottom) / 2 }; };
    const auto at = [&](const Box& b) {
        const auto [x, y] = centre(b);
        return HitTest(l, x, y);
    };
    EXPECT_EQ(at(l.pin), (Hit{ Part::Pin }));
    EXPECT_EQ(at(l.clear), (Hit{ Part::Clear }));
    EXPECT_EQ(at(l.title), (Hit{ Part::Header }));
    EXPECT_EQ(at(l.footer), (Hit{ Part::Footer }));
    const auto& row = l.rows[1];
    EXPECT_EQ(at(row.thumb), (Hit{ Part::Item, row.index }));
    EXPECT_EQ(at(row.name), (Hit{ Part::Item, row.index }));
    EXPECT_EQ(at(row.reveal), (Hit{ Part::Reveal, row.index }));
    EXPECT_EQ(at(row.remove), (Hit{ Part::Remove, row.index }));
    // The part of row 0 scrolled up under the header is the header, not the row.
    EXPECT_LT(l.rows[0].row.top, l.list.top);
    EXPECT_EQ(HitTest(l, l.rows[0].thumb.left + 1, l.list.top - 2), (Hit{ Part::Header }));
    EXPECT_EQ(HitTest(l, l.rows[0].thumb.left + 1, l.list.top + 2), (Hit{ Part::Item, 0 }));
    EXPECT_EQ(HitTest(l, -5, 50), (Hit{}));   // outside
    // Buttons never overlap the name.
    EXPECT_LE(row.name.right, row.reveal.left);
    EXPECT_LE(row.reveal.right, row.remove.left);
}

// ---- Carry -----------------------------------------------------------------------------------------

TEST(Shelf, DropFilesBlockListsEveryPath)
{
    const std::vector<std::wstring> files = { L"C:\\a b\\one.png", L"D:\\\u00fcber\\two.mp4" };
    const auto block = MakeDropFiles(files);
    ASSERT_TRUE(block);
    EXPECT_EQ(FilesIn(block.get()), files);
}

namespace
{
    std::vector<std::wstring> DraggedFiles(const std::vector<std::wstring>& files)
    {
        const auto data = MakeFilesDataObject(files);
        if (!data)
            return {};
        FORMATETC fmt{ CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
        STGMEDIUM medium{};
        if (data->GetData(&fmt, &medium) != S_OK)
            return {};
        auto got = FilesIn(medium.hGlobal);
        ReleaseStgMedium(&medium);
        return got;
    }

    void ExpectSamePaths(const std::vector<std::wstring>& got, const std::vector<std::wstring>& want)
    {
        ASSERT_EQ(got.size(), want.size());
        for (size_t i = 0; i < got.size(); ++i)
            EXPECT_TRUE(SamePath(got[i], want[i])) << Narrow(got[i]);
    }
}

TEST(Shelf, DragDataCarriesTheFiles)
{
    TempDir dir;
    std::filesystem::create_directories(dir.path() / L"sub");
    const std::vector<std::wstring> together = { dir / L"one.txt", dir / L"two.txt" };
    const std::vector<std::wstring> apart = { dir / L"one.txt", dir / L"sub\\three.txt" };
    for (const auto& f : { together[0], together[1], apart[1] })
        WriteText(f, "x");

    ExpectSamePaths(DraggedFiles(together), together);
    // Files from two folders. Wine has no SHCreateDataObject and its desktop data object names such
    // files wrongly, so there only the count is checked; Windows (CI) checks the paths.
    Microsoft::WRL::ComPtr<IDataObject> probe;
    if (SHCreateDataObject(nullptr, 0, nullptr, nullptr, IID_PPV_ARGS(&probe)) == E_NOTIMPL)
        EXPECT_EQ(DraggedFiles(apart).size(), 2u);
    else
        ExpectSamePaths(DraggedFiles(apart), apart);
    // Nothing that exists: no data object.
    EXPECT_FALSE(MakeFilesDataObject({}));
    EXPECT_FALSE(MakeFilesDataObject({ dir / L"nope\\missing.txt" }));
}

TEST(Shelf, ClipboardPictureIsFlattenedOnWhite)
{
    TempDir dir;
    const auto png = dir / L"pic.png";
    ASSERT_EQ(WritePng(png, 8, 4), S_OK);   // left half transparent
    EXPECT_TRUE(IsClipboardImage(png));
    EXPECT_TRUE(IsClipboardImage(L"C:\\x.JPEG"));
    EXPECT_FALSE(IsClipboardImage(L"C:\\x.mp4"));
    ClipboardImage image;
    std::wstring error;
    ASSERT_EQ(MakeClipboardImage(png, image, error), S_OK) << Narrow(error);
    EXPECT_EQ(image.width, 8u);
    EXPECT_EQ(image.height, 4u);
    ASSERT_GT(image.png.size(), 8u);
    EXPECT_EQ(image.png[1], 'P');   // \x89PNG
    ASSERT_EQ(image.dib.size(), sizeof(BITMAPINFOHEADER) + 8u * 4u * 4u);
    BITMAPINFOHEADER header{};
    memcpy(&header, image.dib.data(), sizeof(header));
    EXPECT_EQ(header.biWidth, 8);
    EXPECT_EQ(header.biHeight, 4);   // bottom-up
    EXPECT_EQ(header.biBitCount, 32);
    const uint8_t* px = image.dib.data() + sizeof(header);
    // A transparent pixel (left half) is white; an opaque one keeps its colour (red channel 128).
    EXPECT_EQ(px[0], 255);
    EXPECT_EQ(px[1], 255);
    EXPECT_EQ(px[2], 255);
    EXPECT_EQ(px[3], 255);
    EXPECT_EQ(px[7 * 4 + 2], 128);

    EXPECT_TRUE(FAILED(MakeClipboardImage(dir / L"missing.png", image, error)));
}

// ---- The Float slice -------------------------------------------------------------------------------

TEST(Shelf, FloatIsAFileActionOnTheAltWheel)
{
    TempDir dir;
    Recipes recipes(dir);
    const auto& f = recipes.Get("float");
    EXPECT_EQ(f.ring, "files");
    EXPECT_TRUE(f.IsFileAction());
    EXPECT_EQ(f.icon, 0xE718);
    EXPECT_NE(recipes.registry.Find("file.float"), nullptr);
}

TEST(Shelf, InstantActionReachesTheShelf)
{
    std::vector<std::wstring> got;
    SetInstantFileAction("file.float", [&](const std::vector<std::wstring>& files, HWND) {
        got = files;
        return FileActionResult{ true, L"on the shelf" };
    });
    const auto r = RunInstantFileAction("file.float", { L"C:\\a.png" }, nullptr);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.message, L"on the shelf");
    EXPECT_EQ(got.size(), 1u);
    SetInstantFileAction("file.float", nullptr);   // the shelf is gone: the slice says so
    EXPECT_FALSE(RunInstantFileAction("file.float", { L"C:\\a.png" }, nullptr).ok);
}
