#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// The metadata inspector's model (Edit Metadata, part 1): fields grouped by file, image, camera, GPS,
// media tracks and chapters, with a search over all of them. Pure; reading the fields from a file is
// MetadataReader.h.
namespace sd::inspect
{
    struct Field
    {
        std::wstring group;   // "File", "Camera", "Video track 1", "Chapters"...
        std::wstring name;    // "Camera model"
        std::wstring value;   // "Canon EOS R6"
    };

    struct Group
    {
        std::wstring name;
        std::vector<Field> fields;
    };

    // Groups in the order the inspector lists them: File, Image, Camera, GPS, Description, Media, the
    // tracks (in track order), Chapters, Tags, then the raw EXIF / XMP / IPTC blocks and the rest.
    // Fields keep their order within a group; a field whose name and value repeat one already in the
    // group is dropped.
    std::vector<Group> GroupFields(const std::vector<Field>& fields);

    // Fields matching every word of `query` (case-insensitive) in their group, name or value.
    // An empty query matches everything.
    std::vector<Field> Filter(const std::vector<Field>& fields, std::wstring_view query);

    // Which group a Windows property (its canonical name, "System.GPS.Latitude") belongs to; empty
    // for properties the File group already shows (size, dates, path) or that mean nothing to a user.
    std::wstring PropertyGroup(std::wstring_view canonicalName);

    // WIC metadata query paths ("/app1/ifd/exif/{ushort=33434}") as a group and a readable name
    // ("EXIF", "ExposureTime"). `skip` for blocks that are not fields (thumbnails, padding).
    struct PathName
    {
        std::wstring group, name;
        bool skip = false;
    };
    PathName DescribeWicPath(std::wstring_view path);
    // EXIF tag names (TIFF/EXIF IFDs, or the GPS IFD when `gps`); "Tag 0x1234" when unknown.
    std::wstring ExifTagName(unsigned tag, bool gps);

    // "1/250", "2.8", "72"; WIC hands EXIF rationals over as numerator and denominator.
    std::wstring FormatRational(int64_t numerator, int64_t denominator);
    // "0:01:23.450" from microseconds
    std::wstring DurationText(int64_t us);
}
