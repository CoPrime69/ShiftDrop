#pragma once
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

// The page list that Organize PDF Pages, Merge PDFs and Edit PDF build: which page of which source
// file comes next, how much it turns, and whether an edited copy of it (PageEdits.h) stands in for
// it. The page views work on this list; the engine turns it into a PDF with QPDF (PdfDoc.h). Plain
// data, so every edit is unit-tested.
namespace sd::pdf
{
    struct PageRef
    {
        uint32_t file = 0;   // index into the source files
        uint32_t page = 0;   // 0-based page in that file
        int rotation = 0;    // extra clockwise turn: 0, 90, 180 or 270
        // An edited copy of the page (a one-page PDF, PageEdits.h) written in its place; empty for the
        // page as it is. The source file is never changed.
        std::wstring edited;

        bool operator==(const PageRef&) const = default;
    };
    using PagePlan = std::vector<PageRef>;

    // Every page of every file, in order (what Merge PDFs writes).
    PagePlan AllPages(const std::vector<uint32_t>& pageCounts);
    // Every page of the files in `order` (indices into pageCounts), file by file.
    PagePlan AllPages(const std::vector<uint32_t>& pageCounts, const std::vector<uint32_t>& order);

    // [[file, page, rotation], ...] for the recipe's params, and back; an edited page adds its file's
    // path as a fourth item. FromJson checks the shape;
    // CheckPlan checks it against the files' page counts.
    nlohmann::json PlanToJson(const PagePlan& plan);
    bool PlanFromJson(const nlohmann::json& json, PagePlan& plan, std::string& error);
    bool CheckPlan(const PagePlan& plan, const std::vector<uint32_t>& pageCounts, std::string& error);

    // Moves the selected items (indices, any order) so they sit together, in their current order,
    // before the item that was at `insertBefore` (size() = the end). Returns their new indices.
    template <class T>
    std::vector<size_t> MoveItems(std::vector<T>& items, std::vector<size_t> selection, size_t insertBefore)
    {
        std::sort(selection.begin(), selection.end());
        selection.erase(std::unique(selection.begin(), selection.end()), selection.end());
        std::erase_if(selection, [&](size_t i) { return i >= items.size(); });
        insertBefore = std::min(insertBefore, items.size());
        std::vector<T> moved, rest;
        size_t target = 0;   // insertion point counted in `rest`
        for (size_t i = 0, s = 0; i < items.size(); ++i)
        {
            if (i == insertBefore)
                target = rest.size();
            if (s < selection.size() && selection[s] == i)
            {
                moved.push_back(items[i]);
                ++s;
            }
            else
                rest.push_back(items[i]);
        }
        if (insertBefore == items.size())
            target = rest.size();
        std::vector<size_t> now;
        rest.insert(rest.begin() + static_cast<std::ptrdiff_t>(target), moved.begin(), moved.end());
        for (size_t k = 0; k < moved.size(); ++k)
            now.push_back(target + k);
        items = std::move(rest);
        return now;
    }

    // ---- Merge's file list and its page view -------------------------------------------------------
    // Both work on one plan whose `file` indices are the dropped files' (they never change); `order`
    // is the list's order of those indices, without the files left out.

    // True when each file's pages sit together (in whatever order the files come).
    bool FilesStayTogether(const PagePlan& plan);
    // The plan after the list went from `before` to `after`: pages of files no longer listed go.
    // When the files' order changed, each file's pages follow their file, keeping their own order,
    // turns and edits; pages that had been moved in among another file's go back to their own file,
    // and `regrouped` (if given) says so.
    PagePlan PlanForFileOrder(const PagePlan& plan, const std::vector<uint32_t>& before, const std::vector<uint32_t>& after,
                              bool* regrouped = nullptr);
    // The plan with each `file` counted in `order` instead (what the engine gets, with the files
    // passed in list order). Pages of files not in `order` are dropped.
    PagePlan InListOrder(const PagePlan& plan, const std::vector<uint32_t>& order);
    // How many of `file`'s pages the plan keeps.
    size_t PagesKept(const PagePlan& plan, uint32_t file);
    // How many pages have an edited copy standing in for them.
    size_t EditedPages(const PagePlan& plan);

    // Turns the selected pages clockwise by `degrees` (a multiple of 90, may be negative).
    void RotatePages(PagePlan& plan, const std::vector<size_t>& selection, int degrees);
    // Gives the selected pages back their unedited original (the turn stays).
    void RevertPages(PagePlan& plan, const std::vector<size_t>& selection);
    // Puts a copy of each selected page right after it. Returns the copies' indices.
    std::vector<size_t> DuplicatePages(PagePlan& plan, const std::vector<size_t>& selection);
    // Removes the selected items. Returns the index that should get the focus afterwards (the item
    // after the first removed one, or the new last one), or -1 when nothing is left.
    template <class T>
    long long RemoveItems(std::vector<T>& items, std::vector<size_t> selection)
    {
        std::sort(selection.begin(), selection.end());
        selection.erase(std::unique(selection.begin(), selection.end()), selection.end());
        std::erase_if(selection, [&](size_t i) { return i >= items.size(); });
        if (selection.empty())
            return items.empty() ? -1 : 0;
        const size_t first = selection.front();
        for (auto it = selection.rbegin(); it != selection.rend(); ++it)
            items.erase(items.begin() + static_cast<std::ptrdiff_t>(*it));
        if (items.empty())
            return -1;
        return static_cast<long long>(std::min(first, items.size() - 1));
    }
}
