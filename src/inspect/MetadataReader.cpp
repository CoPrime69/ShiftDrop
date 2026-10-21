#include "inspect/MetadataReader.h"
#include "FormatText.h"
#include "Log.h"
#include "Media.h"
#include "OutputPath.h"
#include "PathUtil.h"
#include "Utf.h"

#include <windows.h>
#include <propsys.h>
#include <propvarutil.h>
#include <shobjidl.h>
#include <wincodec.h>
#include <wil/resource.h>
#include <wrl/client.h>

#include <filesystem>
#include <format>

using Microsoft::WRL::ComPtr;

namespace sd::inspect
{
    namespace
    {
        std::wstring TimeText(const FILETIME& ft)
        {
            SYSTEMTIME utc{}, local{};
            if (!FileTimeToSystemTime(&ft, &utc) || !SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local))
                return {};
            return std::format(L"{:04}-{:02}-{:02} {:02}:{:02}:{:02}", local.wYear, local.wMonth, local.wDay, local.wHour,
                               local.wMinute, local.wSecond);
        }

        std::wstring Join(const std::vector<std::wstring>& parts, std::wstring_view sep)
        {
            std::wstring out;
            for (size_t i = 0; i < parts.size(); ++i)
                out += (i ? std::wstring(sep) : L"") + parts[i];
            return out;
        }

        std::wstring Rational64(ULONGLONG packed)
        {
            return FormatRational(static_cast<int64_t>(packed & 0xFFFFFFFFull), static_cast<int64_t>(packed >> 32));
        }

        std::wstring SignedRational64(LONGLONG packed)
        {
            const auto u = static_cast<ULONGLONG>(packed);
            return FormatRational(static_cast<int32_t>(u & 0xFFFFFFFFull), static_cast<int32_t>(u >> 32));
        }

        // Bytes as text when they are printable ASCII ("0232" for ExifVersion), else a count.
        std::wstring BytesText(const BYTE* data, ULONG size)
        {
            if (size > 0 && size <= 64)
            {
                bool printable = true;
                ULONG n = size;
                while (n > 0 && data[n - 1] == 0)
                    --n;
                for (ULONG i = 0; i < n && printable; ++i)
                    printable = data[i] >= 0x20 && data[i] < 0x7F;
                if (printable && n > 0)
                    return std::wstring(data, data + n);
                if (size <= 8)
                {
                    std::wstring hex;
                    for (ULONG i = 0; i < size; ++i)
                        hex += std::format(L"{}{:02X}", i ? L" " : L"", data[i]);
                    return hex;
                }
            }
            return std::format(L"({} bytes)", size);
        }

        void Add(std::vector<Field>& out, const std::wstring& group, const std::wstring& name, const std::wstring& value)
        {
            if (!value.empty())
                out.push_back(Field{ group, name, value });
        }

        // Every field under `reader`, recursing into nested blocks (/app1 -> /ifd -> /exif ...).
        void Walk(IWICMetadataQueryReader* reader, const std::wstring& prefix, std::vector<Field>& out, int depth)
        {
            ComPtr<IEnumString> names;
            if (depth > 8 || FAILED(reader->GetEnumerator(&names)))
                return;
            LPOLESTR raw = nullptr;
            while (names->Next(1, &raw, nullptr) == S_OK)
            {
                const std::wstring name = raw;
                CoTaskMemFree(raw);
                const std::wstring path = prefix + name;
                PROPVARIANT value;
                PropVariantInit(&value);
                if (FAILED(reader->GetMetadataByName(name.c_str(), &value)))
                    continue;
                auto clear = wil::scope_exit([&] { PropVariantClear(&value); });
                if (value.vt == VT_UNKNOWN && value.punkVal)
                {
                    ComPtr<IWICMetadataQueryReader> child;
                    if (SUCCEEDED(value.punkVal->QueryInterface(IID_PPV_ARGS(&child))))
                        Walk(child.Get(), path, out, depth + 1);
                    continue;
                }
                const auto where = DescribeWicPath(path);
                if (!where.skip)
                    Add(out, where.group, where.name, ValueText(value));
            }
        }

        std::wstring PixelFormatText(IWICImagingFactory* factory, const WICPixelFormatGUID& format)
        {
            ComPtr<IWICComponentInfo> info;
            ComPtr<IWICPixelFormatInfo> pixel;
            UINT bits = 0;
            if (SUCCEEDED(factory->CreateComponentInfo(format, &info)) && SUCCEEDED(info.As(&pixel)) && SUCCEEDED(pixel->GetBitsPerPixel(&bits)))
            {
                UINT channels = 0;
                pixel->GetChannelCount(&channels);
                return std::format(L"{} bits per pixel, {} channel{}", bits, channels, channels == 1 ? L"" : L"s");
            }
            return {};
        }
    }

    std::wstring ValueText(const PROPVARIANT& v)
    {
        switch (v.vt)
        {
        case VT_EMPTY:
        case VT_NULL:
            return {};
        case VT_LPSTR:
            return v.pszVal ? Widen(v.pszVal) : L"";
        case VT_LPWSTR:
            return v.pwszVal ? v.pwszVal : L"";
        case VT_BSTR:
            return v.bstrVal ? std::wstring(v.bstrVal, SysStringLen(v.bstrVal)) : L"";
        case VT_I1: return std::to_wstring(v.cVal);
        case VT_UI1: return std::to_wstring(v.bVal);
        case VT_I2: return std::to_wstring(v.iVal);
        case VT_UI2: return std::to_wstring(v.uiVal);
        case VT_I4:
        case VT_INT: return std::to_wstring(v.lVal);
        case VT_UI4:
        case VT_UINT: return std::to_wstring(v.ulVal);
        case VT_UI8: return Rational64(v.uhVal.QuadPart);
        case VT_I8: return SignedRational64(v.hVal.QuadPart);
        case VT_R4: return std::format(L"{:g}", v.fltVal);
        case VT_R8: return std::format(L"{:g}", v.dblVal);
        case VT_BOOL: return v.boolVal ? L"Yes" : L"No";
        case VT_FILETIME: return TimeText(v.filetime);
        case VT_BLOB: return BytesText(v.blob.pBlobData, v.blob.cbSize);
        case VT_VECTOR | VT_UI1: return BytesText(v.caub.pElems, v.caub.cElems);
        case VT_VECTOR | VT_I1: return BytesText(reinterpret_cast<const BYTE*>(v.cac.pElems), v.cac.cElems);
        default:
            break;
        }
        std::vector<std::wstring> parts;
        const auto many = [&](ULONG count, auto&& item) {
            for (ULONG i = 0; i < count && i < 32; ++i)
                parts.push_back(item(i));
            if (count > 32)
                parts.push_back(std::format(L"… ({} values)", count));
            return Join(parts, L", ");
        };
        switch (v.vt)
        {
        case VT_VECTOR | VT_UI2: return many(v.caui.cElems, [&](ULONG i) { return std::to_wstring(v.caui.pElems[i]); });
        case VT_VECTOR | VT_I2: return many(v.cai.cElems, [&](ULONG i) { return std::to_wstring(v.cai.pElems[i]); });
        case VT_VECTOR | VT_UI4: return many(v.caul.cElems, [&](ULONG i) { return std::to_wstring(v.caul.pElems[i]); });
        case VT_VECTOR | VT_I4: return many(v.cal.cElems, [&](ULONG i) { return std::to_wstring(v.cal.pElems[i]); });
        case VT_VECTOR | VT_UI8: return many(v.cauh.cElems, [&](ULONG i) { return Rational64(v.cauh.pElems[i].QuadPart); });
        case VT_VECTOR | VT_I8: return many(v.cah.cElems, [&](ULONG i) { return SignedRational64(v.cah.pElems[i].QuadPart); });
        case VT_VECTOR | VT_R8: return many(v.cadbl.cElems, [&](ULONG i) { return std::format(L"{:g}", v.cadbl.pElems[i]); });
        case VT_VECTOR | VT_LPSTR: return many(v.calpstr.cElems, [&](ULONG i) { return Widen(v.calpstr.pElems[i] ? v.calpstr.pElems[i] : ""); });
        case VT_VECTOR | VT_LPWSTR: return many(v.calpwstr.cElems, [&](ULONG i) { return std::wstring(v.calpwstr.pElems[i] ? v.calpwstr.pElems[i] : L""); });
        default:
            break;
        }
        wil::unique_cotaskmem_string text;
        if (SUCCEEDED(PropVariantToStringAlloc(v, &text)) && text)
            return text.get();
        return std::format(L"(type {})", static_cast<unsigned>(v.vt));
    }

    std::vector<Field> FileFields(const std::wstring& path, std::wstring* error)
    {
        std::vector<Field> f;
        WIN32_FILE_ATTRIBUTE_DATA data{};
        if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data))
        {
            if (error)
                *error = std::format(L"Could not read the file (error {})", GetLastError());
            return f;
        }
        const std::filesystem::path p(path);
        Add(f, L"File", L"Name", p.filename().wstring());
        Add(f, L"File", L"Folder", p.parent_path().wstring());
        if (!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            Add(f, L"File", L"Size", ExactSizeText(FileSizeOf(data)));
        Add(f, L"File", L"Created", TimeText(data.ftCreationTime));
        Add(f, L"File", L"Modified", TimeText(data.ftLastWriteTime));
        Add(f, L"File", L"Accessed", TimeText(data.ftLastAccessTime));
        std::vector<std::wstring> attrs;
        const std::pair<DWORD, const wchar_t*> kAttrs[] = {
            { FILE_ATTRIBUTE_DIRECTORY, L"Folder" }, { FILE_ATTRIBUTE_READONLY, L"Read-only" }, { FILE_ATTRIBUTE_HIDDEN, L"Hidden" },
            { FILE_ATTRIBUTE_SYSTEM, L"System" }, { FILE_ATTRIBUTE_ARCHIVE, L"Archive" }, { FILE_ATTRIBUTE_COMPRESSED, L"Compressed" },
            { FILE_ATTRIBUTE_ENCRYPTED, L"Encrypted" }, { FILE_ATTRIBUTE_OFFLINE, L"Offline" },
            { FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS, L"Online-only (cloud)" }, { FILE_ATTRIBUTE_SPARSE_FILE, L"Sparse" },
        };
        for (const auto& [bit, name] : kAttrs)
            if (data.dwFileAttributes & bit)
                attrs.push_back(name);
        Add(f, L"File", L"Attributes", attrs.empty() ? L"Normal" : Join(attrs, L", "));
        return f;
    }

    std::vector<Field> PropertyFields(const std::wstring& path)
    {
        std::vector<Field> f;
        ComPtr<IPropertyStore> store;
        // Best effort: a slow or partly unreadable file still gives what the handler could read.
        if (FAILED(SHGetPropertyStoreFromParsingName(path.c_str(), nullptr, GPS_BESTEFFORT, IID_PPV_ARGS(&store))))
            return f;
        DWORD count = 0;
        store->GetCount(&count);
        for (DWORD i = 0; i < count; ++i)
        {
            PROPERTYKEY key{};
            if (FAILED(store->GetAt(i, &key)))
                continue;
            PROPVARIANT value;
            PropVariantInit(&value);
            if (FAILED(store->GetValue(key, &value)))
                continue;
            auto clear = wil::scope_exit([&] { PropVariantClear(&value); });
            if (value.vt == VT_EMPTY)
                continue;
            ComPtr<IPropertyDescription> desc;
            wil::unique_cotaskmem_string canonical, display;
            if (SUCCEEDED(PSGetPropertyDescription(key, IID_PPV_ARGS(&desc))))
            {
                desc->GetCanonicalName(&canonical);
                desc->GetDisplayName(&display);
            }
            if (canonical && std::wstring_view(canonical.get()) == L"System.ItemTypeText")
            {
                wil::unique_cotaskmem_string text;
                if (SUCCEEDED(PSFormatForDisplayAlloc(key, value, PDFF_DEFAULT, &text)) && text)
                    Add(f, L"File", L"Type", text.get());
                continue;
            }
            const std::wstring name = canonical ? canonical.get() : L"";
            const auto group = PropertyGroup(name);
            if (group.empty() || name.empty())
                continue;
            std::wstring label = display && display.get()[0] ? display.get() : name.substr(name.rfind(L'.') + 1);
            std::wstring text;
            wil::unique_cotaskmem_string formatted;
            if (SUCCEEDED(PSFormatForDisplayAlloc(key, value, PDFF_DEFAULT, &formatted)) && formatted)
                text = formatted.get();
            else
                text = ValueText(value);
            Add(f, group, label, text);
        }
        return f;
    }

    std::vector<Field> ImageFields(const std::wstring& path)
    {
        std::vector<Field> f;
        ComPtr<IWICImagingFactory> factory;
        ComPtr<IWICBitmapDecoder> decoder;
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) ||
            FAILED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder)))
            return f;
        ComPtr<IWICBitmapDecoderInfo> info;
        if (SUCCEEDED(decoder->GetDecoderInfo(&info)))
        {
            UINT len = 0;
            info->GetFriendlyName(0, nullptr, &len);
            std::wstring name(len, L'\0');
            if (len && SUCCEEDED(info->GetFriendlyName(len, name.data(), &len)))
            {
                while (!name.empty() && name.back() == L'\0')
                    name.pop_back();
                Add(f, L"Image", L"Decoder", name);
            }
        }
        UINT frames = 0;
        decoder->GetFrameCount(&frames);
        if (frames > 1)
            Add(f, L"Image", L"Frames", std::to_wstring(frames));
        ComPtr<IWICBitmapFrameDecode> frame;
        if (FAILED(decoder->GetFrame(0, &frame)))
            return f;
        UINT w = 0, h = 0;
        frame->GetSize(&w, &h);
        Add(f, L"Image", L"Dimensions", std::format(L"{} × {} pixels", w, h));
        double dx = 0, dy = 0;
        if (SUCCEEDED(frame->GetResolution(&dx, &dy)) && dx > 0)
            Add(f, L"Image", L"Resolution", dx == dy ? std::format(L"{:g} DPI", dx) : std::format(L"{:g} × {:g} DPI", dx, dy));
        WICPixelFormatGUID format{};
        if (SUCCEEDED(frame->GetPixelFormat(&format)))
            Add(f, L"Image", L"Pixel format", PixelFormatText(factory.Get(), format));
        ComPtr<IWICColorContext> colour;
        UINT contexts = 0;
        if (SUCCEEDED(factory->CreateColorContext(&colour)))
        {
            IWICColorContext* list[] = { colour.Get() };
            if (SUCCEEDED(frame->GetColorContexts(1, list, &contexts)) && contexts > 0)
            {
                WICColorContextType type{};
                colour->GetType(&type);
                Add(f, L"Image", L"Colour profile", type == WICColorContextProfile ? L"Embedded ICC profile" : L"EXIF colour space");
            }
        }
        // Container-level blocks (GIF/PNG headers) and the frame's EXIF/XMP/IPTC.
        ComPtr<IWICMetadataQueryReader> reader;
        if (SUCCEEDED(decoder->GetMetadataQueryReader(&reader)))
            Walk(reader.Get(), L"", f, 0);
        reader.Reset();
        if (SUCCEEDED(frame->GetMetadataQueryReader(&reader)))
            Walk(reader.Get(), L"", f, 0);
        return f;
    }

    ProbeReport Probe(const FfmpegEngine& ffmpeg, const std::wstring& path, std::stop_token stop, MediaInfo* info)
    {
        std::vector<std::string> log;
        // Exits with 1 by design: no output was named. The log is what we want.
        ffmpeg.Run(ProbeArgs(path), nullptr, stop, &log);
        if (info)
            *info = ParseMediaInfo(log);
        return ParseProbeReport(log);
    }

    FileMetadata ReadMetadata(const std::wstring& path, const FfmpegEngine* ffmpeg, std::stop_token stop)
    {
        FileMetadata m;
        m.path = path;
        m.fields = FileFields(path, &m.error);
        if (!m.error.empty())
            return m;
        const auto append = [&m](std::vector<Field> more) { m.fields.insert(m.fields.end(), more.begin(), more.end()); };
        const auto ext = LowerExtension(path);
        const bool media = MediaKindOfExtension(ext) != MediaKind::None;
        if (!media && !stop.stop_requested())
            append(ImageFields(path));
        if (!stop.stop_requested())
            append(PropertyFields(path));
        if (media && !stop.stop_requested())
        {
            if (ffmpeg && ffmpeg->Available())
                append(ProbeFields(Probe(*ffmpeg, path, stop)));
            else
                m.fields.push_back(Field{ L"Media", L"Tracks", L"Not available: the media engine (engines\\ffmpeg.exe) is missing" });
        }
        return m;
    }
}
