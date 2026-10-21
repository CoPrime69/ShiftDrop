#include "metadata/Metadata.h"
#include "ImagePipeline.h"
#include "OutputPath.h"
#include "Utf.h"
#include "Wic.h"
#include "canvas/Raster.h"
#include "metadata/Container.h"
#include "metadata/Exif.h"

#include <propvarutil.h>
#include <wincodec.h>
#include <wil/resource.h>
#include <wrl/client.h>

#include <algorithm>
#include <cctype>
#include <functional>
#include <cwctype>
#include <format>
#include <map>
#include <string_view>

using Microsoft::WRL::ComPtr;

namespace sd::metadata
{
    namespace
    {
        struct Found
        {
            Field field;
            int block = -1;   // index into the container's block list
        };

        std::wstring Latin1(std::span<const uint8_t> b)
        {
            std::wstring s;
            for (const uint8_t c : b)
            {
                if (!c)
                    break;
                s.push_back(static_cast<wchar_t>(c));
            }
            return s;
        }

        std::wstring Kb(size_t bytes)
        {
            return bytes >= 1024 ? std::format(L"{} KB", (bytes + 512) / 1024) : std::format(L"{} bytes", bytes);
        }

        // The TIFF structure inside an EXIF block: JPEG's APP1 and some WebP writers start it with "Exif\0\0".
        std::span<const uint8_t> TiffOf(std::span<const uint8_t> payload)
        {
            if (payload.size() >= 6 && std::equal(payload.begin(), payload.begin() + 6, "Exif\0\0"))
                return payload.subspan(6);
            return payload;
        }

        size_t CountXmpProperties(std::string_view x)
        {
            // Properties are "prefix:Name" elements or attributes outside the rdf:/x:/xmlns: plumbing.
            size_t n = 0;
            for (size_t i = 0; i < x.size(); ++i)
            {
                const bool element = x[i] == '<' && i + 1 < x.size() && x[i + 1] != '/' && x[i + 1] != '?' && x[i + 1] != '!';
                const bool attribute = (x[i] == ' ' || x[i] == '\n' || x[i] == '\t' || x[i] == '\r');
                if (!element && !attribute)
                    continue;
                size_t j = i + 1;
                size_t colon = std::string_view::npos;
                while (j < x.size() && (std::isalnum(static_cast<unsigned char>(x[j])) || x[j] == ':' || x[j] == '_' || x[j] == '-' || x[j] == '.'))
                {
                    if (x[j] == ':' && colon == std::string_view::npos)
                        colon = j;
                    ++j;
                }
                if (colon == std::string_view::npos || colon == i + 1 || j == colon + 1)
                    continue;
                const std::string_view prefix = x.substr(i + 1, colon - i - 1);
                if (prefix == "rdf" || prefix == "x" || prefix == "xmlns" || prefix == "xml")
                    continue;
                if (attribute && !(j < x.size() && x[j] == '='))
                    continue;
                ++n;
            }
            return n;
        }

        const wchar_t* IptcName(int record, int dataset)
        {
            if (record != 2)
                return nullptr;
            switch (dataset)
            {
            case 5: return L"Title";
            case 25: return L"Keywords";
            case 40: return L"Instructions";
            case 55: return L"Date created";
            case 60: return L"Time created";
            case 80: return L"By-line";
            case 85: return L"By-line title";
            case 90: return L"City";
            case 92: return L"Sublocation";
            case 95: return L"State or province";
            case 100: return L"Country code";
            case 101: return L"Country";
            case 105: return L"Headline";
            case 110: return L"Credit";
            case 115: return L"Source";
            case 116: return L"Copyright";
            case 118: return L"Contact";
            case 120: return L"Caption";
            case 122: return L"Caption writer";
            default: return nullptr;
            }
        }

        void ReadIptc(std::span<const uint8_t> irb, int block, std::vector<Found>& out)
        {
            // "Photoshop 3.0\0", then 8BIM resources: id, Pascal name (even-padded), size, data (even-padded).
            size_t pos = 14;
            bool other = false;
            while (pos + 12 <= irb.size() && std::equal(irb.begin() + static_cast<ptrdiff_t>(pos), irb.begin() + static_cast<ptrdiff_t>(pos + 4), "8BIM"))
            {
                const int id = irb[pos + 4] << 8 | irb[pos + 5];
                size_t p = pos + 6;
                const size_t nameLen = irb[p];
                p += (nameLen + 2) & ~size_t{ 1 };
                if (p + 4 > irb.size())
                    break;
                const size_t size = size_t{ irb[p] } << 24 | size_t{ irb[p + 1] } << 16 | size_t{ irb[p + 2] } << 8 | irb[p + 3];
                p += 4;
                if (size > irb.size() - p)
                    break;
                if (id == 0x0404)
                {
                    // IPTC-IIM datasets: 0x1C, record, dataset, 16-bit size, data.
                    for (size_t q = p; q + 5 <= p + size && irb[q] == 0x1C;)
                    {
                        const int record = irb[q + 1], dataset = irb[q + 2];
                        const size_t len = size_t{ irb[q + 3] } << 8 | irb[q + 4];
                        if (q + 5 + len > p + size)
                            break;
                        const auto data = irb.subspan(q + 5, len);
                        if (!(record == 1 && dataset == 90) && !(record == 2 && dataset == 0))   // charset and version markers
                        {
                            const wchar_t* name = IptcName(record, dataset);
                            out.push_back({ Field{ std::format("iptc.{}:{}", record, dataset), L"IPTC",
                                                   name ? name : std::format(L"IPTC {}:{}", record, dataset), Widen(std::string(data.begin(), data.end())), true, false },
                                            block });
                        }
                        q += 5 + len;
                    }
                }
                else
                    other = true;
                pos = p + size + (size & 1);
            }
            if (other)
                out.push_back({ Field{ "photoshop", L"IPTC", L"Photoshop data", L"(editing history, captions, previews)", true, false }, block });
        }

        constexpr uint16_t kOrientationTag = 0x0112;

        // An EXIF block that says only which way up the picture is: what a clean copy keeps.
        bool OrientationOnly(std::span<const uint8_t> payload)
        {
            ExifData exif;
            if (!exif.Parse(TiffOf(payload)) || !exif.ThumbnailBytes().empty())
                return false;
            for (const auto& dir : exif.Directories())
                for (const auto& e : dir.entries)
                    if (dir.ifd != Ifd::Primary || e.tag != kOrientationTag)
                        return false;
            return true;
        }

        void ReadExifBlock(std::span<const uint8_t> payload, int block, std::vector<Found>& out)
        {
            ExifData exif;
            if (!exif.Parse(TiffOf(payload)))
            {
                out.push_back({ Field{ "exif", L"Other", L"EXIF block", L"(unreadable)", true, false }, block });
                return;
            }
            for (const auto& dir : exif.Directories())
                for (const auto& e : dir.entries)
                {
                    Field f;
                    f.key = std::format("exif.{}.{}", IfdKey(dir.ifd), e.tag);
                    f.group = TagGroup(dir.ifd, e.tag);
                    f.name = TagName(dir.ifd, e.tag);
                    f.value = FormatValue(exif, dir.ifd, e);
                    f.editable = dir.ifd != Ifd::Thumbnail && e.type != type::Undefined;
                    // Which way up the picture is: not about the person, and a clean copy keeps it.
                    f.personal = !(dir.ifd == Ifd::Primary && e.tag == kOrientationTag);
                    out.push_back({ std::move(f), block });
                }
            if (!exif.ThumbnailBytes().empty())
                out.push_back({ Field{ "exif.1st.thumbnail", L"Thumbnail", L"Thumbnail", L"JPEG, " + Kb(exif.ThumbnailBytes().size()), true, false }, block });
        }

        std::vector<Found> Collect(std::span<const uint8_t> bytes, Container container, const std::vector<Block>& blocks)
        {
            std::vector<Found> out;
            int comments = 0, extra = 0, apps = 0, colours = 0;
            for (size_t i = 0; i < blocks.size(); ++i)
            {
                const auto& b = blocks[i];
                const int index = static_cast<int>(i);
                const auto payload = bytes.subspan(b.payload, b.payloadSize);
                switch (b.kind)
                {
                case BlockKind::Exif:
                    ReadExifBlock(payload, index, out);
                    break;
                case BlockKind::Xmp:
                {
                    const std::string_view text(reinterpret_cast<const char*>(payload.data()), payload.size());
                    out.push_back({ Field{ "xmp", L"XMP", L"XMP packet", std::format(L"{} properties", CountXmpProperties(text)), true, false }, index });
                    // XMP often repeats the location (exif:GPSLatitude) even where EXIF has none.
                    if (text.find("GPSLatitude") != std::string_view::npos)
                        out.push_back({ Field{ "xmp", L"Location", L"Location (in XMP)", L"latitude and longitude", true, false }, index });
                    break;
                }
                case BlockKind::Iptc:
                    ReadIptc(payload, index, out);
                    break;
                case BlockKind::Comment:
                {
                    std::wstring text = container == Container::Gif ? L"(comment)" : Latin1(payload);
                    out.push_back({ Field{ std::format("comment.{}", comments++), L"Comments", L"Comment", text, true, false }, index });
                    break;
                }
                case BlockKind::Text:
                {
                    // tEXt: keyword NUL text (Latin-1); iTXt: keyword NUL flag method lang NUL key NUL text (UTF-8).
                    const std::string keyword(b.name.size() > 5 ? b.name.substr(5) : std::string());
                    std::wstring value = L"(compressed)";
                    const size_t nul = std::find(payload.begin(), payload.end(), 0) - payload.begin();
                    if (b.name.starts_with("tEXt") && nul < payload.size())
                        value = Latin1(payload.subspan(nul + 1));
                    else if (b.name.starts_with("iTXt") && nul + 2 < payload.size() && payload[nul + 1] == 0)
                    {
                        size_t p = nul + 3;
                        for (int skip = 0; skip < 2 && p < payload.size(); ++skip)
                            p = std::find(payload.begin() + static_cast<ptrdiff_t>(p), payload.end(), 0) - payload.begin() + 1;
                        if (p <= payload.size())
                            value = Widen(std::string(payload.begin() + static_cast<ptrdiff_t>(p), payload.end()));
                    }
                    const std::wstring wkey = Widen(keyword);
                    const bool software = keyword == "Software";
                    const bool author = keyword == "Author" || keyword == "Copyright";
                    out.push_back({ Field{ "png.text." + keyword, software ? L"Software" : author ? L"Author" : L"Description", wkey, value, true, false }, index });
                    break;
                }
                case BlockKind::Time:
                {
                    std::wstring value;
                    if (payload.size() >= 7)
                        value = std::format(L"{:04}-{:02}-{:02} {:02}:{:02}:{:02}", payload[0] << 8 | payload[1], payload[2], payload[3], payload[4], payload[5], payload[6]);
                    out.push_back({ Field{ "png.time", L"Dates", L"Last modified", value, true, false }, index });
                    break;
                }
                case BlockKind::Thumbnail:
                    out.push_back({ Field{ "thumbnail", L"Thumbnail", L"Thumbnail", Kb(b.payloadSize), true, false }, index });
                    break;
                case BlockKind::Jfif:
                    if (b.payloadSize >= 14 && payload[12] * payload[13] > 0)
                        out.push_back({ Field{ "thumbnail", L"Thumbnail", L"JFIF thumbnail", std::format(L"{} \u00d7 {}", payload[12], payload[13]), true, false }, index });
                    break;
                case BlockKind::ExtraImage:
                    out.push_back({ Field{ std::format("extra.{}", extra++), L"Hidden data", Widen(b.name), Kb(b.payloadSize), true, false }, index });
                    break;
                case BlockKind::Trailer:
                    out.push_back({ Field{ "trailer", L"Hidden data", L"Data after the picture", Kb(b.size), true, false }, index });
                    break;
                case BlockKind::Other:
                    out.push_back({ Field{ std::format("app.{}", apps++), L"Other", Widen(b.name), Kb(b.payloadSize), true, false }, index });
                    break;
                case BlockKind::Colour:
                    out.push_back({ Field{ std::format("colour.{}", colours++), L"Colour", Widen(b.name), Kb(b.payloadSize), false, false }, index });
                    break;
                case BlockKind::Image:
                    break;
                }
            }
            return out;
        }

        std::wstring PropText(const PROPVARIANT& v)
        {
            if (v.vt == VT_BLOB)
                return std::format(L"({} bytes)", v.blob.cbSize);
            if (v.vt == VT_UNKNOWN || v.vt == VT_EMPTY)
                return {};
            wil::unique_cotaskmem_string text;
            if (SUCCEEDED(PropVariantToStringAlloc(v, &text)) && text)
            {
                std::wstring s = text.get();
                return s.size() > 200 ? s.substr(0, 200) + L"\u2026" : s;
            }
            return L"(value)";
        }

        void Walk(IWICMetadataQueryReader* reader, const std::wstring& prefix, int depth, const std::function<void(const std::wstring&, const PROPVARIANT&)>& visit)
        {
            ComPtr<IEnumString> names;
            if (depth > 8 || FAILED(reader->GetEnumerator(&names)))
                return;
            LPOLESTR raw = nullptr;
            while (names->Next(1, &raw, nullptr) == S_OK)
            {
                const std::wstring name = raw;
                CoTaskMemFree(raw);
                PROPVARIANT value;
                PropVariantInit(&value);
                if (SUCCEEDED(reader->GetMetadataByName(name.c_str(), &value)))
                {
                    const std::wstring full = prefix + name;
                    visit(full, value);
                    ComPtr<IWICMetadataQueryReader> child;
                    if (value.vt == VT_UNKNOWN && value.punkVal && SUCCEEDED(value.punkVal->QueryInterface(IID_PPV_ARGS(&child))))
                        Walk(child.Get(), full, depth + 1, visit);
                }
                PropVariantClear(&value);
            }
        }

        bool WalkFile(const std::wstring& path, const std::function<void(const std::wstring&, const PROPVARIANT&)>& visit)
        {
            const auto factory = wic::Factory();
            ComPtr<IWICBitmapDecoder> decoder;
            if (!factory ||
                FAILED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder)))
                return false;
            ComPtr<IWICMetadataQueryReader> reader;
            if (SUCCEEDED(decoder->GetMetadataQueryReader(&reader)))   // container-level (GIF, some TIFF)
                Walk(reader.Get(), L"", 0, visit);
            ComPtr<IWICBitmapFrameDecode> frame;
            if (SUCCEEDED(decoder->GetFrame(0, &frame)) && SUCCEEDED(frame->GetMetadataQueryReader(&reader)))
                Walk(reader.Get(), L"", 0, visit);
            return true;
        }

        std::wstring WicGroup(const std::wstring& lower)
        {
            if (lower.find(L"/gps") != std::wstring::npos) return L"Location";
            if (lower.find(L"/xmp") != std::wstring::npos) return L"XMP";
            if (lower.find(L"/iptc") != std::wstring::npos || lower.find(L"/irb") != std::wstring::npos) return L"IPTC";
            if (lower.find(L"/thumb") != std::wstring::npos) return L"Thumbnail";
            if (lower.find(L"/exif") != std::wstring::npos) return L"Camera";
            if (lower.find(L"/com") != std::wstring::npos || lower.find(L"/text") != std::wstring::npos) return L"Comments";
            return L"Other";
        }

        std::wstring Lower(std::wstring s)
        {
            for (auto& c : s)
                c = static_cast<wchar_t>(std::towlower(c));
            return s;
        }

        // TIFF IFD0 tags that describe the person or the camera (a TIFF's IFD0 also holds its pixel layout).
        bool IsPersonalTiffTag(unsigned tag)
        {
            switch (tag)
            {
            case 270: case 271: case 272: case 305: case 306: case 315: case 316: case 33432: case 700: case 33723: case 34377:
            case 34665: case 34853: case 37724: case 40091: case 40092: case 40093: case 40094: case 40095:
                return true;
            default:
                return false;
            }
        }

        std::span<const uint8_t> Bytes(const std::vector<uint8_t>& v) { return { v.data(), v.size() }; }

        // The EXIF orientation of a JPEG, PNG or WebP (1, normal, when there is none).
        uint32_t OrientationOf(std::span<const uint8_t> bytes)
        {
            std::vector<Block> blocks;
            if (!ListBlocks(bytes, DetectContainer(bytes), blocks))
                return 1;
            for (const auto& b : blocks)
            {
                ExifData exif;
                if (b.kind == BlockKind::Exif && exif.Parse(TiffOf(bytes.subspan(b.payload, b.payloadSize))))
                {
                    const auto* e = exif.Find(Ifd::Primary, kOrientationTag);
                    return e ? exif.UInt(*e) : 1;
                }
            }
            return 1;
        }

        // The WIC paths an orientation-only EXIF block shows up as: the block and its first IFD as
        // readers ("/app1", "/app1/{ushort=0}" or "/app1/ifd") and the orientation entry itself
        // ("/app1/{ushort=0}/{ushort=274}"). Any other entry in that IFD is a path of its own, so it
        // is still caught.
        bool IsOrientationPath(const std::wstring& path, bool reader)
        {
            std::vector<std::wstring> segments;
            for (size_t start = 0; start < path.size();)
            {
                size_t end = path.find(L'/', start + 1);
                if (end == std::wstring::npos)
                    end = path.size();
                std::wstring segment = Lower(path.substr(start, end - start));
                if (segment.starts_with(L"/"))
                    segment.erase(0, 1);
                if (segment.starts_with(L"["))   // "[0]app1"
                    segment.erase(0, std::min(segment.size(), segment.find(L']') + 1));
                segments.push_back(std::move(segment));
                start = end;
            }
            if (segments.empty() || (segments[0] != L"app1" && segments[0] != L"exif"))
                return false;
            const auto isIfd = [](const std::wstring& s) { return s == L"ifd" || s == L"{ushort=0}"; };
            if (segments.size() == 1)
                return reader;
            if (segments.size() == 2)
                return reader && isIfd(segments[1]);
            return segments.size() == 3 && !reader && isIfd(segments[1]) && segments[2] == L"{ushort=274}";
        }

        HRESULT WriteReencoded(const std::wstring& source, const std::wstring& target, const Inventory& before, ScrubReport& report,
                               std::wstring& extension, std::wstring& error);
    }

    size_t Inventory::PersonalCount() const
    {
        return static_cast<size_t>(std::count_if(fields.begin(), fields.end(), [](const Field& f) { return f.personal; }));
    }

    std::vector<std::wstring> Inventory::Groups() const
    {
        static const wchar_t* const kOrder[] = { L"Location", L"Camera", L"Dates", L"Author", L"Description", L"Software", L"Image",
                                                 L"Thumbnail", L"XMP", L"IPTC", L"Comments", L"Hidden data", L"Colour", L"Other" };
        std::vector<std::wstring> out;
        for (const wchar_t* g : kOrder)
            if (std::any_of(fields.begin(), fields.end(), [&](const Field& f) { return f.group == g; }))
                out.emplace_back(g);
        for (const auto& f : fields)
            if (std::find(out.begin(), out.end(), f.group) == out.end())
                out.push_back(f.group);
        return out;
    }

    Inventory ReadMetadata(std::span<const uint8_t> bytes)
    {
        Inventory inv;
        const Container container = DetectContainer(bytes);
        std::vector<Block> blocks;
        if (container == Container::Tiff)
        {
            ExifData tiff;
            if (tiff.Parse(bytes))
                for (const auto& dir : tiff.Directories())
                    for (const auto& e : dir.entries)
                        if (dir.ifd != Ifd::Primary || IsPersonalTiffTag(e.tag))
                            inv.fields.push_back(Field{ std::format("exif.{}.{}", IfdKey(dir.ifd), e.tag), TagGroup(dir.ifd, e.tag), TagName(dir.ifd, e.tag),
                                                        FormatValue(tiff, dir.ifd, e), true, false });
            return inv;
        }
        if (!ListBlocks(bytes, container, blocks))
            return inv;
        inv.lossless = true;
        for (auto& f : Collect(bytes, container, blocks))
            inv.fields.push_back(std::move(f.field));
        return inv;
    }

    Inventory ReadMetadata(const std::wstring& path)
    {
        std::vector<uint8_t> bytes;
        if (!ReadFileBytes(path, bytes))
            return {};
        const Container container = DetectContainer(Bytes(bytes));
        if (container != Container::Unknown)
            return ReadMetadata(Bytes(bytes));
        // HEIC, AVIF, JPEG XR...: what WIC's readers expose.
        Inventory inv;
        WalkFile(path, [&](const std::wstring& full, const PROPVARIANT& v) {
            if (v.vt == VT_UNKNOWN)
                return;
            const std::wstring lower = Lower(full);
            inv.fields.push_back(Field{ "wic:" + Narrow(full), WicGroup(lower), full, PropText(v), IsPersonalWicPath(full), false });
        });
        return inv;
    }

    ScrubReport ReportFor(const Inventory& removed)
    {
        ScrubReport r;
        r.lossless = removed.lossless;
        // Most telling first: where you were, what you shot with, who you are, when.
        static const std::pair<const wchar_t*, const wchar_t*> kKinds[] = {
            { L"Location", L"GPS" },         { L"Camera", L"camera" },   { L"Author", L"author" },       { L"Dates", L"dates" },
            { L"Thumbnail", L"thumbnail" },  { L"XMP", L"XMP" },         { L"IPTC", L"IPTC" },           { L"Comments", L"comments" },
            { L"Description", L"captions" }, { L"Software", L"software" }, { L"Hidden data", L"hidden data" },
        };
        for (const auto& f : removed.fields)
            r.fields += f.personal ? 1 : 0;
        for (const auto& [group, kind] : kKinds)
            if (std::any_of(removed.fields.begin(), removed.fields.end(), [&](const Field& f) { return f.personal && f.group == group; }))
                r.kinds.emplace_back(kind);
        return r;
    }

    std::wstring ScrubReport::Summary() const
    {
        if (fields == 0)
            return L"No metadata found";
        std::wstring s = L"Removed ";
        for (size_t i = 0; i < std::min<size_t>(kinds.size(), 2); ++i)
            s += kinds[i] + L", ";
        s += fields == 1 ? L"1 field" : std::format(L"{} fields", fields);
        return s;
    }

    HRESULT WriteWithoutMetadata(const std::wstring& source, const std::wstring& target, ScrubReport& report, std::wstring& extension,
                                 std::wstring& error)
    {
        std::vector<uint8_t> bytes;
        if (!ReadFileBytes(source, bytes))
        {
            error = L"cannot read the file";
            return HRESULT_FROM_WIN32(GetLastError());
        }
        const Container container = DetectContainer(Bytes(bytes));
        if (container == Container::Jpeg || container == Container::Png || container == Container::WebP || container == Container::Gif)
        {
            const Inventory before = ReadMetadata(Bytes(bytes));
            std::vector<uint8_t> out;
            if (!StripPersonal(Bytes(bytes), out, nullptr, &error))
                return WINCODEC_ERR_BADHEADER;
            // A sideways photo stays the right way up: its orientation goes back in, alone.
            bool placed = true;
            if (const uint32_t orientation = OrientationOf(Bytes(bytes)); orientation >= 2 && orientation <= 8)
            {
                ExifData only;
                only.Set(Ifd::Primary, ExifEntry{ kOrientationTag, type::Short, 1, { static_cast<uint8_t>(orientation), 0 } });
                const auto tiff = only.Serialize();
                std::vector<uint8_t> withOrientation;
                placed = ReplaceExif(Bytes(out), Bytes(tiff), withOrientation, nullptr);
                if (placed)
                    out = std::move(withOrientation);
            }
            if (!placed)
                return WriteReencoded(source, target, before, report, extension, error);   // e.g. a simple WebP: no room for EXIF
            if (!WriteFileBytes(target, Bytes(out)))
            {
                error = L"cannot write the copy";
                return HRESULT_FROM_WIN32(GetLastError());
            }
            report = ReportFor(before);
            report.lossless = true;
            extension = LowerExtension(source);
            return S_OK;
        }
        // Everything else: decode and write the pixels into a fresh file.
        return WriteReencoded(source, target, ReadMetadata(source), report, extension, error);
    }

    namespace
    {
        HRESULT WriteReencoded(const std::wstring& source, const std::wstring& target, const Inventory& before, ScrubReport& report,
                               std::wstring& extension, std::wstring& error)
        {
        canvas::Raster image;
        if (const HRESULT hr = canvas::LoadRaster(source, image, error); FAILED(hr))
            return hr;
        const ImageFormatInfo* format = nullptr;
        if (const HRESULT hr = canvas::SaveEdited(image, source, target, error, &format); FAILED(hr))
            return hr;
        report = ReportFor(before);
        report.reencoded = true;
        report.lossless = !format->lossy;
        extension = IsAlreadyFormat(*format, LowerExtension(source)) ? LowerExtension(source) : std::wstring(format->extension);
        return S_OK;
        }
    }

    bool IsPersonalWicPath(const std::wstring& path)
    {
        static const std::wstring_view kPersonal[] = { L"app1", L"app13", L"com", L"exif", L"gps", L"xmp", L"iptc", L"irb", L"8bimiptc", L"thumb",
                                                       L"text", L"itxt", L"ztxt", L"time", L"commentext", L"xmpext", L"interop", L"xmpstruct" };
        const std::wstring lower = Lower(path);
        size_t depth = 0;
        for (size_t start = 0; start < lower.size();)
        {
            size_t end = lower.find(L'/', start + 1);
            if (end == std::wstring::npos)
                end = lower.size();
            std::wstring_view segment(lower.data() + start, end - start);
            if (segment.starts_with(L"/"))
                segment.remove_prefix(1);
            if (segment.starts_with(L"["))   // "[0]app1": an indexed block
                segment.remove_prefix(std::min(segment.size(), segment.find(L']') + 1));
            for (const auto p : kPersonal)
                if (segment == p)
                    return true;
            // A TIFF's own IFD: the camera and author tags, not its pixel layout.
            if (depth == 1 && lower.starts_with(L"/ifd/") && segment.starts_with(L"{ushort="))
                if (IsPersonalTiffTag(static_cast<unsigned>(std::wcstoul(segment.data() + 8, nullptr, 10))))
                    return true;
            ++depth;
            start = end;
        }
        return false;
    }

    std::vector<std::wstring> WicMetadataPaths(const std::wstring& path)
    {
        std::vector<std::wstring> paths;
        WalkFile(path, [&](const std::wstring& full, const PROPVARIANT&) { paths.push_back(full); });
        return paths;
    }

    Verification VerifyNoMetadata(const std::wstring& path)
    {
        Verification v;
        std::vector<uint8_t> bytes;
        if (!ReadFileBytes(path, bytes))
        {
            v.leftovers.push_back(L"the file can't be read");
            return v;
        }
        // 1. Our own parse of the container.
        bool orientationOnly = false;   // an EXIF block holding only the orientation, which a clean copy keeps
        const Container container = DetectContainer(Bytes(bytes));
        std::vector<Block> blocks;
        if (container == Container::Tiff)
        {
            for (const auto& f : ReadMetadata(Bytes(bytes)).fields)
                v.leftovers.push_back(f.name);
        }
        else if (ListBlocks(Bytes(bytes), container, blocks))
        {
            for (const auto& b : blocks)
            {
                if (b.kind == BlockKind::Exif && OrientationOnly(Bytes(bytes).subspan(b.payload, b.payloadSize)))
                {
                    orientationOnly = true;
                    continue;
                }
                if (IsPersonal(b.kind))
                    v.leftovers.push_back(Widen(b.name));
                else if (b.kind == BlockKind::Jfif && b.payloadSize >= 14 && bytes[b.payload + 12] * bytes[b.payload + 13] > 0)
                    v.leftovers.push_back(L"JFIF thumbnail");
            }
        }
        else if (container != Container::Unknown)
            v.leftovers.push_back(L"the file's structure is damaged");
        // 2. Every path WIC's metadata readers expose.
        v.wicChecked = WalkFile(path, [&](const std::wstring& full, const PROPVARIANT& value) {
            if (orientationOnly && IsOrientationPath(full, value.vt == VT_UNKNOWN))
                return;
            if (IsPersonalWicPath(full) && std::find(v.leftovers.begin(), v.leftovers.end(), full) == v.leftovers.end())
                v.leftovers.push_back(full);
        });
        // 3. The picture still opens.
        canvas::Raster probe;
        std::wstring error;
        v.opens = SUCCEEDED(canvas::LoadRaster(path, probe, error, 64));
        v.clean = v.leftovers.empty() && v.opens;
        return v;
    }

    namespace
    {
        bool ParseExifKey(const std::string& key, Ifd& ifd, uint16_t& tag)
        {
            // "exif.<ifd>.<tag>"
            if (!key.starts_with("exif."))
                return false;
            const auto dot = key.find('.', 5);
            if (dot == std::string::npos)
                return false;
            const auto which = IfdFromKey(std::string_view(key).substr(5, dot - 5));
            if (!which)
                return false;
            char* end = nullptr;
            const unsigned long t = std::strtoul(key.c_str() + dot + 1, &end, 10);
            if (end == key.c_str() + dot + 1 || *end || t > 0xFFFF)
                return false;
            ifd = *which;
            tag = static_cast<uint16_t>(t);
            return true;
        }

        // The file's EXIF (the first EXIF block), or an empty one.
        ExifData ExifOf(std::span<const uint8_t> bytes, const std::vector<Block>& blocks)
        {
            ExifData exif;
            for (const auto& b : blocks)
                if (b.kind == BlockKind::Exif && exif.Parse(TiffOf(bytes.subspan(b.payload, b.payloadSize))))
                    break;
            return exif;
        }
    }

    bool RemoveFields(std::span<const uint8_t> bytes, const std::vector<std::string>& keys, std::vector<uint8_t>& out, std::wstring& error)
    {
        const Container container = DetectContainer(bytes);
        std::vector<Block> blocks;
        if (container == Container::Unknown || container == Container::Tiff || !ListBlocks(bytes, container, blocks))
        {
            error = L"removing single fields needs a JPEG, PNG, WebP or GIF file";
            return false;
        }
        // 1. EXIF entries: edit the EXIF block and write it back.
        bool exifEdited = false;
        ExifData exif = ExifOf(bytes, blocks);
        for (const auto& key : keys)
        {
            Ifd ifd{};
            uint16_t tag = 0;
            if (key == "exif.1st.thumbnail")
                exif.RemoveIfd(Ifd::Thumbnail), exifEdited = true;
            else if (key.starts_with("exif.") && key.find('.', 5) == std::string::npos)
            {
                if (const auto which = IfdFromKey(std::string_view(key).substr(5)))
                    exif.RemoveIfd(*which), exifEdited = true;
            }
            else if (ParseExifKey(key, ifd, tag))
                exifEdited |= exif.Remove(ifd, tag);
        }
        std::vector<uint8_t> current(bytes.begin(), bytes.end());
        if (exifEdited)
        {
            const auto tiff = exif.Serialize();
            std::vector<uint8_t> next;
            if (!ReplaceExif(Bytes(current), Bytes(tiff), next, &error))
                return false;
            current = std::move(next);
        }
        // 2. Whole blocks (XMP, IPTC, comments, PNG text...): found by the fields' keys.
        if (!ListBlocks(Bytes(current), container, blocks))
        {
            error = L"the file's structure is damaged";
            return false;
        }
        std::vector<Block> drop;
        for (const auto& found : Collect(Bytes(current), container, blocks))
        {
            if (found.field.key.starts_with("exif") || found.block < 0)
                continue;
            for (const auto& key : keys)
                if (found.field.key == key || (key == "iptc" && found.field.key.starts_with("iptc")))
                    drop.push_back(blocks[static_cast<size_t>(found.block)]);
        }
        if (drop.empty())
        {
            out = std::move(current);
            return true;
        }
        return metadata::RemoveBlocks(Bytes(current), drop, out, &error);
    }

    bool SetField(std::span<const uint8_t> bytes, const std::string& key, const std::wstring& value, std::vector<uint8_t>& out, std::wstring& error)
    {
        Ifd ifd{};
        uint16_t tag = 0;
        if (!ParseExifKey(key, ifd, tag) || ifd == Ifd::Thumbnail)
        {
            error = L"only EXIF fields can be set";
            return false;
        }
        const Container container = DetectContainer(bytes);
        std::vector<Block> blocks;
        if (!ListBlocks(bytes, container, blocks))
        {
            error = L"editing metadata needs a JPEG, PNG or WebP file";
            return false;
        }
        ExifData exif = ExifOf(bytes, blocks);
        const auto entry = exif.FromText(ifd, tag, value);
        if (!entry)
        {
            error = std::format(L"'{}' is not a valid value for {}", value, TagName(ifd, tag));
            return false;
        }
        exif.Set(ifd, *entry);
        const auto tiff = exif.Serialize();
        return ReplaceExif(bytes, Bytes(tiff), out, &error);
    }

    HRESULT RemoveFields(const std::wstring& source, const std::wstring& target, const std::vector<std::string>& keys, std::wstring& error)
    {
        std::vector<uint8_t> bytes, out;
        if (!ReadFileBytes(source, bytes))
            return HRESULT_FROM_WIN32(GetLastError());
        if (!RemoveFields(Bytes(bytes), keys, out, error))
            return E_FAIL;
        return WriteFileBytes(target, Bytes(out)) ? S_OK : HRESULT_FROM_WIN32(GetLastError());
    }

    HRESULT SetField(const std::wstring& source, const std::wstring& target, const std::string& key, const std::wstring& value, std::wstring& error)
    {
        std::vector<uint8_t> bytes, out;
        if (!ReadFileBytes(source, bytes))
            return HRESULT_FROM_WIN32(GetLastError());
        if (!SetField(Bytes(bytes), key, value, out, error))
            return E_FAIL;
        return WriteFileBytes(target, Bytes(out)) ? S_OK : HRESULT_FROM_WIN32(GetLastError());
    }

    bool CarryEditedExif(std::span<const uint8_t> source, std::vector<uint8_t>& edited, int width, int height)
    {
        const Container container = DetectContainer(source);
        std::vector<Block> blocks;
        if (container == Container::Unknown || container == Container::Tiff || !ListBlocks(source, container, blocks))
            return false;
        ExifData exif = ExifOf(source, blocks);
        if (exif.Directories().empty())
            return false;
        exif.RemoveIfd(Ifd::Thumbnail);
        exif.Remove(Ifd::Primary, 0x0112);   // orientation: absent means normal
        exif.Remove(Ifd::Primary, 0x0100);   // TIFF-style image width and height
        exif.Remove(Ifd::Primary, 0x0101);
        for (const auto& [tag, value] : { std::pair<uint16_t, int>{ 0xA002, width }, { 0xA003, height } })
        {
            if (!exif.Find(Ifd::Exif, tag))
                continue;
            if (const auto entry = exif.FromText(Ifd::Exif, tag, std::to_wstring(value)))
                exif.Set(Ifd::Exif, *entry);
            else
                exif.Remove(Ifd::Exif, tag);
        }
        const auto tiff = exif.Serialize();
        if (tiff.empty())
            return false;
        std::vector<uint8_t> out;
        if (!ReplaceExif(Bytes(edited), Bytes(tiff), out, nullptr))
            return false;
        edited = std::move(out);
        return true;
    }
}

