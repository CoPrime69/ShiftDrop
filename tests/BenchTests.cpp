// Speed benchmark (docs/ARCHITECTURE.md, "Speed"): times every main path on typical files it makes
// itself, through the same runner the app uses, plus the app's own overhead (the probe before each
// ffmpeg run, starting ffmpeg, the H.264 encoder test, the wheel's predictions).
//
// Skipped unless SHIFTDROP_BENCH=1, so the normal test run stays quick. Run it with
//   set SHIFTDROP_BENCH=1
//   shiftdrop_tests --gtest_filter=Bench.*
// The table is printed at the end; with SHIFTDROP_BENCH_OUT=<file> it is also written there as
// Markdown (CI adds it to the job summary). Video and audio rows need SHIFTDROP_FFMPEG.
#include "HistoryStore.h"
#include "JobQueue.h"
#include "PipelineRunner.h"
#include "PdfTestUtil.h"
#include "TestUtil.h"
#include "Utf.h"
#include "inspect/MetadataReader.h"
#include "pdf/ImagePdf.h"
#include "pdf/Wic.h"
#include "preview/Predictor.h"
#include "sandbox/Sandbox.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <mutex>
#include <optional>
#include <thread>

#include <webp/decode.h>
#include <webp/encode.h>

using namespace sd;
using namespace sd::test;

namespace
{
    bool Enabled()
    {
        wchar_t v[8]{};
        return GetEnvironmentVariableW(L"SHIFTDROP_BENCH", v, 8) > 0 && v[0] == L'1';
    }

#define SD_BENCH_ONLY()                                                  \
    if (!Enabled())                                                      \
        GTEST_SKIP() << "set SHIFTDROP_BENCH=1 to run the speed benchmark"

    struct Row
    {
        std::string area, task;
        double ms = -1;        // -1: not run (see note)
        std::string note;      // output size, encoder, why it did not run
    };

    std::mutex g_rowsMutex;
    std::vector<Row> g_rows;

    void Record(std::string area, std::string task, double ms, std::string note = {})
    {
        std::scoped_lock lock(g_rowsMutex);
        g_rows.push_back({ std::move(area), std::move(task), ms, std::move(note) });
    }

    std::string Time(double ms)
    {
        if (ms < 0)
            return "-";
        return ms < 10000 ? std::format("{:.0f} ms", ms) : std::format("{:.1f} s", ms / 1000);
    }

    std::string Bytes(uint64_t n)
    {
        if (n >= 1024 * 1024)
            return std::format("{:.1f} MB", static_cast<double>(n) / (1024.0 * 1024.0));
        return std::format("{:.0f} KB", static_cast<double>(n) / 1024.0);
    }

    double Ms(std::chrono::steady_clock::time_point from)
    {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - from).count();
    }

    // Runs `fn` on an MTA thread, as the app's workers do (WinRT PDF and OCR calls block, which the
    // tests' STA main thread must not do), and returns how long it took.
    double TimeOnMta(const std::function<void()>& fn)
    {
        double ms = 0;
        RunMta([&] {
            const auto start = std::chrono::steady_clock::now();
            fn();
            ms = Ms(start);
        });
        return ms;
    }

    // Prints the table when the whole run ends (and writes it as Markdown to SHIFTDROP_BENCH_OUT).
    class Table : public ::testing::Environment
    {
    public:
        void TearDown() override
        {
            if (g_rows.empty())
                return;
            std::string text = std::format("\nShiftDrop speed benchmark ({} logical processors)\n", std::thread::hardware_concurrency());
            std::string md = std::format("| Area | Task | Time | Notes |\n|---|---|---:|---|\n");
            for (const auto& r : g_rows)
            {
                text += std::format("  {:<9} {:<44} {:>9}  {}\n", r.area, r.task, Time(r.ms), r.note);
                md += std::format("| {} | {} | {} | {} |\n", r.area, r.task, Time(r.ms), r.note);
            }
            std::fputs(text.c_str(), stdout);
            std::fflush(stdout);
            wchar_t out[MAX_PATH]{};
            if (const DWORD n = GetEnvironmentVariableW(L"SHIFTDROP_BENCH_OUT", out, MAX_PATH); n > 0 && n < MAX_PATH)
                std::ofstream(std::filesystem::path(out), std::ios::binary | std::ios::app)
                    << std::format("### ShiftDrop speed benchmark ({} logical processors)\n\n", std::thread::hardware_concurrency()) << md << "\n";
        }
    };
    const auto* g_table = ::testing::AddGlobalTestEnvironment(new Table);

    std::shared_ptr<const FfmpegEngine> Ffmpeg()
    {
        static const auto engine = std::make_shared<const FfmpegEngine>(FfmpegEngine::Locate());
        return engine;
    }

    // ffmpeg as a tool for making inputs (not timed).
    bool Make(const std::vector<std::wstring>& args)
    {
        std::vector<std::wstring> all{ L"-hide_banner", L"-nostdin", L"-loglevel", L"error", L"-y" };
        all.insert(all.end(), args.begin(), args.end());
        std::stop_source never;
        return Ffmpeg()->Run(all, nullptr, never.get_token()).ok;
    }

    // A 4000 x 3000 "photo": smooth colour fields with fine grain, saved as a quality-90 JPEG
    // (about what a 12 MP phone camera writes).
    HRESULT WritePhoto(const std::wstring& path, UINT w, UINT h)
    {
        pdf::Bitmap b;
        b.width = w;
        b.height = h;
        b.bgra.resize(static_cast<size_t>(w) * h * 4);
        std::vector<int> cx(static_cast<size_t>(w) * 3), cy(static_cast<size_t>(h) * 3);
        for (UINT x = 0; x < w; ++x)
            for (int c = 0; c < 3; ++c)
                cx[x * 3 + c] = static_cast<int>(60 * std::sin(static_cast<double>(x) / w * (5 + 2 * c) + c));
        for (UINT y = 0; y < h; ++y)
            for (int c = 0; c < 3; ++c)
                cy[y * 3 + c] = static_cast<int>(40 * std::cos(static_cast<double>(y) / h * (3 + 3 * c) - c));
        uint32_t seed = 7;
        for (UINT y = 0; y < h; ++y)
            for (UINT x = 0; x < w; ++x)
            {
                seed = seed * 1664525u + 1013904223u;
                const int grain = static_cast<int>(seed >> 27) - 16;
                uint8_t* p = &b.bgra[(static_cast<size_t>(y) * w + x) * 4];
                for (int c = 0; c < 3; ++c)
                    p[c] = static_cast<uint8_t>(std::clamp(120 + cx[x * 3 + c] + cy[y * 3 + c] + grain, 0, 255));
                p[3] = 255;
            }
        return pdf::EncodeImageFile(b, path, true, 90, 72);
    }

    // A scanned A4 page at 300 dpi: off-white paper with a little grain and rows of dark "text".
    HRESULT WritePage(const std::wstring& path, UINT w, UINT h)
    {
        pdf::Bitmap b;
        b.width = w;
        b.height = h;
        b.bgra.resize(static_cast<size_t>(w) * h * 4);
        uint32_t seed = 11;
        for (UINT y = 0; y < h; ++y)
            for (UINT x = 0; x < w; ++x)
            {
                seed = seed * 1664525u + 1013904223u;
                uint8_t* p = &b.bgra[(static_cast<size_t>(y) * w + x) * 4];
                const bool margin = x < 250 || x > w - 250 || y < 300 || y > h - 300;
                const bool textRow = (y / 50) % 2 == 0 && (y % 50) > 8 && (y % 50) < 40;
                const bool word = ((x / 22 + y / 50 * 7) % 9) < 7;
                const int v = !margin && textRow && word && (seed >> 28) < 10 ? 40 : 242 + static_cast<int>(seed >> 30);
                p[0] = p[1] = p[2] = static_cast<uint8_t>(v);
                p[3] = 255;
            }
        return pdf::EncodeImageFile(b, path, true, 85, 300);
    }

    // A 2560 x 1440 screenshot: white page, coloured panels and rows of dark "text".
    HRESULT WriteScreenshot(const std::wstring& path, UINT w, UINT h)
    {
        pdf::Bitmap b;
        b.width = w;
        b.height = h;
        b.bgra.assign(static_cast<size_t>(w) * h * 4, 255);
        uint32_t seed = 3;
        for (UINT y = 0; y < h; ++y)
            for (UINT x = 0; x < w; ++x)
            {
                uint8_t* p = &b.bgra[(static_cast<size_t>(y) * w + x) * 4];
                if (y < 60 || x < 280)
                {
                    p[0] = 240, p[1] = 230, p[2] = 225;   // title bar and side panel
                    continue;
                }
                seed = seed * 1664525u + 1013904223u;
                const bool textRow = (y / 18) % 2 == 0 && (y % 18) > 3 && (y % 18) < 15;
                const bool word = ((x / 7 + y / 18 * 13) % 11) < 8;
                if (textRow && word && (seed >> 28) < 9)
                    p[0] = p[1] = p[2] = static_cast<uint8_t>(30 + (seed >> 26));
            }
        return pdf::EncodeImageFile(b, path, false, 0, 96);
    }

    // The inputs, made once per run in %TEMP%\sdbench-<pid> (kept out of the timings).
    struct Inputs
    {
        std::filesystem::path dir;
        std::wstring photo, screenshot;
        std::wstring clip, avi, song, wav;
        std::vector<std::wstring> photos, songs;
        std::wstring scanPdf, textPdf, markdown, csv, folder;
        bool media = false;

        std::wstring operator/(const std::wstring& name) const { return (dir / name).wstring(); }
    };

    const Inputs& In()
    {
        static const Inputs in = [] {
            Inputs i;
            i.dir = std::filesystem::temp_directory_path() / std::format(L"sdbench-{}", GetCurrentProcessId());
            std::filesystem::remove_all(i.dir);
            std::filesystem::create_directories(i.dir);
            RunMta([&] {
                i.photo = i / L"photo.jpg";
                EXPECT_HRESULT_SUCCEEDED(WritePhoto(i.photo, 4000, 3000));
                i.screenshot = i / L"screenshot.png";
                EXPECT_HRESULT_SUCCEEDED(WriteScreenshot(i.screenshot, 2560, 1440));
                std::filesystem::create_directories(i.dir / L"batch");
                for (int n = 1; n <= 8; ++n)
                {
                    i.photos.push_back((i.dir / L"batch" / std::format(L"IMG_{:04}.jpg", n)).wstring());
                    std::filesystem::copy_file(i.photo, i.photos.back());
                }
            });

            if (Ffmpeg()->Available())
            {
                // 30 s of 1080p30 H.264 + AAC (about 8 Mbit/s, like a phone clip), with grain so the
                // encoders have real work.
                i.clip = i / L"clip.mp4";
                const H264Encoder h264 = Ffmpeg()->H264();
                auto video = H264QualityArgs(h264, 23, 8'000'000);
                std::vector<std::wstring> args{ L"-f", L"lavfi", L"-i", L"testsrc2=s=1920x1080:r=30:d=30,noise=alls=6:allf=t",
                                                L"-f", L"lavfi", L"-i", L"sine=f=330:sample_rate=48000:d=30" };
                args.insert(args.end(), video.begin(), video.end());
                args.insert(args.end(), { L"-c:a", L"aac", L"-b:a", L"160k", L"-shortest", L"-f", L"mp4", i.clip });
                i.media = Make(args);
                // The same clip as MPEG-4 Part 2 in AVI (a format MP4 doesn't take as it is).
                i.avi = i / L"clip.avi";
                i.media = i.media && Make({ L"-i", i.clip, L"-c:v", L"mpeg4", L"-q:v", L"3", L"-c:a", L"libmp3lame", L"-b:a", L"192k", L"-f", L"avi", i.avi });
                // A 4-minute "song": a few tones with noise, as a 192 kbit/s MP3 and as WAV.
                i.song = i / L"song.mp3";
                const std::wstring tones = L"aevalsrc=0.25*sin(2*PI*220*t)+0.2*sin(2*PI*277*t)*sin(PI*t/4)+0.15*sin(2*PI*330*t)+0.05*(random(0)-0.5):s=44100:c=stereo:d=240";
                i.media = i.media && Make({ L"-f", L"lavfi", L"-i", tones, L"-c:a", L"libmp3lame", L"-b:a", L"192k", L"-f", L"mp3", i.song });
                i.wav = i / L"song.wav";
                i.media = i.media && Make({ L"-i", i.song, L"-c:a", L"pcm_s16le", L"-f", L"wav", i.wav });
                std::filesystem::create_directories(i.dir / L"songs");
                for (int n = 1; n <= 8; ++n)
                {
                    i.songs.push_back((i.dir / L"songs" / std::format(L"track {:02}.wav", n)).wstring());
                    i.media = i.media && Make({ L"-ss", std::to_wstring(n * 10), L"-t", L"60", L"-i", i.wav, L"-c:a", L"pcm_s16le", L"-f", L"wav", i.songs.back() });
                }
            }

            // A 20-page "scan" (a JPEG per page, like a phone scanner app makes) and a 20-page text PDF.
            RunMta([&] {
                std::vector<std::wstring> pages;
                for (int n = 0; n < 20; ++n)
                {
                    pages.push_back(i / std::format(L"page{:02}.jpg", n));
                    EXPECT_HRESULT_SUCCEEDED(WritePage(pages.back(), 2480, 3508));
                }
                i.scanPdf = i / L"scan.pdf";
                pdf::ImagePdfBuilder builder(i.scanPdf);
                for (const auto& p : pages)
                {
                    pdf::ImageSource image;
                    std::wstring error;
                    EXPECT_TRUE(pdf::LoadImageSource(p, image, error) && builder.AddPage(image, nullptr, error)) << Narrow(error);
                }
                std::wstring error;
                EXPECT_TRUE(builder.Finish(error)) << Narrow(error);
                for (const auto& p : pages)
                    DeleteFileW(p.c_str());
            });
            i.textPdf = i / L"report.pdf";
            {
                std::vector<std::string> contents;
                for (int page = 0; page < 20; ++page)
                {
                    std::string c = "BT /F1 11 Tf 13 TL 60 740 Td ";
                    for (int line = 0; line < 52; ++line)
                        c += std::format("(Page {} line {}: the quick brown fox jumps over the lazy dog, again and again.) '\n", page + 1, line + 1);
                    contents.push_back(c + "ET");
                }
                WriteTextPdf(i.textPdf, contents);
            }

            // A 30-page document in Markdown, and a 20 000-row spreadsheet as CSV.
            i.markdown = i / L"notes.md";
            {
                std::string md;
                for (int s = 1; s <= 60; ++s)
                {
                    md += std::format("## Section {}\n\n", s);
                    for (int p = 0; p < 4; ++p)
                        md += "Lorem ipsum dolor sit amet, consectetur adipiscing elit, sed do eiusmod tempor incididunt ut labore et dolore "
                              "magna aliqua. Ut enim ad minim veniam, quis nostrud exercitation ullamco laboris nisi ut aliquip ex ea commodo.\n\n";
                    md += "- one\n- two\n- three\n\n| A | B | C |\n|---|---|---|\n| 1 | 2 | 3 |\n| 4 | 5 | 6 |\n\n";
                }
                WriteText(i.markdown, md);
            }
            i.csv = i / L"sales.csv";
            {
                std::string csv = "Date,Region,Product,Units,Price,Total,Rep,Note\r\n";
                for (int r = 0; r < 20000; ++r)
                    csv += std::format("2026-{:02}-{:02},Region {},Product {},{},{}.99,{}.50,Rep {},\"note, {}\"\r\n", r % 12 + 1, r % 28 + 1, r % 9, r % 7,
                                       r % 97, r % 50, r % 997, r % 31, r);
                WriteText(i.csv, csv);
            }

            // A project folder: 40 text and code files and the 8 photos.
            i.folder = i / L"Project";
            std::filesystem::create_directories(std::filesystem::path(i.folder) / L"src");
            for (int n = 0; n < 40; ++n)
            {
                std::string text;
                for (int line = 0; line < 2000; ++line)
                    text += std::format("int value_{}_{} = {}; // a line of source code\n", n, line, line * 7);
                WriteText((std::filesystem::path(i.folder) / L"src" / std::format(L"file{:02}.cpp", n)).wstring(), text);
            }
            for (const auto& p : i.photos)
                std::filesystem::copy_file(p, std::filesystem::path(i.folder) / std::filesystem::path(p).filename());
            return i;
        }();
        return in;
    }

    // One runner for every row, like the app's.
    struct Bench
    {
        Bench()
        {
            wchar_t self[MAX_PATH]{};
            GetModuleFileNameW(nullptr, self, MAX_PATH);
            sandbox::SetWorkerPath(self);   // the archive worker is this exe (TestMain.cpp)
            RunMta([&] { codecs = std::make_shared<const ImageCodecs>(); });
            runner = std::make_unique<PipelineRunner>(codecs, nullptr, Ffmpeg()->Available() ? Ffmpeg() : nullptr);
        }
        TempDir recipesDir;
        Recipes recipes{ recipesDir };
        std::shared_ptr<const ImageCodecs> codecs;
        std::unique_ptr<PipelineRunner> runner;
    };

    Bench& B()
    {
        static Bench bench;
        return bench;
    }

    void RemoveOutput(const ItemResult& r)
    {
        std::error_code ec;
        if (!r.output.empty())
            std::filesystem::remove_all(r.output, ec);
    }

    std::optional<Recipe> Find(std::string_view id)
    {
        if (const auto* r = B().recipes.store.Find(id))
            return *r;
        return std::nullopt;
    }

    // Times one recipe on one file through the runner: the median of three runs when it is quick,
    // else one run. The output is deleted after each run, so every run writes the same name.
    void TimeRecipe(const char* area, const std::string& task, const Recipe& recipe, const std::wstring& source)
    {
        std::vector<double> times;
        ItemResult last;
        for (int run = 0; run < 3; ++run)
        {
            ItemResult r;
            const double ms = TimeOnMta([&] { r = B().runner->Run(recipe, 0, source, std::stop_token{}); });
            if (!r.ok)
            {
                Record(area, task, -1, "did not run: " + Narrow(r.error));
                return;
            }
            times.push_back(ms);
            last = r;
            RemoveOutput(r);
            if (ms > 1500)
                break;
        }
        std::sort(times.begin(), times.end());
        Record(area, task, times[times.size() / 2], Bytes(last.size));
    }

    void TimeRecipe(const char* area, const std::string& task, std::string_view id, const std::wstring& source)
    {
        if (const auto r = Find(id))
            TimeRecipe(area, task, *r, source);
        else
            Record(area, task, -1, std::format("no recipe {}", id));
    }

    void TimeBatch(const char* area, const std::string& task, const Recipe& recipe, const std::vector<std::wstring>& sources)
    {
        ItemResult r;
        const double ms = TimeOnMta([&] { r = B().runner->RunBatch(recipe, 0, sources, std::stop_token{}); });
        if (!r.ok)
        {
            Record(area, task, -1, "did not run: " + Narrow(r.error));
            return;
        }
        Record(area, task, ms, Bytes(r.size));
        RemoveOutput(r);
    }

    // Many files through the runner on `threads` workers, as the app runs a drop of several files.
    void TimeFiles(const char* area, const std::string& task, std::string_view id, const std::vector<std::wstring>& sources, unsigned threads)
    {
        const auto recipe = Find(id);
        if (!recipe)
            return Record(area, task, -1, std::format("no recipe {}", id));
        std::mutex m;
        std::condition_variable done;
        size_t left = sources.size();
        std::vector<ItemResult> results(sources.size());
        const auto start = std::chrono::steady_clock::now();
        {
            JobQueue queue(threads);
            for (size_t i = 0; i < sources.size(); ++i)
                queue.Submit([&, i](std::stop_token) {
                    results[i] = B().runner->Run(*recipe, 0, sources[i], std::stop_token{});
                    std::scoped_lock lock(m);
                    if (--left == 0)
                        done.notify_one();
                });
            std::unique_lock lock(m);
            done.wait(lock, [&] { return left == 0; });
        }
        const double ms = Ms(start);
        uint64_t total = 0;
        for (const auto& r : results)
        {
            if (!r.ok)
                return Record(area, task, -1, "did not run: " + Narrow(r.error));
            total += r.size;
            RemoveOutput(r);
        }
        Record(area, task, ms, std::format("{} files, {} worker(s), {}", sources.size(), threads, Bytes(total)));
    }

    Recipe WithParams(Recipe r, const nlohmann::json& params)
    {
        for (const auto& [k, v] : params.items())
            r.steps[0].params[k] = v;
        return r;
    }

    // The worker count the app gives a lane (main.cpp).
    unsigned AppThreads(JobLane lane)
    {
        return JobLanes::Threads(lane);
    }
}

TEST(Bench, Images)
{
    SD_BENCH_ONLY();
    const auto& in = In();
    const char* a = "Image";
    TimeRecipe(a, "12 MP JPEG -> PNG", "to-png", in.photo);
    TimeRecipe(a, "12 MP JPEG -> WebP", "to-webp", in.photo);
    TimeRecipe(a, "12 MP JPEG -> TIFF", "to-tiff", in.photo);
    TimeRecipe(a, "12 MP JPEG -> HEIC", "to-heic", in.photo);
    TimeRecipe(a, "12 MP JPEG -> AVIF", "to-avif", in.photo);
    TimeRecipe(a, "12 MP JPEG -> 1920 px", "max-1920", in.photo);
    TimeRecipe(a, "12 MP JPEG -> 50%", "half-size", in.photo);
    TimeRecipe(a, "12 MP JPEG -> Web (resize + WebP)", "web-ready", in.photo);
    TimeRecipe(a, "12 MP JPEG -> Rotate", "rotate-right", in.photo);
    TimeRecipe(a, "12 MP JPEG -> Compress", "compress-image", in.photo);
    TimeRecipe(a, "12 MP JPEG -> Under 1 MB", "image-under-1mb", in.photo);
    TimeRecipe(a, "12 MP JPEG -> 50% smaller", "reduce-image-50", in.photo);
    TimeRecipe(a, "1440p PNG screenshot -> JPG", "to-jpg", in.screenshot);
    TimeRecipe(a, "1440p PNG screenshot -> WebP", "to-webp", in.screenshot);
    TimeRecipe(a, "1440p PNG screenshot -> 1080 px", "max-1080", in.screenshot);
    const auto lane = B().runner->Lane(*Find("to-webp"));
    TimeFiles(a, "8 photos -> WebP (app's pool)", "to-webp", in.photos, AppThreads(lane));
    TimeFiles(a, "8 photos -> JPG 1920 px (app's pool)", "max-1920", in.photos, AppThreads(lane));
    TimeFiles(a, "8 photos -> WebP (one per core)", "to-webp", in.photos, std::max(1u, std::thread::hardware_concurrency()));
}

TEST(Bench, Video)
{
    SD_BENCH_ONLY();
    const auto& in = In();
    if (!in.media)
        GTEST_SKIP() << "SHIFTDROP_FFMPEG is not set";
    const char* a = "Video";
    Record(a, "H.264 encoder on this PC", -1, Narrow(EncoderName(Ffmpeg()->H264())));
    TimeRecipe(a, "1080p 30 s MP4 -> MOV (streams copied)", "to-mov", in.clip);
    TimeRecipe(a, "1080p 30 s MP4 -> MKV (streams copied)", "to-mkv", in.clip);
    TimeRecipe(a, "1080p 30 s AVI -> MP4 (H.264 encode)", "to-mp4", in.avi);
    TimeRecipe(a, "1080p 30 s MP4 -> WebM (VP9)", "to-webm", in.clip);
    TimeRecipe(a, "1080p 30 s MP4 -> GIF", "to-gif", in.clip);
    TimeRecipe(a, "1080p 30 s MP4 -> MP3 (sound only)", "to-mp3", in.clip);
    TimeRecipe(a, "1080p 30 s MP4 -> AVI", "to-avi", in.clip);
    TimeRecipe(a, "1080p 30 s MP4 -> Compress", "compress-media", in.clip);
    TimeRecipe(a, "1080p 30 s MP4 -> 50% smaller", "reduce-media-50", in.clip);
    TimeRecipe(a, "1080p 30 s MP4 -> Under 10 MB", "media-under-10mb", in.clip);
    if (const auto trim = Find("video-trim"))
    {
        TimeRecipe(a, "Trim 5-20 s, exact", WithParams(*trim, { { "ask", false }, { "start", 5 }, { "end", 20 }, { "mode", "exact" } }), in.clip);
        TimeRecipe(a, "Trim 5-20 s, fast (copy)", WithParams(*trim, { { "ask", false }, { "start", 5 }, { "end", 20 }, { "mode", "fast" } }), in.clip);
    }
    if (const auto join = Find("video-join"))
    {
        const auto second = in / L"clip 2.mp4";
        std::filesystem::copy_file(in.clip, second, std::filesystem::copy_options::overwrite_existing);
        TimeBatch(a, "Join two MP4s (same format: copy)", WithParams(*join, { { "ask", false } }), { in.clip, second });
        TimeBatch(a, "Join MP4 + AVI (encode)", WithParams(*join, { { "ask", false } }), { in.clip, in.avi });
    }
}

TEST(Bench, Audio)
{
    SD_BENCH_ONLY();
    const auto& in = In();
    if (!in.media)
        GTEST_SKIP() << "SHIFTDROP_FFMPEG is not set";
    const char* a = "Audio";
    TimeRecipe(a, "4 min MP3 -> M4A", "to-m4a", in.song);
    TimeRecipe(a, "4 min MP3 -> WAV", "to-wav", in.song);
    TimeRecipe(a, "4 min MP3 -> FLAC", "to-flac", in.song);
    TimeRecipe(a, "4 min MP3 -> OGG", "to-ogg", in.song);
    TimeRecipe(a, "4 min MP3 -> Opus", "to-opus", in.song);
    TimeRecipe(a, "4 min WAV -> MP3", "to-mp3", in.wav);
    TimeRecipe(a, "4 min MP3 -> Compress", "compress-media", in.song);
    if (const auto normalize = Find("audio-normalize"))
        TimeRecipe(a, "4 min MP3 -> Normalize", WithParams(*normalize, { { "ask", false }, { "target", -16 } }), in.song);
    const auto lane = B().runner->Lane(*Find("to-mp3"));
    TimeFiles(a, "8 one-minute WAVs -> MP3 (app's lanes)", "to-mp3", in.songs, AppThreads(lane));
}

TEST(Bench, Documents)
{
    SD_BENCH_ONLY();
    const auto& in = In();
    const char* a = "PDF";
    TimeRecipe(a, "20-page scan -> PNG pages", "pdf-to-png", in.scanPdf);
    TimeRecipe(a, "20-page scan -> JPG pages", "pdf-to-jpg", in.scanPdf);
    TimeRecipe(a, "20-page text PDF -> PNG pages", "pdf-to-png", in.textPdf);
    TimeRecipe(a, "20-page scan -> Compress", "pdf-compress", in.scanPdf);
    TimeRecipe(a, "20-page scan -> 50% smaller", "reduce-pdf-50", in.scanPdf);
    TimeRecipe(a, "20-page text PDF -> TXT", "pdf-to-txt", in.textPdf);
    TimeRecipe(a, "20-page scan -> Searchable PDF (OCR)", "ocr-searchable-pdf", in.scanPdf);
    if (const auto pdf = Find("images-to-pdf"))
        TimeBatch(a, "8 photos -> one PDF", *pdf, in.photos);

    a = "Docs";
    TimeRecipe(a, "30-page Markdown -> DOCX", "docs-to-docx", in.markdown);
    TimeRecipe(a, "30-page Markdown -> HTML", "docs-to-html", in.markdown);
    TimeRecipe(a, "30-page Markdown -> PDF", "docs-to-pdf", in.markdown);
    TimeRecipe(a, "20 000-row CSV -> XLSX", "sheets-to-xlsx", in.csv);
    TimeRecipe(a, "20 000-row CSV -> ODS", "sheets-to-ods", in.csv);
}

TEST(Bench, Archives)
{
    SD_BENCH_ONLY();
    const auto& in = In();
    const char* a = "Archive";
    for (const auto& [id, task] : { std::pair{ "archive-folder-zip", "Folder (48 files, 30 MB) -> ZIP" },
                                    std::pair{ "archive-folder-7z", "Folder -> 7z" }, std::pair{ "archive-folder-targz", "Folder -> TAR.GZ" } })
    {
        if (const auto r = Find(id))
            TimeBatch(a, task, *r, { in.folder });
    }
    // Extract: the folder's ZIP, read by the sandboxed worker.
    const auto zipRecipe = Find("archive-folder-zip");
    ASSERT_TRUE(zipRecipe);
    ItemResult zip;
    RunMta([&] { zip = B().runner->RunBatch(*zipRecipe, 0, { in.folder }, std::stop_token{}); });
    ASSERT_TRUE(zip.ok) << Narrow(zip.error);
    TimeRecipe(a, "Extract that ZIP (sandboxed worker)", "archive-extract", zip.output);
    RemoveOutput(zip);
}

TEST(Bench, Overhead)
{
    SD_BENCH_ONLY();
    const auto& in = In();
    const char* a = "Overhead";
    if (in.media)
    {
        std::vector<double> starts, probes, inspects;
        std::stop_source never;
        for (int i = 0; i < 5; ++i)
        {
            auto t = std::chrono::steady_clock::now();
            Ffmpeg()->Run({ L"-hide_banner", L"-nostdin", L"-version" }, nullptr, never.get_token());
            starts.push_back(Ms(t));
            t = std::chrono::steady_clock::now();
            const auto info = Ffmpeg()->Probe(in.clip);
            probes.push_back(Ms(t));
            EXPECT_EQ(info.videoCodec, "h264");
            t = std::chrono::steady_clock::now();
            inspect::Probe(*Ffmpeg(), in.clip, never.get_token());
            inspects.push_back(Ms(t));
        }
        for (auto* v : { &starts, &probes, &inspects })
            std::sort(v->begin(), v->end());
        Record(a, "Start ffmpeg (ffmpeg -version)", starts[2]);
        Record(a, "Probe before a conversion", probes[2]);
        Record(a, "Probe for the wheel's prediction", inspects[2]);
        // The H.264 encoder test the app runs once at startup.
        const FfmpegEngine fresh(Ffmpeg()->Path());
        const auto t = std::chrono::steady_clock::now();
        const auto chosen = fresh.H264();
        Record(a, "H.264 encoder test at startup", Ms(t), Narrow(EncoderName(chosen)));
    }
    Record(a, "Image codec list at startup", TimeOnMta([] { ImageCodecs codecs; }));

    // The wheel's per-file prediction (what the card under a highlighted slice shows): first time
    // for a file, then from the cache.
    using preview::Predictor;
    const auto predict = [&](const std::string& task, const std::wstring& file, std::string_view id) {
        const auto recipe = Find(id);
        if (!recipe)
            return;
        Predictor predictor(in.media ? Ffmpeg() : nullptr, {});
        double first = 0, again = 0;
        std::wstring size;
        RunMta([&] {
            auto t = std::chrono::steady_clock::now();
            const auto p = predictor.Predict(1, { file }, *recipe, std::stop_token{});
            first = Ms(t);
            size = p.size;
            t = std::chrono::steady_clock::now();
            predictor.Predict(2, { file }, *recipe, std::stop_token{});
            again = Ms(t);
        });
        Record(a, "Prediction: " + task, first, std::format("{}; again {}", Narrow(size), Time(again)));
    };
    predict("12 MP JPEG as WebP", in.photo, "to-webp");
    predict("12 MP JPEG at 1920 px", in.photo, "max-1920");
    predict("PNG screenshot as JPG", in.screenshot, "to-jpg");
    if (in.media)
    {
        predict("1080p MP4 as WebM", in.clip, "to-webm");
        predict("4 min MP3 as M4A", in.song, "to-m4a");
    }
}

// ffmpeg settings side by side on the same input, for choosing the defaults in Media.cpp and the
// tools (each row is one ffmpeg run; the rows above are what the app does).
TEST(Bench, FfmpegOptions)
{
    SD_BENCH_ONLY();
    const auto& in = In();
    if (!in.media)
        GTEST_SKIP() << "SHIFTDROP_FFMPEG is not set";
    const char* a = "Options";
    const std::wstring cores = std::to_wstring(std::max(1u, std::thread::hardware_concurrency()));
    const auto out = in / L"options.out";
    const auto time = [&](const std::string& task, std::vector<std::wstring> input, std::vector<std::wstring> output) {
        std::vector<std::wstring> args{ L"-hide_banner", L"-nostdin", L"-y" };
        args.insert(args.end(), input.begin(), input.end());
        const bool null = output.back() == L"null";
        args.insert(args.end(), output.begin(), output.end());
        args.push_back(null ? L"-" : out);
        std::stop_source never;
        const auto t = std::chrono::steady_clock::now();
        const auto r = Ffmpeg()->Run(args, nullptr, never.get_token());
        const double ms = Ms(t);
        std::error_code ec;
        const auto size = std::filesystem::exists(out) ? std::filesystem::file_size(out, ec) : 0;
        std::filesystem::remove(out, ec);
        Record(a, task, r.ok ? ms : -1, r.ok ? (size ? Bytes(size) : std::string()) : "failed: " + Narrow(r.error));
    };
    const std::vector<std::wstring> ten{ L"-t", L"10", L"-i", in.clip };
    const auto with = [](std::vector<std::wstring> x, const std::vector<std::wstring>& y) {
        x.insert(x.end(), y.begin(), y.end());
        return x;
    };
    // H.264 on the CPU: Media Foundation, and OpenH264 with one slice (as it was) and a slice per core.
    const std::vector<std::wstring> v{ L"-map", L"0:v:0", L"-an", L"-vf", L"crop=trunc(iw/2)*2:trunc(ih/2)*2" };
    time("10 s 1080p H.264 Media Foundation (now)", ten,
         with(v, { L"-c:v", L"h264_mf", L"-rate_control", L"u_vbr", L"-b:v", L"8000000", L"-pix_fmt", L"nv12", L"-f", L"mp4" }));
    time("10 s 1080p H.264 OpenH264, one slice (was)", ten,
         with(v, { L"-c:v", L"libopenh264", L"-rc_mode", L"bitrate", L"-b:v", L"8000000", L"-pix_fmt", L"yuv420p", L"-f", L"mp4" }));
    time("10 s 1080p H.264 OpenH264, threads = cores", ten,
         with(v, { L"-c:v", L"libopenh264", L"-threads", cores, L"-rc_mode", L"bitrate", L"-b:v", L"8000000", L"-pix_fmt", L"yuv420p", L"-f", L"mp4" }));
    time("10 s 1080p H.264 OpenH264, a slice per core (now)", ten,
         with(v, { L"-c:v", L"libopenh264", L"-threads", cores, L"-slices", cores, L"-rc_mode", L"bitrate", L"-b:v", L"8000000", L"-pix_fmt",
                   L"yuv420p", L"-f", L"mp4" }));
    // VP9 for WebM, and AV1 for comparison.
    const std::vector<std::wstring> vp9{ L"-map", L"0:v:0", L"-an", L"-c:v", L"libvpx-vp9", L"-crf", L"32", L"-b:v", L"0", L"-row-mt", L"1" };
    time("10 s 1080p VP9 good, cpu-used 5 (was)", ten, with(vp9, { L"-deadline", L"good", L"-cpu-used", L"5", L"-f", L"webm" }));
    time("10 s 1080p VP9 good, cpu-used 5, 4 tiles, threads", ten,
         with(vp9, { L"-deadline", L"good", L"-cpu-used", L"5", L"-tile-columns", L"2", L"-threads", cores, L"-f", L"webm" }));
    time("10 s 1080p VP9 good, cpu-used 6, 4 tiles, threads", ten,
         with(vp9, { L"-deadline", L"good", L"-cpu-used", L"6", L"-tile-columns", L"2", L"-threads", cores, L"-f", L"webm" }));
    time("10 s 1080p VP9 realtime, cpu-used 6 (now)", ten, with(vp9, { L"-deadline", L"realtime", L"-cpu-used", L"6", L"-f", L"webm" }));
    time("10 s 1080p VP9 realtime, cpu-used 8, 4 tiles", ten,
         with(vp9, { L"-deadline", L"realtime", L"-cpu-used", L"8", L"-tile-columns", L"2", L"-threads", cores, L"-f", L"webm" }));
    time("10 s 1080p AV1 SVT preset 10", ten, { L"-map", L"0:v:0", L"-an", L"-c:v", L"libsvtav1", L"-preset", L"10", L"-crf", L"35", L"-f", L"webm" });
    // GIF: the palette pass as now, and with more filter threads.
    const std::wstring gif = L"fps=12,scale='min(480,iw*sar)':-2:flags=lanczos,setsar=1,split[a][b];[a]palettegen=stats_mode=diff[p];[b][p]paletteuse=dither=bayer:bayer_scale=4";
    time("30 s 1080p GIF (now)", { L"-i", in.clip }, { L"-map", L"0:v:0", L"-an", L"-vf", gif, L"-loop", L"0", L"-f", L"gif" });
    time("30 s 1080p GIF, filter threads = cores", { L"-filter_threads", cores, L"-i", in.clip },
         { L"-map", L"0:v:0", L"-an", L"-vf", gif, L"-loop", L"0", L"-f", L"gif" });
    time("30 s 1080p H.264 decode only", { L"-i", in.clip }, { L"-map", L"0:v:0", L"-an", L"-f", L"null" });
    // Loudness: loudnorm's measuring pass (as it was) against ebur128, and applying the gain.
    time("4 min loudness, loudnorm measure (was)", { L"-i", in.song },
         { L"-map", L"0:a:0", L"-vn", L"-af", L"loudnorm=I=-16:TP=-1:LRA=11:print_format=json", L"-f", L"null" });
    time("4 min loudness, ebur128 measure (now)", { L"-i", in.song }, { L"-map", L"0:a:0", L"-vn", L"-af", L"ebur128=peak=true:framelog=quiet", L"-f", L"null" });
    time("4 min loudnorm linear apply to MP3 (was)", { L"-i", in.song },
         { L"-map", L"0:a:0", L"-vn", L"-af",
           L"loudnorm=I=-16:TP=-1:LRA=20:measured_I=-20:measured_TP=-6:measured_LRA=5:measured_thresh=-30:offset=0:linear=true:print_format=json",
           L"-ar", L"44100", L"-c:a", L"libmp3lame", L"-q:a", L"2", L"-f", L"mp3" });
    time("4 min volume gain to MP3 (now, when it fits)", { L"-i", in.song }, { L"-map", L"0:a:0", L"-vn", L"-af", L"volume=4dB", L"-c:a", L"libmp3lame", L"-q:a", L"2", L"-f", L"mp3" });
    time("4 min MP3 decode only", { L"-i", in.song }, { L"-map", L"0:a:0", L"-f", L"null" });
}

// WebP settings on the 12 MP photo at the app's quality (85): time, size and SSIM of the result
// against the photo (libwebp's own measure, in dB: higher is closer).
TEST(Bench, WebpOptions)
{
    SD_BENCH_ONLY();
    const auto& in = In();
    using Microsoft::WRL::ComPtr;
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> bgra;
    ASSERT_HRESULT_SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)));
    ASSERT_HRESULT_SUCCEEDED(factory->CreateDecoderFromFilename(in.photo.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder));
    ASSERT_HRESULT_SUCCEEDED(decoder->GetFrame(0, &frame));
    ASSERT_HRESULT_SUCCEEDED(factory->CreateFormatConverter(&bgra));
    ASSERT_HRESULT_SUCCEEDED(bgra->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom));
    UINT w = 0, h = 0;
    bgra->GetSize(&w, &h);
    std::vector<BYTE> pixels(static_cast<size_t>(w) * h * 4);
    ASSERT_HRESULT_SUCCEEDED(bgra->CopyPixels(nullptr, w * 4, static_cast<UINT>(pixels.size()), pixels.data()));

    WebPPicture original;
    ASSERT_TRUE(WebPPictureInit(&original));
    original.width = static_cast<int>(w);
    original.height = static_cast<int>(h);
    original.use_argb = 1;
    ASSERT_TRUE(WebPPictureImportBGRA(&original, pixels.data(), static_cast<int>(w * 4)));
    const auto run = [&](const char* task, int method, int threads) {
        WebPConfig config;
        WebPPicture picture;
        WebPMemoryWriter memory;
        if (!WebPConfigPreset(&config, WEBP_PRESET_DEFAULT, 85) || !WebPPictureInit(&picture))
            return;
        config.method = method;
        config.thread_level = threads;
        picture.width = static_cast<int>(w);
        picture.height = static_cast<int>(h);
        WebPMemoryWriterInit(&memory);
        picture.writer = WebPMemoryWrite;
        picture.custom_ptr = &memory;
        const auto t = std::chrono::steady_clock::now();
        const bool ok = WebPPictureImportBGRA(&picture, pixels.data(), static_cast<int>(w * 4)) && WebPEncode(&config, &picture);
        const double ms = Ms(t);
        std::string note = ok ? Bytes(memory.size) : "failed";
        int dw = 0, dh = 0;
        if (uint8_t* decoded = ok ? WebPDecodeBGRA(memory.mem, memory.size, &dw, &dh) : nullptr)
        {
            WebPPicture back;
            float result[5]{};
            if (WebPPictureInit(&back))
            {
                back.width = dw;
                back.height = dh;
                back.use_argb = 1;
                if (WebPPictureImportBGRA(&back, decoded, dw * 4) && WebPPictureDistortion(&original, &back, 1, result))
                    note += std::format(", SSIM {:.2f} dB", result[4]);
                WebPPictureFree(&back);
            }
            WebPFree(decoded);
        }
        WebPPictureFree(&picture);
        WebPMemoryWriterClear(&memory);
        Record("WebP", task, ok ? ms : -1, note);
    };
    run("12 MP, method 4, one thread (was)", 4, 0);
    run("12 MP, method 4, two threads (now)", 4, 1);
    run("12 MP, method 3, two threads", 3, 1);
    run("12 MP, method 2, two threads", 2, 1);
    WebPPictureFree(&original);
}
