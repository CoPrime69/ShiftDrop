#pragma once
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// What a finished batch's toast says and what its buttons do, as the XML Windows' toast platform
// takes (Windows.UI.Notifications). Pure, so it is tested; Toasts.h shows it.
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
    };
    // The toast: title and text, a click shows the outputs, "Open folder" does the same and "Open"
    // (only for a single output) opens it.
    std::wstring BuildXml(const Content& content);
    // Text for an XML attribute or element: & < > " ' escaped, control characters dropped.
    std::wstring EscapeXml(std::wstring_view text);
}
