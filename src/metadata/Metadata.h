#pragma once
#include <windows.h>

#include <cstdint>
#include <span>
#include <string>
#include <vector>

// Image metadata: read it grouped, remove some or all of it, set a field, and verify a file is
// clean. Share-safe and Remove metadata use the removal and verification.
//
// JPEG, PNG, WebP and GIF are edited at the container level (Container.h, Exif.h): the compressed
// pixels are copied byte for byte, so nothing is re-encoded and nothing is lost. Other formats
// (TIFF, HEIC, AVIF...) are read through WIC's metadata readers, and cleaned by re-encoding the
// pixels into a fresh file.
//
// Every write goes to `target`, an existing empty staging file (StagedOutput); the source is only
// read.
namespace sd::metadata
{
    struct Field
    {
        // Stable id for RemoveFields / SetField:
        //   "exif.<ifd>.<tag>"  an EXIF entry; <ifd> is 0th, exif, gps, interop or 1st; <tag> decimal
        //   "exif.1st.thumbnail", "exif.gps" (the whole GPS directory)
        //   "xmp", "iptc.<record>:<dataset>", "photoshop", "comment.<n>", "png.text.<keyword>",
        //   "png.time", "thumbnail", "extra.<n>", "trailer", "app.<n>", "colour.<n>"
        //   "wic:<query path>" for formats read through WIC (read only)
        std::string key;
        std::wstring group;   // "Location", "Camera", "Dates", "Author", "Description", "Software",
                              // "Image", "Thumbnail", "XMP", "IPTC", "Comments", "Hidden data", "Colour", "Other"
        std::wstring name;    // "Camera model"
        std::wstring value;   // "Pixel 9 Pro"
        bool personal = true; // false for colour and structural information a clean copy keeps
        bool editable = false; // SetField can change it
    };

    struct Inventory
    {
        std::vector<Field> fields;
        bool lossless = false;   // a container we edit without re-encoding

        size_t PersonalCount() const;
        // The groups that have fields, in display order.
        std::vector<std::wstring> Groups() const;
    };

    Inventory ReadMetadata(std::span<const uint8_t> bytes);
    Inventory ReadMetadata(const std::wstring& path);   // container parse, or WIC for other formats

    // What a scrub took out, for the result line.
    struct ScrubReport
    {
        size_t fields = 0;
        std::vector<std::wstring> kinds;   // "GPS", "camera", "dates"... most telling first
        bool lossless = false;             // pixels copied as they were
        bool reencoded = false;            // written as a new file (other formats)
        // "Removed GPS, camera, 14 fields" / "No metadata found".
        std::wstring Summary() const;
    };
    ScrubReport ReportFor(const Inventory& removed);

    // Writes `source` with no personal metadata to `target` (the staging file). `extension` gets the
    // output's extension (the source's own, or the re-encoded format's).
    HRESULT WriteWithoutMetadata(const std::wstring& source, const std::wstring& target, ScrubReport& report, std::wstring& extension,
                                 std::wstring& error);

    struct Verification
    {
        bool clean = false;
        bool opens = false;                   // the picture still decodes
        bool wicChecked = false;              // WIC could read the file's metadata tree
        std::vector<std::wstring> leftovers;  // what is still there, if anything
    };
    // Re-opens a file and checks that no EXIF, GPS, XMP, IPTC, maker notes, thumbnails, comments,
    // text or trailing data remain: by our own container parse, and by enumerating every metadata
    // path WIC's query readers expose.
    Verification VerifyNoMetadata(const std::wstring& path);

    // Every metadata query path WIC finds in the file (container and first frame), e.g.
    // "/app1/ifd/gps/{ushort=2}". Empty if WIC can't open it.
    std::vector<std::wstring> WicMetadataPaths(const std::wstring& path);
    // Whether a WIC query path is personal metadata (EXIF, GPS, XMP, IPTC, comments, text...).
    bool IsPersonalWicPath(const std::wstring& path);

    // Edit Metadata: removes the fields with these keys, or sets one EXIF field from text, writing
    // the result to `target`. JPEG, PNG and WebP (extended) only, losslessly.
    HRESULT RemoveFields(const std::wstring& source, const std::wstring& target, const std::vector<std::string>& keys, std::wstring& error);
    HRESULT SetField(const std::wstring& source, const std::wstring& target, const std::string& key, const std::wstring& value, std::wstring& error);
    // The same on bytes in memory.
    bool RemoveFields(std::span<const uint8_t> bytes, const std::vector<std::string>& keys, std::vector<uint8_t>& out, std::wstring& error);
    bool SetField(std::span<const uint8_t> bytes, const std::string& key, const std::wstring& value, std::vector<uint8_t>& out, std::wstring& error);

    // Crop and the other editors: the source's EXIF carried into an edited copy (`edited`, as
    // SaveRaster wrote it, JPEG or PNG). Dates, camera and location stay; the embedded thumbnail
    // goes (it would still show what was cropped away), the orientation becomes normal (the pixels
    // are already upright) and the stored pixel size is the new one. False when there was no EXIF
    // or it could not be placed; `edited` is then unchanged.
    bool CarryEditedExif(std::span<const uint8_t> source, std::vector<uint8_t>& edited, int width, int height);
}
