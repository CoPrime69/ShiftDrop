#include "JobQueue.h"
#include "PipelineRunner.h"
#include "TestUtil.h"
#include "officebridge/OfficeBridge.h"

#include <chrono>
#include <condition_variable>
#include <mutex>

using namespace sd;
using namespace sd::office;
using namespace sd::test;

TEST(Office, FamiliesByExtension)
{
    EXPECT_EQ(FamilyOf(L"C:\\a\\Report.DOCX"), Family::Word);
    EXPECT_EQ(FamilyOf(L"old.doc"), Family::Word);
    EXPECT_EQ(FamilyOf(L"letter.odt"), Family::Word);
    EXPECT_EQ(FamilyOf(L"budget.xlsx"), Family::Excel);
    EXPECT_EQ(FamilyOf(L"budget.xlsb"), Family::Excel);
    EXPECT_EQ(FamilyOf(L"talk.pptx"), Family::PowerPoint);
    EXPECT_EQ(FamilyOf(L"talk.ppsx"), Family::PowerPoint);
    EXPECT_EQ(FamilyOf(L"talk.odp"), Family::PowerPoint);
    EXPECT_EQ(FamilyOf(L"photo.png"), Family::None);
    EXPECT_EQ(FamilyOf(L"data.csv"), Family::None);
}

TEST(Office, BackendChoice)
{
    Apps none;
    EXPECT_EQ(Choose(none, Family::Word, "pdf"), Backend::None);

    Apps libre;
    libre.soffice = L"C:\\Program Files\\LibreOffice\\program\\soffice.exe";
    EXPECT_EQ(Choose(libre, Family::Word, "pdf"), Backend::LibreOffice);
    EXPECT_EQ(Choose(libre, Family::PowerPoint, "pdf"), Backend::LibreOffice);
    EXPECT_EQ(Choose(libre, Family::PowerPoint, "png"), Backend::None);   // slide pictures need PowerPoint
    EXPECT_EQ(Choose(libre, Family::None, "pdf"), Backend::None);

    Apps office = libre;
    office.word = office.excel = office.powerpoint = true;
    EXPECT_EQ(Choose(office, Family::Word, "pdf"), Backend::Word);
    EXPECT_EQ(Choose(office, Family::Excel, "pdf"), Backend::Excel);
    EXPECT_EQ(Choose(office, Family::PowerPoint, "pdf"), Backend::PowerPoint);
    EXPECT_EQ(Choose(office, Family::PowerPoint, "jpg"), Backend::PowerPoint);
    EXPECT_EQ(Choose(office, Family::Word, "png"), Backend::None);
    EXPECT_EQ(Choose(office, Family::Word, "docx"), Backend::None);

    Apps wordOnly = libre;
    wordOnly.word = true;
    EXPECT_EQ(Choose(wordOnly, Family::Excel, "pdf"), Backend::LibreOffice);   // falls back per family
}

TEST(Office, LibreOfficeCommandLine)
{
    EXPECT_EQ(FileUrl(L"C:\\Users\\Ann Lee\\AppData\\Local\\ShiftDrop\\libreoffice"),
              L"file:///C:/Users/Ann%20Lee/AppData/Local/ShiftDrop/libreoffice");
    EXPECT_EQ(FileUrl(L"\\\\server\\share\\x"), L"file://server/share/x");
    EXPECT_EQ(FileUrl(L"C:\\Ünï"), L"file:///C:/%C3%9Cn%C3%AF");

    EXPECT_EQ(LibreOfficeCommandLine(L"C:\\LO\\soffice.exe", L"file:///C:/p", L"C:\\Temp\\out", L"C:\\Docs\\a b.pptx"),
              L"\"C:\\LO\\soffice.exe\" --headless --invisible --norestore --nologo --nodefault --nolockcheck --nofirststartwizard "
              L"\"-env:UserInstallation=file:///C:/p\" --convert-to pdf --outdir \"C:\\Temp\\out\" \"C:\\Docs\\a b.pptx\"");
    // A trailing backslash must not escape the closing quote.
    EXPECT_NE(LibreOfficeCommandLine(L"s.exe", L"u", L"C:\\", L"x").find(L"\"C:\\\\\""), std::wstring::npos);
}

TEST(Office, SlideNamesAndSizes)
{
    EXPECT_EQ(SlideFileName(3, 9, L".png"), L"Slide 03.png");
    EXPECT_EQ(SlideFileName(7, 120, L".jpg"), L"Slide 007.jpg");
    const auto wide = SlidePixels(960, 540);
    EXPECT_EQ(wide.cx, 1920);
    EXPECT_EQ(wide.cy, 1080);
    const auto classic = SlidePixels(720, 540);
    EXPECT_EQ(classic.cx, 1920);
    EXPECT_EQ(classic.cy, 1440);
    const auto portrait = SlidePixels(540, 720);
    EXPECT_EQ(portrait.cx, 1440);
    EXPECT_EQ(portrait.cy, 1920);
    EXPECT_EQ(SlidePixels(0, 0).cx, 1920);
}

TEST(Office, SlicesOnlyWhenAnAppIsInstalled)
{
    TempDir dir;
    Recipes recipes(dir);
    PipelineRunner runner(std::make_shared<const ImageCodecs>(), nullptr);
    const auto apps = DetectApps();
    const auto deck = dir / L"talk.pptx";
    WriteText(deck, "not really a deck");
    EXPECT_EQ(runner.Applies(recipes.Get("office-to-pdf"), deck), Choose(apps, Family::PowerPoint, "pdf") != Backend::None);
    EXPECT_EQ(runner.Applies(recipes.Get("office-to-png"), deck), apps.powerpoint);
    EXPECT_FALSE(runner.Applies(recipes.Get("office-to-pdf"), dir / L"photo.png"));
    EXPECT_FALSE(runner.Applies(recipes.Get("office-to-jpg"), dir / L"report.docx"));
    const auto* engine = runner.EngineFor(recipes.Get("office-to-pdf"));
    ASSERT_NE(engine, nullptr);
    EXPECT_EQ(engine->Lane(recipes.Get("office-to-pdf")), JobLane::Office);
}

namespace
{
    // Stands in for an office export that runs until the test lets it finish (or the lane stops).
    class BlockedExport final : public IEngine
    {
    public:
        bool Handles(const Recipe&) const override { return true; }
        bool Applies(const Recipe&, const std::wstring&) const override { return true; }
        bool Accepts(const Recipe&, const std::wstring&, std::wstring*) const override { return true; }
        JobLane Lane(const Recipe&) const override { return JobLane::Office; }

        ItemResult Run(const Recipe&, int64_t, const std::wstring&, std::stop_token stop, const ProgressFn&) const override
        {
            std::unique_lock lock(m_mutex);
            m_started = true;
            m_cv.notify_all();
            m_cv.wait(lock, stop, [&] { return m_release; });
            m_finished = !stop.stop_requested();
            m_cv.notify_all();
            return ItemResult{ true };
        }

        bool WaitStarted()
        {
            std::unique_lock lock(m_mutex);
            return m_cv.wait_for(lock, std::chrono::seconds(10), [&] { return m_started; });
        }
        bool Finished() const
        {
            std::scoped_lock lock(m_mutex);
            return m_finished;
        }
        void Release()
        {
            std::scoped_lock lock(m_mutex);
            m_release = true;
            m_cv.notify_all();
        }
        bool WaitFinished()
        {
            std::unique_lock lock(m_mutex);
            return m_cv.wait_for(lock, std::chrono::seconds(10), [&] { return m_finished; });
        }

    private:
        mutable std::mutex m_mutex;
        mutable std::condition_variable_any m_cv;
        mutable bool m_started = false, m_release = false, m_finished = false;
    };
}

TEST(Office, MovesDoNotWaitBehindAnExport)
{
    EXPECT_EQ(JobLanes::Threads(JobLane::Office, 4), 1u);
    EXPECT_EQ(JobLanes::Threads(JobLane::Shell, 4), 1u);
    EXPECT_EQ(JobLanes::Threads(JobLane::Pool, 4), 4u);

    JobLanes lanes(2);
    EXPECT_NE(&lanes.For(JobLane::Office), &lanes.For(JobLane::Shell));
    BlockedExport exporter;
    const Recipe recipe;
    lanes.For(exporter.Lane(recipe)).Submit([&](std::stop_token stop) { exporter.Run(recipe, 0, L"talk.pptx", stop, {}); });
    ASSERT_TRUE(exporter.WaitStarted());

    // A move on the shell lane runs to the end while the export is still busy.
    std::mutex m;
    std::condition_variable cv;
    bool moved = false, sta = false;
    lanes.For(JobLane::Shell).Submit([&](std::stop_token) {
        APTTYPE type{};
        APTTYPEQUALIFIER qualifier{};
        sta = SUCCEEDED(CoGetApartmentType(&type, &qualifier)) && type == APTTYPE_STA;
        std::scoped_lock lock(m);
        moved = true;
        cv.notify_all();
    });
    {
        std::unique_lock lock(m);
        EXPECT_TRUE(cv.wait_for(lock, std::chrono::seconds(10), [&] { return moved; }));
    }
    EXPECT_TRUE(sta);
    EXPECT_FALSE(exporter.Finished());
    exporter.Release();
    EXPECT_TRUE(exporter.WaitFinished());
}
