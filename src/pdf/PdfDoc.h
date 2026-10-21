#pragma once
#include "Engine.h"
#include "pdf/PagePlan.h"

#include <qpdf/QPDF.hh>
#include <qpdf/QPDFPageObjectHelper.hh>

#include <memory>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

// QPDF (Apache-2.0) does everything structural: opening any PDF (repairing damaged ones), merging,
// reordering, rotating, duplicating and removing pages, and writing compact files. These helpers wrap
// it for wide Windows paths and turn its exceptions into readable errors.
namespace sd::pdf
{
    // Opens a PDF for reading and editing. Null with `error` set when it can't be read, including a
    // PDF locked with a password (owner-password-only files open fine).
    std::unique_ptr<QPDF> OpenPdf(const std::wstring& path, std::wstring& error);

    struct WriteOptions
    {
        bool compact = false;   // object streams + recompressed streams (Compress PDF)
    };
    // Writes `pdf` to `path` (truncating it: a staging file). Keeps encryption the source had.
    // `progress` gets 0..1 as QPDF writes; a stop request abandons the write ("cancelled").
    bool WritePdf(QPDF& pdf, const std::wstring& path, const WriteOptions& options, std::wstring& error, std::stop_token stop = {},
                  const ProgressFn& progress = {});

    // Pages in each file; false (with the file named in `error`) if one can't be read.
    bool CountPages(const std::vector<std::wstring>& files, std::vector<uint32_t>& counts, std::wstring& error);

    // Writes `plan`'s pages (from `sources`, by index) as a new PDF at `out`. The first source is the
    // base, so its outline, metadata and other document-level parts carry over; pages of the others
    // are copied in. A page used twice becomes a copy; rotations add to the page's own /Rotate.
    bool AssemblePages(const std::vector<std::wstring>& sources, const PagePlan& plan, const std::wstring& out, std::stop_token stop,
                       const ProgressFn& progress, std::wstring& error);

    // A page's /Rotate, inherited from the page tree too, as 0, 90, 180 or 270 (a missing or odd value
    // counts as 0).
    int RotationOf(QPDFPageObjectHelper& page);

    // A stream's /Filter as a list of names (one name, an array of them, or none).
    std::vector<std::string> StreamFilters(QPDFObjectHandle filter);
    // JPEG data: /DCTDecode (or its short form /DCT).
    bool IsDctFilter(std::string_view filter);
    // A filter that decodes to the exact samples (Flate, LZW, run length, ASCII85, ASCIIHex).
    bool IsLosslessFilter(std::string_view filter);

    // QPDF's exception text as a wide string.
    std::wstring QpdfError(const std::exception& e);
    // The text of the error WritePdf and friends report when a stop request ended them.
    inline constexpr const wchar_t* kCancelled = L"cancelled";
}
