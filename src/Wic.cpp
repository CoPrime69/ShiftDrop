#include "Wic.h"

#include <wil/result_macros.h>

#include <initializer_list>
#include <new>

using Microsoft::WRL::ComPtr;

namespace sd::wic
{
    ComPtr<IWICImagingFactory> Factory()
    {
        ComPtr<IWICImagingFactory> factory;
        CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
        return factory;
    }

    int ExifOrientation(IWICBitmapFrameDecode* frame)
    {
        ComPtr<IWICMetadataQueryReader> reader;
        if (!frame || FAILED(frame->GetMetadataQueryReader(&reader)))
            return 1;
        for (const wchar_t* query : { L"System.Photo.Orientation", L"/app1/ifd/{ushort=274}", L"/ifd/{ushort=274}" })
        {
            PROPVARIANT value;
            PropVariantInit(&value);
            const int orientation = SUCCEEDED(reader->GetMetadataByName(query, &value)) && value.vt == VT_UI2 ? value.uiVal : 0;
            PropVariantClear(&value);
            if (orientation >= 1 && orientation <= 8)
                return orientation;
        }
        return 1;
    }

    WICBitmapTransformOptions ExifTransform(IWICBitmapFrameDecode* frame)
    {
        switch (ExifOrientation(frame))
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

    GUID ContainerFor(std::string_view formatId)
    {
        if (formatId == "png") return GUID_ContainerFormatPng;
        if (formatId == "jpg") return GUID_ContainerFormatJpeg;
        if (formatId == "tiff") return GUID_ContainerFormatTiff;
        if (formatId == "bmp") return GUID_ContainerFormatBmp;
        return GUID_NULL;
    }

    HRESULT CopyBgra(IWICImagingFactory* factory, IWICBitmapSource* source, uint32_t& width, uint32_t& height, std::vector<uint8_t>& pixels,
                     uint64_t maxPixels)
    {
        ComPtr<IWICBitmapSource> bgra = source;
        WICPixelFormatGUID format{};
        RETURN_IF_FAILED(source->GetPixelFormat(&format));
        if (format != GUID_WICPixelFormat32bppBGRA)
        {
            ComPtr<IWICFormatConverter> converter;
            RETURN_IF_FAILED(factory->CreateFormatConverter(&converter));
            RETURN_IF_FAILED(converter->Initialize(source, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0.0,
                                                   WICBitmapPaletteTypeCustom));
            bgra = converter;
        }
        UINT w = 0, h = 0;
        RETURN_IF_FAILED(bgra->GetSize(&w, &h));
        if (w == 0 || h == 0)
            return E_INVALIDARG;
        const uint64_t count = uint64_t{ w } * h;
        if (count > maxPixels || count > kMaxPixels)
            return HRESULT_FROM_WIN32(ERROR_ARITHMETIC_OVERFLOW);   // refused rather than allocating gigabytes
        try
        {
            pixels.resize(static_cast<size_t>(count * 4));
        }
        catch (const std::bad_alloc&)
        {
            return E_OUTOFMEMORY;
        }
        width = w;
        height = h;
        return bgra->CopyPixels(nullptr, w * 4, static_cast<UINT>(pixels.size()), pixels.data());
    }
}
