#pragma once
#include <string>
#include <string_view>

// Scratch files and folders in %TEMP%, named "ShiftDrop-..." so a stray one is easy to recognise.
// Outputs never go here: they are written next to the source through StagedOutput.
namespace sd
{
    // %TEMP% with its trailing backslash, or empty.
    std::wstring TempDirectory();

    // A fresh, unused name in %TEMP%: "ShiftDrop-<tag>-1a2b3c4d5e6f<extension>". The file is not
    // created; the caller writes it and deletes it.
    std::wstring TempFilePath(std::wstring_view tag, std::wstring_view extension);

    // A folder of our own under %TEMP%, deleted with everything in it when this goes away.
    class TempFolder
    {
    public:
        TempFolder();
        ~TempFolder();
        TempFolder(const TempFolder&) = delete;
        TempFolder& operator=(const TempFolder&) = delete;
        bool Ok() const { return !m_path.empty(); }
        const std::wstring& Path() const { return m_path; }
        std::wstring File(const std::wstring& name) const { return m_path + L"\\" + name; }

    private:
        std::wstring m_path;
    };
}
