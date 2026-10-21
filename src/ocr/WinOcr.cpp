// Text recognition with Windows.Media.Ocr: the OCR built into Windows, in the languages the user
// has installed (Settings > Time & language > Language). Nothing is bundled or downloaded. Installs
// itself into ocr::Platform() before main().
#include "ocr/Ocr.h"
#include "Log.h"

#include <unknwn.h>
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Globalization.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Media.Ocr.h>
#include <winrt/Windows.Storage.Streams.h>

#include <algorithm>
#include <cmath>
#include <format>
#include <mutex>

namespace sd::ocr
{
    namespace
    {
        namespace wmo = winrt::Windows::Media::Ocr;
        namespace imaging = winrt::Windows::Graphics::Imaging;

        std::wstring Message(const winrt::hresult_error& e)
        {
            std::wstring text(e.message());
            while (!text.empty() && (text.back() == L'\n' || text.back() == L'\r' || text.back() == L' '))
                text.pop_back();
            return text.empty() ? std::format(L"Windows OCR failed (0x{:08X})", static_cast<uint32_t>(e.code().value)) : text;
        }

        // The language picked in the tray menu; else the user's own languages first (the first one
        // Windows can read), else any installed one.
        wmo::OcrEngine CreateEngine()
        {
            if (const auto tag = LanguageChoice(); !tag.empty())
            {
                if (auto chosen = wmo::OcrEngine::TryCreateFromLanguage(winrt::Windows::Globalization::Language(winrt::hstring(tag))))
                    return chosen;
                log::Warn(L"ocr: {} is no longer installed; reading with the automatic choice", tag);
            }
            auto engine = wmo::OcrEngine::TryCreateFromUserProfileLanguages();
            if (engine)
                return engine;
            for (const auto& language : wmo::OcrEngine::AvailableRecognizerLanguages())
                if ((engine = wmo::OcrEngine::TryCreateFromLanguage(language)))
                    return engine;
            return nullptr;
        }

        std::vector<Language> InstalledLanguages()
        {
            std::vector<Language> out;
            try
            {
                for (const auto& language : wmo::OcrEngine::AvailableRecognizerLanguages())
                    out.push_back(Language{ std::wstring(language.LanguageTag()), std::wstring(language.DisplayName()) });
            }
            catch (const winrt::hresult_error& e)
            {
                log::Warn(L"ocr: cannot list languages: {}", Message(e));
            }
            return out;
        }

        constexpr const wchar_t* kNoLanguage = L"no OCR language is installed (Settings > Time & language > Language)";

        bool IsAvailable(std::wstring* why)
        {
            // Languages rarely change while the app runs; ask Windows once.
            static std::once_flag once;
            static bool available = false;
            static std::wstring reason;
            std::call_once(once, [] {
                try
                {
                    available = static_cast<bool>(CreateEngine());
                    if (!available)
                        reason = kNoLanguage;
                }
                catch (const winrt::hresult_error& e)
                {
                    reason = L"Windows OCR is not available: " + Message(e);
                }
                if (available)
                    log::Info(L"ocr: Windows OCR available");
                else
                    log::Info(L"ocr: {}", reason);
            });
            if (!available && why)
                *why = reason;
            return available;
        }

        // Box-filter shrink of BGRA pixels to w x h (Windows OCR has a size limit).
        pdf::Bitmap Shrink(const pdf::Bitmap& in, uint32_t w, uint32_t h)
        {
            pdf::Bitmap out;
            out.width = w;
            out.height = h;
            out.bgra.resize(static_cast<size_t>(w) * h * 4);
            const double fx = static_cast<double>(in.width) / w, fy = static_cast<double>(in.height) / h;
            for (uint32_t y = 0; y < h; ++y)
            {
                const auto y0 = static_cast<uint32_t>(y * fy);
                const auto y1 = std::max(y0 + 1, std::min(in.height, static_cast<uint32_t>(std::ceil((y + 1) * fy))));
                for (uint32_t x = 0; x < w; ++x)
                {
                    const auto x0 = static_cast<uint32_t>(x * fx);
                    const auto x1 = std::max(x0 + 1, std::min(in.width, static_cast<uint32_t>(std::ceil((x + 1) * fx))));
                    uint32_t sum[3] = {};
                    for (uint32_t sy = y0; sy < y1; ++sy)
                        for (uint32_t sx = x0; sx < x1; ++sx)
                        {
                            const uint8_t* p = &in.bgra[(static_cast<size_t>(sy) * in.width + sx) * 4];
                            sum[0] += p[0];
                            sum[1] += p[1];
                            sum[2] += p[2];
                        }
                    const uint32_t n = (y1 - y0) * (x1 - x0);
                    uint8_t* q = &out.bgra[(static_cast<size_t>(y) * w + x) * 4];
                    q[0] = static_cast<uint8_t>(sum[0] / n);
                    q[1] = static_cast<uint8_t>(sum[1] / n);
                    q[2] = static_cast<uint8_t>(sum[2] / n);
                    q[3] = 255;
                }
            }
            return out;
        }

        bool RecognizeBitmap(const pdf::Bitmap& image, Result& out, std::wstring& error)
        {
            try
            {
                auto engine = CreateEngine();
                if (!engine)
                {
                    error = kNoLanguage;
                    return false;
                }
                // Scale down to Windows' limit; boxes are scaled back to `image`'s pixels.
                const uint32_t limit = wmo::OcrEngine::MaxImageDimension();
                const uint32_t longest = std::max(image.width, image.height);
                double scale = 1.0;
                pdf::Bitmap shrunk;
                const pdf::Bitmap* source = &image;
                if (limit > 0 && longest > limit)
                {
                    scale = static_cast<double>(limit) / longest;
                    shrunk = Shrink(image, std::max<uint32_t>(1, static_cast<uint32_t>(image.width * scale)),
                                    std::max<uint32_t>(1, static_cast<uint32_t>(image.height * scale)));
                    source = &shrunk;
                }
                const double back = static_cast<double>(image.width) / source->width;
                const double backY = static_cast<double>(image.height) / source->height;

                imaging::SoftwareBitmap bitmap(imaging::BitmapPixelFormat::Bgra8, static_cast<int32_t>(source->width),
                                               static_cast<int32_t>(source->height), imaging::BitmapAlphaMode::Premultiplied);
                winrt::Windows::Storage::Streams::Buffer buffer(static_cast<uint32_t>(source->bgra.size()));
                memcpy(buffer.data(), source->bgra.data(), source->bgra.size());
                // Opaque pixels are the same premultiplied or not; callers flatten onto white first.
                for (uint32_t i = 3; i < buffer.Capacity(); i += 4)
                    buffer.data()[i] = 255;
                buffer.Length(static_cast<uint32_t>(source->bgra.size()));
                bitmap.CopyFromBuffer(buffer);

                const auto result = engine.RecognizeAsync(bitmap).get();
                out = {};
                out.width = image.width;
                out.height = image.height;
                out.language = std::wstring(engine.RecognizerLanguage().LanguageTag());
                for (const auto& line : result.Lines())
                {
                    Line l;
                    l.text = std::wstring(line.Text());
                    for (const auto& word : line.Words())
                    {
                        const auto r = word.BoundingRect();
                        l.words.push_back(Word{ std::wstring(word.Text()), r.X * back, r.Y * backY, r.Width * back, r.Height * backY });
                    }
                    if (!l.words.empty())
                        out.lines.push_back(std::move(l));
                }
                return true;
            }
            catch (const winrt::hresult_error& e)
            {
                error = Message(e);
                return false;
            }
        }

        const bool g_installed = [] {
            Platform().languages = &InstalledLanguages;
            Platform().available = &IsAvailable;
            Platform().recognize = &RecognizeBitmap;
            return true;
        }();
    }
}
