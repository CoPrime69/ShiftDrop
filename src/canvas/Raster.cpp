#include "canvas/Raster.h"
#include "ErrorText.h"
#include "FileIo.h"
#include "ImageHooks.h"
#include "ImagePipeline.h"
#include "OutputPath.h"
#include "Wic.h"
#include "formats/Pixels.h"

#include <shlwapi.h>
#include <wincodec.h>
#include <wil/resource.h>
#include <wil/result_macros.h>
#include <wrl/client.h>

#include <webp/decode.h>
#include <webp/encode.h>

#include <format>
#include <initializer_list>
#include <new>

using Microsoft::WRL::ComPtr;

namespace sd::canvas
{
    namespace
    {
        std::vector<uint8_t> ReadIcc(IWICImagingFactory* factory, IWICBitmapFrameDecode* frame)
        {
            std::vector<uint8_t> bytes;
            UINT count = 0;
            if (!frame || FAILED(frame->GetColorContexts(0, nullptr, &count)) || count == 0)
                return bytes;
            std::vector<ComPtr<IWICColorContext>> contexts(count);
            std::vector<IWICColorContext*> raw(count);
            for (UINT i = 0; i < count; ++i)
            {
                if (FAILED(factory->CreateColorContext(&contexts[i])))
                    return bytes;
                raw[i] = contexts[i].Get();
            }
            if (FAILED(frame->GetColorContexts(count, raw.data(), &count)))
                return bytes;
            for (UINT i = 0; i < count; ++i)
            {
                WICColorContextType type{};
                UINT size = 0;
                if (SUCCEEDED(raw[i]->GetType(&type)) && type == WICColorContextProfile && SUCCEEDED(raw[i]->GetProfileBytes(0, nullptr, &size)) &&
                    size > 0)
                {
                    bytes.resize(size);
                    if (FAILED(raw[i]->GetProfileBytes(size, bytes.data(), &size)))
                        bytes.clear();
                    return bytes;
                }
            }
            return bytes;
        }

        HRESULT Fail(std::wstring& error, std::wstring_view what, HRESULT hr)
        {
            error = std::format(L"{}: {}", what, HResultText(hr));
            return hr;
        }

        // Straight BGRA -> 24 bpp BGR on white, rows padded to 4 bytes, for JPEG, BMP and HEIC.
        std::vector<uint8_t> FlattenOnWhite(const Raster& image, UINT& stride)
        {
            return formats::FlattenToBgr(image.px.data(), image.width, image.height, stride);
        }
    }

    bool Raster::Allocate(UINT w, UINT h)
    {
        const uint64_t bytes = uint64_t{ w } * h * 4;
        if (w == 0 || h == 0 || bytes > (uint64_t{ 1 } << 32))
            return false;
        try
        {
            px.assign(static_cast<size_t>(bytes), 0);
        }
        catch (const std::bad_alloc&)
        {
            return false;
        }
        width = w;
        height = h;
        return true;
    }

    HRESULT LoadRaster(const std::wstring& path, Raster& out, std::wstring& error, UINT maxEdge, uint64_t maxPixels)
    {
        out = {};
        auto factory = wic::Factory();
        if (!factory)
            return Fail(error, L"WIC", REGDB_E_CLASSNOTREG);

        ComPtr<IWICBitmapSource> pixels;
        ComPtr<IWICBitmapFrameDecode> frame;
        if (const auto* hook = DecodeHookFor(LowerExtension(path)))   // AVIF, SVG (src/formats): decoded upright
        {
            std::wstring why;
            if (const HRESULT hr = hook->Decode(factory.Get(), path, {}, pixels, why); FAILED(hr))
            {
                error = why.empty() ? std::format(L"decode: {}", HResultText(hr)) : L"decode: " + why;
                return hr;
            }
        }
        else if (LowerExtension(path) == L".webp")
        {
            std::vector<uint8_t> bytes;
            if (!ReadFileBytes(path, bytes))
                return Fail(error, L"read", HRESULT_FROM_WIN32(GetLastError()));
            int w = 0, h = 0;
            uint8_t* decoded = WebPDecodeBGRA(bytes.data(), bytes.size(), &w, &h);
            if (!decoded)
                return Fail(error, L"decode", WINCODEC_ERR_BADIMAGE);
            auto release = wil::scope_exit([&] { WebPFree(decoded); });
            ComPtr<IWICBitmap> bitmap;   // copies the pixels
            if (const HRESULT hr = factory->CreateBitmapFromMemory(static_cast<UINT>(w), static_cast<UINT>(h), GUID_WICPixelFormat32bppBGRA,
                                                                   static_cast<UINT>(w) * 4, static_cast<UINT>(w) * 4 * static_cast<UINT>(h), decoded, &bitmap);
                FAILED(hr))
                return Fail(error, L"decode", hr);
            pixels = bitmap;
        }
        else
        {
            ComPtr<IWICBitmapDecoder> decoder;
            if (const HRESULT hr = factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder);
                FAILED(hr))
                return Fail(error, L"decode", hr);
            if (const HRESULT hr = decoder->GetFrame(0, &frame); FAILED(hr))
                return Fail(error, L"decode", hr);
            frame->GetResolution(&out.dpiX, &out.dpiY);
            out.icc = ReadIcc(factory.Get(), frame.Get());
            pixels = frame;
        }

        UINT w = 0, h = 0;
        pixels->GetSize(&w, &h);
        if (maxEdge > 0 && std::max(w, h) > maxEdge)
        {
            const double k = static_cast<double>(maxEdge) / std::max(w, h);
            ComPtr<IWICBitmapScaler> scaler;
            if (SUCCEEDED(factory->CreateBitmapScaler(&scaler)) &&
                SUCCEEDED(scaler->Initialize(pixels.Get(), std::max(1u, static_cast<UINT>(w * k + 0.5)), std::max(1u, static_cast<UINT>(h * k + 0.5)),
                                             WICBitmapInterpolationModeHighQualityCubic)))
                pixels = scaler;
        }
        if (const auto t = wic::ExifTransform(frame.Get()); t != WICBitmapTransformRotate0)
        {
            // The flip-rotator needs random access to its source; a scaler streams, so cache it first.
            ComPtr<IWICBitmap> cached;
            ComPtr<IWICBitmapFlipRotator> rotator;
            if (SUCCEEDED(factory->CreateBitmapFromSource(pixels.Get(), WICBitmapCacheOnLoad, &cached)) &&
                SUCCEEDED(factory->CreateBitmapFlipRotator(&rotator)) && SUCCEEDED(rotator->Initialize(cached.Get(), t)))
                pixels = rotator;
        }
        if (const HRESULT hr = wic::CopyBgra(factory.Get(), pixels.Get(), out.width, out.height, out.px, maxPixels); FAILED(hr))
        {
            out = {};
            return Fail(error, L"decode", hr);
        }
        return S_OK;
    }

    const ImageFormatInfo& EditedFormatFor(const std::wstring& ext)
    {
        if (const auto* kept = KeptFormatOf(ext))   // HEIC: HEIC with Windows' HEIF encoder, else JPEG
            return *kept;
        static const wchar_t* const kCamera[] = { L".heic", L".heif", L".hif", L".avif", L".jxr", L".wdp", L".hdp", L".jxl", L".dng",
                                                  L".cr2", L".cr3", L".nef", L".arw", L".orf", L".rw2", L".raf", L".srw", L".pef" };
        for (const wchar_t* c : kCamera)
            if (ext == c)
                return *FindImageFormat("jpg");
        return *FindImageFormat("png");
    }

    int EditedQuality(const ImageFormatInfo& format)
    {
        const std::string_view id(format.id);
        return id == "jpg" ? 92 : id == "webp" ? 90 : id == "avif" ? 80 : 100;
    }

    HRESULT SaveRaster(const Raster& image, const ImageFormatInfo& format, int quality, IStream* out, std::wstring& error)
    {
        if (image.Empty() || image.px.size() < static_cast<size_t>(image.width) * image.height * 4)
            return Fail(error, L"encode", E_INVALIDARG);
        if (const auto* hook = EncodeHookFor(format.id))   // AVIF, HEIC (src/formats)
        {
            auto factory = wic::Factory();
            ComPtr<IWICBitmap> bitmap;
            if (!factory)
                return Fail(error, L"WIC", REGDB_E_CLASSNOTREG);
            // A format without alpha (HEIC) gets the picture flattened on white, as JPEG does.
            UINT stride = image.width * 4;
            std::vector<uint8_t> flat;
            if (!format.keepsAlpha)
                flat = FlattenOnWhite(image, stride);
            const auto& px = format.keepsAlpha ? image.px : flat;
            if (const HRESULT hr = factory->CreateBitmapFromMemory(image.width, image.height,
                                                                   format.keepsAlpha ? GUID_WICPixelFormat32bppBGRA : GUID_WICPixelFormat24bppBGR,
                                                                   stride, static_cast<UINT>(px.size()), const_cast<BYTE*>(px.data()), &bitmap);
                FAILED(hr))
                return Fail(error, L"encode", hr);
            std::wstring why;
            if (const HRESULT hr = hook->Encode(factory.Get(), bitmap.Get(), quality, out, why); FAILED(hr))
            {
                error = why.empty() ? std::format(L"encode: {}", HResultText(hr)) : L"encode: " + why;
                return hr;
            }
            return out->Commit(STGC_DEFAULT);
        }
        if (std::string_view(format.id) == "webp")
        {
            uint8_t* encoded = nullptr;
            const size_t size = WebPEncodeBGRA(image.px.data(), static_cast<int>(image.width), static_cast<int>(image.height),
                                               static_cast<int>(image.width) * 4, static_cast<float>(quality), &encoded);
            if (size == 0 || !encoded)
                return Fail(error, L"encode", E_FAIL);
            auto release = wil::scope_exit([&] { WebPFree(encoded); });
            ULONG written = 0;
            if (const HRESULT hr = out->Write(encoded, static_cast<ULONG>(size), &written); FAILED(hr) || written != size)
                return Fail(error, L"write", FAILED(hr) ? hr : HRESULT_FROM_WIN32(ERROR_WRITE_FAULT));
            return out->Commit(STGC_DEFAULT);
        }

        auto factory = wic::Factory();
        if (!factory)
            return Fail(error, L"WIC", REGDB_E_CLASSNOTREG);
        ComPtr<IWICBitmapEncoder> encoder;
        ComPtr<IWICBitmapFrameEncode> frame;
        ComPtr<IPropertyBag2> options;
        if (const HRESULT hr = factory->CreateEncoder(wic::ContainerFor(format.id), nullptr, &encoder); FAILED(hr))
            return Fail(error, L"encode", hr);
        if (const HRESULT hr = encoder->Initialize(out, WICBitmapEncoderNoCache); FAILED(hr))
            return Fail(error, L"encode", hr);
        if (const HRESULT hr = encoder->CreateNewFrame(&frame, &options); FAILED(hr))
            return Fail(error, L"encode", hr);
        if (format.lossy && options)
        {
            PROPBAG2 name{};
            name.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
            VARIANT value;
            VariantInit(&value);
            value.vt = VT_R4;
            value.fltVal = static_cast<float>(quality) / 100.f;
            options->Write(1, &name, &value);
        }
        if (const HRESULT hr = frame->Initialize(options.Get()); FAILED(hr))
            return Fail(error, L"encode", hr);
        frame->SetSize(image.width, image.height);
        if (image.dpiX > 0 && image.dpiY > 0)
            frame->SetResolution(image.dpiX, image.dpiY);
        if (!image.icc.empty())
        {
            // Optional: an encoder that can't hold a profile (BMP) says no, and the pixels are still right.
            ComPtr<IWICColorContext> context;
            if (SUCCEEDED(factory->CreateColorContext(&context)) &&
                SUCCEEDED(context->InitializeFromMemory(image.icc.data(), static_cast<UINT>(image.icc.size()))))
            {
                IWICColorContext* list[] = { context.Get() };
                frame->SetColorContexts(1, list);
            }
        }

        const bool alpha = format.keepsAlpha;
        UINT stride = image.width * 4;
        std::vector<uint8_t> flat;
        if (!alpha)
            flat = FlattenOnWhite(image, stride);
        const WICPixelFormatGUID wanted = alpha ? GUID_WICPixelFormat32bppBGRA : GUID_WICPixelFormat24bppBGR;
        ComPtr<IWICBitmap> bitmap;   // wraps (copies) our buffer so the encoder can convert from it
        if (const HRESULT hr = factory->CreateBitmapFromMemory(image.width, image.height, wanted, stride,
                                                               static_cast<UINT>(alpha ? image.px.size() : flat.size()),
                                                               const_cast<BYTE*>(alpha ? image.px.data() : flat.data()), &bitmap);
            FAILED(hr))
            return Fail(error, L"encode", hr);
        WICPixelFormatGUID chosen = wanted;
        if (const HRESULT hr = frame->SetPixelFormat(&chosen); FAILED(hr))
            return Fail(error, L"encode", hr);
        ComPtr<IWICBitmapSource> source = bitmap;
        if (chosen != wanted)
        {
            ComPtr<IWICFormatConverter> fit;
            if (SUCCEEDED(factory->CreateFormatConverter(&fit)) &&
                SUCCEEDED(fit->Initialize(bitmap.Get(), chosen, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeMedianCut)))
                source = fit;
        }
        if (const HRESULT hr = frame->WriteSource(source.Get(), nullptr); FAILED(hr))
            return Fail(error, L"encode", hr);
        if (const HRESULT hr = frame->Commit(); FAILED(hr))
            return Fail(error, L"encode", hr);
        if (const HRESULT hr = encoder->Commit(); FAILED(hr))
            return Fail(error, L"encode", hr);
        return out->Commit(STGC_DEFAULT);
    }

    HRESULT SaveRasterToFile(const Raster& image, const ImageFormatInfo& format, int quality, const std::wstring& path, std::wstring& error)
    {
        ComPtr<IStream> stream;
        // The path is our own staging file (StagedOutput), already created empty: open it for writing.
        if (const HRESULT hr = SHCreateStreamOnFileEx(path.c_str(), STGM_WRITE | STGM_SHARE_EXCLUSIVE | STGM_CREATE, FILE_ATTRIBUTE_NORMAL, FALSE,
                                                      nullptr, &stream);
            FAILED(hr))
            return Fail(error, L"write", hr);
        return SaveRaster(image, format, quality, stream.Get(), error);
    }

    HRESULT EncodeRaster(const Raster& image, const ImageFormatInfo& format, int quality, std::vector<uint8_t>& bytes, std::wstring& error)
    {
        bytes.clear();
        ComPtr<IStream> stream;
        if (const HRESULT hr = CreateStreamOnHGlobal(nullptr, TRUE, &stream); FAILED(hr))
            return Fail(error, L"encode", hr);
        if (const HRESULT hr = SaveRaster(image, format, quality, stream.Get(), error); FAILED(hr))
            return hr;
        // The stream's own size (its HGLOBAL can be rounded up).
        STATSTG stat{};
        if (const HRESULT hr = stream->Stat(&stat, STATFLAG_NONAME); FAILED(hr))
            return Fail(error, L"encode", hr);
        const ULARGE_INTEGER end = stat.cbSize;
        HGLOBAL memory = nullptr;
        if (const HRESULT hr = GetHGlobalFromStream(stream.Get(), &memory); FAILED(hr))
            return Fail(error, L"encode", hr);
        const auto* data = static_cast<const uint8_t*>(GlobalLock(memory));
        if (!data)
            return Fail(error, L"encode", E_OUTOFMEMORY);
        bytes.assign(data, data + static_cast<size_t>(end.QuadPart));
        GlobalUnlock(memory);
        return S_OK;
    }

    HRESULT SaveEdited(const Raster& image, const std::wstring& source, const std::wstring& target, std::wstring& error, const ImageFormatInfo** format)
    {
        const auto& edited = EditedFormatFor(LowerExtension(source));
        if (format)
            *format = &edited;
        return SaveRasterToFile(image, edited, EditedQuality(edited), target, error);
    }

    HRESULT EncodeEdited(const Raster& image, const std::wstring& source, std::vector<uint8_t>& bytes, std::wstring& error,
                         const ImageFormatInfo** format)
    {
        const auto& edited = EditedFormatFor(LowerExtension(source));
        if (format)
            *format = &edited;
        return EncodeRaster(image, edited, EditedQuality(edited), bytes, error);
    }
}
