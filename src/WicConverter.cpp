#include "WicConverter.h"
#include "Log.h"
#include "OutputPath.h"

#include <windows.h>
#include <propvarutil.h>
#include <wincodec.h>
#include <wil/resource.h>
#include <wil/result_macros.h>
#include <wrl/client.h>

#include <cwctype>
#include <initializer_list>
#include <format>
#include <sstream>

using Microsoft::WRL::ComPtr;

namespace sd
{
    namespace
    {
        struct Target
        {
            const wchar_t* id;
            const wchar_t* label;
            const wchar_t* extension;
            GUID container;
            bool keepsAlpha;        // false: flatten transparency onto white (JPEG has no alpha)
            const wchar_t* aliases; // source extensions that already are this format
        };

        const Target kTargets[] = {
            { L"wic.to.png", L"PNG", L".png", GUID_ContainerFormatPng, true, L".png" },
            { L"wic.to.jpg", L"JPG", L".jpg", GUID_ContainerFormatJpeg, false, L".jpg.jpeg.jpe.jfif" },
            { L"wic.to.bmp", L"BMP", L".bmp", GUID_ContainerFormatBmp, false, L".bmp.dib" },
            { L"wic.to.tiff", L"TIFF", L".tiff", GUID_ContainerFormatTiff, true, L".tif.tiff" },
        };

        const Target* FindTarget(std::wstring_view id)
        {
            for (const auto& t : kTargets)
                if (id == t.id)
                    return &t;
            return nullptr;
        }

        bool IsAlreadyTarget(const Target& t, const std::wstring& ext)
        {
            // aliases is a run of ".ext" tokens; match whole tokens only.
            std::wstring_view all(t.aliases);
            size_t pos = 0;
            while ((pos = all.find(ext, pos)) != std::wstring_view::npos)
            {
                const size_t end = pos + ext.size();
                if (end == all.size() || all[end] == L'.')
                    return true;
                pos = end;
            }
            return false;
        }

        std::wstring HrText(HRESULT hr)
        {
            wil::unique_hlocal_string msg;
            FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                           nullptr, static_cast<DWORD>(hr), 0, reinterpret_cast<LPWSTR>(&msg), 0, nullptr);
            std::wstring text = msg ? msg.get() : L"";
            while (!text.empty() && (text.back() == L'\n' || text.back() == L'\r' || text.back() == L' '))
                text.pop_back();
            return std::format(L"0x{:08X} {}", static_cast<unsigned>(hr), text);
        }

        ComPtr<IWICImagingFactory> CreateFactory()
        {
            ComPtr<IWICImagingFactory> factory;
            CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
            return factory;
        }

        // EXIF orientation (1..8). Cameras store pixels unrotated and set this tag; since we do not copy
        // metadata to the output, we must apply the rotation to the pixels or the result looks sideways.
        WICBitmapTransformOptions OrientationTransform(IWICBitmapFrameDecode* frame)
        {
            ComPtr<IWICMetadataQueryReader> reader;
            if (FAILED(frame->GetMetadataQueryReader(&reader)))   // e.g. BMP has no metadata
                return WICBitmapTransformRotate0;
            // Try the metadata *policy* name first (WIC maps it to the container's EXIF/XMP location), then
            // the raw EXIF tag 274 paths for JPEG and TIFF in case a codec does not implement the policy.
            USHORT orientation = 1;
            for (const wchar_t* query : { L"System.Photo.Orientation", L"/app1/ifd/{ushort=274}", L"/ifd/{ushort=274}" })
            {
                PROPVARIANT value;
                PropVariantInit(&value);
                const HRESULT hr = reader->GetMetadataByName(query, &value);
                const bool found = SUCCEEDED(hr) && value.vt == VT_UI2;
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

        // 32bpp BGRA (straight alpha) -> 24bpp BGR composited over white.
        HRESULT FlattenOnWhite(IWICImagingFactory* factory, IWICBitmapSource* bgra, ComPtr<IWICBitmapSource>& out)
        {
            UINT w = 0, h = 0;
            RETURN_IF_FAILED(bgra->GetSize(&w, &h));
            const UINT64 srcStride = UINT64{ w } * 4;
            const UINT64 dstStride = (UINT64{ w } * 3 + 3) & ~UINT64{ 3 };
            if (srcStride * h > 0xFFFFFFFFull)
                return HRESULT_FROM_WIN32(ERROR_ARITHMETIC_OVERFLOW);   // > 4 GB of pixels: not in M1

            std::vector<BYTE> src(static_cast<size_t>(srcStride * h));
            RETURN_IF_FAILED(bgra->CopyPixels(nullptr, static_cast<UINT>(srcStride), static_cast<UINT>(src.size()), src.data()));
            std::vector<BYTE> dst(static_cast<size_t>(dstStride * h));
            for (UINT y = 0; y < h; ++y)
            {
                const BYTE* s = src.data() + y * srcStride;
                BYTE* d = dst.data() + y * dstStride;
                for (UINT x = 0; x < w; ++x, s += 4, d += 3)
                {
                    const UINT a = s[3];
                    for (int c = 0; c < 3; ++c)
                        d[c] = static_cast<BYTE>((s[c] * a + 255 * (255 - a) + 127) / 255);
                }
            }
            ComPtr<IWICBitmap> bitmap;   // CreateBitmapFromMemory copies the buffer
            RETURN_IF_FAILED(factory->CreateBitmapFromMemory(w, h, GUID_WICPixelFormat24bppBGR, static_cast<UINT>(dstStride),
                                                             static_cast<UINT>(dst.size()), dst.data(), &bitmap));
            out = bitmap;
            return S_OK;
        }

        HRESULT Convert(const Target& target, const std::wstring& source, std::wstring& outputPath, std::stop_token stop)
        {
            auto factory = CreateFactory();
            if (!factory)
                return REGDB_E_CLASSNOTREG;

            ComPtr<IWICBitmapDecoder> decoder;
            RETURN_IF_FAILED(factory->CreateDecoderFromFilename(source.c_str(), nullptr, GENERIC_READ,
                                                                WICDecodeMetadataCacheOnDemand, &decoder));
            ComPtr<IWICBitmapFrameDecode> frame;
            RETURN_IF_FAILED(decoder->GetFrame(0, &frame));   // multi-page TIFF / animated GIF: first frame only (M1)
            if (stop.stop_requested())
                return HRESULT_FROM_WIN32(ERROR_CANCELLED);

            ComPtr<IWICBitmapSource> pixels = frame;
            if (const auto transform = OrientationTransform(frame.Get()); transform != WICBitmapTransformRotate0)
            {
                ComPtr<IWICBitmapFlipRotator> rotator;
                RETURN_IF_FAILED(factory->CreateBitmapFlipRotator(&rotator));
                RETURN_IF_FAILED(rotator->Initialize(pixels.Get(), transform));
                pixels = rotator;
            }

            // Normalise to 32bpp BGRA first; every decoder can convert to it.
            ComPtr<IWICFormatConverter> toBgra;
            RETURN_IF_FAILED(factory->CreateFormatConverter(&toBgra));
            RETURN_IF_FAILED(toBgra->Initialize(pixels.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone,
                                                nullptr, 0.0, WICBitmapPaletteTypeMedianCut));
            pixels = toBgra;
            WICPixelFormatGUID wanted = GUID_WICPixelFormat32bppBGRA;
            if (!target.keepsAlpha)
            {
                RETURN_IF_FAILED(FlattenOnWhite(factory.Get(), pixels.Get(), pixels));
                wanted = GUID_WICPixelFormat24bppBGR;
            }
            if (stop.stop_requested())
                return HRESULT_FROM_WIN32(ERROR_CANCELLED);

            ComPtr<IStream> stream;
            RETURN_IF_FAILED(CreateUniqueOutput(source, target.extension, outputPath, stream));
            bool committed = false;
            auto cleanup = wil::scope_exit([&] {
                if (!committed)
                {
                    stream.Reset();   // close the handle before deleting the half-written file
                    DeleteFileW(outputPath.c_str());
                }
            });

            ComPtr<IWICBitmapEncoder> encoder;
            RETURN_IF_FAILED(factory->CreateEncoder(target.container, nullptr, &encoder));
            RETURN_IF_FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache));
            ComPtr<IWICBitmapFrameEncode> out;
            ComPtr<IPropertyBag2> options;
            RETURN_IF_FAILED(encoder->CreateNewFrame(&out, &options));
            if (target.container == GUID_ContainerFormatJpeg && options)
            {
                PROPBAG2 name{};
                name.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
                VARIANT quality;
                VariantInit(&quality);
                quality.vt = VT_R4;
                quality.fltVal = 0.92f;
                options->Write(1, &name, &quality);
            }
            RETURN_IF_FAILED(out->Initialize(options.Get()));

            UINT w = 0, h = 0;
            RETURN_IF_FAILED(pixels->GetSize(&w, &h));
            RETURN_IF_FAILED(out->SetSize(w, h));
            double dpiX = 96, dpiY = 96;
            if (SUCCEEDED(frame->GetResolution(&dpiX, &dpiY)) && dpiX > 0 && dpiY > 0)
                out->SetResolution(dpiX, dpiY);

            // The encoder may pick a different format than we ask for; convert to whatever it chose.
            WICPixelFormatGUID chosen = wanted;
            RETURN_IF_FAILED(out->SetPixelFormat(&chosen));
            if (chosen != wanted)
            {
                ComPtr<IWICFormatConverter> fit;
                RETURN_IF_FAILED(factory->CreateFormatConverter(&fit));
                RETURN_IF_FAILED(fit->Initialize(pixels.Get(), chosen, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeMedianCut));
                pixels = fit;
            }
            RETURN_IF_FAILED(out->WriteSource(pixels.Get(), nullptr));
            RETURN_IF_FAILED(out->Commit());
            RETURN_IF_FAILED(encoder->Commit());
            RETURN_IF_FAILED(stream->Commit(STGC_DEFAULT));
            committed = true;
            return S_OK;
        }
    }

    WicConverter::WicConverter()
    {
        auto factory = CreateFactory();
        if (!factory)
        {
            m_summary = L"WIC unavailable";
            return;
        }
        std::wstringstream decoders, encoders;
        auto enumerate = [&](DWORD type, std::wstringstream& names, bool collect) {
            ComPtr<IEnumUnknown> items;
            if (FAILED(factory->CreateComponentEnumerator(type, WICComponentEnumerateDefault, &items)))
                return;
            ComPtr<IUnknown> item;
            while (items->Next(1, &item, nullptr) == S_OK)
            {
                ComPtr<IWICBitmapCodecInfo> info;
                if (SUCCEEDED(item.As(&info)))
                {
                    UINT len = 0;
                    info->GetFriendlyName(0, nullptr, &len);
                    std::wstring name(len, L'\0');
                    if (len)
                        info->GetFriendlyName(len, name.data(), &len);
                    if (!name.empty() && name.back() == L'\0')
                        name.pop_back();
                    UINT extLen = 0;
                    info->GetFileExtensions(0, nullptr, &extLen);
                    std::wstring exts(extLen, L'\0');
                    if (extLen)
                        info->GetFileExtensions(extLen, exts.data(), &extLen);
                    if (!exts.empty() && exts.back() == L'\0')
                        exts.pop_back();
                    names << L"\n    " << name << L" [" << exts << L"]";
                    if (collect)
                    {
                        // GetFileExtensions returns a list like ".jpeg,.jpe,.jpg". Windows separates with
                        // commas, but some codecs (and Wine) use semicolons, so accept both.
                        std::wstring ext;
                        for (const wchar_t ch : exts + L",")
                        {
                            if (ch == L',' || ch == L';')
                            {
                                if (!ext.empty())
                                    m_decodable.insert(ext);
                                ext.clear();
                            }
                            else if (ch != L' ')
                            {
                                ext.push_back(static_cast<wchar_t>(std::towlower(ch)));
                            }
                        }
                    }
                }
                item.Reset();
            }
        };
        enumerate(WICDecoder, decoders, true);
        enumerate(WICEncoder, encoders, false);
        m_summary = L"decoders:" + decoders.str() + L"\n  encoders:" + encoders.str();
    }

    bool WicConverter::supports(std::wstring_view extension) const
    {
        return m_decodable.contains(std::wstring(extension));
    }

    std::vector<Action> WicConverter::actions() const
    {
        std::vector<Action> list;
        for (const auto& t : kTargets)
            list.push_back(Action{ t.id, t.label, t.extension });
        return list;
    }

    JobResult WicConverter::run(const Job& job, const ProgressFn& progress, std::stop_token stop) const
    {
        JobResult result;
        const auto* target = FindTarget(job.actionId);
        if (!target)
        {
            result.error = L"unknown action " + job.actionId;
            return result;
        }
        const auto ext = LowerExtension(job.source);
        if (IsAlreadyTarget(*target, ext))
        {
            result.skipped = true;
            result.error = std::format(L"already {}", target->label);
            return result;
        }
        if (!supports(ext))
        {
            result.error = std::format(L"no WIC decoder for '{}' on this PC", ext);
            return result;
        }
        if (progress)
            progress(0.0);
        const HRESULT hr = Convert(*target, job.source, result.output, stop);
        if (progress)
            progress(1.0);
        result.ok = SUCCEEDED(hr);
        if (!result.ok)
        {
            result.error = HrText(hr);
            result.output.clear();
        }
        return result;
    }

    bool IsAlreadyTargetFormat(std::wstring_view actionId, const std::wstring& ext)
    {
        const auto* t = FindTarget(actionId);
        return t && IsAlreadyTarget(*t, ext);
    }
}
