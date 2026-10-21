#include "docs/Bytes.h"
#include "FileIo.h"
#include "docs/PdfSource.h"
#include "docs/Zip.h"

namespace sd::docs
{
    bool PagesPreviewPdf(const std::wstring& path, std::string& pdf, std::wstring& error)
    {
        std::string bytes;
        if (!ReadFileBytes(path, bytes, &error))
            return false;
        ZipReader zip;
        if (!zip.Open(std::move(bytes)))
        {
            // Pages '08 and earlier saved a folder package; a zip is all we read.
            error = L"this Pages file is not a single-file package";
            return false;
        }
        for (const char* name : { "QuickLook/Preview.pdf", "preview.pdf" })
            if (auto data = zip.Read(name); data && data->starts_with("%PDF"))
            {
                pdf = std::move(*data);
                return true;
            }
        error = L"this Pages file has no PDF preview inside (newer Pages saves only a picture); export it from Pages instead";
        return false;
    }
}
