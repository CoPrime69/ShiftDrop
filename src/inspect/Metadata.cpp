#include "inspect/Metadata.h"
#include "FormatText.h"
#include "Strings.h"

#include <algorithm>
#include <cwctype>
#include <format>

namespace sd::inspect
{
    namespace
    {
        bool StartsWith(std::wstring_view s, std::wstring_view prefix) { return s.substr(0, prefix.size()) == prefix; }

        // Lower rank = listed first. Track groups share a rank and keep the order they were met in.
        int Rank(std::wstring_view group)
        {
            static constexpr std::wstring_view kOrder[] = {
                L"File", L"Image", L"Camera", L"GPS", L"Description", L"Media",
            };
            for (size_t i = 0; i < std::size(kOrder); ++i)
                if (group == kOrder[i])
                    return static_cast<int>(i);
            if (group.find(L" track ") != std::wstring_view::npos || StartsWith(group, L"Attachment"))
                return 10;
            static constexpr std::wstring_view kLater[] = {
                L"Chapters", L"Tags", L"EXIF", L"EXIF GPS", L"XMP", L"IPTC", L"PNG text", L"Document", L"Other properties",
            };
            for (size_t i = 0; i < std::size(kLater); ++i)
                if (group == kLater[i])
                    return 20 + static_cast<int>(i);
            return 100;
        }

        struct Tag
        {
            unsigned id;
            const wchar_t* name;
        };
        constexpr Tag kExifTags[] = {
            { 0x00FE, L"NewSubfileType" }, { 0x0100, L"ImageWidth" }, { 0x0101, L"ImageLength" }, { 0x0102, L"BitsPerSample" },
            { 0x0103, L"Compression" }, { 0x0106, L"PhotometricInterpretation" }, { 0x010E, L"ImageDescription" },
            { 0x010F, L"Make" }, { 0x0110, L"Model" }, { 0x0111, L"StripOffsets" }, { 0x0112, L"Orientation" },
            { 0x0115, L"SamplesPerPixel" }, { 0x0116, L"RowsPerStrip" }, { 0x0117, L"StripByteCounts" },
            { 0x011A, L"XResolution" }, { 0x011B, L"YResolution" }, { 0x011C, L"PlanarConfiguration" },
            { 0x0128, L"ResolutionUnit" }, { 0x0131, L"Software" }, { 0x0132, L"DateTime" }, { 0x013B, L"Artist" },
            { 0x013E, L"WhitePoint" }, { 0x013F, L"PrimaryChromaticities" }, { 0x0201, L"JPEGInterchangeFormat" },
            { 0x0202, L"JPEGInterchangeFormatLength" }, { 0x0211, L"YCbCrCoefficients" }, { 0x0213, L"YCbCrPositioning" },
            { 0x0214, L"ReferenceBlackWhite" }, { 0x8298, L"Copyright" }, { 0x829A, L"ExposureTime" }, { 0x829D, L"FNumber" },
            { 0x8769, L"ExifIFD" }, { 0x8822, L"ExposureProgram" }, { 0x8825, L"GPSInfoIFD" }, { 0x8827, L"ISOSpeedRatings" },
            { 0x8830, L"SensitivityType" }, { 0x9000, L"ExifVersion" }, { 0x9003, L"DateTimeOriginal" },
            { 0x9004, L"DateTimeDigitized" }, { 0x9010, L"OffsetTime" }, { 0x9011, L"OffsetTimeOriginal" },
            { 0x9012, L"OffsetTimeDigitized" }, { 0x9101, L"ComponentsConfiguration" }, { 0x9102, L"CompressedBitsPerPixel" },
            { 0x9201, L"ShutterSpeedValue" }, { 0x9202, L"ApertureValue" }, { 0x9203, L"BrightnessValue" },
            { 0x9204, L"ExposureBiasValue" }, { 0x9205, L"MaxApertureValue" }, { 0x9206, L"SubjectDistance" },
            { 0x9207, L"MeteringMode" }, { 0x9208, L"LightSource" }, { 0x9209, L"Flash" }, { 0x920A, L"FocalLength" },
            { 0x9214, L"SubjectArea" }, { 0x927C, L"MakerNote" }, { 0x9286, L"UserComment" }, { 0x9290, L"SubSecTime" },
            { 0x9291, L"SubSecTimeOriginal" }, { 0x9292, L"SubSecTimeDigitized" }, { 0xA000, L"FlashpixVersion" },
            { 0xA001, L"ColorSpace" }, { 0xA002, L"PixelXDimension" }, { 0xA003, L"PixelYDimension" },
            { 0xA005, L"InteroperabilityIFD" }, { 0xA20E, L"FocalPlaneXResolution" }, { 0xA20F, L"FocalPlaneYResolution" },
            { 0xA210, L"FocalPlaneResolutionUnit" }, { 0xA215, L"ExposureIndex" }, { 0xA217, L"SensingMethod" },
            { 0xA300, L"FileSource" }, { 0xA301, L"SceneType" }, { 0xA401, L"CustomRendered" }, { 0xA402, L"ExposureMode" },
            { 0xA403, L"WhiteBalance" }, { 0xA404, L"DigitalZoomRatio" }, { 0xA405, L"FocalLengthIn35mmFilm" },
            { 0xA406, L"SceneCaptureType" }, { 0xA407, L"GainControl" }, { 0xA408, L"Contrast" }, { 0xA409, L"Saturation" },
            { 0xA40A, L"Sharpness" }, { 0xA40C, L"SubjectDistanceRange" }, { 0xA420, L"ImageUniqueID" },
            { 0xA430, L"CameraOwnerName" }, { 0xA431, L"BodySerialNumber" }, { 0xA432, L"LensSpecification" },
            { 0xA433, L"LensMake" }, { 0xA434, L"LensModel" }, { 0xA435, L"LensSerialNumber" },
        };
        constexpr Tag kGpsTags[] = {
            { 0x00, L"GPSVersionID" }, { 0x01, L"GPSLatitudeRef" }, { 0x02, L"GPSLatitude" }, { 0x03, L"GPSLongitudeRef" },
            { 0x04, L"GPSLongitude" }, { 0x05, L"GPSAltitudeRef" }, { 0x06, L"GPSAltitude" }, { 0x07, L"GPSTimeStamp" },
            { 0x08, L"GPSSatellites" }, { 0x09, L"GPSStatus" }, { 0x0A, L"GPSMeasureMode" }, { 0x0B, L"GPSDOP" },
            { 0x0C, L"GPSSpeedRef" }, { 0x0D, L"GPSSpeed" }, { 0x0E, L"GPSTrackRef" }, { 0x0F, L"GPSTrack" },
            { 0x10, L"GPSImgDirectionRef" }, { 0x11, L"GPSImgDirection" }, { 0x12, L"GPSMapDatum" },
            { 0x17, L"GPSDestBearingRef" }, { 0x18, L"GPSDestBearing" }, { 0x1B, L"GPSProcessingMethod" },
            { 0x1D, L"GPSDateStamp" }, { 0x1F, L"GPSHPositioningError" },
        };

        // The number in "{ushort=33434}" (or "{uint=...}"), or -1.
        long TagNumber(std::wstring_view segment)
        {
            const auto eq = segment.find(L'=');
            if (segment.empty() || segment.front() != L'{' || eq == std::wstring_view::npos)
                return -1;
            const auto type = segment.substr(1, eq - 1);
            if (type != L"ushort" && type != L"uint" && type != L"ulong")
                return -1;
            long n = 0;
            for (size_t i = eq + 1; i < segment.size() && segment[i] != L'}'; ++i)
            {
                if (segment[i] < L'0' || segment[i] > L'9')
                    return -1;
                n = n * 10 + (segment[i] - L'0');
            }
            return n;
        }

        // "{str=Software}" -> "Software"; anything else as it is.
        std::wstring Unbrace(std::wstring_view segment)
        {
            if (segment.size() > 2 && segment.front() == L'{' && segment.back() == L'}')
            {
                const auto eq = segment.find(L'=');
                if (eq != std::wstring_view::npos)
                    return std::wstring(segment.substr(eq + 1, segment.size() - eq - 2));
            }
            return std::wstring(segment);
        }

        std::vector<std::wstring_view> Segments(std::wstring_view path)
        {
            std::vector<std::wstring_view> out;
            size_t start = 0;
            while (start < path.size())
            {
                if (path[start] == L'/')
                {
                    ++start;
                    continue;
                }
                size_t end = start;
                int depth = 0;   // a '/' inside {...} does not split
                while (end < path.size() && (path[end] != L'/' || depth > 0))
                {
                    if (path[end] == L'{') ++depth;
                    if (path[end] == L'}') --depth;
                    ++end;
                }
                out.push_back(path.substr(start, end - start));
                start = end;
            }
            return out;
        }
    }

    std::vector<Group> GroupFields(const std::vector<Field>& fields)
    {
        std::vector<Group> groups;
        for (const auto& f : fields)
        {
            auto it = std::find_if(groups.begin(), groups.end(), [&](const Group& g) { return g.name == f.group; });
            if (it == groups.end())
            {
                groups.push_back(Group{ f.group, {} });
                it = groups.end() - 1;
            }
            const bool repeat = std::any_of(it->fields.begin(), it->fields.end(),
                                            [&](const Field& g) { return g.name == f.name && g.value == f.value; });
            if (!repeat)
                it->fields.push_back(f);
        }
        std::stable_sort(groups.begin(), groups.end(), [](const Group& a, const Group& b) { return Rank(a.name) < Rank(b.name); });
        return groups;
    }

    std::vector<Field> Filter(const std::vector<Field>& fields, std::wstring_view query)
    {
        std::vector<std::wstring> words;
        std::wstring word;
        for (const wchar_t c : query)
        {
            if (std::iswspace(c))
            {
                if (!word.empty())
                    words.push_back(Lower(word));
                word.clear();
            }
            else
                word += c;
        }
        if (!word.empty())
            words.push_back(Lower(word));

        std::vector<Field> out;
        for (const auto& f : fields)
        {
            const auto haystack = Lower(f.group + L"\n" + f.name + L"\n" + f.value);
            if (std::all_of(words.begin(), words.end(), [&](const std::wstring& w) { return haystack.find(w) != std::wstring::npos; }))
                out.push_back(f);
        }
        return out;
    }

    std::wstring PropertyGroup(std::wstring_view name)
    {
        // Already in the File group, or internal bookkeeping.
        static constexpr std::wstring_view kSkip[] = {
            L"System.ItemFolder", L"System.ItemPath", L"System.ParsingName", L"System.ParsingPath", L"System.Size",
            L"System.DateModified", L"System.DateCreated", L"System.DateAccessed", L"System.FileAttributes",
            L"System.ItemName", L"System.FileName", L"System.ItemNameDisplay", L"System.SFGAOFlags", L"System.ItemUrl",
            L"System.Link.", L"System.Thumbnail", L"System.ThumbnailCacheId", L"System.ItemDate", L"System.ZoneIdentifier",
            L"System.FileExtension", L"System.IsFolder", L"System.NotUserContent", L"System.OfflineAvailability",
            L"System.OfflineStatus", L"System.SharedWith", L"System.SharingStatus", L"System.Shell.",
            L"System.StorageProviderState", L"System.VolumeId", L"System.PropList.", L"System.Search.", L"System.Kind",
            L"System.FileOwner", L"System.ComputerName", L"System.ItemType", L"System.PerceivedType", L"System.ContentType",
        };
        for (const auto& p : kSkip)
            if (StartsWith(name, p))
                return {};
        if (StartsWith(name, L"System.GPS."))
            return L"GPS";
        if (StartsWith(name, L"System.Photo."))
            return L"Camera";
        if (StartsWith(name, L"System.Image."))
            return L"Image";
        if (StartsWith(name, L"System.Media.") || StartsWith(name, L"System.Video.") || StartsWith(name, L"System.Audio.") ||
            StartsWith(name, L"System.Music."))
            return L"Media";
        if (StartsWith(name, L"System.Document."))
            return L"Document";
        static constexpr std::wstring_view kDescription[] = {
            L"System.Title", L"System.Subject", L"System.Author", L"System.Keywords", L"System.Comment",
            L"System.Copyright", L"System.Rating", L"System.Category", L"System.Company", L"System.ApplicationName",
        };
        for (const auto& p : kDescription)
            if (name == p || (StartsWith(name, p) && name.size() > p.size() && name[p.size()] == L'.'))
                return L"Description";
        return L"Other properties";
    }

    std::wstring ExifTagName(unsigned tag, bool gps)
    {
        if (gps)
        {
            for (const auto& t : kGpsTags)
                if (t.id == tag)
                    return t.name;
        }
        else
        {
            for (const auto& t : kExifTags)
                if (t.id == tag)
                    return t.name;
        }
        return std::format(L"Tag 0x{:04X}", tag);
    }

    PathName DescribeWicPath(std::wstring_view path)
    {
        const auto seg = Segments(path);
        PathName out;
        if (seg.empty())
        {
            out.skip = true;
            return out;
        }
        const auto has = [&](std::wstring_view s) { return std::find(seg.begin(), seg.end(), s) != seg.end(); };
        if (has(L"thumb") || has(L"padding") || has(L"{ushort=40965}") || has(L"{ushort=34665}") || has(L"{ushort=34853}"))
        {
            out.skip = true;   // thumbnails, padding and the pointers to the sub-IFDs
            return out;
        }
        const auto& leaf = seg.back();
        const long tag = TagNumber(leaf);
        if (has(L"xmp"))
        {
            out.group = L"XMP";
            // "/xmp/dc:creator/{ulong=0}" -> "dc:creator [0]"
            std::wstring name;
            for (size_t i = 0; i < seg.size(); ++i)
            {
                if (seg[i] == L"xmp")
                {
                    for (size_t j = i + 1; j < seg.size(); ++j)
                    {
                        const auto part = seg[j];
                        if (TagNumber(part) >= 0)
                            name += std::format(L" [{}]", TagNumber(part));
                        else
                            name += (name.empty() ? L"" : L"/") + Unbrace(part);
                    }
                    break;
                }
            }
            out.name = name.empty() ? L"(packet)" : name;
            return out;
        }
        if (has(L"iptc") || has(L"8bimiptc"))
        {
            out.group = L"IPTC";
            out.name = Unbrace(leaf);
            return out;
        }
        if (has(L"gps"))
        {
            out.group = L"EXIF GPS";
            out.name = tag >= 0 ? ExifTagName(static_cast<unsigned>(tag), true) : Unbrace(leaf);
            return out;
        }
        if (has(L"ifd") || has(L"exif"))
        {
            out.group = L"EXIF";
            out.name = tag >= 0 ? ExifTagName(static_cast<unsigned>(tag), false) : Unbrace(leaf);
            return out;
        }
        if (has(L"tEXt") || has(L"iTXt") || has(L"zTXt"))
        {
            out.group = L"PNG text";
            out.name = Unbrace(leaf);
            return out;
        }
        out.group = L"Other properties";
        std::wstring name;
        for (const auto& s : seg)
            name += (name.empty() ? L"" : L"/") + Unbrace(s);
        out.name = name;
        return out;
    }

    std::wstring FormatRational(int64_t n, int64_t d)
    {
        if (d == 0)
            return std::to_wstring(n);
        if (d < 0)
        {
            n = -n;
            d = -d;
        }
        if (n % d == 0)
            return std::to_wstring(n / d);
        // Exposure times read best as fractions ("1/250"); everything else as a short decimal.
        if (n == 1 || (n > 0 && d % n == 0 && d / n > 1))
            return std::format(L"1/{}", d / n);
        auto text = std::format(L"{:.4f}", static_cast<double>(n) / static_cast<double>(d));
        while (!text.empty() && text.back() == L'0')
            text.pop_back();
        if (!text.empty() && text.back() == L'.')
            text.pop_back();
        return text;
    }

    std::wstring DurationText(int64_t us)
    {
        return us < 0 ? L"unknown" : Timecode(us, false);
    }
}
