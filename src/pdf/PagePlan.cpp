#include "pdf/PagePlan.h"

#include <format>

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

    nlohmann::json PlanToJson(const PagePlan& plan)
    {
        auto out = nlohmann::json::array();
        for (const auto& r : plan)
            out.push_back({ r.file, r.page, r.rotation });
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
            if (!item.is_array() || item.size() != 3 || !item[0].is_number_unsigned() || !item[1].is_number_unsigned() ||
                !item[2].is_number_integer())
            {
                error = std::format("page plan item {} is not [file, page, rotation]", i);
                return false;
            }
            const int rotation = item[2].get<int>();
            if (rotation % 90 != 0 || rotation < 0 || rotation >= 360)
            {
                error = std::format("page plan item {}: rotation {} is not 0, 90, 180 or 270", i, rotation);
                return false;
            }
            plan.push_back(PageRef{ item[0].get<uint32_t>(), item[1].get<uint32_t>(), rotation });
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
