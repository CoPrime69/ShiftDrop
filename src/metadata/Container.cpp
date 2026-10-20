#include "metadata/Container.h"

#include <windows.h>
#include <wil/resource.h>

#include <array>
#include <cstring>
#include <format>
#include <string_view>

namespace sd::metadata
{
    namespace
    {
        uint16_t Be16(std::span<const uint8_t> b, size_t at) { return static_cast<uint16_t>(b[at] << 8 | b[at + 1]); }
        uint32_t Be32(std::span<const uint8_t> b, size_t at)
        {
            return uint32_t{ b[at] } << 24 | uint32_t{ b[at + 1] } << 16 | uint32_t{ b[at + 2] } << 8 | b[at + 3];
        }
        uint32_t Le32(std::span<const uint8_t> b, size_t at)
        {
            return uint32_t{ b[at] } | uint32_t{ b[at + 1] } << 8 | uint32_t{ b[at + 2] } << 16 | uint32_t{ b[at + 3] } << 24;
        }
        void PutLe32(std::vector<uint8_t>& b, size_t at, uint32_t v)
        {
            for (int i = 0; i < 4; ++i)
                b[at + i] = static_cast<uint8_t>(v >> (8 * i));
        }
        void AppendBe32(std::vector<uint8_t>& b, uint32_t v)
        {
            for (int i = 3; i >= 0; --i)
                b.push_back(static_cast<uint8_t>(v >> (8 * i)));
        }

        bool StartsWith(std::span<const uint8_t> b, size_t at, std::string_view s)
        {
            return at + s.size() <= b.size() && std::memcmp(b.data() + at, s.data(), s.size()) == 0;
        }

        uint32_t Crc32(std::span<const uint8_t> data)
        {
            static const auto table = [] {
                std::array<uint32_t, 256> t{};
                for (uint32_t n = 0; n < 256; ++n)
                {
                    uint32_t c = n;
                    for (int k = 0; k < 8; ++k)
                        c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
                    t[n] = c;
                }
                return t;
            }();
            uint32_t c = 0xFFFFFFFFu;
            for (const uint8_t byte : data)
                c = table[(c ^ byte) & 0xFF] ^ (c >> 8);
            return c ^ 0xFFFFFFFFu;
        }

        Block Make(BlockKind kind, size_t offset, size_t size, size_t payload, size_t payloadSize, std::string name)
        {
            return Block{ kind, offset, size, payload, payloadSize, std::move(name) };
        }

        // ---- JPEG ----------------------------------------------------------------------------

        BlockKind JpegAppKind(uint8_t marker, std::span<const uint8_t> b, size_t payload, std::string& name)
        {
            const int n = marker - 0xE0;
            name = std::format("APP{}", n);
            switch (marker)
            {
            case 0xE0:
                if (StartsWith(b, payload, std::string_view("JFIF\0", 5)))
                    return name += " JFIF", BlockKind::Jfif;
                if (StartsWith(b, payload, std::string_view("JFXX\0", 5)))
                    return name += " JFIF thumbnail", BlockKind::Thumbnail;
                return BlockKind::Other;
            case 0xE1:
                if (StartsWith(b, payload, std::string_view("Exif\0", 5)))
                    return name += " Exif", BlockKind::Exif;
                if (StartsWith(b, payload, "http://ns.adobe.com/xap/1.0/") || StartsWith(b, payload, "http://ns.adobe.com/xmp/extension/"))
                    return name += " XMP", BlockKind::Xmp;
                return BlockKind::Other;
            case 0xE2:
                if (StartsWith(b, payload, std::string_view("ICC_PROFILE\0", 12)))
                    return name += " ICC profile", BlockKind::Colour;
                if (StartsWith(b, payload, std::string_view("MPF\0", 4)))
                    return name += " multi-picture index", BlockKind::ExtraImage;
                if (StartsWith(b, payload, std::string_view("FPXR\0", 5)))
                    return name += " FlashPix", BlockKind::ExtraImage;
                return BlockKind::Other;
            case 0xED:
                return name += " Photoshop/IPTC", BlockKind::Iptc;
            case 0xEE:
                // Adobe's APP14 says whether CMYK/YCCK data is colour-transformed; decoding needs it.
                if (StartsWith(b, payload, "Adobe"))
                    return name += " Adobe", BlockKind::Colour;
                return BlockKind::Other;
            default:
                return BlockKind::Other;
            }
        }

        bool ListJpeg(std::span<const uint8_t> b, std::vector<Block>& out)
        {
            if (b.size() < 4 || b[0] != 0xFF || b[1] != 0xD8)
                return false;
            out.push_back(Make(BlockKind::Image, 0, 2, 0, 0, "SOI"));
            size_t pos = 2;
            while (pos < b.size())
            {
                if (b[pos] != 0xFF)
                    return false;
                size_t m = pos;
                while (m < b.size() && b[m] == 0xFF)   // fill bytes
                    ++m;
                if (m >= b.size())
                    return false;
                const uint8_t marker = b[m];
                const size_t afterMarker = m + 1;
                if (marker == 0xD9)
                {
                    out.push_back(Make(BlockKind::Image, pos, afterMarker - pos, 0, 0, "EOI"));
                    pos = afterMarker;
                    break;
                }
                if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7))
                {
                    out.push_back(Make(BlockKind::Image, pos, afterMarker - pos, 0, 0, "marker"));
                    pos = afterMarker;
                    continue;
                }
                if (afterMarker + 2 > b.size())
                    return false;
                const size_t len = Be16(b, afterMarker);
                if (len < 2 || afterMarker + len > b.size())
                    return false;
                const size_t end = afterMarker + len;
                const size_t payload = afterMarker + 2, payloadSize = len - 2;
                std::string name;
                BlockKind kind = BlockKind::Image;
                if (marker >= 0xE0 && marker <= 0xEF)
                    kind = JpegAppKind(marker, b, payload, name);
                else if (marker == 0xFE)
                    kind = BlockKind::Comment, name = "COM";
                else
                    name = std::format("marker {:02X}", marker);
                if (marker != 0xDA)
                {
                    out.push_back(Make(kind, pos, end - pos, payload, payloadSize, std::move(name)));
                    pos = end;
                    continue;
                }
                // SOS: the entropy-coded data runs to the next marker that is not a stuffed byte (FF 00),
                // a restart marker or fill.
                size_t scan = end;
                while (scan + 1 < b.size())
                {
                    if (b[scan] == 0xFF)
                    {
                        const uint8_t next = b[scan + 1];
                        if (next == 0x00 || (next >= 0xD0 && next <= 0xD7) || next == 0xFF)
                        {
                            scan += next == 0xFF ? 1 : 2;
                            continue;
                        }
                        break;
                    }
                    ++scan;
                }
                if (scan + 1 >= b.size())
                    scan = b.size();   // truncated file: keep what there is
                out.push_back(Make(BlockKind::Image, pos, scan - pos, payload, payloadSize, "SOS + scan"));
                pos = scan;
            }
            if (pos < b.size())
                out.push_back(Make(BlockKind::Trailer, pos, b.size() - pos, pos, b.size() - pos, "data after the image"));
            return true;
        }

        // ---- PNG -----------------------------------------------------------------------------

        bool KeepPngChunk(std::string_view type)
        {
            static const std::string_view kKeep[] = { "IHDR", "PLTE", "IDAT", "IEND", "tRNS", "gAMA", "cHRM", "sRGB", "iCCP", "sBIT", "bKGD",
                                                      "pHYs", "hIST", "sPLT", "acTL", "fcTL", "fdAT", "cICP", "mDCv", "cLLi", "mDCV", "cLLI" };
            for (const auto k : kKeep)
                if (type == k)
                    return true;
            return type[0] >= 'A' && type[0] <= 'Z';   // unknown critical chunks: decoding needs them
        }

        bool ListPng(std::span<const uint8_t> b, std::vector<Block>& out)
        {
            static const uint8_t kSig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
            if (b.size() < 8 || std::memcmp(b.data(), kSig, 8) != 0)
                return false;
            out.push_back(Make(BlockKind::Image, 0, 8, 0, 0, "signature"));
            size_t pos = 8;
            bool ended = false;
            while (pos + 12 <= b.size() && !ended)
            {
                const size_t len = Be32(b, pos);
                if (len > b.size() - pos - 12)
                    return false;
                const std::string type(reinterpret_cast<const char*>(b.data() + pos + 4), 4);
                BlockKind kind = BlockKind::Image;
                std::string name = type;
                if (type == "tEXt" || type == "zTXt" || type == "iTXt")
                {
                    kind = BlockKind::Text;
                    const auto* data = reinterpret_cast<const char*>(b.data() + pos + 8);
                    const size_t key = strnlen(data, len);
                    name += " " + std::string(data, key);
                }
                else if (type == "eXIf")
                    kind = BlockKind::Exif;
                else if (type == "tIME")
                    kind = BlockKind::Time;
                else if (type == "iCCP" || type == "gAMA" || type == "cHRM" || type == "sRGB" || type == "cICP")
                    kind = BlockKind::Colour;
                else if (!KeepPngChunk(type))
                    kind = BlockKind::Other;
                ended = type == "IEND";
                out.push_back(Make(kind, pos, len + 12, pos + 8, len, std::move(name)));
                pos += len + 12;
            }
            if (!ended)
                return false;
            if (pos < b.size())
                out.push_back(Make(BlockKind::Trailer, pos, b.size() - pos, pos, b.size() - pos, "data after the image"));
            return true;
        }

        // ---- WebP ----------------------------------------------------------------------------

        bool ListWebP(std::span<const uint8_t> b, std::vector<Block>& out)
        {
            if (b.size() < 12 || !StartsWith(b, 0, "RIFF") || !StartsWith(b, 8, "WEBP"))
                return false;
            const size_t riffEnd = std::min<size_t>(b.size(), size_t{ Le32(b, 4) } + 8);
            out.push_back(Make(BlockKind::Image, 0, 12, 0, 0, "RIFF header"));
            size_t pos = 12;
            while (pos + 8 <= riffEnd)
            {
                const std::string type(reinterpret_cast<const char*>(b.data() + pos), 4);
                const size_t len = Le32(b, pos + 4);
                const size_t padded = len + (len & 1);
                if (padded > riffEnd - pos - 8)
                    return false;
                BlockKind kind = BlockKind::Other;
                if (type == "VP8 " || type == "VP8L" || type == "VP8X" || type == "ALPH" || type == "ANIM" || type == "ANMF")
                    kind = BlockKind::Image;
                else if (type == "ICCP")
                    kind = BlockKind::Colour;
                else if (type == "EXIF")
                    kind = BlockKind::Exif;
                else if (type == "XMP ")
                    kind = BlockKind::Xmp;
                out.push_back(Make(kind, pos, padded + 8, pos + 8, len, type + " chunk"));
                pos += padded + 8;
            }
            if (pos < b.size())
                out.push_back(Make(BlockKind::Trailer, pos, b.size() - pos, pos, b.size() - pos, "data after the image"));
            return true;
        }

        // ---- GIF -----------------------------------------------------------------------------

        // Sub-blocks (a size byte, then that many bytes) up to the zero terminator; returns the end.
        bool SkipSubBlocks(std::span<const uint8_t> b, size_t& pos)
        {
            while (pos < b.size())
            {
                const size_t n = b[pos];
                ++pos;
                if (n == 0)
                    return true;
                pos += n;
            }
            return false;
        }

        bool ListGif(std::span<const uint8_t> b, std::vector<Block>& out)
        {
            if (b.size() < 13 || (!StartsWith(b, 0, "GIF87a") && !StartsWith(b, 0, "GIF89a")))
                return false;
            size_t pos = 13;
            if (b[10] & 0x80)
                pos += 3 * (size_t{ 1 } << ((b[10] & 7) + 1));
            if (pos > b.size())
                return false;
            out.push_back(Make(BlockKind::Image, 0, pos, 0, 0, "header"));
            while (pos < b.size())
            {
                const size_t start = pos;
                const uint8_t introducer = b[pos];
                if (introducer == 0x3B)
                {
                    out.push_back(Make(BlockKind::Image, pos, 1, 0, 0, "trailer"));
                    ++pos;
                    break;
                }
                if (introducer == 0x2C)
                {
                    if (pos + 10 > b.size())
                        return false;
                    const uint8_t flags = b[pos + 9];
                    pos += 10;
                    if (flags & 0x80)
                        pos += 3 * (size_t{ 1 } << ((flags & 7) + 1));
                    ++pos;   // LZW minimum code size
                    if (pos > b.size() || !SkipSubBlocks(b, pos))
                        return false;
                    out.push_back(Make(BlockKind::Image, start, pos - start, 0, 0, "image"));
                    continue;
                }
                if (introducer != 0x21 || pos + 2 > b.size())
                    return false;
                const uint8_t label = b[pos + 1];
                pos += 2;
                BlockKind kind = BlockKind::Image;
                std::string name = "extension";
                if (label == 0xFE)
                    kind = BlockKind::Comment, name = "comment";
                else if (label == 0xFF)
                {
                    // Application extension: an 11-byte identifier first.
                    const bool loop = pos + 12 <= b.size() && (StartsWith(b, pos + 1, "NETSCAPE2.0") || StartsWith(b, pos + 1, "ANIMEXTS1.0"));
                    const bool icc = pos + 12 <= b.size() && StartsWith(b, pos + 1, "ICCRGBG1012");
                    const bool xmp = pos + 12 <= b.size() && StartsWith(b, pos + 1, "XMP DataXMP");
                    kind = loop ? BlockKind::Image : icc ? BlockKind::Colour : xmp ? BlockKind::Xmp : BlockKind::Other;
                    name = xmp ? "XMP" : loop ? "loop" : icc ? "ICC profile" : "application data";
                }
                if (!SkipSubBlocks(b, pos))
                    return false;
                out.push_back(Make(kind, start, pos - start, start, pos - start, std::move(name)));
            }
            if (pos < b.size())
                out.push_back(Make(BlockKind::Trailer, pos, b.size() - pos, pos, b.size() - pos, "data after the image"));
            return true;
        }

        // A JFIF header with its embedded thumbnail cut off (thumbnail size 0 x 0).
        bool JfifThumbnail(std::span<const uint8_t> b, const Block& block, std::vector<uint8_t>& trimmed)
        {
            if (block.payloadSize < 14)
                return false;
            const size_t p = block.payload;
            if (b[p + 12] == 0 && b[p + 13] == 0)
                return false;
            trimmed = { 0xFF, 0xE0, 0x00, 0x10 };
            trimmed.insert(trimmed.end(), b.begin() + static_cast<ptrdiff_t>(p), b.begin() + static_cast<ptrdiff_t>(p + 12));
            trimmed.push_back(0);
            trimmed.push_back(0);
            return true;
        }

        // WebP after chunks were removed or added: the RIFF size, and the extended header's EXIF
        // (0x08) and XMP (0x04) flags, must match what is left.
        void FixWebP(std::vector<uint8_t>& out)
        {
            if (out.size() < 12)
                return;
            bool exif = false, xmp = false;
            size_t vp8x = 0;
            for (size_t pos = 12; pos + 8 <= out.size();)
            {
                const std::string_view type(reinterpret_cast<const char*>(out.data() + pos), 4);
                const size_t len = Le32(out, pos + 4);
                exif |= type == "EXIF";
                xmp |= type == "XMP ";
                if (type == "VP8X")
                    vp8x = pos;
                pos += 8 + len + (len & 1);
            }
            if (vp8x && vp8x + 8 < out.size())
            {
                uint8_t& flags = out[vp8x + 8];
                flags = static_cast<uint8_t>((flags & ~0x0C) | (exif ? 0x08 : 0) | (xmp ? 0x04 : 0));
            }
            PutLe32(out, 4, static_cast<uint32_t>(out.size() - 8));
        }

        void Append(std::vector<uint8_t>& out, std::span<const uint8_t> b, const Block& block)
        {
            out.insert(out.end(), b.begin() + static_cast<ptrdiff_t>(block.offset), b.begin() + static_cast<ptrdiff_t>(block.offset + block.size));
        }
    }

    Container DetectContainer(std::span<const uint8_t> b)
    {
        if (b.size() >= 3 && b[0] == 0xFF && b[1] == 0xD8 && b[2] == 0xFF)
            return Container::Jpeg;
        if (b.size() >= 8 && b[0] == 0x89 && StartsWith(b, 1, "PNG"))
            return Container::Png;
        if (b.size() >= 12 && StartsWith(b, 0, "RIFF") && StartsWith(b, 8, "WEBP"))
            return Container::WebP;
        if (b.size() >= 6 && (StartsWith(b, 0, "GIF87a") || StartsWith(b, 0, "GIF89a")))
            return Container::Gif;
        if (b.size() >= 8 && (StartsWith(b, 0, std::string_view("II*\0", 4)) || StartsWith(b, 0, std::string_view("MM\0*", 4))))
            return Container::Tiff;
        return Container::Unknown;
    }

    bool IsPersonal(BlockKind kind)
    {
        switch (kind)
        {
        case BlockKind::Image:
        case BlockKind::Colour:
        case BlockKind::Jfif:
            return false;
        default:
            return true;
        }
    }

    bool ListBlocks(std::span<const uint8_t> bytes, Container container, std::vector<Block>& blocks)
    {
        blocks.clear();
        switch (container)
        {
        case Container::Jpeg: return ListJpeg(bytes, blocks);
        case Container::Png: return ListPng(bytes, blocks);
        case Container::WebP: return ListWebP(bytes, blocks);
        case Container::Gif: return ListGif(bytes, blocks);
        default: return false;
        }
    }

    bool StripPersonal(std::span<const uint8_t> bytes, std::vector<uint8_t>& out, std::vector<Block>* removed, std::wstring* error)
    {
        const Container container = DetectContainer(bytes);
        std::vector<Block> blocks;
        if (!ListBlocks(bytes, container, blocks))
        {
            if (error)
                *error = container == Container::Unknown ? L"not a JPEG, PNG, WebP or GIF file" : L"the file's structure is damaged";
            return false;
        }
        out.clear();
        out.reserve(bytes.size());
        for (const auto& block : blocks)
        {
            if (IsPersonal(block.kind))
            {
                if (removed)
                    removed->push_back(block);
                continue;
            }
            std::vector<uint8_t> trimmed;
            if (block.kind == BlockKind::Jfif && JfifThumbnail(bytes, block, trimmed))
            {
                out.insert(out.end(), trimmed.begin(), trimmed.end());
                if (removed)
                    removed->push_back(Make(BlockKind::Thumbnail, block.offset, block.size, block.payload, block.payloadSize, "JFIF thumbnail"));
                continue;
            }
            Append(out, bytes, block);
        }
        if (container == Container::WebP)
            FixWebP(out);
        return true;
    }

    bool RemoveBlocks(std::span<const uint8_t> bytes, const std::vector<Block>& drop, std::vector<uint8_t>& out, std::wstring* error)
    {
        const Container container = DetectContainer(bytes);
        std::vector<Block> blocks;
        if (!ListBlocks(bytes, container, blocks))
        {
            if (error)
                *error = L"the file's structure is damaged";
            return false;
        }
        out.clear();
        for (const auto& block : blocks)
        {
            bool skip = false;
            for (const auto& d : drop)
                skip |= d.offset == block.offset && d.size == block.size;
            if (!skip)
                Append(out, bytes, block);
        }
        if (container == Container::WebP)
            FixWebP(out);
        return true;
    }

    bool ReplaceExif(std::span<const uint8_t> bytes, std::span<const uint8_t> tiff, std::vector<uint8_t>& out, std::wstring* error)
    {
        const Container container = DetectContainer(bytes);
        std::vector<Block> blocks;
        if (!ListBlocks(bytes, container, blocks))
        {
            if (error)
                *error = container == Container::Unknown ? L"editing metadata needs a JPEG, PNG or WebP file" : L"the file's structure is damaged";
            return false;
        }
        std::vector<uint8_t> block;   // the new EXIF block, ready to insert
        if (!tiff.empty())
        {
            switch (container)
            {
            case Container::Jpeg:
            {
                const size_t len = tiff.size() + 6 + 2;
                if (len > 0xFFFF)
                {
                    if (error)
                        *error = L"the EXIF data is larger than a JPEG segment can hold (64 KB)";
                    return false;
                }
                block = { 0xFF, 0xE1, static_cast<uint8_t>(len >> 8), static_cast<uint8_t>(len & 0xFF), 'E', 'x', 'i', 'f', 0, 0 };
                block.insert(block.end(), tiff.begin(), tiff.end());
                break;
            }
            case Container::Png:
            {
                AppendBe32(block, static_cast<uint32_t>(tiff.size()));
                const size_t typeAt = block.size();
                block.insert(block.end(), { 'e', 'X', 'I', 'f' });
                block.insert(block.end(), tiff.begin(), tiff.end());
                AppendBe32(block, Crc32(std::span<const uint8_t>(block).subspan(typeAt)));
                break;
            }
            case Container::WebP:
            {
                bool extended = false;
                for (const auto& b : blocks)
                    extended |= b.name == "VP8X chunk";
                if (!extended)
                {
                    if (error)
                        *error = L"this WebP has no extended header to hold metadata";
                    return false;
                }
                block = { 'E', 'X', 'I', 'F', 0, 0, 0, 0 };
                PutLe32(block, 4, static_cast<uint32_t>(tiff.size()));
                block.insert(block.end(), tiff.begin(), tiff.end());
                if (tiff.size() & 1)
                    block.push_back(0);
                break;
            }
            default:
                if (error)
                    *error = L"editing metadata needs a JPEG, PNG or WebP file";
                return false;
            }
        }

        // Where it goes: JPEG after SOI and JFIF; PNG right after IHDR (before any IDAT); WebP after the
        // image data, before XMP (the chunk order the WebP spec gives).
        out.clear();
        bool placed = block.empty();
        for (size_t i = 0; i < blocks.size(); ++i)
        {
            const auto& b = blocks[i];
            if (b.kind == BlockKind::Exif)
            {
                if (!placed)
                {
                    out.insert(out.end(), block.begin(), block.end());
                    placed = true;
                }
                continue;
            }
            const bool beforeThis = !placed && ((container == Container::WebP && (b.kind == BlockKind::Xmp || b.kind == BlockKind::Trailer)));
            if (beforeThis)
            {
                out.insert(out.end(), block.begin(), block.end());
                placed = true;
            }
            Append(out, bytes, b);
            const bool afterThis = !placed && ((container == Container::Jpeg && (b.name == "SOI" || b.kind == BlockKind::Jfif) &&
                                                !(i + 1 < blocks.size() && blocks[i + 1].kind == BlockKind::Jfif)) ||
                                               (container == Container::Png && b.name == "IHDR"));
            if (afterThis)
            {
                out.insert(out.end(), block.begin(), block.end());
                placed = true;
            }
        }
        if (!placed)
        {
            // WebP with nothing after the image data: the end of the RIFF.
            out.insert(out.end(), block.begin(), block.end());
        }
        if (container == Container::WebP)
            FixWebP(out);
        return true;
    }

    bool ReadFileBytes(const std::wstring& path, std::vector<uint8_t>& bytes)
    {
        wil::unique_hfile file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
        LARGE_INTEGER size{};
        if (!file || !GetFileSizeEx(file.get(), &size) || size.QuadPart > (1ll << 31))
            return false;
        bytes.resize(static_cast<size_t>(size.QuadPart));
        DWORD read = 0;
        if (!bytes.empty() && !ReadFile(file.get(), bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr))
            return false;
        bytes.resize(read);
        return true;
    }

    bool WriteFileBytes(const std::wstring& path, std::span<const uint8_t> bytes)
    {
        wil::unique_hfile file(CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (!file)
            return false;
        DWORD written = 0;
        if (!bytes.empty() && (!WriteFile(file.get(), bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) || written != bytes.size()))
            return false;
        return SetEndOfFile(file.get()) != FALSE && FlushFileBuffers(file.get()) != FALSE;
    }
}
