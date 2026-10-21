#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// The entry model every archive path shares: a reader (libarchive, in the sandboxed worker or in
// process) produces entries; a sink (a folder, another archive) consumes them. Between the two sits
// GuardSink, which makes names safe and enforces the limits, always in the trusted parent.
namespace sd::archive
{
    enum class EntryType : uint8_t
    {
        File,
        Directory,
        Symlink,
        Hardlink,   // a second name for a file earlier in the archive (`link`)
        Other,      // devices, FIFOs, sockets: never extracted
    };

    struct Entry
    {
        std::wstring name;        // as stored ('/'-separated); after GuardSink, a safe relative path
        EntryType type = EntryType::File;
        int64_t size = -1;        // declared size; -1 = unknown until the data ends
        int64_t mtime = 0;        // FILETIME ticks (100 ns since 1601, UTC); 0 = not stored
        int64_t atime = 0;
        std::wstring link;        // symlink or hard link target
    };

    enum class Status : uint8_t
    {
        Ok,
        Cancelled,
        Password,      // encrypted entries or headers
        Corrupt,       // damaged or truncated archive, or not an archive at all
        Limit,         // too big, too many entries, or a zip bomb
        Unsafe,        // a name that would land outside the output folder
        Io,            // reading the source or writing the output failed
        Worker,        // the sandboxed worker failed or sent something malformed
    };

    struct Result
    {
        Status status = Status::Ok;
        std::wstring message;
        bool ok() const { return status == Status::Ok; }
    };
    inline Result Fail(Status status, std::wstring message) { return Result{ status, std::move(message) }; }

    class IEntrySink
    {
    public:
        virtual ~IEntrySink() = default;
        virtual Result Begin(const Entry& entry) = 0;
        virtual Result Data(const void* bytes, size_t size) = 0;   // file entries only
        virtual Result End() = 0;
    };

    // Generous, but finite: real archives never come near them, zip bombs hit them fast.
    struct Limits
    {
        uint64_t maxTotalBytes = 64ull << 30;   // everything uncompressed, 64 GiB
        uint64_t maxEntries = 1'000'000;
        // Once more than `ratioAfterBytes` came out, the output may not exceed `maxRatio` times the
        // archive's own size. Text compresses about 10:1; a file of zeros 1000:1.
        double maxRatio = 2000.0;
        uint64_t ratioAfterBytes = 1ull << 30;
    };

    // Turns an archive entry name into a safe relative path, one valid Windows name per part.
    // False (with `why`) for a name that must never be extracted: absolute paths, drive letters, UNC
    // paths and ".." anywhere. Characters Windows forbids become '_', reserved device names (CON,
    // NUL, COM1...) get a '_' prefix, and trailing dots and spaces are dropped (Windows would drop them
    // anyway, which turns "..." into ".."). A name of only "." parts gives no parts and true.
    bool SafeParts(std::wstring_view name, std::vector<std::wstring>& parts, std::wstring* why);
    std::wstring JoinParts(const std::vector<std::wstring>& parts, wchar_t separator);

    // Raw name bytes from an archive that has no charset flag: UTF-8 when they are valid UTF-8,
    // otherwise the OEM code page (CP437 in the US), which is what old Windows and DOS zip tools wrote.
    std::wstring DecodeName(std::string_view raw, unsigned codePage);

    // "\\?\C:\long\path" or "\\?\UNC\server\share\..." so extraction is not limited to MAX_PATH.
    // Only for paths built from SafeParts: the prefix turns off Windows' own path clean-up.
    std::wstring LongPath(const std::wstring& path);

    // FILETIME ticks <-> Unix seconds + nanoseconds.
    int64_t TicksFromUnix(int64_t seconds, long nanoseconds);
    void UnixFromTicks(int64_t ticks, int64_t& seconds, long& nanoseconds);
}
