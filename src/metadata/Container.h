#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

// The block structure of the image containers whose metadata we can remove without touching the
// pixels: JPEG segments, PNG chunks, WebP (RIFF) chunks and GIF blocks. Everything here works on
// bytes in memory, so it is exact and testable without WIC.
namespace sd::metadata
{
    enum class Container { Unknown, Jpeg, Png, WebP, Gif, Tiff };
    Container DetectContainer(std::span<const uint8_t> bytes);

    enum class BlockKind
    {
        Image,          // pixel data and what decoding needs (kept)
        Colour,         // colour profile and colour/transform info: ICC, Adobe APP14, gAMA, sRGB... (kept)
        Jfif,           // JFIF header (kept, without its thumbnail)
        Exif,           // EXIF/TIFF block: camera, dates, GPS, thumbnail
        Xmp,
        Iptc,           // Photoshop IRB with IPTC
        Comment,
        Thumbnail,      // JFIF extension thumbnail, or a JFIF header's embedded one
        ExtraImage,     // MPF index / FlashPix: extra pictures (depth maps, previews) and their index
        Text,           // PNG tEXt / zTXt / iTXt
        Time,           // PNG tIME
        Trailer,        // bytes after the end of the image (motion-photo video, maker trailers)
        Other,          // unknown application data
    };

    // True for kinds a share-safe copy must not carry.
    bool IsPersonal(BlockKind kind);

    struct Block
    {
        BlockKind kind = BlockKind::Image;
        size_t offset = 0;          // of the whole block in the file
        size_t size = 0;
        size_t payload = 0;         // offset of its content (after markers, lengths and signatures)
        size_t payloadSize = 0;
        std::string name;           // "APP1 Exif", "tEXt Author", "EXIF chunk"...
    };

    // The blocks of a JPEG, PNG, WebP or GIF file in order. False if the structure is broken.
    bool ListBlocks(std::span<const uint8_t> bytes, Container container, std::vector<Block>& blocks);

    // A copy of the file with every personal block removed (IsPersonal) and the rest byte for byte:
    // the compressed pixels are untouched, so nothing is lost. A JFIF header keeps its density but
    // loses its thumbnail; WebP's extended header flags and RIFF size are fixed up. `removed` gets
    // the blocks taken out. False (with `error`) if the file can't be parsed.
    bool StripPersonal(std::span<const uint8_t> bytes, std::vector<uint8_t>& out, std::vector<Block>* removed, std::wstring* error);

    // Container-level edits used by Edit Metadata (see Metadata.h): replace (or insert) the EXIF
    // block with `tiff` (a TIFF structure; empty removes it), keeping everything else.
    bool ReplaceExif(std::span<const uint8_t> bytes, std::span<const uint8_t> tiff, std::vector<uint8_t>& out, std::wstring* error);
    // Removes the blocks for which `drop` returns true, keeping everything else byte for byte.
    bool RemoveBlocks(std::span<const uint8_t> bytes, const std::vector<Block>& drop, std::vector<uint8_t>& out, std::wstring* error);
}
