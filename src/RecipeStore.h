#pragma once
#include "Recipe.h"

#include <string>
#include <vector>

namespace sd
{
    // Built-in recipes plus the user's own JSON files from %LOCALAPPDATA%\ShiftDrop\recipes\*.json.
    // A user file with the same id as a built-in replaces it. Wheel order: built-ins first, then
    // user recipes sorted by file name.
    class RecipeStore
    {
    public:
        // Seeds the folder with an example ("web-ready.json") the first time it does not exist.
        void Load(const ActionRegistry& registry, const std::wstring& folder);

        const std::vector<Recipe>& All() const { return m_recipes; }
        const Recipe* Find(std::string_view id) const;
        const std::vector<std::wstring>& Errors() const { return m_errors; }   // one line per broken file

        static const char* BuiltInJson();   // exposed for tests
        static const char* ExampleJson();

    private:
        std::vector<Recipe> m_recipes;
        std::vector<std::wstring> m_errors;
    };
}
