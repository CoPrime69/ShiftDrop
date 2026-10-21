#pragma once
#include <windows.h>

#include <functional>
#include <stop_token>
#include <string>
#include <string_view>

// Office bridge: when Microsoft Office or LibreOffice is installed, it makes the faithful
// conversions our own code can't: presentations to PDF and to slide pictures, and Word and Excel
// files to a PDF exactly as the app prints them. Nothing is bundled; the apps are found once at
// startup. Microsoft Office is driven through COM (out of process, invisible, macros off, alerts
// off, with a timeout, always closed again); LibreOffice runs headless as a windowless child in a
// kill-on-close job, with a profile of our own so it never touches the user's running instance.
namespace sd::office
{
    enum class Family
    {
        None,
        Word,         // .doc .docx .docm .dotx .dotm .odt .rtf
        Excel,        // .xls .xlsx .xlsm .xlsb .ods
        PowerPoint,   // .ppt .pptx .pptm .pps .ppsx .odp
    };
    Family FamilyOf(std::wstring_view path);

    // What is installed.
    struct Apps
    {
        bool word = false;
        bool excel = false;
        bool powerpoint = false;
        std::wstring soffice;   // LibreOffice's soffice.exe, or empty
    };
    // Registry (Word.Application etc. with a local server; LibreOffice's UNO InstallPath) and
    // Program Files. Cheap: a few registry reads and file checks.
    Apps DetectApps();

    enum class Backend
    {
        None,
        Word,
        Excel,
        PowerPoint,
        LibreOffice,
    };
    // Who converts a file of `family` to `target` ("pdf", "png", "jpg"): the Microsoft app when
    // installed, else LibreOffice for PDFs. Slide pictures need PowerPoint (LibreOffice exports
    // only the first slide as a picture).
    Backend Choose(const Apps& apps, Family family, std::string_view target);
    const wchar_t* BackendName(Backend backend);

    // "file:///C:/Users/Ann%20Lee/x" for LibreOffice's -env:UserInstallation; UNC paths become
    // "file://server/share/...".
    std::wstring FileUrl(std::wstring_view path);
    // The whole soffice command line for converting `source` to PDF in `outDir`.
    std::wstring LibreOfficeCommandLine(const std::wstring& soffice, const std::wstring& profileUrl, const std::wstring& outDir,
                                        const std::wstring& source);
    // "Slide 01.png" ... with the number padded to the width of `count` (at least two digits).
    std::wstring SlideFileName(int index, int count, std::wstring_view extension);
    // Picture size for a slide `slideWidth` x `slideHeight` points: 1920 pixels on the long side.
    SIZE SlidePixels(float slideWidth, float slideHeight);

    struct Job
    {
        std::wstring source;
        std::string target;          // "pdf", "png" or "jpg"
        std::wstring output;         // pdf: our staging file, to be filled; png/jpg: an empty staging folder
        std::stop_token stop;
        DWORD timeoutMs = 5 * 60 * 1000;   // without progress; each slide exported restarts it
        std::function<void(double)> progress;
    };
    struct Outcome
    {
        bool ok = false;
        bool cancelled = false;
        std::wstring error;
    };
    // Microsoft Office through COM. Call on an STA thread (JobLane::Office).
    Outcome ConvertWithOffice(Backend app, const Job& job);
    // LibreOffice: converts to PDF in a temp folder and copies the result into job.output.
    Outcome ConvertWithLibreOffice(const std::wstring& soffice, const Job& job);
}
