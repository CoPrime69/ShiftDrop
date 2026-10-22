#include "HistoryView.h"
#include "Strings.h"

#include <windows.h>

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
    std::wstring FilesText(const RunRecord& run)
    {
        if (run.files.empty())
            return run.recipeLabel;
        if (run.files.size() == 1)
            return run.files.front();
        return std::format(L"{} and {} more", run.files.front(), run.files.size() - 1);
    }

    bool ParseStarted(std::wstring_view started, LocalTime& out)
    {
        // "YYYY-MM-DD HH:MM", seconds optional.
        const auto number = [&](size_t at, size_t length, int& value) {
            if (at + length > started.size())
                return false;
            value = 0;
            for (size_t i = at; i < at + length; ++i)
            {
                if (started[i] < L'0' || started[i] > L'9')
                    return false;
                value = value * 10 + (started[i] - L'0');
            }
            return true;
        };
        int y = 0, mo = 0, d = 0, h = 0, mi = 0;
        if (started.size() < 16 || started[4] != L'-' || started[7] != L'-' || started[10] != L' ' || started[13] != L':' ||
            !number(0, 4, y) || !number(5, 2, mo) || !number(8, 2, d) || !number(11, 2, h) || !number(14, 2, mi))
            return false;
        const std::chrono::year_month_day day{ std::chrono::year{ y }, std::chrono::month{ static_cast<unsigned>(mo) },
                                               std::chrono::day{ static_cast<unsigned>(d) } };
        if (!day.ok() || h > 23 || mi > 59)
            return false;
        out = { day, h, mi };
        return true;
    }

    namespace
    {
        constexpr const wchar_t* kMonths[] = { L"Jan", L"Feb", L"Mar", L"Apr", L"May", L"Jun", L"Jul", L"Aug", L"Sep", L"Oct", L"Nov", L"Dec" };
        constexpr const wchar_t* kMonthNames[] = { L"January", L"February", L"March",     L"April",   L"May",      L"June",
                                                   L"July",    L"August",   L"September", L"October", L"November", L"December" };
        constexpr const wchar_t* kWeekdays[] = { L"Sunday", L"Monday", L"Tuesday", L"Wednesday", L"Thursday", L"Friday", L"Saturday" };

        int DaysBetween(std::chrono::year_month_day from, std::chrono::year_month_day to)
        {
            return static_cast<int>((std::chrono::sys_days{ to } - std::chrono::sys_days{ from }).count());
        }
    }

    LocalTime LocalNow()
    {
        SYSTEMTIME t{};
        GetLocalTime(&t);   // RunRecord::started is local time too
        return { std::chrono::year_month_day{ std::chrono::year{ t.wYear }, std::chrono::month{ t.wMonth }, std::chrono::day{ t.wDay } }, t.wHour, t.wMinute };
    }

    std::wstring WhenText(std::wstring_view started, const LocalTime& now)
    {
        LocalTime t;
        if (!ParseStarted(started, t))
            return std::wstring(started);
        const int days = DaysBetween(t.day, now.day);
        const int month = static_cast<int>(static_cast<unsigned>(t.day.month())) - 1;
        const unsigned dayOfMonth = static_cast<unsigned>(t.day.day());
        if (days == 0)
        {
            const int minutes = (now.hour * 60 + now.minute) - (t.hour * 60 + t.minute);
            if (minutes < 1)
                return L"Just now";
            if (minutes < 60)
                return std::format(L"{} min ago", minutes);
            return std::format(L"{} h ago", minutes / 60);
        }
        if (days == 1)
            return std::format(L"Yesterday {:02}:{:02}", t.hour, t.minute);
        if (t.day.year() == now.day.year())
            return std::format(L"{} {} {:02}:{:02}", dayOfMonth, kMonths[month], t.hour, t.minute);
        return std::format(L"{} {} {}", dayOfMonth, kMonths[month], static_cast<int>(t.day.year()));
    }

    std::wstring DayTitle(std::wstring_view started, std::chrono::year_month_day today)
    {
        LocalTime t;
        if (!ParseStarted(started, t))
            return L"Earlier";
        const int days = DaysBetween(t.day, today);
        if (days == 0)
            return L"Today";
        if (days == 1)
            return L"Yesterday";
        const std::chrono::weekday weekday{ std::chrono::sys_days{ t.day } };
        const int month = static_cast<int>(static_cast<unsigned>(t.day.month())) - 1;
        auto title = std::format(L"{} {} {}", kWeekdays[weekday.c_encoding()], static_cast<unsigned>(t.day.day()), kMonthNames[month]);
        if (t.day.year() != today.year())
            title += std::format(L" {}", static_cast<int>(t.day.year()));
        return title;
    }
}
