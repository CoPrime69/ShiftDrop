#pragma once
#include "Ffmpeg.h"
#include "inspect/Metadata.h"
#include "inspect/ProbeReport.h"

#include <propidl.h>

#include <stop_token>
#include <string>
#include <vector>

// Reads a file's metadata for the inspector, from three sources:
//   - the Windows property system (IPropertyStore, the same fields Explorer's Details pane shows),
//   - WIC's metadata query reader for images: every EXIF, GPS, XMP, IPTC and PNG text field,
//   - ffmpeg's probe for video and audio: container, tracks with their tags, chapters.
// Read-only. Call on a worker thread with COM initialised; it can take a moment (ffmpeg runs once).
namespace sd::inspect
{
    struct FileMetadata
    {
        std::wstring path;
        std::vector<Field> fields;   // ungrouped; GroupFields() orders them
        std::wstring error;          // set when the file could not be read at all
    };

    FileMetadata ReadMetadata(const std::wstring& path, const FfmpegEngine* ffmpeg, std::stop_token stop);

    // The parts, for tests.
    std::vector<Field> FileFields(const std::wstring& path, std::wstring* error);
    std::vector<Field> PropertyFields(const std::wstring& path);
    std::vector<Field> ImageFields(const std::wstring& path);   // size, format, then the raw metadata blocks
    // ffmpeg's probe of a media file (one run, no output written). `info` gets Media.h's view of it.
    ProbeReport Probe(const FfmpegEngine& ffmpeg, const std::wstring& path, std::stop_token stop, MediaInfo* info = nullptr);

    // A metadata value as text. WIC stores EXIF rationals in 64-bit integers: numerator in the low
    // 32 bits, denominator in the high 32 bits.
    std::wstring ValueText(const PROPVARIANT& value);
}
