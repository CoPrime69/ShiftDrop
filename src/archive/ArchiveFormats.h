#pragma once
#include <string>
#include <string_view>

namespace sd::archive
{
    // What an archive file is, from its name. Reading also checks the content: libarchive picks the
    // real format from the first bytes, so a ZIP renamed to .rar still opens.
    enum class Kind
    {
        None,
        Zip,
        Tar,
        TarGz,
        TarBz2,
        TarXz,
        SevenZip,
        Rar,       // RAR 4 and RAR 5, read only
        Gz,        // one compressed file (notes.txt.gz), no archive inside
        Bz2,
        Xz,
    };

    Kind KindOfPath(std::wstring_view path);
    inline bool IsArchive(std::wstring_view path) { return KindOfPath(path) != Kind::None; }
    // A single compressed file rather than an archive of entries.
    bool IsRaw(Kind kind);

    // The archive's name without its archive extension(s): "photos.tar.gz" -> "photos",
    // "notes.txt.gz" -> "notes.txt" (the file inside), "a.zip" -> "a".
    std::wstring ArchiveStem(std::wstring_view path);

    // Formats ShiftDrop writes.
    enum class WriteFormat
    {
        Zip,
        TarGz,
        SevenZip,
    };

    struct WriteFormatInfo
    {
        WriteFormat format;
        const char* id;            // recipe param value: "zip", "tar.gz", "7z"
        const wchar_t* label;      // "ZIP"
        const wchar_t* extension;  // ".zip"
    };
    const WriteFormatInfo* FindWriteFormat(std::string_view id);
    const WriteFormatInfo& Info(WriteFormat format);
    // Converting this archive to `format` would change nothing (ZIP to ZIP, .tgz to TAR.GZ).
    bool IsSameFormat(Kind kind, WriteFormat format);
}
