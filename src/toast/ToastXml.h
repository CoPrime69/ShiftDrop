#pragma once
#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// When a finished batch gets a toast, what it says and what its buttons do, as the XML Windows'
// toast platform takes (Windows.UI.Notifications). Pure, so it is tested; Toasts.h shows it.
namespace sd::toast
{
    // The AppUserModelID the toasts are shown under, and the COM class Windows starts when one is
    // clicked. The installer puts both on the Start-menu shortcut (installer/ShiftDrop.iss): Windows
    // shows an unpackaged app's toasts only when a shortcut carries its ID.
    inline constexpr wchar_t kAppId[] = L"CoPrime69.ShiftDrop";
    inline constexpr wchar_t kActivatorClsid[] = L"{AF377B89-066C-4AD3-8387-8B77C419B8EE}";

    // What a click asks for: show the outputs in Explorer (the toast itself, "Open folder"), or open
    // the one output ("Open").
    enum class Verb { Reveal, Open };
    struct Activation
    {
        Verb verb = Verb::Reveal;
        std::vector<std::wstring> paths;   // full paths
    };

    // The arguments a click hands back: "reveal|C:\a.png|C:\b.png" ('|' is never in a path). The
    // whole toast must stay under 5 KB, so paths stop once `budget` characters are used (the first
    // is always kept); Explorer then selects the ones that fit.
    inline constexpr size_t kArgumentBudget = 1000;
    std::wstring Arguments(Verb verb, const std::vector<std::wstring>& paths, size_t budget = kArgumentBudget);
    // Arguments back into an Activation; nothing for anything else (an unknown verb, a relative path).
    std::optional<Activation> ParseArguments(std::wstring_view arguments);

    struct Content
    {
        std::wstring title;                 // "4 files converted to PNG"
        std::wstring text;                  // a line or two under it
        std::vector<std::wstring> outputs;  // what the buttons act on; none: a toast with no buttons
        bool failed = false;                // nothing was made: a Dismiss button instead of Open
    };
    // The line under a batch's title, as the brand kit words it: "photo (converted).webp and 2 more,
    // next to the originals." (`names` are the outputs' file names, `where` SavedWhere's phrase).
    std::wstring Summary(const std::vector<std::wstring>& names, std::wstring_view where);
    // A batch that runs longer than this may have lost the user's eye, so it gets a toast even when
    // its own top panel showed the result.
    inline constexpr std::chrono::seconds kLongJob{ 10 };
    // Does a finished batch get a toast (a balloon where toasts can't be shown)? When a later batch
    // owns the top panel, always, as before. When the panel showed this one, only if it took longer
    // than kLongJob and was not cancelled (the user was there to press the ×). One batch, one answer,
    // so a batch of many files is one toast.
    bool ShouldNotify(bool shownInPanel, std::chrono::milliseconds took, bool cancelled);

    // The toast: title and text, a click shows the outputs, "Open" (only for a single output) opens
    // it and "Open folder" shows them. A failed one has only Dismiss.
    std::wstring BuildXml(const Content& content);
    // Text for an XML attribute or element: & < > " ' escaped, control characters dropped.
    std::wstring EscapeXml(std::wstring_view text);
}
