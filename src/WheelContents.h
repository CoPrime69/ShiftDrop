#pragma once
#include "Features.h"

#include <string>
#include <string_view>
#include <vector>

// Settings › Wheel: what each section of the wheel offers, by the kind of file its slices are for, and
// how turning a slice or a section on and off changes Features (the store in settings.ini). Pure, so
// the tests check the rules; the Settings window shows it as section tabs, a card per kind of file
// with a chip per slice, and a small preview of the wheel those chips make.
namespace sd
{
    // What Settings lists: every section, the kinds of file its slices are for, and the slices.
    struct SettingsTree
    {
        struct Slice
        {
            std::string id;          // recipe id
            std::wstring label;      // "TIFF"
            std::wstring detail;     // "TIFF, lossless"
            wchar_t icon = 0;        // the icon its wheel tile shows (0: its label alone, a format name)
        };
        struct Group
        {
            std::wstring title;      // "Photos", "Video", "Any file"
            std::vector<Slice> slices;
        };
        struct Section
        {
            Ring ring = Ring::Convert;
            std::wstring title;
            std::vector<Group> groups;
        };
        std::vector<Section> sections;

        const Section* Find(Ring ring) const;
    };

    namespace contents
    {
        // A chip is lit when its slice is on. A section that is off keeps its slices' choices (they come
        // back as they were when it is turned on again); its chips show them, dimmed.
        bool SliceOn(const Features& features, const std::string& id);
        Features WithSlice(Features features, const std::string& id, bool on);

        // The wheel needs at least one section, so the last one on can't be turned off.
        bool CanTurnOff(const Features& features, Ring ring);
        // Turning off the last section on is refused: `features` comes back unchanged.
        Features WithSection(Features features, Ring ring, bool on);
        // Reset to default: the section on, and every slice it lists on.
        Features Reset(Features features, const SettingsTree::Section& section);

        struct Count
        {
            int on = 0, total = 0;
        };
        Count CountOn(const Features& features, const SettingsTree::Group& group);
        Count CountOn(const Features& features, const SettingsTree::Section& section);
        // "9 of 9 actions on", "5 of 6 on".
        std::wstring CountText(Count count);
        // The line over the section's cards: "Convert is on. 6 kinds of file, 31 actions on the wheel."
        std::wstring Summary(const Features& features, const SettingsTree::Section& section);
        // The formats or actions a group holds, for its card while it is folded: "MP4 · WebM · MOV".
        std::wstring Listing(const SettingsTree::Group& group, size_t most = 6);

        // Find an action: every word of `query` is in the slice's label or detail (ignoring case). An
        // empty query matches everything.
        bool Matches(const SettingsTree::Slice& slice, std::wstring_view query);
        // The tiles the group's slices put on the wheel, in order: the ones that are on.
        std::vector<SettingsTree::Slice> PreviewSlices(const Features& features, const SettingsTree::Group& group);
    }
}
