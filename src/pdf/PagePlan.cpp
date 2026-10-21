#include "pdf/PagePlan.h"
#include "Utf.h"

#include <format>
#include <map>
#include <set>

namespace sd::pdf
{
    PagePlan AllPages(const std::vector<uint32_t>& pageCounts)
    {
        PagePlan plan;
        for (uint32_t f = 0; f < pageCounts.size(); ++f)
            for (uint32_t p = 0; p < pageCounts[f]; ++p)
                plan.push_back(PageRef{ f, p, 0 });
        return plan;
    }

    PagePlan AllPages(const std::vector<uint32_t>& pageCounts, const std::vector<uint32_t>& order)
    {
        PagePlan plan;
        for (const uint32_t f : order)
            if (f < pageCounts.size())
                for (uint32_t p = 0; p < pageCounts[f]; ++p)
                    plan.push_back(PageRef{ f, p, 0 });
        return plan;
    }

    nlohmann::json PlanToJson(const PagePlan& plan)
    {
        auto out = nlohmann::json::array();
        for (const auto& r : plan)
        {
            auto item = nlohmann::json::array({ r.file, r.page, r.rotation });
            if (!r.edited.empty())
                item.push_back(Narrow(r.edited));
            out.push_back(std::move(item));
        }
        return out;
    }

    bool PlanFromJson(const nlohmann::json& json, PagePlan& plan, std::string& error)
    {
        plan.clear();
        if (!json.is_array())
        {
            error = "the page plan is not a list";
            return false;
        }
        for (size_t i = 0; i < json.size(); ++i)
        {
            const auto& item = json[i];
            if (!item.is_array() || item.size() < 3 || item.size() > 4 || !item[0].is_number_unsigned() || !item[1].is_number_unsigned() ||
                !item[2].is_number_integer() || (item.size() == 4 && (!item[3].is_string() || item[3].get<std::string>().empty())))
            {
                error = std::format("page plan item {} is not [file, page, rotation] or [file, page, rotation, edited page]", i);
                return false;
            }
            const int rotation = item[2].get<int>();
            if (rotation % 90 != 0 || rotation < 0 || rotation >= 360)
            {
                error = std::format("page plan item {}: rotation {} is not 0, 90, 180 or 270", i, rotation);
                return false;
            }
            plan.push_back(PageRef{ item[0].get<uint32_t>(), item[1].get<uint32_t>(), rotation,
                                    item.size() == 4 ? Widen(item[3].get<std::string>()) : std::wstring() });
        }
        return true;
    }

    bool CheckPlan(const PagePlan& plan, const std::vector<uint32_t>& pageCounts, std::string& error)
    {
        if (plan.empty())
        {
            error = "no pages left to save";
            return false;
        }
        for (const auto& r : plan)
        {
            if (r.file >= pageCounts.size() || r.page >= pageCounts[r.file])
            {
                error = std::format("page {} of file {} does not exist", r.page + 1, r.file + 1);
                return false;
            }
        }
        return true;
    }

    void RotatePages(PagePlan& plan, const std::vector<size_t>& selection, int degrees)
    {
        for (const size_t i : selection)
            if (i < plan.size())
                plan[i].rotation = (((plan[i].rotation + degrees) % 360) + 360) % 360;
    }

    void RevertPages(PagePlan& plan, const std::vector<size_t>& selection)
    {
        for (const size_t i : selection)
            if (i < plan.size())
                plan[i].edited.clear();
    }

    bool FilesStayTogether(const PagePlan& plan)
    {
        std::set<uint32_t> finished;   // files whose run of pages has ended
        for (size_t i = 0; i < plan.size(); ++i)
        {
            if (finished.contains(plan[i].file))
                return false;
            if (i + 1 < plan.size() && plan[i + 1].file != plan[i].file)
                finished.insert(plan[i].file);
        }
        return true;
    }

    PagePlan PlanForFileOrder(const PagePlan& plan, const std::vector<uint32_t>& before, const std::vector<uint32_t>& after,
                              bool* regrouped)
    {
        if (regrouped)
            *regrouped = false;
        std::map<uint32_t, size_t> rank;
        for (size_t i = 0; i < after.size(); ++i)
            rank.emplace(after[i], i);
        PagePlan kept;
        for (const auto& r : plan)
            if (rank.contains(r.file))
                kept.push_back(r);
        // Only a change in the order of the files that stay moves pages; leaving a file out does not.
        std::vector<uint32_t> stayed;
        for (const uint32_t f : before)
            if (rank.contains(f))
                stayed.push_back(f);
        if (stayed == after)
            return kept;
        const bool together = FilesStayTogether(kept);
        PagePlan sorted = kept;
        std::stable_sort(sorted.begin(), sorted.end(), [&](const PageRef& a, const PageRef& b) { return rank[a.file] < rank[b.file]; });
        if (regrouped)
            *regrouped = !together && sorted != kept;
        return sorted;
    }

    PagePlan InListOrder(const PagePlan& plan, const std::vector<uint32_t>& order)
    {
        std::map<uint32_t, uint32_t> position;
        for (uint32_t i = 0; i < order.size(); ++i)
            position.emplace(order[i], i);
        PagePlan out;
        for (const auto& r : plan)
            if (const auto it = position.find(r.file); it != position.end())
            {
                out.push_back(r);
                out.back().file = it->second;
            }
        return out;
    }

    size_t PagesKept(const PagePlan& plan, uint32_t file)
    {
        return static_cast<size_t>(std::count_if(plan.begin(), plan.end(), [&](const PageRef& r) { return r.file == file; }));
    }

    size_t EditedPages(const PagePlan& plan)
    {
        return static_cast<size_t>(std::count_if(plan.begin(), plan.end(), [](const PageRef& r) { return !r.edited.empty(); }));
    }

    std::vector<size_t> DuplicatePages(PagePlan& plan, const std::vector<size_t>& selection)
    {
        std::vector<size_t> sorted = selection;
        std::sort(sorted.begin(), sorted.end());
        sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());
        std::erase_if(sorted, [&](size_t i) { return i >= plan.size(); });
        std::vector<size_t> copies;
        // Back to front, so earlier indices stay valid; then shift each copy's index by the copies
        // inserted before it.
        for (auto it = sorted.rbegin(); it != sorted.rend(); ++it)
        {
            const PageRef copy = plan[*it];
            plan.insert(plan.begin() + static_cast<std::ptrdiff_t>(*it + 1), copy);
        }
        for (size_t k = 0; k < sorted.size(); ++k)
            copies.push_back(sorted[k] + k + 1);
        return copies;
    }
}
