// Windows OCR and face detection through C++/WinRT. Built only with MSVC (the Windows SDK has the
// C++/WinRT headers), as its own object library linked into the app and the tests: the core
// library stays plain Win32 + COM.
#include "redact/Vision.h"

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Media.FaceAnalysis.h>
#include <winrt/Windows.Media.Ocr.h>
#include <winrt/Windows.Storage.Streams.h>

#include <algorithm>
#include <cmath>
#include <mutex>

namespace
{
    namespace imaging = winrt::Windows::Graphics::Imaging;
    namespace ocr = winrt::Windows::Media::Ocr;
    namespace faces = winrt::Windows::Media::FaceAnalysis;
    using sd::canvas::Raster;
    using sd::canvas::RectD;

    // The picture as a premultiplied BGRA SoftwareBitmap whose longer side is at most `maxEdge`,
    // shrunk by averaging (area sampling keeps small text readable). `scale` maps back to pixels.
    imaging::SoftwareBitmap ToBitmap(const Raster& image, uint32_t maxEdge, double& scale)
    {
        scale = std::min(1.0, static_cast<double>(maxEdge) / std::max(image.width, image.height));
        const uint32_t w = std::max(1u, static_cast<uint32_t>(std::lround(image.width * scale)));
        const uint32_t h = std::max(1u, static_cast<uint32_t>(std::lround(image.height * scale)));
        scale = static_cast<double>(w) / image.width;
        winrt::Windows::Storage::Streams::Buffer buffer(w * h * 4);
        buffer.Length(w * h * 4);
        uint8_t* out = buffer.data();
        for (uint32_t y = 0; y < h; ++y)
        {
            const uint32_t y0 = static_cast<uint32_t>(y / scale), y1 = std::max(y0 + 1, std::min(image.height, static_cast<uint32_t>((y + 1) / scale)));
            for (uint32_t x = 0; x < w; ++x)
            {
                const uint32_t x0 = static_cast<uint32_t>(x / scale), x1 = std::max(x0 + 1, std::min(image.width, static_cast<uint32_t>((x + 1) / scale)));
                uint64_t sum[4]{};
                for (uint32_t sy = y0; sy < y1; ++sy)
                    for (uint32_t sx = x0; sx < x1; ++sx)
                    {
                        const uint8_t* p = image.At(sx, sy);
                        for (int c = 0; c < 3; ++c)
                            sum[c] += uint64_t{ p[c] } * p[3] / 255;
                        sum[3] += p[3];
                    }
                const uint64_t n = uint64_t{ x1 - x0 } * (y1 - y0);
                uint8_t* d = out + (static_cast<size_t>(y) * w + x) * 4;
                for (int c = 0; c < 4; ++c)
                    d[c] = static_cast<uint8_t>(sum[c] / n);
            }
        }
        return imaging::SoftwareBitmap::CreateCopyFromBuffer(buffer, imaging::BitmapPixelFormat::Bgra8, static_cast<int32_t>(w),
                                                             static_cast<int32_t>(h), imaging::BitmapAlphaMode::Premultiplied);
    }

    bool TextAvailable()
    {
        static std::once_flag once;
        static bool available = false;
        std::call_once(once, [] {
            try
            {
                available = ocr::OcrEngine::TryCreateFromUserProfileLanguages() != nullptr;
            }
            catch (const winrt::hresult_error&)
            {
                available = false;
            }
        });
        return available;
    }

    bool FacesAvailable()
    {
        static std::once_flag once;
        static bool available = false;
        std::call_once(once, [] {
            try
            {
                available = faces::FaceDetector::IsSupported();
            }
            catch (const winrt::hresult_error&)
            {
                available = false;
            }
        });
        return available;
    }

    std::vector<sd::redact::OcrLine> ReadText(const Raster& image, std::wstring* error)
    {
        std::vector<sd::redact::OcrLine> lines;
        try
        {
            const auto engine = ocr::OcrEngine::TryCreateFromUserProfileLanguages();
            if (!engine)
            {
                if (error)
                    *error = L"no text-recognition language is installed (Settings > Time & language > Language)";
                return lines;
            }
            double scale = 1;
            const auto bitmap = ToBitmap(image, ocr::OcrEngine::MaxImageDimension(), scale);
            const auto result = engine.RecognizeAsync(bitmap).get();
            for (const auto& line : result.Lines())
            {
                sd::redact::OcrLine out;
                for (const auto& word : line.Words())
                {
                    const auto r = word.BoundingRect();
                    out.words.push_back({ std::wstring(word.Text()), RectD{ r.X / scale, r.Y / scale, r.Width / scale, r.Height / scale } });
                }
                if (!out.words.empty())
                    lines.push_back(std::move(out));
            }
        }
        catch (const winrt::hresult_error& e)
        {
            if (error)
                *error = std::wstring(e.message());
        }
        return lines;
    }

    std::vector<RectD> FindFaces(const Raster& image, std::wstring* error)
    {
        std::vector<RectD> found;
        try
        {
            if (!faces::FaceDetector::IsSupported())
            {
                if (error)
                    *error = L"face detection is not supported on this PC";
                return found;
            }
            const auto detector = faces::FaceDetector::CreateAsync().get();
            double scale = 1;
            const auto bitmap = imaging::SoftwareBitmap::Convert(ToBitmap(image, 1600, scale), imaging::BitmapPixelFormat::Gray8);
            for (const auto& face : detector.DetectFacesAsync(bitmap).get())
            {
                const auto b = face.FaceBox();
                found.push_back(RectD{ b.X / scale, b.Y / scale, b.Width / scale, b.Height / scale });
            }
        }
        catch (const winrt::hresult_error& e)
        {
            if (error)
                *error = std::wstring(e.message());
        }
        return found;
    }

    const sd::vision::Provider kProvider{ &TextAvailable, &FacesAvailable, &ReadText, &FindFaces };

    // Linked as an object library, so this initialiser always runs and plugs the provider in.
    [[maybe_unused]] const bool kRegistered = (sd::vision::RegisterProvider(&kProvider), true);
}
