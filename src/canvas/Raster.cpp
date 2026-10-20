#include "canvas/Raster.h"
#include "ImagePipeline.h"
#include "OutputPath.h"

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
        ComPtr<IWICImagingFactory> Factory()
        {
            ComPtr<IWICImagingFactory> factory;
            CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
            return factory;
        }

        // EXIF orientation (1..8) as a WIC transform. Read through the metadata policy name first, then
        // the raw tag 274 paths for codecs that don't implement the policy.
        WICBitmapTransformOptions Orientation(IWICBitmapFrameDecode* frame)
        {
            ComPtr<IWICMetadataQueryReader> reader;
            if (!frame || FAILED(frame->GetMetadataQueryReader(&reader)))
                return WICBitmapTransformRotate0;
            USHORT orientation = 1;
            for (const wchar_t* query : { L"System.Photo.Orientation", L"/app1/ifd/{ushort=274}", L"/ifd/{ushort=274}" })
            {
                PROPVARIANT value;
                PropVariantInit(&value);
                const bool found = SUCCEEDED(reader->GetMetadataByName(query, &value)) && value.vt == VT_UI2;
                if (found)
                    orientation = value.uiVal;
                PropVariantClear(&value);
                if (found)
                    break;
            }
            switch (orientation)
            {
            case 2: return WICBitmapTransformFlipHorizontal;
            case 3: return WICBitmapTransformRotate180;
            case 4: return WICBitmapTransformFlipVertical;
            case 5: return static_cast<WICBitmapTransformOptions>(WICBitmapTransformRotate90 | WICBitmapTransformFlipHorizontal);
            case 6: return WICBitmapTransformRotate90;
            case 7: return static_cast<WICBitmapTransformOptions>(WICBitmapTransformRotate270 | WICBitmapTransformFlipHorizontal);
            case 8: return WICBitmapTransformRotate270;
            default: return WICBitmapTransformRotate0;
            }
        }

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

        HRESULT ReadWholeFile(const std::wstring& path, std::vector<uint8_t>& bytes)
        {
            wil::unique_hfile file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
            if (!file)
                return HRESULT_FROM_WIN32(GetLastError());
            LARGE_INTEGER size{};
            RETURN_IF_WIN32_BOOL_FALSE(GetFileSizeEx(file.get(), &size));
            if (size.QuadPart > (1ll << 31))
                return HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE);
            bytes.resize(static_cast<size_t>(size.QuadPart));
            DWORD read = 0;
            RETURN_IF_WIN32_BOOL_FALSE(ReadFile(file.get(), bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr));
            bytes.resize(read);
            return S_OK;
        }

        HRESULT CopyOut(IWICImagingFactory* factory, IWICBitmapSource* source, Raster& out)
        {
            ComPtr<IWICFormatConverter> bgra;
            RETURN_IF_FAILED(factory->CreateFormatConverter(&bgra));
            RETURN_IF_FAILED(bgra->Initialize(source, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeMedianCut));
            UINT w = 0, h = 0;
            RETURN_IF_FAILED(bgra->GetSize(&w, &h));
            if (!out.Allocate(w, h))
                return E_OUTOFMEMORY;
            return bgra->CopyPixels(nullptr, w * 4, static_cast<UINT>(out.px.size()), out.px.data());
        }

        HRESULT Fail(std::wstring& error, std::wstring_view what, HRESULT hr)
        {
            error = std::format(L"{}: {}", what, HResultText(hr));
            return hr;
        }

        // Straight BGRA -> 24 bpp BGR on white, rows padded to 4 bytes, for JPEG and BMP.
        std::vector<uint8_t> FlattenOnWhite(const Raster& image, UINT& stride)
        {
            stride = (image.width * 3 + 3) & ~3u;
            std::vector<uint8_t> out(static_cast<size_t>(stride) * image.height);
            for (UINT y = 0; y < image.height; ++y)
            {
                const uint8_t* s = image.At(0, y);
                uint8_t* d = out.data() + static_cast<size_t>(y) * stride;
                for (UINT x = 0; x < image.width; ++x, s += 4, d += 3)
                {
                    const unsigned a = s[3];
                    for (int c = 0; c < 3; ++c)
                        d[c] = static_cast<uint8_t>((s[c] * a + 255 * (255 - a) + 127) / 255);
                }
            }
            return out;
        }

        GUID ContainerFor(std::string_view id)
        {
            if (id == "png") return GUID_ContainerFormatPng;
            if (id == "jpg") return GUID_ContainerFormatJpeg;
            if (id == "tiff") return GUID_ContainerFormatTiff;
            if (id == "bmp") return GUID_ContainerFormatBmp;
            return GUID_NULL;
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

    HRESULT LoadRaster(const std::wstring& path, Raster& out, std::wstring& error, UINT maxEdge)
    {
        out = {};
        auto factory = Factory();
        if (!factory)
            return Fail(error, L"WIC", REGDB_E_CLASSNOTREG);

        ComPtr<IWICBitmapSource> pixels;
        ComPtr<IWICBitmapFrameDecode> frame;
        if (LowerExtension(path) == L".webp")
        {
            std::vector<uint8_t> bytes;
            if (const HRESULT hr = ReadWholeFile(path, bytes); FAILED(hr))
                return Fail(error, L"read", hr);
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
        if (const auto t = Orientation(frame.Get()); t != WICBitmapTransformRotate0)
        {
            // The flip-rotator needs random access to its source; a scaler streams, so cache it first.
            ComPtr<IWICBitmap> cached;
            ComPtr<IWICBitmapFlipRotator> rotator;
            if (SUCCEEDED(factory->CreateBitmapFromSource(pixels.Get(), WICBitmapCacheOnLoad, &cached)) &&
                SUCCEEDED(factory->CreateBitmapFlipRotator(&rotator)) && SUCCEEDED(rotator->Initialize(cached.Get(), t)))
                pixels = rotator;
        }
        const double dpiX = out.dpiX, dpiY = out.dpiY;
        auto icc = std::move(out.icc);
        if (const HRESULT hr = CopyOut(factory.Get(), pixels.Get(), out); FAILED(hr))
            return Fail(error, L"decode", hr);
        out.dpiX = dpiX;
        out.dpiY = dpiY;
        out.icc = std::move(icc);
        return S_OK;
    }

    const ImageFormatInfo& EditedFormatFor(const std::wstring& ext)
    {
        if (const auto* own = FormatOfExtension(ext))
            return *own;
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
        return id == "jpg" ? 92 : id == "webp" ? 90 : 100;
    }

    HRESULT SaveRaster(const Raster& image, const ImageFormatInfo& format, int quality, IStream* out, std::wstring& error)
    {
        if (image.Empty() || image.px.size() < static_cast<size_t>(image.width) * image.height * 4)
            return Fail(error, L"encode", E_INVALIDARG);
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

        auto factory = Factory();
        if (!factory)
            return Fail(error, L"WIC", REGDB_E_CLASSNOTREG);
        ComPtr<IWICBitmapEncoder> encoder;
        ComPtr<IWICBitmapFrameEncode> frame;
        ComPtr<IPropertyBag2> options;
        if (const HRESULT hr = factory->CreateEncoder(ContainerFor(format.id), nullptr, &encoder); FAILED(hr))
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
}
