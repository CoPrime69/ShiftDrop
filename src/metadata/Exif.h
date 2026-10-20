#pragma once
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

// EXIF: the TIFF structure inside a JPEG's APP1, a PNG's eXIf or a WebP's EXIF chunk. Parsed into
// directories of raw entries, edited, and written back out as a fresh, compact TIFF structure.
namespace sd::metadata
{
    enum class Ifd { Primary, Exif, Gps, Interop, Thumbnail };   // IFD0, Exif IFD, GPS IFD, Interop IFD, IFD1
    const char* IfdKey(Ifd ifd);                                  // "0th", "exif", "gps", "interop", "1st"
    std::optional<Ifd> IfdFromKey(std::string_view key);

    namespace type
    {
        constexpr uint16_t Byte = 1, Ascii = 2, Short = 3, Long = 4, Rational = 5, SByte = 6, Undefined = 7, SShort = 8, SLong = 9,
                           SRational = 10, Float = 11, Double = 12;
    }
    size_t TypeSize(uint16_t type);   // bytes per value; 0 for unknown types

    struct ExifEntry
    {
        uint16_t tag = 0;
        uint16_t type = 0;
        uint32_t count = 0;
        std::vector<uint8_t> value;   // count * TypeSize(type) bytes, in the block's byte order
    };

    struct ExifDirectory
    {
        Ifd ifd = Ifd::Primary;
        std::vector<ExifEntry> entries;   // without the pointer tags (Exif, GPS, Interop, thumbnail offset)
    };

    class ExifData
    {
    public:
        // Parses a TIFF structure ("II*\0" or "MM\0*" first). False if it is not one.
        bool Parse(std::span<const uint8_t> tiff);
        // A fresh TIFF structure with the same byte order: IFD0 and its Exif/GPS/Interop IFDs, then
        // IFD1 and the thumbnail if there is one. Empty if there is nothing left to write.
        std::vector<uint8_t> Serialize() const;

        bool LittleEndian() const { return m_little; }
        const std::vector<ExifDirectory>& Directories() const { return m_dirs; }
        const std::vector<uint8_t>& ThumbnailBytes() const { return m_thumbnail; }
        size_t FieldCount() const;   // every entry, plus one for a thumbnail

        const ExifEntry* Find(Ifd ifd, uint16_t tag) const;
        bool Remove(Ifd ifd, uint16_t tag);
        void RemoveDirectory(Ifd ifd);   // the whole GPS IFD, the thumbnail...
        void Set(Ifd ifd, ExifEntry entry);

        // Reading values in the block's byte order.
        uint32_t UInt(const ExifEntry& e, size_t index = 0) const;
        double Number(const ExifEntry& e, size_t index = 0) const;   // rationals as doubles
        // Builds an entry from text for a known tag ("Canon", "2026:10:20 12:00:00", "1/250", "5.6"):
        // the tag table gives the type. Nullopt if the text doesn't fit.
        std::optional<ExifEntry> FromText(Ifd ifd, uint16_t tag, const std::wstring& text) const;

    private:
        bool ParseDirectory(std::span<const uint8_t> b, uint32_t offset, Ifd ifd, int depth, std::vector<uint32_t>& seen);
        uint16_t U16(std::span<const uint8_t> b, size_t at) const;
        uint32_t U32(std::span<const uint8_t> b, size_t at) const;

        bool m_little = true;
        std::vector<ExifDirectory> m_dirs;
        std::vector<uint8_t> m_thumbnail;
    };

    // Names and grouping for display and the share-safe report.
    std::wstring TagName(Ifd ifd, uint16_t tag);                  // "Make", "GPS latitude"; "Tag 0x1234" if unknown
    // "Location", "Camera", "Dates", "Author", "Software", "Description", "Image", "Thumbnail", "Other".
    std::wstring TagGroup(Ifd ifd, uint16_t tag);
    std::wstring FormatValue(const ExifData& exif, Ifd ifd, const ExifEntry& entry);
    // Decimal degrees from a GPS latitude/longitude entry and its N/S or E/W reference.
    std::optional<double> GpsDegrees(const ExifData& exif, bool latitude);
}
