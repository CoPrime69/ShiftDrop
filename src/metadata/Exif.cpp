#include "metadata/Exif.h"

#include <algorithm>
#include <cmath>
#include <cwchar>
#include <format>
#include <map>

namespace sd::metadata
{
    namespace
    {
        constexpr uint16_t kExifPointer = 0x8769, kGpsPointer = 0x8825, kInteropPointer = 0xA005;
        constexpr uint16_t kThumbOffset = 0x0201, kThumbLength = 0x0202;

        struct TagInfo
        {
            Ifd ifd;
            uint16_t tag;
            const wchar_t* name;
            const wchar_t* group;
            uint16_t type;
        };

        // The tags people care about (what they say about the person, the camera and the picture).
        // Others are listed by number, in their directory's group.
        const TagInfo kTags[] = {
            { Ifd::Primary, 0x010E, L"Description", L"Description", type::Ascii },
            { Ifd::Primary, 0x010F, L"Camera make", L"Camera", type::Ascii },
            { Ifd::Primary, 0x0110, L"Camera model", L"Camera", type::Ascii },
            { Ifd::Primary, 0x0112, L"Orientation", L"Image", type::Short },
            { Ifd::Primary, 0x011A, L"Horizontal resolution", L"Image", type::Rational },
            { Ifd::Primary, 0x011B, L"Vertical resolution", L"Image", type::Rational },
            { Ifd::Primary, 0x0128, L"Resolution unit", L"Image", type::Short },
            { Ifd::Primary, 0x0131, L"Software", L"Software", type::Ascii },
            { Ifd::Primary, 0x0132, L"Date modified", L"Dates", type::Ascii },
            { Ifd::Primary, 0x013B, L"Artist", L"Author", type::Ascii },
            { Ifd::Primary, 0x013C, L"Host computer", L"Software", type::Ascii },
            { Ifd::Primary, 0x0213, L"YCbCr positioning", L"Image", type::Short },
            { Ifd::Primary, 0x02BC, L"XMP packet", L"Description", type::Byte },
            { Ifd::Primary, 0x8298, L"Copyright", L"Author", type::Ascii },
            { Ifd::Primary, 0x83BB, L"IPTC data", L"Description", type::Undefined },
            { Ifd::Primary, 0x9C9B, L"Title", L"Description", type::Byte },
            { Ifd::Primary, 0x9C9C, L"Comment", L"Description", type::Byte },
            { Ifd::Primary, 0x9C9D, L"Author", L"Author", type::Byte },
            { Ifd::Primary, 0x9C9E, L"Keywords", L"Description", type::Byte },
            { Ifd::Primary, 0x9C9F, L"Subject", L"Description", type::Byte },
            { Ifd::Exif, 0x829A, L"Exposure time", L"Camera", type::Rational },
            { Ifd::Exif, 0x829D, L"F-number", L"Camera", type::Rational },
            { Ifd::Exif, 0x8822, L"Exposure program", L"Camera", type::Short },
            { Ifd::Exif, 0x8827, L"ISO", L"Camera", type::Short },
            { Ifd::Exif, 0x9000, L"EXIF version", L"Other", type::Undefined },
            { Ifd::Exif, 0x9003, L"Date taken", L"Dates", type::Ascii },
            { Ifd::Exif, 0x9004, L"Date digitized", L"Dates", type::Ascii },
            { Ifd::Exif, 0x9010, L"Time zone", L"Dates", type::Ascii },
            { Ifd::Exif, 0x9011, L"Time zone (taken)", L"Dates", type::Ascii },
            { Ifd::Exif, 0x9012, L"Time zone (digitized)", L"Dates", type::Ascii },
            { Ifd::Exif, 0x9201, L"Shutter speed", L"Camera", type::SRational },
            { Ifd::Exif, 0x9202, L"Aperture", L"Camera", type::Rational },
            { Ifd::Exif, 0x9204, L"Exposure bias", L"Camera", type::SRational },
            { Ifd::Exif, 0x9207, L"Metering mode", L"Camera", type::Short },
            { Ifd::Exif, 0x9209, L"Flash", L"Camera", type::Short },
            { Ifd::Exif, 0x920A, L"Focal length", L"Camera", type::Rational },
            { Ifd::Exif, 0x927C, L"Maker notes", L"Camera", type::Undefined },
            { Ifd::Exif, 0x9286, L"User comment", L"Description", type::Undefined },
            { Ifd::Exif, 0x9290, L"Subsecond time", L"Dates", type::Ascii },
            { Ifd::Exif, 0x9291, L"Subsecond time (taken)", L"Dates", type::Ascii },
            { Ifd::Exif, 0x9292, L"Subsecond time (digitized)", L"Dates", type::Ascii },
            { Ifd::Exif, 0xA001, L"Colour space", L"Image", type::Short },
            { Ifd::Exif, 0xA002, L"Width", L"Image", type::Long },
            { Ifd::Exif, 0xA003, L"Height", L"Image", type::Long },
            { Ifd::Exif, 0xA402, L"Exposure mode", L"Camera", type::Short },
            { Ifd::Exif, 0xA403, L"White balance", L"Camera", type::Short },
            { Ifd::Exif, 0xA405, L"Focal length (35 mm)", L"Camera", type::Short },
            { Ifd::Exif, 0xA406, L"Scene type", L"Camera", type::Short },
            { Ifd::Exif, 0xA420, L"Image unique ID", L"Camera", type::Ascii },
            { Ifd::Exif, 0xA430, L"Camera owner", L"Author", type::Ascii },
            { Ifd::Exif, 0xA431, L"Camera serial number", L"Camera", type::Ascii },
            { Ifd::Exif, 0xA432, L"Lens specification", L"Camera", type::Rational },
            { Ifd::Exif, 0xA433, L"Lens make", L"Camera", type::Ascii },
            { Ifd::Exif, 0xA434, L"Lens model", L"Camera", type::Ascii },
            { Ifd::Exif, 0xA435, L"Lens serial number", L"Camera", type::Ascii },
            { Ifd::Gps, 0x0000, L"GPS version", L"Location", type::Byte },
            { Ifd::Gps, 0x0001, L"Latitude reference", L"Location", type::Ascii },
            { Ifd::Gps, 0x0002, L"Latitude", L"Location", type::Rational },
            { Ifd::Gps, 0x0003, L"Longitude reference", L"Location", type::Ascii },
            { Ifd::Gps, 0x0004, L"Longitude", L"Location", type::Rational },
            { Ifd::Gps, 0x0005, L"Altitude reference", L"Location", type::Byte },
            { Ifd::Gps, 0x0006, L"Altitude", L"Location", type::Rational },
            { Ifd::Gps, 0x0007, L"GPS time", L"Location", type::Rational },
            { Ifd::Gps, 0x000C, L"Speed unit", L"Location", type::Ascii },
            { Ifd::Gps, 0x000D, L"Speed", L"Location", type::Rational },
            { Ifd::Gps, 0x0010, L"Direction reference", L"Location", type::Ascii },
            { Ifd::Gps, 0x0011, L"Direction", L"Location", type::Rational },
            { Ifd::Gps, 0x0012, L"Map datum", L"Location", type::Ascii },
            { Ifd::Gps, 0x001D, L"GPS date", L"Location", type::Ascii },
            { Ifd::Interop, 0x0001, L"Interoperability index", L"Other", type::Ascii },
        };

        const TagInfo* Info(Ifd ifd, uint16_t tag)
        {
            for (const auto& t : kTags)
                if (t.ifd == ifd && t.tag == tag)
                    return &t;
            return nullptr;
        }

        std::wstring Trim(std::wstring s)
        {
            while (!s.empty() && (s.back() == L'\0' || s.back() == L' '))
                s.pop_back();
            return s;
        }

        std::wstring Latin1(const uint8_t* p, size_t n)
        {
            std::wstring s;
            for (size_t i = 0; i < n && p[i]; ++i)
                s.push_back(static_cast<wchar_t>(p[i]));
            return Trim(s);
        }

        std::wstring Number(double v)
        {
            if (std::abs(v - std::round(v)) < 1e-9)
                return std::format(L"{}", static_cast<long long>(std::llround(v)));
            return std::format(L"{:.4g}", v);
        }
    }

    const char* IfdKey(Ifd ifd)
    {
        switch (ifd)
        {
        case Ifd::Primary: return "0th";
        case Ifd::Exif: return "exif";
        case Ifd::Gps: return "gps";
        case Ifd::Interop: return "interop";
        case Ifd::Thumbnail: return "1st";
        }
        return "";
    }

    std::optional<Ifd> IfdFromKey(std::string_view key)
    {
        for (const Ifd i : { Ifd::Primary, Ifd::Exif, Ifd::Gps, Ifd::Interop, Ifd::Thumbnail })
            if (key == IfdKey(i))
                return i;
        return std::nullopt;
    }

    size_t TypeSize(uint16_t t)
    {
        switch (t)
        {
        case type::Byte:
        case type::Ascii:
        case type::SByte:
        case type::Undefined: return 1;
        case type::Short:
        case type::SShort: return 2;
        case type::Long:
        case type::SLong:
        case type::Float:
        case 13: return 4;   // IFD
        case type::Rational:
        case type::SRational:
        case type::Double: return 8;
        default: return 0;
        }
    }

    uint16_t ExifData::U16(std::span<const uint8_t> b, size_t at) const
    {
        return m_little ? static_cast<uint16_t>(b[at] | b[at + 1] << 8) : static_cast<uint16_t>(b[at] << 8 | b[at + 1]);
    }

    uint32_t ExifData::U32(std::span<const uint8_t> b, size_t at) const
    {
        return m_little ? uint32_t{ b[at] } | uint32_t{ b[at + 1] } << 8 | uint32_t{ b[at + 2] } << 16 | uint32_t{ b[at + 3] } << 24
                        : uint32_t{ b[at] } << 24 | uint32_t{ b[at + 1] } << 16 | uint32_t{ b[at + 2] } << 8 | uint32_t{ b[at + 3] };
    }

    bool ExifData::Parse(std::span<const uint8_t> b)
    {
        m_dirs.clear();
        m_thumbnail.clear();
        if (b.size() < 8)
            return false;
        if (b[0] == 'I' && b[1] == 'I')
            m_little = true;
        else if (b[0] == 'M' && b[1] == 'M')
            m_little = false;
        else
            return false;
        if (U16(b, 2) != 42)
            return false;
        std::vector<uint32_t> seen;
        return ParseDirectory(b, U32(b, 4), Ifd::Primary, 0, seen);
    }

    bool ExifData::ParseDirectory(std::span<const uint8_t> b, uint32_t offset, Ifd ifd, int depth, std::vector<uint32_t>& seen)
    {
        // Offsets come from the file: guard against loops, overlaps and out-of-range values.
        if (depth > 4 || offset < 8 || offset + 2 > b.size() || std::find(seen.begin(), seen.end(), offset) != seen.end())
            return false;
        seen.push_back(offset);
        const size_t n = U16(b, offset);
        if (offset + 2 + n * 12 + 4 > b.size())
            return false;
        ExifDirectory dir;
        dir.ifd = ifd;
        uint32_t thumbOffset = 0, thumbLength = 0;
        std::vector<std::pair<Ifd, uint32_t>> children;
        for (size_t i = 0; i < n; ++i)
        {
            const size_t at = offset + 2 + i * 12;
            ExifEntry e;
            e.tag = U16(b, at);
            e.type = U16(b, at + 2);
            e.count = U32(b, at + 4);
            const size_t unit = TypeSize(e.type);
            if (unit == 0 || e.count > b.size())
                continue;
            const size_t size = unit * e.count;
            const size_t from = size <= 4 ? at + 8 : U32(b, at + 8);
            if (from + size > b.size())
                continue;
            if ((ifd == Ifd::Primary && (e.tag == kExifPointer || e.tag == kGpsPointer)) || (ifd == Ifd::Exif && e.tag == kInteropPointer))
            {
                children.emplace_back(e.tag == kExifPointer ? Ifd::Exif : e.tag == kGpsPointer ? Ifd::Gps : Ifd::Interop, U32(b, at + 8));
                continue;
            }
            if (ifd == Ifd::Thumbnail && (e.tag == kThumbOffset || e.tag == kThumbLength))
            {
                (e.tag == kThumbOffset ? thumbOffset : thumbLength) = U32(b, at + 8);
                continue;
            }
            e.value.assign(b.begin() + static_cast<ptrdiff_t>(from), b.begin() + static_cast<ptrdiff_t>(from + size));
            dir.entries.push_back(std::move(e));
        }
        if (thumbOffset && thumbLength && thumbOffset + uint64_t{ thumbLength } <= b.size())
            m_thumbnail.assign(b.begin() + thumbOffset, b.begin() + thumbOffset + thumbLength);
        const uint32_t next = U32(b, offset + 2 + n * 12);
        m_dirs.push_back(std::move(dir));
        for (const auto& [child, at] : children)
            ParseDirectory(b, at, child, depth + 1, seen);   // a broken sub-directory is left out, not fatal
        if (ifd == Ifd::Primary && next)
            ParseDirectory(b, next, Ifd::Thumbnail, depth + 1, seen);
        return true;
    }

    size_t ExifData::FieldCount() const
    {
        size_t n = m_thumbnail.empty() ? 0 : 1;
        for (const auto& d : m_dirs)
            n += d.entries.size();
        return n;
    }

    const ExifEntry* ExifData::Find(Ifd ifd, uint16_t tag) const
    {
        for (const auto& d : m_dirs)
            if (d.ifd == ifd)
                for (const auto& e : d.entries)
                    if (e.tag == tag)
                        return &e;
        return nullptr;
    }

    bool ExifData::Remove(Ifd ifd, uint16_t tag)
    {
        for (auto& d : m_dirs)
            if (d.ifd == ifd)
            {
                const auto before = d.entries.size();
                std::erase_if(d.entries, [&](const ExifEntry& e) { return e.tag == tag; });
                return d.entries.size() != before;
            }
        return false;
    }

    void ExifData::RemoveDirectory(Ifd ifd)
    {
        std::erase_if(m_dirs, [&](const ExifDirectory& d) { return d.ifd == ifd; });
        if (ifd == Ifd::Thumbnail)
            m_thumbnail.clear();
    }

    void ExifData::Set(Ifd ifd, ExifEntry entry)
    {
        auto it = std::find_if(m_dirs.begin(), m_dirs.end(), [&](const ExifDirectory& d) { return d.ifd == ifd; });
        if (it == m_dirs.end())
        {
            m_dirs.push_back(ExifDirectory{ ifd, {} });
            it = m_dirs.end() - 1;
        }
        for (auto& e : it->entries)
            if (e.tag == entry.tag)
            {
                e = std::move(entry);
                return;
            }
        it->entries.push_back(std::move(entry));
    }

    uint32_t ExifData::UInt(const ExifEntry& e, size_t i) const
    {
        const size_t unit = TypeSize(e.type);
        if (unit == 0 || (i + 1) * unit > e.value.size())
            return 0;
        switch (e.type)
        {
        case type::Byte:
        case type::Undefined:
        case type::Ascii: return e.value[i];
        case type::SByte: return static_cast<uint32_t>(static_cast<int8_t>(e.value[i]));
        case type::Short:
        case type::SShort: return U16(e.value, i * 2);
        default: return U32(e.value, i * unit);
        }
    }

    double ExifData::Number(const ExifEntry& e, size_t i) const
    {
        if (e.type == type::Rational || e.type == type::SRational)
        {
            if ((i + 1) * 8 > e.value.size())
                return 0;
            const uint32_t n = U32(e.value, i * 8), d = U32(e.value, i * 8 + 4);
            if (d == 0)
                return 0;
            return e.type == type::SRational ? static_cast<double>(static_cast<int32_t>(n)) / static_cast<int32_t>(d)
                                             : static_cast<double>(n) / d;
        }
        if (e.type == type::SShort)
            return static_cast<int16_t>(UInt(e, i));
        if (e.type == type::SLong)
            return static_cast<int32_t>(UInt(e, i));
        return UInt(e, i);
    }

    namespace
    {
        // Output helpers in a chosen byte order.
        struct Writer
        {
            bool little;
            std::vector<uint8_t>& b;
            void U16(size_t at, uint16_t v)
            {
                b[at] = static_cast<uint8_t>(little ? v : v >> 8);
                b[at + 1] = static_cast<uint8_t>(little ? v >> 8 : v);
            }
            void U32(size_t at, uint32_t v)
            {
                for (int i = 0; i < 4; ++i)
                    b[at + static_cast<size_t>(i)] = static_cast<uint8_t>(v >> (little ? 8 * i : 8 * (3 - i)));
            }
        };

        struct Planned
        {
            Ifd ifd;
            std::vector<ExifEntry> entries;   // sorted, pointer tags included (values filled in later)
            uint32_t offset = 0;
            size_t dataSize = 0;
            size_t Size() const { return 2 + entries.size() * 12 + 4 + dataSize; }
        };
    }

    std::vector<uint8_t> ExifData::Serialize() const
    {
        const auto entriesOf = [&](Ifd ifd) {
            for (const auto& d : m_dirs)
                if (d.ifd == ifd)
                    return d.entries;
            return std::vector<ExifEntry>{};
        };
        const auto pointer = [](uint16_t tag) { return ExifEntry{ tag, type::Long, 1, std::vector<uint8_t>(4, 0) }; };
        std::map<Ifd, Planned> plan;
        auto interop = entriesOf(Ifd::Interop);
        auto exif = entriesOf(Ifd::Exif);
        auto gps = entriesOf(Ifd::Gps);
        auto primary = entriesOf(Ifd::Primary);
        auto thumb = entriesOf(Ifd::Thumbnail);
        if (!interop.empty())
            exif.push_back(pointer(kInteropPointer));
        if (!exif.empty())
            primary.push_back(pointer(kExifPointer));
        if (!gps.empty())
            primary.push_back(pointer(kGpsPointer));
        if (!m_thumbnail.empty())
        {
            thumb.push_back(pointer(kThumbOffset));
            ExifEntry length = pointer(kThumbLength);
            thumb.push_back(length);
        }
        if (primary.empty() && thumb.empty())
            return {};
        if (primary.empty())
            primary.push_back(ExifEntry{ 0x0112, type::Short, 1, m_little ? std::vector<uint8_t>{ 1, 0 } : std::vector<uint8_t>{ 0, 1 } });
        const std::pair<Ifd, std::vector<ExifEntry>*> order[] = {
            { Ifd::Primary, &primary }, { Ifd::Exif, &exif }, { Ifd::Gps, &gps }, { Ifd::Interop, &interop }, { Ifd::Thumbnail, &thumb } };
        uint32_t at = 8;
        for (const auto& [ifd, list] : order)
        {
            if (list->empty())
                continue;
            Planned p{ ifd, *list };
            std::sort(p.entries.begin(), p.entries.end(), [](const ExifEntry& a, const ExifEntry& b) { return a.tag < b.tag; });
            for (const auto& e : p.entries)
                if (e.value.size() > 4)
                    p.dataSize += e.value.size() + (e.value.size() & 1);
            p.offset = at;
            at += static_cast<uint32_t>(p.Size());
            plan[ifd] = std::move(p);
        }
        const uint32_t thumbAt = at;
        std::vector<uint8_t> out(at + m_thumbnail.size(), 0);
        Writer w{ m_little, out };
        out[0] = out[1] = m_little ? 'I' : 'M';
        w.U16(2, 42);
        w.U32(4, 8);
        for (auto& [ifd, p] : plan)
        {
            size_t pos = p.offset;
            w.U16(pos, static_cast<uint16_t>(p.entries.size()));
            pos += 2;
            size_t data = p.offset + 2 + p.entries.size() * 12 + 4;
            for (const auto& e : p.entries)
            {
                w.U16(pos, e.tag);
                w.U16(pos + 2, e.type);
                w.U32(pos + 4, e.count);
                uint32_t pointerValue = 0;
                bool isPointer = true;
                if (ifd == Ifd::Primary && e.tag == kExifPointer && plan.contains(Ifd::Exif))
                    pointerValue = plan[Ifd::Exif].offset;
                else if (ifd == Ifd::Primary && e.tag == kGpsPointer && plan.contains(Ifd::Gps))
                    pointerValue = plan[Ifd::Gps].offset;
                else if (ifd == Ifd::Exif && e.tag == kInteropPointer && plan.contains(Ifd::Interop))
                    pointerValue = plan[Ifd::Interop].offset;
                else if (ifd == Ifd::Thumbnail && e.tag == kThumbOffset)
                    pointerValue = thumbAt;
                else if (ifd == Ifd::Thumbnail && e.tag == kThumbLength)
                    pointerValue = static_cast<uint32_t>(m_thumbnail.size());
                else
                    isPointer = false;
                if (isPointer)
                    w.U32(pos + 8, pointerValue);
                else if (e.value.size() <= 4)
                    std::copy(e.value.begin(), e.value.end(), out.begin() + static_cast<ptrdiff_t>(pos + 8));
                else
                {
                    w.U32(pos + 8, static_cast<uint32_t>(data));
                    std::copy(e.value.begin(), e.value.end(), out.begin() + static_cast<ptrdiff_t>(data));
                    data += e.value.size() + (e.value.size() & 1);
                }
                pos += 12;
            }
            // Next IFD: IFD0 links to IFD1 (the thumbnail's directory); every other chain ends here.
            w.U32(pos, ifd == Ifd::Primary && plan.contains(Ifd::Thumbnail) ? plan[Ifd::Thumbnail].offset : 0);
        }
        std::copy(m_thumbnail.begin(), m_thumbnail.end(), out.begin() + thumbAt);
        return out;
    }

    std::optional<ExifEntry> ExifData::FromText(Ifd ifd, uint16_t tag, const std::wstring& text) const
    {
        const TagInfo* info = Info(ifd, tag);
        const ExifEntry* existing = Find(ifd, tag);
        const uint16_t t = info ? info->type : existing ? existing->type : type::Ascii;
        ExifEntry e{ tag, t, 0, {} };
        std::vector<uint8_t> tmp(8);
        Writer w{ m_little, tmp };
        const auto push = [&](size_t n) { e.value.insert(e.value.end(), tmp.begin(), tmp.begin() + static_cast<ptrdiff_t>(n)); };
        switch (t)
        {
        case type::Ascii:
            for (const wchar_t c : text)
            {
                if (c > 0x7E || c < 0x20)
                    return std::nullopt;   // EXIF ASCII is 7-bit
                e.value.push_back(static_cast<uint8_t>(c));
            }
            e.value.push_back(0);
            e.count = static_cast<uint32_t>(e.value.size());
            return e;
        case type::Byte:
            if (ifd == Ifd::Primary && tag >= 0x9C9B && tag <= 0x9C9F)
            {
                // Windows' XP tags: UTF-16LE, NUL-terminated, whatever the block's byte order.
                for (const wchar_t c : text + L'\0')
                {
                    e.value.push_back(static_cast<uint8_t>(c & 0xFF));
                    e.value.push_back(static_cast<uint8_t>(c >> 8));
                }
                e.count = static_cast<uint32_t>(e.value.size());
                return e;
            }
            [[fallthrough]];
        case type::Short:
        case type::Long:
        {
            wchar_t* end = nullptr;
            const unsigned long v = std::wcstoul(text.c_str(), &end, 10);
            if (end == text.c_str() || *end)
                return std::nullopt;
            if (t == type::Byte)
                tmp[0] = static_cast<uint8_t>(v), push(1);
            else if (t == type::Short)
                w.U16(0, static_cast<uint16_t>(v)), push(2);
            else
                w.U32(0, static_cast<uint32_t>(v)), push(4);
            e.count = 1;
            return e;
        }
        case type::Rational:
        case type::SRational:
        {
            const auto rational = [&](double v) {
                // a/b with b = 10000, or the fraction as typed ("1/250").
                const int32_t d = 10000;
                const auto n = static_cast<int32_t>(std::llround(v * d));
                w.U32(0, static_cast<uint32_t>(n));
                w.U32(4, static_cast<uint32_t>(d));
                push(8);
            };
            if (ifd == Ifd::Gps && (tag == 0x0002 || tag == 0x0004))
            {
                // Decimal degrees -> degrees, minutes, seconds.
                wchar_t* end = nullptr;
                double v = std::abs(std::wcstod(text.c_str(), &end));
                if (end == text.c_str())
                    return std::nullopt;
                const double deg = std::floor(v);
                v = (v - deg) * 60;
                const double min = std::floor(v);
                rational(deg);
                rational(min);
                rational((v - min) * 60);
                e.count = 3;
                return e;
            }
            const auto slash = text.find(L'/');
            wchar_t* end = nullptr;
            if (slash != std::wstring::npos)
            {
                const long n = std::wcstol(text.c_str(), &end, 10);
                const long d = std::wcstol(text.c_str() + slash + 1, nullptr, 10);
                if (d == 0)
                    return std::nullopt;
                w.U32(0, static_cast<uint32_t>(n));
                w.U32(4, static_cast<uint32_t>(d));
                push(8);
            }
            else
            {
                const double v = std::wcstod(text.c_str(), &end);
                if (end == text.c_str())
                    return std::nullopt;
                rational(v);
            }
            e.count = 1;
            return e;
        }
        default:
            return std::nullopt;
        }
    }

    std::wstring TagName(Ifd ifd, uint16_t tag)
    {
        if (const auto* t = Info(ifd, tag))
            return t->name;
        return std::format(L"Tag 0x{:04X}", tag);
    }

    std::wstring TagGroup(Ifd ifd, uint16_t tag)
    {
        if (const auto* t = Info(ifd, tag))
            return t->group;
        switch (ifd)
        {
        case Ifd::Gps: return L"Location";
        case Ifd::Thumbnail: return L"Thumbnail";
        case Ifd::Exif: return L"Camera";
        default: return L"Other";
        }
    }

    std::wstring FormatValue(const ExifData& exif, Ifd ifd, const ExifEntry& e)
    {
        if (e.type == type::Ascii)
            return Latin1(e.value.data(), e.value.size());
        if (ifd == Ifd::Primary && e.tag >= 0x9C9B && e.tag <= 0x9C9F)
        {
            std::wstring s;
            for (size_t i = 0; i + 1 < e.value.size(); i += 2)
                s.push_back(static_cast<wchar_t>(e.value[i] | e.value[i + 1] << 8));
            return Trim(s);
        }
        if (ifd == Ifd::Exif && e.tag == 0x9286 && e.value.size() > 8)   // user comment: 8-byte charset, then text
            return Latin1(e.value.data() + 8, e.value.size() - 8);
        if (ifd == Ifd::Exif && e.tag == 0x9000)
            return Latin1(e.value.data(), e.value.size());
        if (e.type == type::Undefined || (e.type == type::Byte && e.count > 8))
            return std::format(L"({} bytes)", e.value.size());
        if (ifd == Ifd::Gps && (e.tag == 0x0002 || e.tag == 0x0004) && e.count == 3)
            return std::format(L"{}° {}′ {:.2f}″", Number(exif.Number(e, 0)), Number(exif.Number(e, 1)), exif.Number(e, 2));
        if (ifd == Ifd::Exif && e.tag == 0x829A)
        {
            const double v = exif.Number(e);
            return v > 0 && v < 1 ? std::format(L"1/{} s", std::llround(1 / v)) : Number(v) + L" s";
        }
        if (ifd == Ifd::Exif && e.tag == 0x829D)
            return L"f/" + Number(exif.Number(e));
        if (ifd == Ifd::Exif && e.tag == 0x920A)
            return Number(exif.Number(e)) + L" mm";
        std::wstring s;
        for (uint32_t i = 0; i < std::min<uint32_t>(e.count, 8); ++i)
            s += (i ? L", " : L"") + Number(exif.Number(e, i));
        if (e.count > 8)
            s += L", …";
        return s;
    }

    std::optional<double> GpsDegrees(const ExifData& exif, bool latitude)
    {
        const ExifEntry* value = exif.Find(Ifd::Gps, latitude ? 0x0002 : 0x0004);
        if (!value || value->count < 3)
            return std::nullopt;
        double v = exif.Number(*value, 0) + exif.Number(*value, 1) / 60 + exif.Number(*value, 2) / 3600;
        if (const ExifEntry* ref = exif.Find(Ifd::Gps, latitude ? 0x0001 : 0x0003); ref && !ref->value.empty())
            if (ref->value[0] == 'S' || ref->value[0] == 'W')
                v = -v;
        return v;
    }
}
