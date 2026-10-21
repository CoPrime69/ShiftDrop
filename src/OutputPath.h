#pragma once
#include <windows.h>
#include <objidl.h>
#include <wrl/client.h>

#include <string>
#include <string_view>

namespace sd
{
    // Lower-case extension of a path, including the dot (".jpg"), or empty.
    std::wstring LowerExtension(std::wstring_view path);

    // How outputs are named: Settings > Output names, [Output] Naming in settings.ini. Every style
    // keeps the never-overwrite rule (CommitStaging): a taken name gets the next number.
    enum class OutputNaming
    {
        Parentheses,   // "photo (converted).png", "photo (converted 2).png" (the default)
        Dash,          // "photo-converted.png", "photo-converted-2.png"
        Underscore,    // "photo_converted.png", "photo_converted_2.png"
        Subfolder,     // the same name in a "ShiftDrop" folder next to the source: "photo.png", "photo (2).png"
    };
    inline constexpr wchar_t kOutputSubfolder[] = L"ShiftDrop";
    // Thread-safe: workers name outputs while the Settings window changes it.
    void SetOutputNaming(OutputNaming naming);
    OutputNaming CurrentOutputNaming();

    // The n-th candidate name (n = 1 is the first choice) in the current style, or in `naming`.
    // A suffix with spaces ("no metadata") takes the style's separator in the dash and underscore styles.
    // An empty suffix gives the stem alone ("Archive.zip"), then "Archive (2).zip" ("Archive-2.zip"...).
    std::wstring OutputName(std::wstring_view stem, std::wstring_view suffix, int n, std::wstring_view extension);
    std::wstring OutputName(OutputNaming naming, std::wstring_view stem, std::wstring_view suffix, int n, std::wstring_view extension);

    // Output is written in two steps so that a crash or kill never leaves a half-written file under
    // the final name (ADR 0003):
    //  1. CreateStaging: a new file "~photo.1a2b3c4d.sdpart" next to the source, opened for writing.
    //  2. CommitStaging: rename it to the first free "photo (suffix[ n]).ext". MoveFileEx without
    //     MOVEFILE_REPLACE_EXISTING fails if the name exists, so we never overwrite, even in a race,
    //     and a rename within one folder is atomic.
    // Both use OutputFolder(source) (below), which is the source's own folder unless redirected; in the
    // Subfolder style CreateStaging makes its "ShiftDrop" folder first.
    HRESULT CreateStaging(const std::wstring& source, std::wstring& stagingPath, Microsoft::WRL::ComPtr<IStream>& stream);
    HRESULT CommitStaging(const std::wstring& stagingPath, const std::wstring& source, std::wstring_view suffix,
                          std::wstring_view extension, std::wstring& finalPath);
    // The parts of those two steps the other writers share (StagedOutput, VirtualDrop): a new staging
    // name "~photo.1a2b3c4d.sdpart" for `stem`, and the no-replace rename of a staging file to the
    // first free OutputName in `naming` in its own folder.
    std::wstring StagingName(std::wstring_view stem);
    HRESULT RenameToFreeName(const std::wstring& stagingPath, OutputNaming naming, std::wstring_view stem, std::wstring_view suffix,
                             std::wstring_view extension, std::wstring& finalPath);

    // Files dragged out of a browser or a mail app have no folder of their own: VirtualDrop.h saves
    // them in a private drop folder first, and their outputs go to the folder the user chose (the
    // Desktop by default). This is the one place that redirect lives: every output path (CreateStaging,
    // CommitStaging, StagedOutput, the predicted name) comes from OutputFolder.
    //
    // Outputs of anything inside `folder` (at any depth) go to `outputFolder`; an empty outputFolder
    // removes the redirect. Thread-safe: workers read it while the UI thread changes it.
    void RedirectOutputs(const std::wstring& folder, const std::wstring& outputFolder);
    // The folder outputs of `source` are written to: its own, or where a redirect sends them, plus
    // "ShiftDrop" in the Subfolder style (not added again to a source already in such a folder).
    std::wstring OutputFolder(const std::wstring& source);
    std::wstring OutputFolder(OutputNaming naming, const std::wstring& source);
    // Creates that "ShiftDrop" folder when it is not there yet (CreateStaging and StagedOutput do).
    HRESULT MakeOutputFolder(const std::wstring& folder);
}
