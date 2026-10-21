#include "formats/Frames.h"
#include "ImagePipeline.h"
#include "Log.h"
#include "OutputPath.h"
#include "formats/Avif.h"

#include <shlwapi.h>
#include <wil/resource.h>
#include <webp/encode.h>
#include <webp/mux.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <format>

using Microsoft::WRL::ComPtr;

namespace sd::formats
{
    namespace
    {
        const std::wstring kCancelled = L"cancelled";

        bool IsGif(const std::wstring& ext) { return ext == L".gif"; }
        bool IsTiff(const std::wstring& ext) { return ext == L".tif" || ext == L".tiff"; }

        ComPtr<IWICImagingFactory> Factory()
        {
            ComPtr<IWICImagingFactory> factory;
            CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
            return factory;
        }

        HRESULT OpenDecoder(IWICImagingFactory* factory, const std::wstring& path, ComPtr<IWICBitmapDecoder>& decoder)
        {
            return factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder);
        }

        // A small unsigned number from a metadata query ("/imgdesc/Left"), or `fallback`.
        uint32_t Number(IWICMetadataQueryReader* reader, const wchar_t* name, uint32_t fallback)
        {
            if (!reader)
                return fallback;
            PROPVARIANT value;
            PropVariantInit(&value);
            if (FAILED(reader->GetMetadataByName(name, &value)))
                return fallback;
            uint32_t n = fallback;
            switch (value.vt)
            {
            case VT_UI1: n = value.bVal; break;
            case VT_UI2: n = value.uiVal; break;
            case VT_UI4: n = value.ulVal; break;
            case VT_BOOL: n = value.boolVal ? 1 : 0; break;
            default: break;
            }
            PropVariantClear(&value);
            return n;
        }

        ComPtr<IWICMetadataQueryReader> Metadata(IWICBitmapDecoder* decoder)
        {
            ComPtr<IWICMetadataQueryReader> reader;
            decoder->GetMetadataQueryReader(&reader);
            return reader;
        }

        ComPtr<IWICMetadataQueryReader> Metadata(IWICBitmapFrameDecode* frame)
        {
            ComPtr<IWICMetadataQueryReader> reader;
            frame->GetMetadataQueryReader(&reader);
            return reader;
        }

        // Browsers show a GIF frame with no delay, or 10 ms, for 100 ms; so do we.
        int GifDelayMs(uint32_t hundredths) { return hundredths <= 1 ? 100 : static_cast<int>(hundredths) * 10; }

        // Draws `frame` onto `canvas` at (left, top): GIF pixels are either fully see-through (the
        // canvas shows through) or fully opaque.
        void Draw(Image& canvas, const Image& frame, uint32_t left, uint32_t top)
        {
            if (left >= canvas.width)
                return;
            for (uint32_t y = 0; y < frame.height && top + y < canvas.height; ++y)
            {
                const uint8_t* s = frame.Row(y);
                uint8_t* d = canvas.Row(top + y) + static_cast<size_t>(left) * 4;
                for (uint32_t x = 0; x < frame.width && left + x < canvas.width; ++x, s += 4, d += 4)
                    if (s[3] != 0)
                        std::copy(s, s + 4, d);
            }
        }

        void Clear(Image& canvas, uint32_t left, uint32_t top, uint32_t width, uint32_t height)
        {
            for (uint32_t y = top; y < std::min(canvas.height, top + height); ++y)
            {
                uint8_t* row = canvas.Row(y);
                const uint32_t x0 = std::min(left, canvas.width), x1 = std::min(canvas.width, left + width);
                std::fill(row + static_cast<size_t>(x0) * 4, row + static_cast<size_t>(x1) * 4, uint8_t{ 0 });
            }
        }

        HRESULT WriteBytes(const std::wstring& path, const uint8_t* data, size_t size, DWORD create)
        {
            ComPtr<IStream> stream;
            RETURN_IF_FAILED(SHCreateStreamOnFileEx(path.c_str(), STGM_WRITE | STGM_SHARE_DENY_WRITE | create, FILE_ATTRIBUTE_NORMAL, TRUE,
                                                    nullptr, &stream));
            ULONG written = 0;
            RETURN_IF_FAILED(stream->Write(data, static_cast<ULONG>(size), &written));
            return written == size ? stream->Commit(STGC_DEFAULT) : HRESULT_FROM_WIN32(ERROR_WRITE_FAULT);
        }

        // Animated WebP through libwebp's WebPAnimEncoder (which also trims each frame to what changed).
        class WebPAnimation
        {
        public:
            explicit WebPAnimation(int quality) : m_quality(quality) {}
            ~WebPAnimation()
            {
                if (m_encoder)
                    WebPAnimEncoderDelete(m_encoder);
            }

            bool Add(const Image& frame, int delayMs)
            {
                if (!m_encoder)
                {
                    WebPAnimEncoderOptions options;
                    if (!WebPAnimEncoderOptionsInit(&options))
                        return false;
                    options.anim_params.loop_count = 0;   // loop for ever, as GIFs almost always do
                    m_encoder = WebPAnimEncoderNew(static_cast<int>(frame.width), static_cast<int>(frame.height), &options);
                    if (!m_encoder || !WebPConfigInit(&m_config))
                        return false;
                    m_config.quality = static_cast<float>(m_quality);
                    m_config.method = 3;   // one step faster than libwebp's default, as for still WebP
                    m_config.thread_level = 1;
                    if (!WebPValidateConfig(&m_config))
                        return false;
                }
                WebPPicture picture;
                if (!WebPPictureInit(&picture))
                    return false;
                picture.use_argb = 1;
                picture.width = static_cast<int>(frame.width);
                picture.height = static_cast<int>(frame.height);
                const bool ok = WebPPictureImportBGRA(&picture, frame.bgra.data(), static_cast<int>(frame.width) * 4) &&
                                WebPAnimEncoderAdd(m_encoder, &picture, m_timestamp, &m_config);
                WebPPictureFree(&picture);
                m_timestamp += std::max(1, delayMs);
                return ok;
            }

            bool Finish(std::vector<uint8_t>& out)
            {
                if (!m_encoder || !WebPAnimEncoderAdd(m_encoder, nullptr, m_timestamp, nullptr))
                    return false;
                WebPData data;
                WebPDataInit(&data);
                if (!WebPAnimEncoderAssemble(m_encoder, &data))
                    return false;
                out.assign(data.bytes, data.bytes + data.size);
                WebPDataClear(&data);
                return true;
            }

            std::wstring Error() const
            {
                const char* text = m_encoder ? WebPAnimEncoderGetError(m_encoder) : nullptr;
                return text && *text ? std::wstring(text, text + strlen(text)) : L"WebP animation encode failed";
            }

        private:
            int m_quality;
            WebPAnimEncoder* m_encoder = nullptr;
            WebPConfig m_config{};
            int m_timestamp = 0;
        };

        // A multi-page TIFF through WIC, one page at a time.
        class TiffPages
        {
        public:
            HRESULT Open(IWICImagingFactory* factory, const std::wstring& path)
            {
                m_factory = factory;
                RETURN_IF_FAILED(SHCreateStreamOnFileEx(path.c_str(), STGM_WRITE | STGM_SHARE_DENY_WRITE | STGM_CREATE, FILE_ATTRIBUTE_NORMAL,
                                                        TRUE, nullptr, &m_stream));
                RETURN_IF_FAILED(factory->CreateEncoder(GUID_ContainerFormatTiff, nullptr, &m_encoder));
                return m_encoder->Initialize(m_stream.Get(), WICBitmapEncoderNoCache);
            }

            HRESULT Add(const Image& page)
            {
                ComPtr<IWICBitmapFrameEncode> frame;
                ComPtr<IPropertyBag2> options;
                RETURN_IF_FAILED(m_encoder->CreateNewFrame(&frame, &options));
                RETURN_IF_FAILED(frame->Initialize(options.Get()));
                RETURN_IF_FAILED(frame->SetSize(page.width, page.height));
                WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
                RETURN_IF_FAILED(frame->SetPixelFormat(&format));
                ComPtr<IWICBitmapSource> pixels;
                RETURN_IF_FAILED(ToWicBitmap(m_factory, page, pixels));
                if (format != GUID_WICPixelFormat32bppBGRA)
                {
                    ComPtr<IWICFormatConverter> fit;
                    RETURN_IF_FAILED(m_factory->CreateFormatConverter(&fit));
                    RETURN_IF_FAILED(fit->Initialize(pixels.Get(), format, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeMedianCut));
                    pixels = fit;
                }
                RETURN_IF_FAILED(frame->WriteSource(pixels.Get(), nullptr));
                return frame->Commit();
            }

            HRESULT Finish()
            {
                RETURN_IF_FAILED(m_encoder->Commit());
                return m_stream->Commit(STGC_DEFAULT);
            }

        private:
            IWICImagingFactory* m_factory = nullptr;
            ComPtr<IStream> m_stream;
            ComPtr<IWICBitmapEncoder> m_encoder;
        };
    }

    uint32_t FrameCount(const std::wstring& path)
    {
        const auto ext = LowerExtension(path);
        if (!IsGif(ext) && !IsTiff(ext))
            return 1;
        auto factory = Factory();
        ComPtr<IWICBitmapDecoder> decoder;
        UINT count = 0;
        if (!factory || FAILED(OpenDecoder(factory.Get(), path, decoder)) || FAILED(decoder->GetFrameCount(&count)))
            return 1;
        return std::max<UINT>(count, 1);
    }

    HRESULT ForEachFrame(const std::wstring& path, std::stop_token stop, const FrameFn& each, std::wstring& error)
    {
        auto factory = Factory();
        if (!factory)
        {
            error = L"WIC unavailable";
            return REGDB_E_CLASSNOTREG;
        }
        ComPtr<IWICBitmapDecoder> decoder;
        UINT count = 0;
        HRESULT hr = OpenDecoder(factory.Get(), path, decoder);
        if (SUCCEEDED(hr))
            hr = decoder->GetFrameCount(&count);
        if (FAILED(hr) || count == 0)
        {
            error = std::format(L"decode: {}", HResultText(FAILED(hr) ? hr : WINCODEC_ERR_BADIMAGE));
            return FAILED(hr) ? hr : WINCODEC_ERR_BADIMAGE;
        }
        GUID container{};
        decoder->GetContainerFormat(&container);
        const bool gif = container == GUID_ContainerFormatGif;

        Image canvas, saved;
        if (gif)
        {
            const auto screen = Metadata(decoder.Get());
            canvas.width = Number(screen.Get(), L"/logscrdesc/Width", 0);
            canvas.height = Number(screen.Get(), L"/logscrdesc/Height", 0);
        }
        for (UINT i = 0; i < count; ++i)
        {
            if (stop.stop_requested())
                return HRESULT_FROM_WIN32(ERROR_CANCELLED);
            ComPtr<IWICBitmapFrameDecode> frame;
            Image picture;
            hr = decoder->GetFrame(i, &frame);
            if (SUCCEEDED(hr))
                hr = FromWicSource(factory.Get(), frame.Get(), picture);
            if (FAILED(hr))
            {
                error = std::format(L"frame {}: {}", i + 1, HResultText(hr));
                return hr;
            }
            if (!gif)
            {
                if (!each(picture, i, count, 0))
                    return S_OK;
                continue;
            }

            const auto meta = Metadata(frame.Get());
            const uint32_t left = Number(meta.Get(), L"/imgdesc/Left", 0), top = Number(meta.Get(), L"/imgdesc/Top", 0);
            const uint32_t disposal = Number(meta.Get(), L"/grctlext/Disposal", 0);
            const int delay = GifDelayMs(Number(meta.Get(), L"/grctlext/Delay", 0));
            if (canvas.bgra.empty())
            {
                // A screen with no stated size is as big as what is drawn on it.
                canvas.width = std::max(canvas.width, left + picture.width);
                canvas.height = std::max(canvas.height, top + picture.height);
                canvas.bgra.assign(static_cast<size_t>(canvas.width) * canvas.height * 4, 0);   // starts see-through
            }
            if (disposal == 3)
                saved = canvas;
            Draw(canvas, picture, left, top);
            if (!each(canvas, i, count, delay))
                return S_OK;
            if (disposal == 2)
                Clear(canvas, left, top, picture.width, picture.height);
            else if (disposal == 3)
                canvas = saved;
        }
        return S_OK;
    }

    FramesOutput FramesOutputFor(const std::wstring& lowerSourceExtension, std::string_view formatId)
    {
        if (IsGif(lowerSourceExtension) && (formatId == "webp" || formatId == "avif"))
            return FramesOutput::Animation;
        if (formatId == "tiff")
            return FramesOutput::Pages;
        return FramesOutput::Folder;   // TIFF pages are pages, not an animation: a WebP or AVIF each
    }

    ItemResult ConvertFrames(const Recipe& recipe, const ImageFormatInfo& format, HistoryStore* history, int64_t runId,
                             const std::wstring& source, std::stop_token stop, const std::function<void(double)>& progress)
    {
        const auto ext = LowerExtension(source);
        const FramesOutput kind = FramesOutputFor(ext, format.id);
        const int quality = recipe.steps.back().params.value("quality", 90);
        const std::wstring word = IsGif(ext) ? L"frame" : L"page";

        StagedOutput out(history, runId, source, kind == FramesOutput::Folder);
        if (!out.Ready())
            return out.Fail(out.Error());
        const auto report = [&](uint32_t index, uint32_t count) {
            if (progress)
                progress(static_cast<double>(index + 1) / count);
        };

        auto factory = Factory();
        if (!factory)
            return out.Fail(L"WIC unavailable");
        std::wstring error;
        uint32_t frames = 0;
        HRESULT failed = S_OK;
        const auto stopWith = [&](HRESULT hr, std::wstring why) {
            failed = hr;
            error = std::move(why);
            return false;
        };

        HRESULT hr = S_OK;
        if (kind == FramesOutput::Animation)
        {
            const bool webp = std::string_view(format.id) == "webp";
            WebPAnimation webpAnim(quality);
            AvifOptions options;
            options.quality = std::clamp(quality, 1, 100);
            AvifSequence avif(options);
            hr = ForEachFrame(source, stop, [&](const Image& frame, uint32_t index, uint32_t count, int delayMs) {
                if (webp ? !webpAnim.Add(frame, delayMs) : FAILED(avif.Add(frame, delayMs, error)))
                    return stopWith(E_FAIL, webp ? webpAnim.Error() : L"frame " + std::to_wstring(index + 1) + L": " + error);
                ++frames;
                report(index, count);
                return !stop.stop_requested();
            }, error);
            if (SUCCEEDED(hr) && SUCCEEDED(failed) && !stop.stop_requested())
            {
                std::vector<uint8_t> bytes;
                if (webp ? !webpAnim.Finish(bytes) : FAILED(avif.Finish(bytes, error)))
                    stopWith(E_FAIL, webp ? webpAnim.Error() : error);
                else if (const HRESULT w = WriteBytes(out.Path(), bytes.data(), bytes.size(), STGM_CREATE); FAILED(w))
                    stopWith(w, std::format(L"write: {}", HResultText(w)));
            }
        }
        else if (kind == FramesOutput::Pages)
        {
            TiffPages tiff;
            if (const HRESULT o = tiff.Open(factory.Get(), out.Path()); FAILED(o))
                return out.Fail(std::format(L"encode: {}", HResultText(o)));
            hr = ForEachFrame(source, stop, [&](const Image& frame, uint32_t index, uint32_t count, int) {
                if (const HRESULT a = tiff.Add(frame); FAILED(a))
                    return stopWith(a, std::format(L"{} {}: {}", word, index + 1, HResultText(a)));
                ++frames;
                report(index, count);
                return !stop.stop_requested();
            }, error);
            if (SUCCEEDED(hr) && SUCCEEDED(failed) && !stop.stop_requested())
                if (const HRESULT f = tiff.Finish(); FAILED(f))
                    stopWith(f, std::format(L"encode: {}", HResultText(f)));
        }
        else
        {
            const std::wstring stem = std::filesystem::path(source).stem().wstring();
            hr = ForEachFrame(source, stop, [&](const Image& frame, uint32_t index, uint32_t count, int) {
                const int digits = std::max(2, static_cast<int>(std::to_wstring(count).size()));
                const auto path = std::format(L"{}\\{} {} {:0{}}{}", out.Path(), stem, word, index + 1, digits, format.extension);
                ComPtr<IWICBitmapSource> pixels;
                ComPtr<IStream> file;
                HRESULT h = ToWicBitmap(factory.Get(), frame, pixels);
                if (SUCCEEDED(h))   // a new file in our own new staging folder: never replaces anything
                    h = SHCreateStreamOnFileEx(path.c_str(), STGM_WRITE | STGM_SHARE_DENY_WRITE | STGM_FAILIFTHERE, FILE_ATTRIBUTE_NORMAL, TRUE,
                                               nullptr, &file);
                std::wstring why;
                if (SUCCEEDED(h))
                    h = EncodePicture(pixels.Get(), format, quality, file.Get(), why);
                if (FAILED(h))
                    return stopWith(h, std::format(L"{} {}: {}", word, index + 1, why.empty() ? HResultText(h) : why));
                ++frames;
                report(index, count);
                return !stop.stop_requested();
            }, error);
        }

        if (stop.stop_requested() || hr == HRESULT_FROM_WIN32(ERROR_CANCELLED))
            return out.Fail(kCancelled, true);
        if (FAILED(hr) || FAILED(failed))
            return out.Fail(error.empty() ? HResultText(FAILED(hr) ? hr : failed) : error);

        log::Info(L"frames: {} -> {} {}s as {} ({})", source, frames, word, format.label,
                  kind == FramesOutput::Animation ? L"animated" : kind == FramesOutput::Pages ? L"multi-page" : L"folder");
        auto result = kind == FramesOutput::Folder ? out.Commit(L"frames", L"") : out.Commit(recipe.suffix, format.extension);
        if (result.ok)
        {
            if (kind == FramesOutput::Animation)
                result.message = std::format(L"Animated {}, {} frames", format.label, frames);
            else if (kind == FramesOutput::Pages)
                result.message = std::format(L"TIFF with {} {}s", frames, word);
            else
                result.message = std::format(L"{} {}s saved as {}", frames, word, format.label);
        }
        return result;
    }
}
