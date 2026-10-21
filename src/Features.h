#pragma once
#include "Recipe.h"
#include "Settings.h"

#include <array>
#include <set>
#include <string>

namespace sd
{
    // What the user turned off in Settings: whole sections of the wheel, and single slices (recipes,
    // by id). Everything is on by default. Kept in settings.ini, [Features]: Convert=0 and so on for a
    // section, Off=to-tiff,to-bmp for slices.
    struct Features
    {
        std::array<bool, kRingCount> sections{ true, true, true, true };
        std::set<std::string> off;   // recipe ids

        bool SectionOn(Ring ring) const { return sections[static_cast<size_t>(ring)]; }
        bool RecipeOn(const Recipe& recipe) const;
        int SectionsOn() const;
        bool operator==(const Features&) const = default;
    };
    Features LoadFeatures();
    void SaveFeatures(const Features& features);
    // The [Features] Off value and back: ids joined by commas.
    std::string JoinIds(const std::set<std::string>& ids);
    std::set<std::string> SplitIds(std::string_view text);
}
