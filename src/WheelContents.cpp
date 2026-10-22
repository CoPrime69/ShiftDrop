#include "WheelContents.h"
#include "Strings.h"

#include <format>

namespace sd
{
    const SettingsTree::Section* SettingsTree::Find(Ring ring) const
    {
        for (const auto& section : sections)
            if (section.ring == ring)
                return &section;
        return nullptr;
    }

    namespace contents
    {
        bool SliceOn(const Features& features, const std::string& id)
        {
            return !features.off.contains(id);
        }

        Features WithSlice(Features features, const std::string& id, bool on)
        {
            if (on)
                features.off.erase(id);
            else
                features.off.insert(id);
            return features;
        }

        bool CanTurnOff(const Features& features, Ring ring)
        {
            return !features.SectionOn(ring) || features.SectionsOn() > 1;
        }

        Features WithSection(Features features, Ring ring, bool on)
        {
            if (!on && !CanTurnOff(features, ring))
                return features;
            features.sections[static_cast<size_t>(ring)] = on;
            return features;
        }

        Features Reset(Features features, const SettingsTree::Section& section)
        {
            features.sections[static_cast<size_t>(section.ring)] = true;
            for (const auto& group : section.groups)
                for (const auto& slice : group.slices)
                    features.off.erase(slice.id);
            return features;
        }

        Count CountOn(const Features& features, const SettingsTree::Group& group)
        {
            Count c;
            for (const auto& slice : group.slices)
            {
                ++c.total;
                c.on += SliceOn(features, slice.id) ? 1 : 0;
            }
            return c;
        }

        Count CountOn(const Features& features, const SettingsTree::Section& section)
        {
            Count c;
            for (const auto& group : section.groups)
            {
                const auto g = CountOn(features, group);
                c.on += g.on;
                c.total += g.total;
            }
            return c;
        }

        std::wstring CountText(Count count)
        {
            return std::format(L"{} of {} on", count.on, count.total);
        }

        std::wstring Summary(const Features& features, const SettingsTree::Section& section)
        {
            size_t kinds = 0;
            for (const auto& group : section.groups)
                kinds += group.slices.empty() ? 0 : 1;
            const auto count = CountOn(features, section);
            const auto plural = [](size_t n, const wchar_t* one, const wchar_t* many) { return std::format(L"{} {}", n, n == 1 ? one : many); };
            if (!features.SectionOn(section.ring))
                return std::format(L"{} is off: it isn\u2019t on the wheel.", section.title);
            return std::format(L"{} is on. {}, {} on the wheel.", section.title, plural(kinds, L"kind of file", L"kinds of file"),
                               plural(static_cast<size_t>(count.on), L"action", L"actions"));
        }

        std::wstring Listing(const SettingsTree::Group& group, size_t most)
        {
            std::wstring text;
            for (size_t i = 0; i < group.slices.size() && i < most; ++i)
                text += (i ? L" \u00B7 " : L"") + group.slices[i].label;
            if (group.slices.size() > most)
                text += L" \u2026";
            return text;
        }

        bool Matches(const SettingsTree::Slice& slice, std::wstring_view query)
        {
            const auto words = SearchWords(query);
            return HasEveryWord(slice.label + L" " + slice.detail, words);
        }

        std::vector<SettingsTree::Slice> PreviewSlices(const Features& features, const SettingsTree::Group& group)
        {
            std::vector<SettingsTree::Slice> on;
            for (const auto& slice : group.slices)
                if (SliceOn(features, slice.id))
                    on.push_back(slice);
            return on;
        }

        std::vector<ShownGroup> Shown(const SettingsTree& tree, Ring ring, std::wstring_view query)
        {
            std::vector<ShownGroup> shown;
            const auto* section = tree.Find(ring);
            if (!section)
                return shown;
            const bool searching = !SearchWords(query).empty();
            for (const auto& group : section->groups)
            {
                ShownGroup g{ &group, {}, searching };
                for (size_t i = 0; i < group.slices.size(); ++i)
                    if (Matches(group.slices[i], query))
                        g.chips.push_back(i);
                if (!group.slices.empty() && !(searching && g.chips.empty()))
                    shown.push_back(std::move(g));
            }
            return shown;
        }
    }
}
