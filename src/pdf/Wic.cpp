#include "pdf/Wic.h"
#include "../Wic.h"   // the core WIC helpers, not this folder's pdf/Wic.h
#include "canvas/Raster.h"

#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>

namespace sd::pdf
{
    using Microsoft::WRL::ComPtr;

    namespace
    {
        // Reads all of an HGLOBAL-backed stream into a string.
        HRESULT ReadBack(IStream* stream, std::string& out)
        {
            STATSTG stat{};
            HRESULT hr = stream->Stat(&stat, STATFLAG_NONAME);
            if (FAILED(hr))
                return hr;
            out.resize(static_cast<size_t>(stat.cbSize.QuadPart));
            const LARGE_INTEGER zero{};
            hr = stream->Seek(zero, STREAM_SEEK_SET, nullptr);
            ULONG read = 0;
            if (SUCCEEDED(hr) && !out.empty())
                hr = stream->Read(out.data(), static_cast<ULONG>(out.size()), &read);
            if (SUCCEEDED(hr) && read != out.size())
                hr = E_FAIL;
            return hr;
        }

        WICPixelFormatGUID FormatOf(const Samples& s) { return s.channels == 1 ? GUID_WICPixelFormat8bppGray : GUID_WICPixelFormat24bppBGR; }

        // PDF stores RGB, WIC wants BGR: the same swap both ways.
        void SwapRedBlue(Samples& s)
        {
            if (s.channels != 3)
                return;
            for (size_t i = 0; i + 2 < s.data.size(); i += 3)
                std::swap(s.data[i], s.data[i + 2]);
        }

        HRESULT MemoryBitmap(IWICImagingFactory* factory, Samples& s, ComPtr<IWICBitmap>& bitmap)
        {
            const UINT stride = s.width * s.channels;
            return factory->CreateBitmapFromMemory(s.width, s.height, FormatOf(s), stride, static_cast<UINT>(s.data.size()), s.data.data(), &bitmap);
        }
    }

    bool DecodeImageFile(const std::wstring& path, Bitmap& out, std::wstring& error)
    {
        canvas::Raster picture;
        // Over 400 megapixels is refused rather than allocating gigabytes.
        if (FAILED(canvas::LoadRaster(path, picture, error, 0, 400'000'000)))
            return false;
        out.width = picture.width;
        out.height = picture.height;
        out.bgra = std::move(picture.px);
        return true;
    }

    bool ReadResolution(const std::wstring& path, double& dpiX, double& dpiY)
    {
        auto factory = wic::Factory();
        ComPtr<IWICBitmapDecoder> decoder;
        ComPtr<IWICBitmapFrameDecode> frame;
        if (!factory || FAILED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder)) ||
            FAILED(decoder->GetFrame(0, &frame)) || FAILED(frame->GetResolution(&dpiX, &dpiY)))
            return false;
        return dpiX > 1 && dpiY > 1;
    }

    int ReadOrientation(const std::wstring& path)
    {
        auto factory = wic::Factory();
        ComPtr<IWICBitmapDecoder> decoder;
        ComPtr<IWICBitmapFrameDecode> frame;
        if (!factory || FAILED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder)) ||
            FAILED(decoder->GetFrame(0, &frame)))
            return 1;
        return wic::ExifOrientation(frame.Get());
    }

    HRESULT EncodeImageFile(const Bitmap& image, const std::wstring& path, bool jpeg, int quality, double dpi)
    {
        if (image.Empty())
            return E_INVALIDARG;
        auto factory = wic::Factory();
        if (!factory)
            return E_FAIL;
        ComPtr<IWICBitmap> bitmap;
        ComPtr<IWICFormatConverter> bgr;
        ComPtr<IWICStream> stream;
        ComPtr<IWICBitmapEncoder> encoder;
        ComPtr<IWICBitmapFrameEncode> frame;
        ComPtr<IPropertyBag2> props;
        HRESULT hr = factory->CreateBitmapFromMemory(image.width, image.height, GUID_WICPixelFormat32bppBGRA, image.width * 4,
                                                     static_cast<UINT>(image.bgra.size()), const_cast<BYTE*>(image.bgra.data()), &bitmap);
        // Rendered pages are opaque: 24-bit output is smaller and opens everywhere.
        if (SUCCEEDED(hr)) hr = factory->CreateFormatConverter(&bgr);
        if (SUCCEEDED(hr)) hr = bgr->Initialize(bitmap.Get(), GUID_WICPixelFormat24bppBGR, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom);
        if (SUCCEEDED(hr)) hr = factory->CreateStream(&stream);
        if (SUCCEEDED(hr)) hr = stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE);
        if (SUCCEEDED(hr)) hr = factory->CreateEncoder(jpeg ? GUID_ContainerFormatJpeg : GUID_ContainerFormatPng, nullptr, &encoder);
        if (SUCCEEDED(hr)) hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
        if (SUCCEEDED(hr)) hr = encoder->CreateNewFrame(&frame, &props);
        if (SUCCEEDED(hr) && jpeg && props)
        {
            PROPBAG2 option{};
            option.pstrName = const_cast<wchar_t*>(L"ImageQuality");
            VARIANT value;
            VariantInit(&value);
            value.vt = VT_R4;
            value.fltVal = static_cast<float>(std::clamp(quality, 1, 100)) / 100.0f;
            hr = props->Write(1, &option, &value);
        }
        if (SUCCEEDED(hr)) hr = frame->Initialize(props.Get());
        if (SUCCEEDED(hr)) hr = frame->SetSize(image.width, image.height);
        WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
        if (SUCCEEDED(hr)) hr = frame->SetPixelFormat(&format);
        if (SUCCEEDED(hr) && dpi > 0) frame->SetResolution(dpi, dpi);   // optional; some encoders ignore it
        if (SUCCEEDED(hr)) hr = frame->WriteSource(bgr.Get(), nullptr);
        if (SUCCEEDED(hr)) hr = frame->Commit();
        if (SUCCEEDED(hr)) hr = encoder->Commit();
        return hr;
    }

    HRESULT DecodeJpeg(const std::string& bytes, Samples& out)
    {
        auto factory = wic::Factory();
        if (!factory || bytes.empty())
            return E_FAIL;
        ComPtr<IWICStream> stream;
        ComPtr<IWICBitmapDecoder> decoder;
        ComPtr<IWICBitmapFrameDecode> frame;
        ComPtr<IWICFormatConverter> converter;
        HRESULT hr = factory->CreateStream(&stream);
        if (SUCCEEDED(hr)) hr = stream->InitializeFromMemory(reinterpret_cast<BYTE*>(const_cast<char*>(bytes.data())), static_cast<DWORD>(bytes.size()));
        if (SUCCEEDED(hr)) hr = factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder);
        if (SUCCEEDED(hr)) hr = decoder->GetFrame(0, &frame);
        WICPixelFormatGUID native{};
        if (SUCCEEDED(hr)) hr = frame->GetPixelFormat(&native);
        Samples s;
        s.channels = SUCCEEDED(hr) && native == GUID_WICPixelFormat8bppGray ? 1 : 3;
        if (SUCCEEDED(hr)) hr = factory->CreateFormatConverter(&converter);
        if (SUCCEEDED(hr)) hr = converter->Initialize(frame.Get(), FormatOf(s), WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom);
        if (SUCCEEDED(hr)) hr = converter->GetSize(&s.width, &s.height);
        if (SUCCEEDED(hr))
        {
            s.data.resize(static_cast<size_t>(s.width) * s.height * s.channels);
            hr = converter->CopyPixels(nullptr, s.width * s.channels, static_cast<UINT>(s.data.size()), s.data.data());
        }
        if (SUCCEEDED(hr))
        {
            SwapRedBlue(s);
            out = std::move(s);
        }
        return hr;
    }

    HRESULT EncodeJpeg(const Samples& in, int quality, std::string& out)
    {
        auto factory = wic::Factory();
        if (!factory || (in.channels != 1 && in.channels != 3) || in.data.size() != static_cast<size_t>(in.width) * in.height * in.channels)
            return E_INVALIDARG;
        Samples bgr = in;
        SwapRedBlue(bgr);
        ComPtr<IWICBitmap> bitmap;
        ComPtr<IStream> stream;
        ComPtr<IWICBitmapEncoder> encoder;
        ComPtr<IWICBitmapFrameEncode> frame;
        ComPtr<IPropertyBag2> props;
        HRESULT hr = MemoryBitmap(factory.Get(), bgr, bitmap);
        if (SUCCEEDED(hr)) hr = CreateStreamOnHGlobal(nullptr, TRUE, &stream);
        if (SUCCEEDED(hr)) hr = factory->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &encoder);
        if (SUCCEEDED(hr)) hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
        if (SUCCEEDED(hr)) hr = encoder->CreateNewFrame(&frame, &props);
        if (SUCCEEDED(hr) && props)
        {
            PROPBAG2 option{};
            option.pstrName = const_cast<wchar_t*>(L"ImageQuality");
            VARIANT value;
            VariantInit(&value);
            value.vt = VT_R4;
            value.fltVal = static_cast<float>(std::clamp(quality, 1, 100)) / 100.0f;
            hr = props->Write(1, &option, &value);
        }
        if (SUCCEEDED(hr)) hr = frame->Initialize(props.Get());
        if (SUCCEEDED(hr)) hr = frame->SetSize(in.width, in.height);
        WICPixelFormatGUID format = FormatOf(in);
        if (SUCCEEDED(hr)) hr = frame->SetPixelFormat(&format);
        if (SUCCEEDED(hr) && format != FormatOf(in))
            hr = E_FAIL;   // the encoder would convert; not expected for gray or BGR
        if (SUCCEEDED(hr)) hr = frame->WriteSource(bitmap.Get(), nullptr);
        if (SUCCEEDED(hr)) hr = frame->Commit();
        if (SUCCEEDED(hr)) hr = encoder->Commit();
        if (SUCCEEDED(hr)) hr = ReadBack(stream.Get(), out);
        return hr;
    }

    HRESULT EncodePng(const Samples& in, std::string& out)
    {
        auto factory = wic::Factory();
        if (!factory || (in.channels != 1 && in.channels != 3 && in.channels != 4) || in.width == 0 || in.height == 0 ||
            in.data.size() != static_cast<size_t>(in.width) * in.height * in.channels)
            return E_INVALIDARG;
        Samples bgr = in;
        if (bgr.channels == 4)
            for (size_t i = 0; i + 3 < bgr.data.size(); i += 4)
                std::swap(bgr.data[i], bgr.data[i + 2]);
        else
            SwapRedBlue(bgr);
        WICPixelFormatGUID format = bgr.channels == 4 ? GUID_WICPixelFormat32bppBGRA : FormatOf(bgr);
        const WICPixelFormatGUID wanted = format;
        ComPtr<IWICBitmap> bitmap;
        ComPtr<IStream> stream;
        ComPtr<IWICBitmapEncoder> encoder;
        ComPtr<IWICBitmapFrameEncode> frame;
        HRESULT hr = factory->CreateBitmapFromMemory(bgr.width, bgr.height, format, bgr.width * bgr.channels, static_cast<UINT>(bgr.data.size()),
                                                     bgr.data.data(), &bitmap);
        if (SUCCEEDED(hr)) hr = CreateStreamOnHGlobal(nullptr, TRUE, &stream);
        if (SUCCEEDED(hr)) hr = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
        if (SUCCEEDED(hr)) hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
        if (SUCCEEDED(hr)) hr = encoder->CreateNewFrame(&frame, nullptr);
        if (SUCCEEDED(hr)) hr = frame->Initialize(nullptr);
        if (SUCCEEDED(hr)) hr = frame->SetSize(bgr.width, bgr.height);
        if (SUCCEEDED(hr)) hr = frame->SetPixelFormat(&format);
        if (SUCCEEDED(hr) && format != wanted)
            hr = E_FAIL;   // the PNG encoder takes gray, BGR and BGRA as they are
        if (SUCCEEDED(hr)) hr = frame->WriteSource(bitmap.Get(), nullptr);
        if (SUCCEEDED(hr)) hr = frame->Commit();
        if (SUCCEEDED(hr)) hr = encoder->Commit();
        if (SUCCEEDED(hr)) hr = ReadBack(stream.Get(), out);
        return hr;
    }

    HRESULT ResizeSamples(Samples& samples, uint32_t width, uint32_t height)
    {
        auto factory = wic::Factory();
        if (!factory || width == 0 || height == 0)
            return E_INVALIDARG;
        ComPtr<IWICBitmap> bitmap;
        ComPtr<IWICBitmapScaler> scaler;
        HRESULT hr = MemoryBitmap(factory.Get(), samples, bitmap);
        if (SUCCEEDED(hr)) hr = factory->CreateBitmapScaler(&scaler);
        if (SUCCEEDED(hr)) hr = scaler->Initialize(bitmap.Get(), width, height, WICBitmapInterpolationModeHighQualityCubic);
        Samples out;
        out.width = width;
        out.height = height;
        out.channels = samples.channels;
        out.data.resize(static_cast<size_t>(width) * height * samples.channels);
        if (SUCCEEDED(hr)) hr = scaler->CopyPixels(nullptr, width * samples.channels, static_cast<UINT>(out.data.size()), out.data.data());
        if (SUCCEEDED(hr))
            samples = std::move(out);
        return hr;
    }
}
