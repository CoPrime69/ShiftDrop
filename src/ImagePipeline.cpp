#include "ImagePipeline.h"
#include "ErrorText.h"
#include "FileIo.h"
#include "ImageHooks.h"
#include "Log.h"
#include "OutputPath.h"
#include "PathUtil.h"

#include <propvarutil.h>
#include <shlwapi.h>
#include <wincodec.h>
#include <wil/resource.h>
#include <wil/result_macros.h>
#include <wrl/client.h>

#include <webp/decode.h>
#include <webp/encode.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <cwctype>
#include <format>
#include <initializer_list>
#include <sstream>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace sd
{
    namespace
    {
        ComPtr<IWICImagingFactory> CreateFactory()
        {
            ComPtr<IWICImagingFactory> factory;
            CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
            return factory;
        }

        GUID ContainerFor(std::string_view format)
        {
            if (format == "png") return GUID_ContainerFormatPng;
            if (format == "jpg") return GUID_ContainerFormatJpeg;
            if (format == "tiff") return GUID_ContainerFormatTiff;
            if (format == "bmp") return GUID_ContainerFormatBmp;
            return GUID_NULL;   // webp: encoded with libwebp, not WIC
        }

        // EXIF orientation (1..8). Cameras store pixels unrotated and set this tag; since metadata is
        // not carried to the output, the rotation must be applied to the pixels or the result looks sideways.
        WICBitmapTransformOptions OrientationTransform(IWICBitmapFrameDecode* frame)
        {
            ComPtr<IWICMetadataQueryReader> reader;
            if (!frame || FAILED(frame->GetMetadataQueryReader(&reader)))   // e.g. BMP has no metadata
                return WICBitmapTransformRotate0;
            // The metadata *policy* name first (WIC maps it to the container's EXIF/XMP location), then the
            // raw EXIF tag 274 paths for JPEG and TIFF in case a codec does not implement the policy.
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

        // libwebp decode -> WIC bitmap (32bpp BGRA), so the rest of the pipeline is format-agnostic.
        HRESULT DecodeWebP(IWICImagingFactory* factory, const std::wstring& path, ComPtr<IWICBitmapSource>& out)
        {
            std::vector<uint8_t> bytes;
            if (!ReadFileBytes(path, bytes))
                return HRESULT_FROM_WIN32(GetLastError());
            int w = 0, h = 0;
            uint8_t* pixels = WebPDecodeBGRA(bytes.data(), bytes.size(), &w, &h);
            if (!pixels)
                return WINCODEC_ERR_BADIMAGE;
            auto release = wil::scope_exit([&] { WebPFree(pixels); });
            ComPtr<IWICBitmap> bitmap;
            RETURN_IF_FAILED(factory->CreateBitmapFromMemory(static_cast<UINT>(w), static_cast<UINT>(h), GUID_WICPixelFormat32bppBGRA,
                                                             static_cast<UINT>(w) * 4, static_cast<UINT>(w) * 4 * static_cast<UINT>(h),
                                                             pixels, &bitmap));
            out = bitmap;
            return S_OK;
        }

        HRESULT ToBgra(IWICImagingFactory* factory, IWICBitmapSource* src, ComPtr<IWICBitmapSource>& out)
        {
            ComPtr<IWICFormatConverter> conv;
            RETURN_IF_FAILED(factory->CreateFormatConverter(&conv));
            RETURN_IF_FAILED(conv->Initialize(src, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0.0,
                                              WICBitmapPaletteTypeMedianCut));
            out = conv;
            return S_OK;
        }

        // A rotation by 90 or 270 degrees (mirrored or not): width and height swap.
        bool Turns(WICBitmapTransformOptions t)
        {
            const int turn = t & (WICBitmapTransformRotate90 | WICBitmapTransformRotate180 | WICBitmapTransformRotate270);
            return turn == WICBitmapTransformRotate90 || turn == WICBitmapTransformRotate270;
        }

        // Wraps src in a flip-rotator. Anything but a mirror left to right reads the source out of
        // order (a column per output row when it turns), and a decoder or scaler streams: asked for
        // rows out of order it starts over each time (minutes for a 12 MP JPEG on Windows). So the
        // picture is decoded into memory first, once, as src/canvas/Raster.cpp does.
        HRESULT FlipRotate(IWICImagingFactory* factory, IWICBitmapSource* src, WICBitmapTransformOptions t, ComPtr<IWICBitmapSource>& out)
        {
            ComPtr<IWICBitmapSource> source = src;
            if (ComPtr<IWICBitmap> bitmap; t != WICBitmapTransformFlipHorizontal && FAILED(src->QueryInterface(IID_PPV_ARGS(&bitmap))))
            {
                RETURN_IF_FAILED(factory->CreateBitmapFromSource(src, WICBitmapCacheOnLoad, &bitmap));
                source = bitmap;
            }
            ComPtr<IWICBitmapFlipRotator> rotator;
            RETURN_IF_FAILED(factory->CreateBitmapFlipRotator(&rotator));
            RETURN_IF_FAILED(rotator->Initialize(source.Get(), t));
            out = rotator;
            return S_OK;
        }

        // The size an image.resize or image.scale step makes of a w x h picture; false when it leaves
        // the size alone (neither ever enlarges).
        bool ShrunkSize(const RecipeStep& step, UINT w, UINT h, UINT& nw, UINT& nh)
        {
            double k = 1.0;
            if (step.action == "image.resize")
            {
                const UINT maxEdge = step.params["maxEdge"].get<UINT>();
                if (std::max(w, h) <= maxEdge)
                    return false;
                k = static_cast<double>(maxEdge) / std::max(w, h);
            }
            else if (step.action == "image.scale")
            {
                k = step.params["percent"].get<int>() / 100.0;
                if (k >= 1.0)
                    return false;
            }
            else
                return false;
            nw = std::max(1u, static_cast<UINT>(w * k + 0.5));
            nh = std::max(1u, static_cast<UINT>(h * k + 0.5));
            return true;
        }

        // The size of the recipe's first shrink of an upright w x h picture, turned to the way the
        // file stores it (`storedSideways`: EXIF orientation 5 to 8). False when nothing shrinks it.
        bool FirstShrink(const Recipe& recipe, UINT w, UINT h, bool storedSideways, UINT& tw, UINT& th)
        {
            bool sideways = storedSideways;
            for (const auto& step : recipe.steps)
            {
                if (ShrunkSize(step, w, h, tw, th))
                {
                    if (sideways)
                        std::swap(tw, th);
                    return true;
                }
                if (step.action == "image.rotate" && step.params["degrees"].get<std::string>() != "180")
                {
                    std::swap(w, h);
                    sideways = !sideways;
                }
            }
            return false;
        }

        // True when the source's pixel format has no alpha (a camera JPEG, a 24-bit PNG): such a
        // picture goes to the encoder as 24-bit BGR, with no 32-bit copy to flatten on white. A
        // palette format (GIF, 8-bit PNG) never counts: its transparency is in the palette, which
        // SupportsTransparency does not look at.
        bool IsOpaque(IWICImagingFactory* factory, IWICBitmapSource* src)
        {
            WICPixelFormatGUID format{};
            ComPtr<IWICComponentInfo> info;
            ComPtr<IWICPixelFormatInfo2> pixel;
            BOOL transparency = TRUE;
            if (FAILED(src->GetPixelFormat(&format)) || format == GUID_WICPixelFormat1bppIndexed || format == GUID_WICPixelFormat2bppIndexed ||
                format == GUID_WICPixelFormat4bppIndexed || format == GUID_WICPixelFormat8bppIndexed)
                return false;
            return SUCCEEDED(factory->CreateComponentInfo(format, &info)) &&
                   SUCCEEDED(info.As(&pixel)) && SUCCEEDED(pixel->SupportsTransparency(&transparency)) && !transparency;
        }

        HRESULT ToBgr(IWICImagingFactory* factory, IWICBitmapSource* src, ComPtr<IWICBitmapSource>& out)
        {
            ComPtr<IWICFormatConverter> conv;
            RETURN_IF_FAILED(factory->CreateFormatConverter(&conv));
            RETURN_IF_FAILED(conv->Initialize(src, GUID_WICPixelFormat24bppBGR, WICBitmapDitherTypeNone, nullptr, 0.0,
                                              WICBitmapPaletteTypeMedianCut));
            out = conv;
            return S_OK;
        }

        // A decoder that can scale while it decodes (WIC's JPEG decoder does it in the DCT, by 1/2, 1/4
        // or 1/8) makes a smaller picture far faster than decoding every pixel and scaling down. Picks
        // the smallest such size that is still at least minW x minH, so the scaler after it makes the
        // final size exactly as before. S_FALSE (and `out` untouched) when there is none.
        HRESULT DecodeScaled(IWICImagingFactory* factory, IWICBitmapFrameDecode* frame, UINT minW, UINT minH, ComPtr<IWICBitmapSource>& out)
        {
            ComPtr<IWICBitmapSourceTransform> transform;
            UINT w = 0, h = 0;
            if (FAILED(frame->QueryInterface(IID_PPV_ARGS(&transform))) || FAILED(frame->GetSize(&w, &h)) || minW == 0 || minH == 0)
                return S_FALSE;
            for (const UINT d : { 8u, 4u, 2u })
            {
                UINT sw = (w + d - 1) / d, sh = (h + d - 1) / d;
                if (sw < minW || sh < minH || FAILED(transform->GetClosestSize(&sw, &sh)) || sw < minW || sh < minH || sw >= w || sh >= h)
                    continue;
                WICPixelFormatGUID format{};
                RETURN_IF_FAILED(frame->GetPixelFormat(&format));
                RETURN_IF_FAILED(transform->GetClosestPixelFormat(&format));
                ComPtr<IWICBitmap> bitmap;
                RETURN_IF_FAILED(factory->CreateBitmap(sw, sh, format, WICBitmapCacheOnLoad, &bitmap));
                ComPtr<IWICBitmapLock> lock;
                const WICRect all{ 0, 0, static_cast<INT>(sw), static_cast<INT>(sh) };
                RETURN_IF_FAILED(bitmap->Lock(&all, WICBitmapLockWrite, &lock));
                UINT stride = 0, size = 0;
                BYTE* data = nullptr;
                RETURN_IF_FAILED(lock->GetStride(&stride));
                RETURN_IF_FAILED(lock->GetDataPointer(&size, &data));
                RETURN_IF_FAILED(transform->CopyPixels(nullptr, sw, sh, &format, WICBitmapTransformRotate0, stride, size, data));
                lock.Reset();
                out = bitmap;
                return S_OK;
            }
            return S_FALSE;
        }

        // Pulls BGRA pixels out of the chain. This is where the lazy IWICBitmapSource chain actually runs.
        HRESULT CopyBgra(IWICBitmapSource* bgra, UINT& w, UINT& h, std::vector<BYTE>& pixels)
        {
            RETURN_IF_FAILED(bgra->GetSize(&w, &h));
            const UINT64 stride = UINT64{ w } * 4;
            if (stride * h > 0xFFFFFFFFull)
                return HRESULT_FROM_WIN32(ERROR_ARITHMETIC_OVERFLOW);   // > 4 GB of pixels
            pixels.resize(static_cast<size_t>(stride * h));
            return bgra->CopyPixels(nullptr, static_cast<UINT>(stride), static_cast<UINT>(pixels.size()), pixels.data());
        }

        // 32bpp BGRA (straight alpha) -> 24bpp BGR composited over white, for formats without alpha.
        HRESULT FlattenOnWhite(IWICImagingFactory* factory, IWICBitmapSource* bgra, ComPtr<IWICBitmapSource>& out)
        {
            UINT w = 0, h = 0;
            std::vector<BYTE> src;
            RETURN_IF_FAILED(CopyBgra(bgra, w, h, src));
            const UINT64 dstStride = (UINT64{ w } * 3 + 3) & ~UINT64{ 3 };
            std::vector<BYTE> dst(static_cast<size_t>(dstStride * h));
            for (UINT y = 0; y < h; ++y)
            {
                const BYTE* s = src.data() + size_t{ y } * w * 4;
                BYTE* d = dst.data() + size_t{ y } * dstStride;
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

        // libwebp's default preset at `quality` (what WebPEncodeBGRA uses), allowed a second thread
        // for its analysis pass and the alpha plane, which leaves the result the same. Takes 32-bit
        // BGRA, or 24-bit BGR for an opaque picture (a quarter less to copy, no alpha to look for).
        HRESULT EncodeWebP(IWICBitmapSource* src, float quality, IStream* out)
        {
            WICPixelFormatGUID format{};
            RETURN_IF_FAILED(src->GetPixelFormat(&format));
            const bool bgr = format == GUID_WICPixelFormat24bppBGR;
            UINT w = 0, h = 0;
            RETURN_IF_FAILED(src->GetSize(&w, &h));
            const UINT64 stride = UINT64{ w } * (bgr ? 3 : 4);
            if (stride * h > 0xFFFFFFFFull || w > WEBP_MAX_DIMENSION || h > WEBP_MAX_DIMENSION)
                return HRESULT_FROM_WIN32(ERROR_ARITHMETIC_OVERFLOW);
            std::vector<BYTE> pixels(static_cast<size_t>(stride * h));
            RETURN_IF_FAILED(src->CopyPixels(nullptr, static_cast<UINT>(stride), static_cast<UINT>(pixels.size()), pixels.data()));

            WebPConfig config;
            WebPPicture picture;
            if (!WebPConfigPreset(&config, WEBP_PRESET_DEFAULT, quality) || !WebPPictureInit(&picture))
                return E_FAIL;
            config.thread_level = 1;
            picture.width = static_cast<int>(w);
            picture.height = static_cast<int>(h);
            WebPMemoryWriter memory;
            WebPMemoryWriterInit(&memory);
            picture.writer = WebPMemoryWrite;
            picture.custom_ptr = &memory;
            auto release = wil::scope_exit([&] {
                WebPPictureFree(&picture);
                WebPMemoryWriterClear(&memory);
            });
            const int imported = bgr ? WebPPictureImportBGR(&picture, pixels.data(), static_cast<int>(stride))
                                     : WebPPictureImportBGRA(&picture, pixels.data(), static_cast<int>(stride));
            if (!imported || !WebPEncode(&config, &picture) || memory.size == 0)
                return E_FAIL;
            ULONG written = 0;
            RETURN_IF_FAILED(out->Write(memory.mem, static_cast<ULONG>(memory.size), &written));
            return written == memory.size ? S_OK : HRESULT_FROM_WIN32(ERROR_WRITE_FAULT);
        }

        HRESULT EncodeWic(IWICImagingFactory* factory, IWICBitmapSource* pixels, const ImageFormatInfo& format, float quality,
                          double dpiX, double dpiY, IStream* out)
        {
            ComPtr<IWICBitmapEncoder> encoder;
            RETURN_IF_FAILED(factory->CreateEncoder(ContainerFor(format.id), nullptr, &encoder));
            RETURN_IF_FAILED(encoder->Initialize(out, WICBitmapEncoderNoCache));
            ComPtr<IWICBitmapFrameEncode> frame;
            ComPtr<IPropertyBag2> options;
            RETURN_IF_FAILED(encoder->CreateNewFrame(&frame, &options));
            if (format.lossy && options)
            {
                PROPBAG2 name{};
                name.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
                VARIANT value;
                VariantInit(&value);
                value.vt = VT_R4;
                value.fltVal = quality / 100.f;
                options->Write(1, &name, &value);
            }
            RETURN_IF_FAILED(frame->Initialize(options.Get()));
            UINT w = 0, h = 0;
            RETURN_IF_FAILED(pixels->GetSize(&w, &h));
            RETURN_IF_FAILED(frame->SetSize(w, h));
            if (dpiX > 0 && dpiY > 0)
                frame->SetResolution(dpiX, dpiY);

            // The encoder may pick a different pixel format than we ask for; convert to whatever it chose.
            // An opaque picture comes as 24-bit BGR and is written without an alpha channel.
            WICPixelFormatGUID given{};
            RETURN_IF_FAILED(pixels->GetPixelFormat(&given));
            const WICPixelFormatGUID wanted =
                format.keepsAlpha && given != GUID_WICPixelFormat24bppBGR ? GUID_WICPixelFormat32bppBGRA : GUID_WICPixelFormat24bppBGR;
            WICPixelFormatGUID chosen = wanted;
            RETURN_IF_FAILED(frame->SetPixelFormat(&chosen));
            ComPtr<IWICBitmapSource> source = pixels;
            if (chosen != wanted)
            {
                ComPtr<IWICFormatConverter> fit;
                RETURN_IF_FAILED(factory->CreateFormatConverter(&fit));
                RETURN_IF_FAILED(fit->Initialize(pixels, chosen, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeMedianCut));
                source = fit;
            }
            RETURN_IF_FAILED(frame->WriteSource(source.Get(), nullptr));
            RETURN_IF_FAILED(frame->Commit());
            return encoder->Commit();
        }

        int64_t StreamBytes(IStream* stream)
        {
            STATSTG stat{};
            return SUCCEEDED(stream->Stat(&stat, STATFLAG_NONAME)) ? static_cast<int64_t>(stat.cbSize.QuadPart) : -1;
        }

        HRESULT CopyStream(IStream* from, IStream* to)
        {
            RETURN_IF_FAILED(from->Seek({}, STREAM_SEEK_SET, nullptr));
            ULARGE_INTEGER all;
            all.QuadPart = ~0ull;
            return from->CopyTo(to, all, nullptr, nullptr);
        }

        using EncodeFn = std::function<HRESULT(IWICBitmapSource*, float, IStream*)>;

        // image.compress: with no target, one encode at `quality` that must come out smaller than the
        // source. With maxKB or percent, the highest quality (40..92) that fits; if even quality 40 is too
        // big, the picture is scaled down and searched again (at most 4 times). Never writes a result over
        // the target. The pixels are decoded once into memory first, so every try only re-encodes.
        HRESULT CompressToTarget(IWICImagingFactory* factory, IWICBitmapSource* pixels, const nlohmann::json& params, float quality,
                                 int64_t sourceBytes, const EncodeFn& encode, IStream* out, std::stop_token stop, std::wstring& error)
        {
            const int maxKB = params.value("maxKB", 0);
            const int percent = params.value("percent", 0);
            const int64_t target = maxKB > 0 ? static_cast<int64_t>(maxKB) * 1024 : percent > 0 ? sourceBytes * percent / 100 : 0;
            if (maxKB > 0 && sourceBytes > 0 && sourceBytes <= target)
            {
                error = maxKB % 1024 == 0 ? std::format(L"already under {} MB", maxKB / 1024) : std::format(L"already under {} KB", maxKB);
                return E_ABORT;
            }

            ComPtr<IWICBitmap> decoded;   // decode once; each try below only encodes
            RETURN_IF_FAILED(factory->CreateBitmapFromSource(pixels, WICBitmapCacheOnLoad, &decoded));
            ComPtr<IWICBitmapSource> current = decoded;
            const auto encodeToMemory = [&](IWICBitmapSource* src, int q, ComPtr<IStream>& mem) -> int64_t {
                mem.Attach(SHCreateMemStream(nullptr, 0));
                if (!mem || FAILED(encode(src, static_cast<float>(q), mem.Get())))
                    return -1;
                return StreamBytes(mem.Get());
            };

            if (target <= 0)
            {
                ComPtr<IStream> mem;
                const int64_t got = encodeToMemory(current.Get(), static_cast<int>(quality), mem);
                if (got < 0)
                {
                    error = L"encode failed";
                    return E_FAIL;
                }
                if (sourceBytes > 0 && got >= sourceBytes)
                {
                    error = L"already compressed: the result was not smaller";
                    return E_ABORT;
                }
                return CopyStream(mem.Get(), out);
            }

            for (int shrink = 0; shrink <= 4; ++shrink)
            {
                if (stop.stop_requested())
                    return HRESULT_FROM_WIN32(ERROR_CANCELLED);
                ComPtr<IStream> best;
                int64_t smallest = -1;
                const int q = BestQualityUnder(target, 40, 92, [&](int tryQ) {
                    ComPtr<IStream> mem;
                    const int64_t got = encodeToMemory(current.Get(), tryQ, mem);
                    if (got >= 0 && got <= target)
                        best = mem;   // the search only moves up after a fit, so the last fit is the best
                    if (got >= 0 && (smallest < 0 || got < smallest))
                        smallest = got;
                    return got;
                });
                if (q >= 0 && best)
                {
                    UINT w = 0, h = 0;
                    current->GetSize(&w, &h);
                    log::Info(L"image: compress fit at quality {} and {}x{} ({} bytes, target {})", q, w, h, StreamBytes(best.Get()), target);
                    return CopyStream(best.Get(), out);
                }
                if (smallest < 0)
                {
                    error = L"encode failed";
                    return E_FAIL;
                }
                UINT w = 0, h = 0;
                current->GetSize(&w, &h);
                const double k = ShrinkFactor(target, smallest);
                const UINT nw = static_cast<UINT>(w * k), nh = static_cast<UINT>(h * k);
                if (nw < 16 || nh < 16)
                    break;
                ComPtr<IWICBitmapScaler> scaler;
                RETURN_IF_FAILED(factory->CreateBitmapScaler(&scaler));
                RETURN_IF_FAILED(scaler->Initialize(decoded.Get(), nw, nh, WICBitmapInterpolationModeHighQualityCubic));
                current = scaler;
            }
            error = L"could not get under the target size";
            return E_ABORT;
        }
    }

    ImageCodecs::ImageCodecs()
    {
        m_decodable.insert(L".webp");   // libwebp
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
                    auto read = [&](auto method) {
                        UINT len = 0;
                        (info.Get()->*method)(0, nullptr, &len);
                        std::wstring s(len, L'\0');
                        if (len)
                            (info.Get()->*method)(len, s.data(), &len);
                        while (!s.empty() && s.back() == L'\0')
                            s.pop_back();
                        return s;
                    };
                    const auto name = read(&IWICBitmapCodecInfo::GetFriendlyName);
                    const auto exts = read(&IWICBitmapCodecInfo::GetFileExtensions);
                    names << L"\n    " << name << L" [" << exts << L"]";
                    if (collect)
                    {
                        // ".jpeg,.jpe,.jpg": Windows separates with commas; some codecs (and Wine) use semicolons.
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
        m_summary = L"decoders:" + decoders.str() + L"\n    WebP [.webp] (libwebp)";
        // The format hooks (src/formats) read their own extensions, whether or not WIC has a codec.
        for (const auto* hook : ImageFormatHooks())
        {
            const auto exts = hook->DecodedExtensions();
            for (const auto& ext : exts)
            {
                m_decodable.insert(ext);
                m_summary += L"\n    " + ext + L" (built in)";
            }
        }
        m_summary += L"\n  encoders:" + encoders.str() + L"\n    WebP (libwebp)";
        for (const auto* hook : ImageFormatHooks())
        {
            if (const char* id = hook->EncodedFormat())
            {
                std::wstring why;
                const bool ok = hook->CanEncode(&why);
                m_summary += L"\n    " + std::wstring(id, id + strlen(id)) + (ok ? L" (built in or Windows codec)" : L" unavailable: " + why);
            }
        }
    }

    bool ImageCodecs::CanDecode(const std::wstring& ext) const { return m_decodable.contains(ext); }

    bool ImageCodecs::IsImage(const std::wstring& ext) const
    {
        if (CanDecode(ext))
            return true;
        for (const auto* hook : ImageFormatHooks())
            if (hook->CodecHint(ext, nullptr, nullptr))
                return true;
        return false;
    }

    std::wstring ImageCodecs::WhyNotDecodable(const std::wstring& ext) const
    {
        std::wstring why;
        for (const auto* hook : ImageFormatHooks())
            if (hook->CodecHint(ext, nullptr, &why))
                return why;
        return std::format(L"no image decoder for '{}' on this PC", ext);
    }

    std::wstring ImageCodecs::MissingCodecLabel(const std::wstring& ext) const
    {
        std::wstring label;
        if (!CanDecode(ext))
            for (const auto* hook : ImageFormatHooks())
                if (hook->CodecHint(ext, &label, nullptr))
                    return label;
        return {};
    }

    bool ImageCodecs::CanEncode(const ImageFormatInfo& format, std::wstring* why) const
    {
        // PNG/JPG/TIFF/BMP are always in WIC and WebP is libwebp; the rest ask their hook.
        if (const auto* hook = EncodeHookFor(format.id))
            return hook->CanEncode(why);
        return true;
    }

    HRESULT RunImageRecipe(const Recipe& recipe, const std::wstring& source, IStream* out, std::stop_token stop,
                           std::wstring& error, const ImageFormatInfo* format)
    {
        auto fail = [&](HRESULT hr, std::wstring_view what) {
            error = std::format(L"{}: {}", what, HResultText(hr));
            return hr;
        };
        if (!format)
            format = recipe.OutputFormatFor(LowerExtension(source));
        if (!format)
        {
            error = L"recipe has no image.encode step";
            return E_INVALIDARG;
        }
        auto factory = CreateFactory();
        if (!factory)
            return fail(REGDB_E_CLASSNOTREG, L"WIC");

        // 1. Decode (first frame; multi-page TIFF / animated GIF are first frame only for now).
        ComPtr<IWICBitmapSource> pixels;
        ComPtr<IWICBitmapFrameDecode> frame;
        double dpiX = 0, dpiY = 0;
        const auto sourceExt = LowerExtension(source);
        if (const auto* hook = DecodeHookFor(sourceExt))
        {
            // AVIF, SVG (src/formats): decoded whole and upright, so no EXIF step below.
            std::wstring why;
            if (const HRESULT hr = hook->Decode(factory.Get(), source, stop, pixels, why); FAILED(hr))
            {
                error = why.empty() ? std::format(L"decode: {}", HResultText(hr)) : L"decode: " + why;
                return hr;
            }
        }
        else if (sourceExt == L".webp")
        {
            if (const HRESULT hr = DecodeWebP(factory.Get(), source, pixels); FAILED(hr))
                return fail(hr, L"decode");
        }
        else
        {
            ComPtr<IWICBitmapDecoder> decoder;
            // A file that needs an optional Windows codec (HEIC) says which one when it won't open.
            const auto failDecode = [&](HRESULT hr) {
                for (const auto* hook : ImageFormatHooks())
                    if (std::wstring why; hook->CodecHint(sourceExt, nullptr, &why))
                    {
                        error = std::format(L"decode: {} ({})", why, HResultText(hr));
                        return hr;
                    }
                return fail(hr, L"decode");
            };
            if (const HRESULT hr = factory->CreateDecoderFromFilename(source.c_str(), nullptr, GENERIC_READ,
                                                                      WICDecodeMetadataCacheOnDemand, &decoder);
                FAILED(hr))
                return failDecode(hr);
            if (const HRESULT hr = decoder->GetFrame(0, &frame); FAILED(hr))
                return failDecode(hr);
            frame->GetResolution(&dpiX, &dpiY);
            pixels = frame;
        }
        if (stop.stop_requested())
            return HRESULT_FROM_WIN32(ERROR_CANCELLED);

        // 2. Orientation. When the recipe shrinks the picture anyway, a JPEG is decoded straight at a
        // smaller size that still covers the first shrink (DecodeScaled); every step below still
        // works from the full upright size (`w` x `h`), so the result has exactly the same size.
        const auto orientation = OrientationTransform(frame.Get());
        UINT w = 0, h = 0;
        pixels->GetSize(&w, &h);
        if (Turns(orientation))
            std::swap(w, h);
        if (UINT tw = 0, th = 0; frame && FirstShrink(recipe, w, h, Turns(orientation), tw, th))
            if (ComPtr<IWICBitmapSource> smaller; DecodeScaled(factory.Get(), frame.Get(), tw, th, smaller) == S_OK)
                pixels = smaller;
        if (orientation != WICBitmapTransformRotate0)
        {
            if (ComPtr<IWICBitmapSource> upright; SUCCEEDED(FlipRotate(factory.Get(), pixels.Get(), orientation, upright)))
                pixels = upright;
        }

        // 3. Transforms. Each wraps the previous source; nothing is decoded until the encoder pulls pixels.
        for (const auto& step : recipe.steps)
        {
            if (step.action == "image.resize" || step.action == "image.scale")
            {
                UINT nw = 0, nh = 0;
                if (ShrunkSize(step, w, h, nw, nh))   // never upscale
                {
                    ComPtr<IWICBitmapScaler> scaler;
                    if (const HRESULT hr = factory->CreateBitmapScaler(&scaler); FAILED(hr))
                        return fail(hr, L"resize");
                    if (const HRESULT hr = scaler->Initialize(pixels.Get(), nw, nh, WICBitmapInterpolationModeHighQualityCubic);
                        FAILED(hr))
                        return fail(hr, L"resize");
                    pixels = scaler;
                    w = nw;
                    h = nh;
                }
            }
            else if (step.action == "image.rotate" || step.action == "image.flip")
            {
                WICBitmapTransformOptions t = WICBitmapTransformRotate0;
                if (step.action == "image.rotate")
                {
                    const auto deg = step.params["degrees"].get<std::string>();
                    t = deg == "90" ? WICBitmapTransformRotate90 : deg == "180" ? WICBitmapTransformRotate180 : WICBitmapTransformRotate270;
                }
                else
                {
                    t = step.params["axis"].get<std::string>() == "vertical" ? WICBitmapTransformFlipVertical
                                                                            : WICBitmapTransformFlipHorizontal;
                }
                if (Turns(t))
                    std::swap(w, h);
                if (const HRESULT hr = FlipRotate(factory.Get(), pixels.Get(), t, pixels); FAILED(hr))
                    return fail(hr, L"rotate");
            }
            // image.strip_metadata: recipe outputs never carry the source's metadata (pixels are
            // re-encoded into a fresh container), so the step only documents intent. Verified
            // scrubbing is Share-safe's job (src/sharesafe).
        }

        // 4. Normalise, flatten if needed, encode. A picture without alpha goes to WIC's encoders and
        // libwebp as 24-bit BGR: nothing to flatten, and no alpha channel in a PNG or TIFF of it. The
        // format hooks (AVIF, HEIC) still get BGRA, as they always have.
        const auto* encodeHook = EncodeHookFor(format->id);
        if (!encodeHook && (ContainerFor(format->id) != GUID_NULL || std::string_view(format->id) == "webp") &&
            IsOpaque(factory.Get(), pixels.Get()))
        {
            if (const HRESULT hr = ToBgr(factory.Get(), pixels.Get(), pixels); FAILED(hr))
                return fail(hr, L"convert");
        }
        else
        {
            if (const HRESULT hr = ToBgra(factory.Get(), pixels.Get(), pixels); FAILED(hr))
                return fail(hr, L"convert");
            if (!format->keepsAlpha)
            {
                if (const HRESULT hr = FlattenOnWhite(factory.Get(), pixels.Get(), pixels); FAILED(hr))
                    return fail(hr, L"flatten");
            }
        }
        if (stop.stop_requested())
            return HRESULT_FROM_WIN32(ERROR_CANCELLED);

        const auto& last = recipe.steps.back();
        const float quality = static_cast<float>(last.params.value("quality", 90));
        if (std::wstring why; encodeHook && !encodeHook->CanEncode(&why))
        {
            error = why;
            return E_NOTIMPL;
        }
        std::wstring hookError;
        const auto encode = [&](IWICBitmapSource* src, float q, IStream* to) {
            if (encodeHook)   // AVIF, HEIC (src/formats)
                return encodeHook->Encode(factory.Get(), src, static_cast<int>(q), to, hookError);
            return std::string_view(format->id) == "webp" ? EncodeWebP(src, q, to)
                                                          : EncodeWic(factory.Get(), src, *format, q, dpiX, dpiY, to);
        };
        if (last.action == "image.compress")
        {
            if (const HRESULT hr = CompressToTarget(factory.Get(), pixels.Get(), last.params, quality, static_cast<int64_t>(FileSize(source)), encode, out,
                                                    stop, error);
                FAILED(hr))
                return hr;
            return out->Commit(STGC_DEFAULT);
        }
        if (const HRESULT hr = encode(pixels.Get(), quality, out); FAILED(hr))
        {
            if (hookError.empty())
                return fail(hr, L"encode");
            error = L"encode: " + hookError;
            return hr;
        }
        return out->Commit(STGC_DEFAULT);
    }

    HRESULT EncodePicture(IWICBitmapSource* pixels, const ImageFormatInfo& format, int quality, IStream* out, std::wstring& error)
    {
        auto factory = CreateFactory();
        if (!factory)
        {
            error = L"WIC unavailable";
            return REGDB_E_CLASSNOTREG;
        }
        ComPtr<IWICBitmapSource> bgra;
        HRESULT hr = ToBgra(factory.Get(), pixels, bgra);
        if (SUCCEEDED(hr) && !format.keepsAlpha)
            hr = FlattenOnWhite(factory.Get(), bgra.Get(), bgra);
        if (FAILED(hr))
        {
            error = std::format(L"convert: {}", HResultText(hr));
            return hr;
        }
        std::wstring hookError;
        if (const auto* hook = EncodeHookFor(format.id))
        {
            if (!hook->CanEncode(&error))
                return E_NOTIMPL;
            hr = hook->Encode(factory.Get(), bgra.Get(), quality, out, hookError);
        }
        else
        {
            hr = std::string_view(format.id) == "webp" ? EncodeWebP(bgra.Get(), static_cast<float>(quality), out)
                                                      : EncodeWic(factory.Get(), bgra.Get(), format, static_cast<float>(quality), 0, 0, out);
        }
        if (FAILED(hr))
        {
            error = hookError.empty() ? std::format(L"encode: {}", HResultText(hr)) : L"encode: " + hookError;
            return hr;
        }
        return out->Commit(STGC_DEFAULT);
    }

    int BestQualityUnder(int64_t target, int low, int high, const std::function<int64_t(int)>& sizeAt)
    {
        // Size grows with quality (near enough for JPEG and WebP), so a binary search finds the highest
        // quality that fits in about log2(high - low) encodes.
        int best = -1;
        while (low <= high)
        {
            const int q = (low + high) / 2;
            const int64_t got = sizeAt(q);
            if (got < 0)
                return -1;
            if (got <= target)
            {
                best = q;
                low = q + 1;
            }
            else
            {
                high = q - 1;
            }
        }
        return best;
    }

    double ShrinkFactor(int64_t target, int64_t got)
    {
        // Bytes scale roughly with pixel count, so each side shrinks by the square root, with a margin.
        if (got <= 0 || target <= 0)
            return 1.0;
        return std::clamp(std::sqrt(static_cast<double>(target) / static_cast<double>(got)) * 0.92, 0.1, 0.95);
    }
}
