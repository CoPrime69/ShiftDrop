#include "HistoryView.h"
#include "Strings.h"

#include <algorithm>
#include <format>

namespace sd::history
{
    std::wstring PeriodStart(Period period, std::chrono::year_month_day today)
    {
        if (period == Period::All || !today.ok())
            return {};
        const std::chrono::year_month_day first =
            period == Period::Today ? today : std::chrono::year_month_day{ std::chrono::sys_days{ today } - std::chrono::days{ 6 } };
        return std::format(L"{:04}-{:02}-{:02} 00:00:00", static_cast<int>(first.year()), static_cast<unsigned>(first.month()),
                           static_cast<unsigned>(first.day()));
    }

    bool Matches(const RunRecord& run, const Filter& filter, std::wstring_view since)
    {
        if (!filter.action.empty() && run.recipeLabel != filter.action)
            return false;
        // "2026-10-21 14:31:07" sorts the way it reads, so the text compares as the time does.
        if (!since.empty() && std::wstring_view(run.started) < since)
            return false;
        const auto words = SearchWords(filter.search);
        if (words.empty())
            return true;
        // Every word somewhere in the action or a file's name: "png holiday" finds the holiday
        // photos converted to PNG.
        std::wstring text = run.recipeLabel;
        for (const auto& file : run.files)
            text += L"\n" + file;
        return HasEveryWord(text, words);
    }

    std::vector<size_t> Apply(const std::vector<RunRecord>& runs, const Filter& filter, std::chrono::year_month_day today)
    {
        const auto since = PeriodStart(filter.period, today);
        std::vector<size_t> kept;
        for (size_t i = 0; i < runs.size(); ++i)
            if (Matches(runs[i], filter, since))
                kept.push_back(i);
        return kept;
    }

    std::vector<std::wstring> Actions(const std::vector<RunRecord>& runs)
    {
        std::vector<std::wstring> actions;
        for (const auto& run : runs)
            if (!run.recipeLabel.empty())
                actions.push_back(run.recipeLabel);
        std::sort(actions.begin(), actions.end(), [](const std::wstring& a, const std::wstring& b) { return Lower(a) < Lower(b); });
        actions.erase(std::unique(actions.begin(), actions.end()), actions.end());
        return actions;
    }

    RowState StateOf(const RunRecord& run)
    {
        if (run.status == status::Done)
            return { L"Done", Tone::Normal };
        if (run.status == status::Partial)
            return { L"Partly done", Tone::Normal };   // the Files column says how many
        if (run.status == status::Failed)
            return { L"Failed", Tone::Problem };
        if (run.status == status::Undone)
            return { L"Undone", Tone::Muted };
        if (run.status == status::Interrupted)
            return { L"Interrupted", Tone::Problem };
        if (run.status == status::Running)
            return { L"Running", Tone::Normal };
        return {};
    }

    bool CanUndo(const RunRecord& run)
    {
        return (run.status == status::Done || run.status == status::Partial) && run.done > 0;
    }
}
