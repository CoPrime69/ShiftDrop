#pragma once
#include <cstdint>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

// The small PDF writer behind Images -> PDF and searchable PDFs made from images. It writes objects
// one by one straight to the file (so a 200-photo PDF never sits in memory), then the cross-reference
// table and trailer. Nothing here parses PDF; QPDF does that elsewhere.
namespace sd::pdf
{
    class PdfWriter
    {
    public:
        // Creates the file (it must not exist: it is a fresh staging file or a test path) and writes
        // the header. Check Ok() afterwards.
        explicit PdfWriter(const std::wstring& path);
        bool Ok() const { return m_ok; }

        // Reserves an object number; objects may be written in any order.
        int NewObject();
        // "<< ... >>", "[ ... ]" or any other object body.
        void Object(int id, std::string_view body);
        // A stream: `dict` is the inside of the dictionary without << >> and without /Length.
        void Stream(int id, std::string_view dict, std::string_view data);
        // Deflates `data` (zlib) and writes it with /Filter /FlateDecode.
        void FlateStream(int id, std::string_view dict, std::string_view data);

        // Cross-reference table and trailer. Returns false if anything failed to write.
        bool Finish(int root, int info);

    private:
        void Put(std::string_view s);

        std::ofstream m_out;
        uint64_t m_pos = 0;
        std::vector<uint64_t> m_offsets;   // index = object number - 1; 0 = reserved but not written
        bool m_ok = false;
    };

    // Numbers as PDF wants them: no exponent, at most 4 decimals, no trailing zeros ("12.5", "0", "-3").
    std::string Num(double value);
    // A literal string "(...)" with ( ) \ escaped; bytes outside printable ASCII become octal escapes.
    std::string LiteralString(std::string_view bytes);
    // zlib deflate at the default level; empty on failure.
    std::string Deflate(std::string_view data);
}
