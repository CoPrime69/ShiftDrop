#include "FileActions.h"
#include "OutputPath.h"
#include "PipelineRunner.h"
#include "StagedOutput.h"
#include "TestUtil.h"
#include "Utf.h"
#include "VirtualDrop.h"
#include "preview/Prediction.h"

#include <shlobj.h>
#include <shlwapi.h>
#include <wrl/implements.h>

#include <cstddef>
#include <cstring>

using namespace sd;
using namespace sd::test;
using Microsoft::WRL::ComPtr;

namespace
{
    // What a browser or a mail app offers for a drag: a descriptor with the names, and each file's
    // contents by index, in the medium it chooses. Nothing in it is a path.
    class FakeDrag final
        : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IDataObject>
    {
    public:
        struct File
        {
            std::wstring name;
            std::string bytes;
            DWORD tymed = TYMED_ISTREAM;
            bool fail = false;   // the source refuses to hand it over
        };

        FakeDrag(std::vector<File> files, bool realFiles = false) : m_files(std::move(files)), m_realFiles(realFiles) {}

        STDMETHODIMP GetData(FORMATETC* fmt, STGMEDIUM* medium) override
        {
            *medium = {};
            if (fmt->cfFormat == Descriptor() && (fmt->tymed & TYMED_HGLOBAL))
            {
                const size_t bytes = offsetof(FILEGROUPDESCRIPTORW, fgd) + m_files.size() * sizeof(FILEDESCRIPTORW);
                HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, bytes);
                auto* group = static_cast<FILEGROUPDESCRIPTORW*>(GlobalLock(memory));
                group->cItems = static_cast<UINT>(m_files.size());
                for (size_t i = 0; i < m_files.size(); ++i)
                {
                    auto& d = group->fgd[i];
                    d.dwFlags = FD_FILESIZE;
                    d.nFileSizeLow = static_cast<DWORD>(m_files[i].bytes.size());
                    wcsncpy_s(d.cFileName, MAX_PATH, m_files[i].name.c_str(), _TRUNCATE);
                }
                GlobalUnlock(memory);
                medium->tymed = TYMED_HGLOBAL;
                medium->hGlobal = memory;
                return S_OK;
            }
            if (fmt->cfFormat == Contents())
            {
                if (fmt->lindex < 0 || static_cast<size_t>(fmt->lindex) >= m_files.size())
                    return DV_E_LINDEX;
                const auto& f = m_files[static_cast<size_t>(fmt->lindex)];
                if (f.fail)
                    return E_FAIL;
                if (!(fmt->tymed & f.tymed))
                    return DV_E_TYMED;
                medium->tymed = f.tymed;
                if (f.tymed == TYMED_HGLOBAL)
                {
                    // Rounded up, as GlobalAlloc may: only the descriptor's size is the real one.
                    medium->hGlobal = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, f.bytes.size() + 7);
                    memcpy(GlobalLock(medium->hGlobal), f.bytes.data(), f.bytes.size());
                    GlobalUnlock(medium->hGlobal);
                    return S_OK;
                }
                if (f.tymed == TYMED_ISTREAM)
                {
                    medium->pstm = SHCreateMemStream(reinterpret_cast<const BYTE*>(f.bytes.data()), static_cast<UINT>(f.bytes.size()));
                    // Left at its end, as a source that read it first would: the reader has to seek back.
                    LARGE_INTEGER zero{};
                    medium->pstm->Seek(zero, STREAM_SEEK_END, nullptr);
                    return S_OK;
                }
                // TYMED_ISTORAGE: a .msg-like compound file with one stream holding the bytes.
                ComPtr<ILockBytes> lockBytes;
                ComPtr<IStorage> storage;
                ComPtr<IStream> stream;
                HRESULT hr = CreateILockBytesOnHGlobal(nullptr, TRUE, &lockBytes);
                if (SUCCEEDED(hr))
                    hr = StgCreateDocfileOnILockBytes(lockBytes.Get(), STGM_CREATE | STGM_READWRITE | STGM_SHARE_EXCLUSIVE, 0, &storage);
                if (SUCCEEDED(hr))
                    hr = storage->CreateStream(L"body", STGM_CREATE | STGM_WRITE | STGM_SHARE_EXCLUSIVE, 0, 0, &stream);
                if (SUCCEEDED(hr))
                    hr = stream->Write(f.bytes.data(), static_cast<ULONG>(f.bytes.size()), nullptr);
                stream.Reset();
                if (SUCCEEDED(hr))
                    hr = storage->Commit(STGC_DEFAULT);
                if (FAILED(hr))
                    return hr;
                medium->pstg = storage.Detach();
                return S_OK;
            }
            if (fmt->cfFormat == CF_HDROP && m_realFiles)
            {
                medium->tymed = TYMED_HGLOBAL;
                medium->hGlobal = MakeDropFiles({ L"C:\\real.png" }).release();
                return S_OK;
            }
            return DV_E_FORMATETC;
        }
        STDMETHODIMP QueryGetData(FORMATETC* fmt) override
        {
            const bool has = fmt->cfFormat == Descriptor() || fmt->cfFormat == Contents() || (fmt->cfFormat == CF_HDROP && m_realFiles);
            return has ? S_OK : DV_E_FORMATETC;
        }
        STDMETHODIMP GetDataHere(FORMATETC*, STGMEDIUM*) override { return E_NOTIMPL; }
        STDMETHODIMP GetCanonicalFormatEtc(FORMATETC*, FORMATETC* out) override
        {
            out->ptd = nullptr;
            return E_NOTIMPL;
        }
        STDMETHODIMP SetData(FORMATETC*, STGMEDIUM*, BOOL) override { return E_NOTIMPL; }
        STDMETHODIMP EnumFormatEtc(DWORD, IEnumFORMATETC**) override { return E_NOTIMPL; }
        STDMETHODIMP DAdvise(FORMATETC*, DWORD, IAdviseSink*, DWORD*) override { return OLE_E_ADVISENOTSUPPORTED; }
        STDMETHODIMP DUnadvise(DWORD) override { return OLE_E_ADVISENOTSUPPORTED; }
        STDMETHODIMP EnumDAdvise(IEnumSTATDATA**) override { return OLE_E_ADVISENOTSUPPORTED; }

    private:
        static CLIPFORMAT Descriptor() { return static_cast<CLIPFORMAT>(RegisterClipboardFormatW(CFSTR_FILEDESCRIPTORW)); }
        static CLIPFORMAT Contents() { return static_cast<CLIPFORMAT>(RegisterClipboardFormatW(CFSTR_FILECONTENTS)); }

        std::vector<File> m_files;
        bool m_realFiles;
    };

    ComPtr<IDataObject> MakeDrag(std::vector<FakeDrag::File> files, bool realFiles = false)
    {
        ComPtr<IDataObject> data = Microsoft::WRL::Make<FakeDrag>(std::move(files), realFiles);
        return data;
    }

    std::string PngBytes(const TempDir& dir, UINT w = 64, UINT h = 48)
    {
        const auto path = dir / L"made.png";
        EXPECT_HRESULT_SUCCEEDED(WritePng(path, w, h));
        auto bytes = ReadAll(path);
        DeleteFileW(path.c_str());
        return bytes;
    }

    bool Exists(const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }
}

TEST(VirtualDrop, SafeFileNameKeepsOneHarmlessName)
{
    EXPECT_EQ(SafeFileName(L"photo.png"), L"photo.png");
    EXPECT_EQ(SafeFileName(L"..\\..\\Windows\\evil.exe"), L"evil.exe");
    EXPECT_EQ(SafeFileName(L"C:/Users/x/report.pdf"), L"report.pdf");
    EXPECT_EQ(SafeFileName(L"a<b>c:d\"e|f?g*h.txt"), L"a_b_c_d_e_f_g_h.txt");
    EXPECT_EQ(SafeFileName(L"tab\there\x01.jpg"), L"tabhere.jpg");
    EXPECT_EQ(SafeFileName(L"trailing. . "), L"trailing");
    EXPECT_EQ(SafeFileName(L"   spaced.png"), L"spaced.png");
    EXPECT_EQ(SafeFileName(L""), L"Dropped file");
    EXPECT_EQ(SafeFileName(L".."), L"Dropped file");
    EXPECT_EQ(SafeFileName(L"folder\\"), L"Dropped file");
    // Device names, with or without an extension, in any case.
    EXPECT_EQ(SafeFileName(L"CON"), L"_CON");
    EXPECT_EQ(SafeFileName(L"nul.txt"), L"_nul.txt");
    EXPECT_EQ(SafeFileName(L"com1.png"), L"_com1.png");
    EXPECT_EQ(SafeFileName(L"LPT9"), L"_LPT9");
    EXPECT_EQ(SafeFileName(L"console.txt"), L"console.txt");
    EXPECT_EQ(SafeFileName(L"com10.txt"), L"com10.txt");
    // Long names keep their extension.
    const auto longName = SafeFileName(std::wstring(400, L'x') + L".jpeg");
    EXPECT_LE(longName.size(), 200u);
    EXPECT_TRUE(longName.ends_with(L"x.jpeg"));
}

TEST(VirtualDrop, ParsesAFileGroupDescriptor)
{
    // Three entries as an FTP client or a zip viewer might send them: a folder, a file in it, a file.
    const size_t bytes = offsetof(FILEGROUPDESCRIPTORW, fgd) + 3 * sizeof(FILEDESCRIPTORW);
    wil::unique_hglobal memory(GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, bytes));
    ASSERT_TRUE(memory);
    {
        auto* group = static_cast<FILEGROUPDESCRIPTORW*>(GlobalLock(memory.get()));
        group->cItems = 3;
        group->fgd[0].dwFlags = FD_ATTRIBUTES;
        group->fgd[0].dwFileAttributes = FILE_ATTRIBUTE_DIRECTORY;
        wcscpy_s(group->fgd[0].cFileName, MAX_PATH, L"Pictures");
        group->fgd[1].dwFlags = FD_FILESIZE | FD_ATTRIBUTES;
        group->fgd[1].dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
        group->fgd[1].nFileSizeHigh = 1;
        group->fgd[1].nFileSizeLow = 5;
        wcscpy_s(group->fgd[1].cFileName, MAX_PATH, L"Pictures\\cat.jpg");
        wcscpy_s(group->fgd[2].cFileName, MAX_PATH, L"notes.txt");   // no size given
        GlobalUnlock(memory.get());
    }
    const void* block = GlobalLock(memory.get());
    const auto items = ParseFileGroupDescriptor(block, GlobalSize(memory.get()), true);
    // A block that claims more entries than it holds is read only as far as it goes.
    const auto cut = ParseFileGroupDescriptor(block, offsetof(FILEGROUPDESCRIPTORW, fgd) + 2 * sizeof(FILEDESCRIPTORW) + 10, true);
    GlobalUnlock(memory.get());

    ASSERT_EQ(items.size(), 2u);
    EXPECT_EQ(items[0].name, L"Pictures\\cat.jpg");
    EXPECT_EQ(items[0].index, 1);   // its place in the descriptor, folders counted
    EXPECT_TRUE(items[0].hasSize);
    EXPECT_EQ(items[0].size, (1ull << 32) + 5);
    EXPECT_EQ(items[1].name, L"notes.txt");
    EXPECT_EQ(items[1].index, 2);
    EXPECT_FALSE(items[1].hasSize);
    ASSERT_EQ(cut.size(), 1u);
    EXPECT_EQ(cut[0].name, L"Pictures\\cat.jpg");
    EXPECT_TRUE(ParseFileGroupDescriptor(block, 2, true).empty());
}

TEST(VirtualDrop, ParsesTheAnsiDescriptor)
{
    std::string block(offsetof(FILEGROUPDESCRIPTORA, fgd) + sizeof(FILEDESCRIPTORA), '\0');
    auto* group = reinterpret_cast<FILEGROUPDESCRIPTORA*>(block.data());
    group->cItems = 1;
    strcpy_s(group->fgd[0].cFileName, MAX_PATH, "Invoice.pdf");
    const auto items = ParseFileGroupDescriptor(block.data(), block.size(), false);
    ASSERT_EQ(items.size(), 1u);
    EXPECT_EQ(items[0].name, L"Invoice.pdf");
}

TEST(VirtualDrop, ReadsNamesOnlyWhenThereAreNoRealFiles)
{
    const auto drag = MakeDrag({ { L"image.png", "x" }, { L"sub\\clip.mp4", "y" } });
    const auto items = ReadVirtualItems(drag.Get());
    ASSERT_EQ(items.size(), 2u);
    EXPECT_EQ(items[1].name, L"sub\\clip.mp4");
    EXPECT_EQ(items[1].size, 1u);
    EXPECT_EQ(ReadVirtualItems(MakeDrag({ { L"image.png", "x" } }, true).Get()).size(), 1u);
    EXPECT_TRUE(ReadVirtualItems(nullptr).empty());
}

TEST(VirtualDrop, RealPathsWinUnlessTheyAreTemporaryCopies)
{
    const auto temp = std::filesystem::temp_directory_path();
    EXPECT_TRUE(PreferVirtualFiles({}));
    EXPECT_FALSE(PreferVirtualFiles({ L"C:\\Photos\\real.png" }));
    EXPECT_TRUE(PreferVirtualFiles({ (temp / L"mozilla_x0\\cat.png").wstring() }));
    EXPECT_FALSE(PreferVirtualFiles({ (temp / L"cat.png").wstring(), L"C:\\Photos\\real.png" }));
}

TEST(VirtualDrop, FallsBackToThePathTheDragAlsoGave)
{
    TempDir dir;
    WriteText(dir / L"temp copy.png", "from the path");
    const auto drag = MakeDrag({ { L"cat.png", "", TYMED_ISTREAM, true } });
    auto drop = VirtualDrop::Create(ReadVirtualItems(drag.Get()), dir / L"dropped", dir / L"out", false);
    ASSERT_TRUE(drop);
    drop->Attach(drag.Get());
    drop->SetFallback({ dir / L"temp copy.png" });
    std::wstring error;
    const auto saved = drop->Save({}, error);
    drop->Detach();
    ASSERT_EQ(saved.size(), 1u) << Narrow(error);
    EXPECT_EQ(std::filesystem::path(saved[0]).filename(), L"cat.png");
    EXPECT_EQ(ReadAll(saved[0]), "from the path");
}

TEST(VirtualDrop, RedirectSendsOutputsToTheChosenFolder)
{
    TempDir dir;
    const auto drop = dir / L"dropped\\0badf00d";
    const auto desktop = dir / L"Desktop";
    std::filesystem::create_directories(drop + L"\\Dropped files");
    std::filesystem::create_directories(desktop);
    const auto source = drop + L"\\Dropped files\\a.png";
    WriteText(source, "source");
    WriteText(desktop + L"\\a (converted).png", "earlier output");

    RedirectOutputs(drop + L"\\", desktop);   // a trailing backslash is the same folder
    EXPECT_EQ(OutputFolder(source), desktop);
    EXPECT_EQ(OutputFolder(dir / L"dropped\\0badf00d2\\x.png"), dir / L"dropped\\0badf00d2");   // a sibling is not inside
    EXPECT_EQ(OutputFolder(dir / L"elsewhere\\b.png"), dir / L"elsewhere");

    std::wstring staging;
    ComPtr<IStream> stream;
    ASSERT_HRESULT_SUCCEEDED(CreateStaging(source, staging, stream));
    EXPECT_EQ(std::filesystem::path(staging).parent_path(), std::filesystem::path(desktop));
    ASSERT_HRESULT_SUCCEEDED(stream->Write("new", 3, nullptr));
    stream.Reset();
    std::wstring final;
    ASSERT_HRESULT_SUCCEEDED(CommitStaging(staging, source, L"converted", L".png", final));
    EXPECT_EQ(final, desktop + L"\\a (converted 2).png");   // never over the earlier one
    EXPECT_EQ(ReadAll(desktop + L"\\a (converted).png"), "earlier output");
    EXPECT_EQ(preview::PredictOutputName(source, L"converted", L".png"), L"a (converted 3).png");

    {
        // A folder output, and a batch output named after the drop's folder ("Dropped files (zipped).zip").
        StagedOutput frames(nullptr, 0, source, true);
        ASSERT_TRUE(frames.Ready()) << Narrow(frames.Error());
        EXPECT_EQ(std::filesystem::path(frames.Path()).parent_path(), std::filesystem::path(desktop));
        EXPECT_EQ(frames.Commit(L"frames", L"").output, desktop + L"\\a (frames)");

        OutputPlace place{ drop + L"\\Dropped files", L"Dropped files" };
        StagedOutput zip(nullptr, 0, drop + L"\\Dropped files", false, place);
        ASSERT_TRUE(zip.Ready());
        WriteText(zip.Path(), "zip");
        EXPECT_EQ(zip.Commit(L"zipped", L".zip").output, desktop + L"\\Dropped files (zipped).zip");
    }
    EXPECT_EQ(CountFiles(dir, L".sdpart"), 0u);
    for (const auto& e : std::filesystem::directory_iterator(drop + L"\\Dropped files"))
        EXPECT_EQ(e.path().filename(), L"a.png");   // nothing written next to the staging copy

    RedirectOutputs(drop, L"");
    EXPECT_EQ(OutputFolder(source), drop + L"\\Dropped files");
}

TEST(VirtualDrop, SavesEveryMediumAndConvertsIntoTheChosenFolder)
{
    TempDir dir;
    const auto root = dir / L"dropped";
    const auto desktop = dir / L"Desktop";
    std::filesystem::create_directories(desktop);
    const auto png = PngBytes(dir);

    // An image from a browser (a stream), an attachment (memory), a mail (.msg, a storage), two files
    // of the same name, and one the source refuses to hand over.
    const auto drag = MakeDrag({ { L"https_cat.png", png, TYMED_ISTREAM },
                                 { L"notes.txt", "hello", TYMED_HGLOBAL },
                                 { L"Re: lunch?.msg", "mail body", TYMED_ISTORAGE },
                                 { L"notes.txt", "second", TYMED_HGLOBAL },
                                 { L"gone.jpg", "", TYMED_ISTREAM, true } });
    auto drop = VirtualDrop::Create(ReadVirtualItems(drag.Get()), root, desktop, false);
    ASSERT_TRUE(drop);
    ASSERT_EQ(drop->Paths().size(), 5u);
    EXPECT_FALSE(Exists(drop->Folder()));   // nothing on disk during the drag
    EXPECT_EQ(std::filesystem::path(drop->Paths()[2]).filename(), L"Re_ lunch_.msg");
    EXPECT_EQ(std::filesystem::path(drop->Paths()[3]).filename(), L"notes (2).txt");
    EXPECT_EQ(VirtualDrop::Find(drop->Paths()[0]), drop);
    EXPECT_FALSE(VirtualDrop::Find(dir / L"elsewhere.png"));

    drop->Attach(drag.Get());
    std::wstring error;
    uint64_t reported = 0, reportedTotal = 0;
    const auto saved = drop->Save([&](uint64_t done, uint64_t total) { reported = done; reportedTotal = total; }, error);
    drop->Detach();
    ASSERT_EQ(saved.size(), 4u) << Narrow(error);
    EXPECT_TRUE(drop->Saved());
    EXPECT_NE(error.find(L"gone.jpg"), std::wstring::npos);
    EXPECT_FALSE(Exists(drop->Paths()[4]));
    EXPECT_EQ(ReadAll(saved[0]), png);
    EXPECT_EQ(ReadAll(saved[1]), "hello");   // the descriptor's size, not the rounded-up memory's
    EXPECT_EQ(ReadAll(saved[3]), "second");
    EXPECT_EQ(reportedTotal, png.size() + 5 + 9 + 6);
    EXPECT_GE(reported, png.size() + 5 + 6);
    {
        ComPtr<IStorage> storage;
        ComPtr<IStream> body;
        ASSERT_HRESULT_SUCCEEDED(StgOpenStorage(saved[2].c_str(), nullptr, STGM_READ | STGM_SHARE_EXCLUSIVE, nullptr, 0, &storage));
        ASSERT_HRESULT_SUCCEEDED(storage->OpenStream(L"body", nullptr, STGM_READ | STGM_SHARE_EXCLUSIVE, 0, &body));
        char text[16]{};
        ULONG read = 0;
        body->Read(text, sizeof(text), &read);
        EXPECT_EQ(std::string(text, read), "mail body");
    }
    EXPECT_EQ(drop->Save({}, error), saved);   // once only

    // The slice runs on the saved copy as on any file; the output lands on the "Desktop".
    Recipes recipes{ dir };
    std::wstring dbError;
    auto history = HistoryStore::Open(dir / L"history.db", dbError);
    PipelineRunner runner(std::make_shared<const ImageCodecs>(), history.get());
    const auto& toJpg = recipes.Get("to-jpg");
    ASSERT_TRUE(runner.Accepts(toJpg, saved[0]));
    const auto result = runner.Run(toJpg, history->BeginRun(toJpg, 1), saved[0], std::stop_token{});
    ASSERT_TRUE(result.ok) << Narrow(result.error);
    EXPECT_EQ(result.output, desktop + L"\\https_cat (converted).jpg");
    EXPECT_GT(result.size, 0u);

    // The job is over: the copies go, the output stays.
    const auto folder = drop->Folder();
    drop.reset();
    EXPECT_FALSE(Exists(folder));
    EXPECT_TRUE(Exists(result.output));
    EXPECT_EQ(OutputFolder(saved[0]), std::filesystem::path(saved[0]).parent_path().wstring());   // redirect gone
    EXPECT_FALSE(VirtualDrop::Find(saved[0]));
}

TEST(VirtualDrop, SaveNeedsTheDrag)
{
    TempDir dir;
    auto drop = VirtualDrop::Create({ VirtualItem{ L"a.png", 0, 0, false } }, dir / L"dropped", dir / L"out", true);
    ASSERT_TRUE(drop);
    EXPECT_TRUE(drop->AsksFolder());
    std::wstring error;
    EXPECT_TRUE(drop->Save({}, error).empty());
    EXPECT_FALSE(error.empty());
    EXPECT_FALSE(VirtualDrop::Create({}, dir / L"dropped", dir / L"out", false));
    // No folder to send outputs to: it has to be asked for.
    EXPECT_TRUE(VirtualDrop::Create({ VirtualItem{ L"a.png", 0, 0, false } }, dir / L"dropped", L"", false)->AsksFolder());
}

TEST(VirtualDrop, KeptCopiesNeverReplaceAnything)
{
    TempDir dir;
    const auto out = dir / L"out";
    std::filesystem::create_directories(out);
    WriteText(out + L"\\photo.png", "already here");
    WriteText(dir / L"photo.png", "dragged");
    std::wstring kept;
    ASSERT_HRESULT_SUCCEEDED(KeepVirtualFile(dir / L"photo.png", out, kept));
    EXPECT_EQ(kept, out + L"\\photo (2).png");
    EXPECT_EQ(ReadAll(kept), "dragged");
    EXPECT_EQ(ReadAll(out + L"\\photo.png"), "already here");
    EXPECT_FALSE(Exists(dir / L"photo.png"));
    EXPECT_EQ(CountFiles(dir, L".sdpart"), 0u);
}

TEST(VirtualDrop, StaleDropFoldersAreRemoved)
{
    TempDir dir;
    std::filesystem::create_directories(dir / L"1a2b3c4d\\Dropped files");
    WriteText(dir / L"1a2b3c4d\\Dropped files\\x.png", "x");
    std::filesystem::create_directories(dir / L"keep-me");
    WriteText(dir / L"notes.txt", "x");
    RemoveStaleVirtualDrops(dir.path().wstring());
    EXPECT_FALSE(Exists(dir / L"1a2b3c4d"));
    EXPECT_TRUE(Exists(dir / L"keep-me"));
    EXPECT_TRUE(Exists(dir / L"notes.txt"));
    RemoveStaleVirtualDrops(dir / L"missing");   // no folder yet: nothing to do
}

TEST(VirtualDrop, KnownFolders)
{
    EXPECT_FALSE(VirtualOutputFolder(VirtualOutput::Desktop).empty());
    EXPECT_FALSE(VirtualOutputFolder(VirtualOutput::Downloads).empty());
    EXPECT_EQ(VirtualOutputFolder(VirtualOutput::Ask), VirtualOutputFolder(VirtualOutput::Desktop));
}
