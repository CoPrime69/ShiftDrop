#include "archive/ArchiveFormats.h"
#include "PathUtil.h"
#include "Strings.h"

#include <array>

namespace sd::archive
{
    namespace
    {
        struct Suffix
        {
            const wchar_t* text;
            Kind kind;
        };
        // Longest first, so ".tar.gz" wins over ".gz".
        constexpr std::array kSuffixes{
            Suffix{ L".tar.gz", Kind::TarGz },  Suffix{ L".tar.bz2", Kind::TarBz2 }, Suffix{ L".tar.xz", Kind::TarXz },
            Suffix{ L".tgz", Kind::TarGz },     Suffix{ L".tbz2", Kind::TarBz2 },    Suffix{ L".tbz", Kind::TarBz2 },
            Suffix{ L".txz", Kind::TarXz },     Suffix{ L".zip", Kind::Zip },        Suffix{ L".tar", Kind::Tar },
            Suffix{ L".7z", Kind::SevenZip },   Suffix{ L".rar", Kind::Rar },        Suffix{ L".gz", Kind::Gz },
            Suffix{ L".bz2", Kind::Bz2 },       Suffix{ L".xz", Kind::Xz },
        };

        constexpr std::array kWriteFormats{
            WriteFormatInfo{ WriteFormat::Zip, "zip", L"ZIP", L".zip" },
            WriteFormatInfo{ WriteFormat::TarGz, "tar.gz", L"TAR.GZ", L".tar.gz" },
            WriteFormatInfo{ WriteFormat::SevenZip, "7z", L"7z", L".7z" },
        };

        // The matching suffix, or nullptr. The name must have something before it (".zip" alone is
        // a hidden file with no name, not an archive called "").
        const Suffix* Match(std::wstring_view path)
        {
            const auto name = Lower(FileName(path));
            for (const auto& s : kSuffixes)
            {
                const std::wstring_view suffix(s.text);
                if (name.size() > suffix.size() && name.ends_with(suffix))
                    return &s;
            }
            return nullptr;
        }
    }

    Kind KindOfPath(std::wstring_view path)
    {
        const auto* s = Match(path);
        return s ? s->kind : Kind::None;
    }

    bool IsRaw(Kind kind)
    {
        return kind == Kind::Gz || kind == Kind::Bz2 || kind == Kind::Xz;
    }

    std::wstring ArchiveStem(std::wstring_view path)
    {
        auto name = FileName(path);
        if (const auto* s = Match(path))
            name.resize(name.size() - std::wstring_view(s->text).size());
        return name;
    }

    const WriteFormatInfo* FindWriteFormat(std::string_view id)
    {
        for (const auto& f : kWriteFormats)
            if (id == f.id)
                return &f;
        return nullptr;
    }

    const WriteFormatInfo& Info(WriteFormat format)
    {
        for (const auto& f : kWriteFormats)
            if (f.format == format)
                return f;
        return kWriteFormats[0];
    }

    bool IsSameFormat(Kind kind, WriteFormat format)
    {
        switch (format)
        {
        case WriteFormat::Zip: return kind == Kind::Zip;
        case WriteFormat::TarGz: return kind == Kind::TarGz;
        case WriteFormat::SevenZip: return kind == Kind::SevenZip;
        }
        return false;
    }
}
