#include "ImagePipeline.h"
#include "Log.h"
#include "OutputPath.h"

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

        HRESULT ReadWholeFile(const std::wstring& path, std::vector<uint8_t>& bytes)
        {
            wil::unique_hfile file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                               FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
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

        // libwebp decode -> WIC bitmap (32bpp BGRA), so the rest of the pipeline is format-agnostic.
        HRESULT DecodeWebP(IWICImagingFactory* factory, const std::wstring& path, ComPtr<IWICBitmapSource>& out)
        {
            std::vector<uint8_t> bytes;
            RETURN_IF_FAILED(ReadWholeFile(path, bytes));
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

        HRESULT EncodeWebP(IWICBitmapSource* bgra, float quality, IStream* out)
        {
            UINT w = 0, h = 0;
            std::vector<BYTE> pixels;
            RETURN_IF_FAILED(CopyBgra(bgra, w, h, pixels));
            uint8_t* encoded = nullptr;
            const size_t size = WebPEncodeBGRA(pixels.data(), static_cast<int>(w), static_cast<int>(h), static_cast<int>(w) * 4,
                                               quality, &encoded);
            if (size == 0 || !encoded)
                return E_FAIL;
            auto release = wil::scope_exit([&] { WebPFree(encoded); });
            ULONG written = 0;
            RETURN_IF_FAILED(out->Write(encoded, static_cast<ULONG>(size), &written));
            return written == size ? S_OK : HRESULT_FROM_WIN32(ERROR_WRITE_FAULT);
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
            const WICPixelFormatGUID wanted = format.keepsAlpha ? GUID_WICPixelFormat32bppBGRA : GUID_WICPixelFormat24bppBGR;
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

        int64_t FileBytes(const std::wstring& path)
        {
            WIN32_FILE_ATTRIBUTE_DATA data{};
            if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data))
                return 0;
            return (static_cast<int64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
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

    std::wstring HResultText(HRESULT hr)
    {
        wil::unique_hlocal_string msg;
        FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
                       static_cast<DWORD>(hr), 0, reinterpret_cast<LPWSTR>(&msg), 0, nullptr);
        std::wstring text = msg ? msg.get() : L"";
        while (!text.empty() && (text.back() == L'\n' || text.back() == L'\r' || text.back() == L' '))
            text.pop_back();
        return std::format(L"0x{:08X} {}", static_cast<unsigned>(hr), text);
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
        m_summary = L"decoders:" + decoders.str() + L"\n    WebP [.webp] (libwebp)\n  encoders:" + encoders.str() +
                    L"\n    WebP (libwebp)";
    }

    bool ImageCodecs::CanDecode(const std::wstring& ext) const { return m_decodable.contains(ext); }

    bool ImageCodecs::CanEncode(const ImageFormatInfo&) const
    {
        return true;   // PNG/JPG/TIFF/BMP are always in WIC; WebP is libwebp
    }

    HRESULT RunImageRecipe(const Recipe& recipe, const std::wstring& source, IStream* out, std::stop_token stop,
                           std::wstring& error)
    {
        auto fail = [&](HRESULT hr, std::wstring_view what) {
            error = std::format(L"{}: {}", what, HResultText(hr));
            return hr;
        };
        const auto* format = recipe.OutputFormatFor(LowerExtension(source));
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
        if (LowerExtension(source) == L".webp")
        {
            if (const HRESULT hr = DecodeWebP(factory.Get(), source, pixels); FAILED(hr))
                return fail(hr, L"decode");
        }
        else
        {
            ComPtr<IWICBitmapDecoder> decoder;
            if (const HRESULT hr = factory->CreateDecoderFromFilename(source.c_str(), nullptr, GENERIC_READ,
                                                                      WICDecodeMetadataCacheOnDemand, &decoder);
                FAILED(hr))
                return fail(hr, L"decode");
            if (const HRESULT hr = decoder->GetFrame(0, &frame); FAILED(hr))
                return fail(hr, L"decode");
            frame->GetResolution(&dpiX, &dpiY);
            pixels = frame;
        }
        if (stop.stop_requested())
            return HRESULT_FROM_WIN32(ERROR_CANCELLED);

        // 2. Orientation.
        if (const auto t = OrientationTransform(frame.Get()); t != WICBitmapTransformRotate0)
        {
            ComPtr<IWICBitmapFlipRotator> rotator;
            if (SUCCEEDED(factory->CreateBitmapFlipRotator(&rotator)) && SUCCEEDED(rotator->Initialize(pixels.Get(), t)))
                pixels = rotator;
        }

        // 3. Transforms. Each wraps the previous source; nothing is decoded until the encoder pulls pixels.
        for (const auto& step : recipe.steps)
        {
            if (step.action == "image.resize")
            {
                const UINT maxEdge = step.params["maxEdge"].get<UINT>();
                UINT w = 0, h = 0;
                pixels->GetSize(&w, &h);
                if (std::max(w, h) > maxEdge)   // never upscale
                {
                    const double k = static_cast<double>(maxEdge) / std::max(w, h);
                    const UINT nw = std::max(1u, static_cast<UINT>(w * k + 0.5));
                    const UINT nh = std::max(1u, static_cast<UINT>(h * k + 0.5));
                    ComPtr<IWICBitmapScaler> scaler;
                    if (const HRESULT hr = factory->CreateBitmapScaler(&scaler); FAILED(hr))
                        return fail(hr, L"resize");
                    if (const HRESULT hr = scaler->Initialize(pixels.Get(), nw, nh, WICBitmapInterpolationModeHighQualityCubic);
                        FAILED(hr))
                        return fail(hr, L"resize");
                    pixels = scaler;
                }
            }
            else if (step.action == "image.scale")
            {
                const double k = step.params["percent"].get<int>() / 100.0;
                UINT w = 0, h = 0;
                pixels->GetSize(&w, &h);
                if (k < 1.0)
                {
                    ComPtr<IWICBitmapScaler> scaler;
                    if (const HRESULT hr = factory->CreateBitmapScaler(&scaler); FAILED(hr))
                        return fail(hr, L"scale");
                    if (const HRESULT hr = scaler->Initialize(pixels.Get(), std::max(1u, static_cast<UINT>(w * k + 0.5)),
                                                              std::max(1u, static_cast<UINT>(h * k + 0.5)),
                                                              WICBitmapInterpolationModeHighQualityCubic);
                        FAILED(hr))
                        return fail(hr, L"scale");
                    pixels = scaler;
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
                ComPtr<IWICBitmapFlipRotator> rotator;
                if (const HRESULT hr = factory->CreateBitmapFlipRotator(&rotator); FAILED(hr))
                    return fail(hr, L"rotate");
                if (const HRESULT hr = rotator->Initialize(pixels.Get(), t); FAILED(hr))
                    return fail(hr, L"rotate");
                pixels = rotator;
            }
            // image.strip_metadata: outputs never carry source metadata in this version (pixels are
            // re-encoded into a fresh container), so the step only documents intent. Share-safe (M6)
            // will add verified scrubbing and, for other recipes, opt-in metadata carry-over.
        }

        // 4. Normalise, flatten if needed, encode.
        if (const HRESULT hr = ToBgra(factory.Get(), pixels.Get(), pixels); FAILED(hr))
            return fail(hr, L"convert");
        if (!format->keepsAlpha)
        {
            if (const HRESULT hr = FlattenOnWhite(factory.Get(), pixels.Get(), pixels); FAILED(hr))
                return fail(hr, L"flatten");
        }
        if (stop.stop_requested())
            return HRESULT_FROM_WIN32(ERROR_CANCELLED);

        const auto& last = recipe.steps.back();
        const float quality = static_cast<float>(last.params.value("quality", 90));
        const auto encode = [&](IWICBitmapSource* src, float q, IStream* to) {
            return std::string_view(format->id) == "webp" ? EncodeWebP(src, q, to)
                                                          : EncodeWic(factory.Get(), src, *format, q, dpiX, dpiY, to);
        };
        if (last.action == "image.compress")
        {
            if (const HRESULT hr = CompressToTarget(factory.Get(), pixels.Get(), last.params, quality, FileBytes(source), encode, out,
                                                    stop, error);
                FAILED(hr))
                return hr;
            return out->Commit(STGC_DEFAULT);
        }
        if (const HRESULT hr = encode(pixels.Get(), quality, out); FAILED(hr))
            return fail(hr, L"encode");
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
