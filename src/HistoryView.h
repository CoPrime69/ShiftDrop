#pragma once
#include "HistoryStore.h"

#include <chrono>
#include <string>
#include <string_view>
#include <vector>

// What the History window shows of the runs (HistoryWindow.cpp): the search, the Action and When
// filters, and how each row reads (Done, Undone, Failed...). Pure, so it is unit-tested.
namespace sd::history
{
    enum class Period
    {
        All,
        Today,
        Week,   // the last 7 days: today and the six days before it
    };

    struct Filter
    {
        std::wstring search;          // words matched in the action and the files' names, any case
        std::wstring action;          // one action's label; empty: every action
        Period period = Period::All;
    };

    // The first moment `period` covers, written as RunRecord::started writes times
    // ("2026-10-15 00:00:00"), when today is `today`; empty for Period::All.
    std::wstring PeriodStart(Period period, std::chrono::year_month_day today);

    // Does `run` pass `filter`? `since` is PeriodStart's answer for the filter's period.
    bool Matches(const RunRecord& run, const Filter& filter, std::wstring_view since);

    // The indices of the runs `filter` keeps, in their order.
    std::vector<size_t> Apply(const std::vector<RunRecord>& runs, const Filter& filter, std::chrono::year_month_day today);

    // Every action the runs used, once each, sorted by name: the Action filter's choices.
    std::vector<std::wstring> Actions(const std::vector<RunRecord>& runs);

    // How a row reads: its Result text and the colour of the whole row.
    enum class Tone
    {
        Normal,
        Muted,     // undone: its outputs went to the Recycle Bin (or the files went back)
        Problem,   // failed or interrupted
    };
    struct RowState
    {
        std::wstring text;
        Tone tone = Tone::Normal;
    };
    RowState StateOf(const RunRecord& run);

    // Undo is offered for a run that made something and has not been undone.
    bool CanUndo(const RunRecord& run);
}
