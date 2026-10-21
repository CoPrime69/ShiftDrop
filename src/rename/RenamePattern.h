#pragma once
#include <functional>
#include <string>
#include <string_view>
#include <vector>

// File actions › Rename…: new names from a pattern, checked before anything is renamed. Pure: the
// disk is only asked whether a name is taken (`exists`), so the tests can stand in for it.
//
// The pattern makes the name without its extension, which is kept:
//   {name}    the current name (after Find and replace)    {ext}     the extension, without the dot
//   {n}       1, 2, 3... in list order (from `start`)       {n:000}   the same, padded: 001, 002...
//   {date}    the file's modified date, 2026-10-21          {folder}  the name of the folder it is in
//   {{ }}     a literal brace
// "{name} {n:00}" turns IMG_4411.jpg, IMG_4412.jpg into "IMG_4411 01.jpg", "IMG_4412 02.jpg".
namespace sd::rename
{
    struct Pattern
    {
        std::wstring text = L"{name}";
        std::wstring find;           // replaced in {name}; empty: nothing is replaced
        std::wstring replace;
        bool matchCase = false;
        int start = 1;               // {n} of the first item
    };

    struct Item
    {
        std::wstring path;
        std::wstring date;           // its modified date as {date} shows it
        bool folder = false;         // a folder's whole name is {name}; it has no {ext}
    };

    struct Planned
    {
        std::wstring from;           // full path
        std::wstring to;             // full path of the new name, in the same folder
        std::wstring problem;        // why it can't be renamed; empty when it can
        bool Changes() const { return problem.empty() && to != from; }
    };

    struct Plan
    {
        std::vector<Planned> items;
        std::wstring error;          // the pattern itself is wrong ("{size} is not a field"); items are then empty
        size_t changes = 0;          // items that get a new name
        size_t problems = 0;         // items that can't be renamed
        // Renaming goes ahead only when every name works: nothing is renamed halfway.
        bool CanRun() const { return error.empty() && problems == 0 && changes > 0; }
    };

    // The new file name of `item` as the `n`-th item (0-based), or empty with `error` set.
    std::wstring Expand(const Pattern& pattern, const Item& item, size_t n, std::wstring& error);

    // Why `name` can't be a file name on Windows (empty, reserved, a character Windows refuses, too
    // long), or empty when it is fine.
    std::wstring NameProblem(std::wstring_view name);

    // Every item's new name, with the names that can't be used marked: an invalid name, two items
    // given the same name, or a name already taken by something else in the folder (never
    // overwritten). Changing only the case of a name is fine. `exists` defaults to the disk.
    using Exists = std::function<bool(const std::wstring& path)>;
    Plan MakePlan(const Pattern& pattern, const std::vector<Item>& items, const Exists& exists = {});
}
