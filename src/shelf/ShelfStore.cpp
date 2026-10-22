#include "shelf/ShelfStore.h"
#include "FileIo.h"
#include "Log.h"
#include "Strings.h"
#include "Utf.h"

#include <windows.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <format>
#include <fstream>

namespace sd::shelf
{
    std::string ToJson(const SavedShelf& shelf)
    {
        nlohmann::json j = nlohmann::json::object();
        j["version"] = 1;
        j["items"] = nlohmann::json::array();
        for (const auto& item : shelf.items)
            j["items"].push_back(Narrow(item));
        j["edge"] = EdgeName(shelf.edge);
        if (shelf.x && shelf.y)
        {
            j["x"] = *shelf.x;
            j["y"] = *shelf.y;
        }
        j["shown"] = shelf.shown;
        for (const auto* list : { &shelf.pinned, &shelf.hidden })
        {
            auto& out = j[list == &shelf.pinned ? "pinned" : "hidden"];
            out = nlohmann::json::array();
            for (const auto& path : *list)
                out.push_back(Narrow(path));
        }
        return j.dump(2);
    }

    bool FromJson(std::string_view text, SavedShelf& shelf)
    {
        const auto j = nlohmann::json::parse(text, nullptr, false);
        if (j.is_discarded() || !j.is_object())
            return false;
        SavedShelf out;
        const auto paths = [&](const char* key, std::vector<std::wstring>& list) {
            if (const auto items = j.find(key); items != j.end() && items->is_array())
                for (const auto& item : *items)
                    if (item.is_string() && !item.get<std::string>().empty())
                        list.push_back(Widen(item.get<std::string>()));
        };
        paths("items", out.items);
        paths("pinned", out.pinned);
        paths("hidden", out.hidden);
        if (const auto edge = j.find("edge"); edge != j.end() && edge->is_string())
            out.edge = EdgeFromName(edge->get<std::string>());
        const auto x = j.find("x"), y = j.find("y");
        if (x != j.end() && y != j.end() && x->is_number_integer() && y->is_number_integer())
        {
            out.x = x->get<int>();
            out.y = y->get<int>();
        }
        if (const auto shown = j.find("shown"); shown != j.end() && shown->is_boolean())
            out.shown = shown->get<bool>();
        shelf = std::move(out);
        return true;
    }

    bool Save(const std::wstring& path, const SavedShelf& shelf)
    {
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
        const auto temp = path + L".tmp";
        {
            std::ofstream out(std::filesystem::path(temp), std::ios::binary | std::ios::trunc);
            out << ToJson(shelf);
            if (!out.flush())
                return false;
        }
        // Our own state file, so replacing it is right (outputs are never replaced; see StagedOutput).
        if (!MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        {
            log::Warn(L"shelf: could not save {} ({})", path, GetLastError());
            DeleteFileW(temp.c_str());
            return false;
        }
        return true;
    }

    SavedShelf Load(const std::wstring& path)
    {
        SavedShelf shelf;
        std::string text;
        if (!ReadFileBytes(path, text))
            return shelf;
        if (!FromJson(text, shelf))
            log::Warn(L"shelf: {} is not a shelf file; starting empty", path);
        return shelf;
    }

    bool SamePath(std::wstring_view a, std::wstring_view b)
    {
        if (a.size() != b.size())
            return false;
        std::wstring x(a), y(b);
        std::replace(x.begin(), x.end(), L'/', L'\\');
        std::replace(y.begin(), y.end(), L'/', L'\\');
        return EqualsNoCase(x, y);
    }

    size_t AddToTop(std::vector<std::wstring>& items, const std::vector<std::wstring>& added, size_t limit)
    {
        const auto in = [](const std::vector<std::wstring>& list, const std::wstring& path) {
            return std::any_of(list.begin(), list.end(), [&](const std::wstring& p) { return SamePath(p, path); });
        };
        std::vector<std::wstring> result;
        size_t fresh = 0;
        for (const auto& path : added)
        {
            if (path.empty() || in(result, path))
                continue;
            if (!in(items, path))
                ++fresh;
            result.push_back(path);
        }
        for (auto& path : items)
            if (!in(result, path))
                result.push_back(std::move(path));
        if (result.size() > limit)
            result.resize(limit);
        items = std::move(result);
        return fresh;
    }

    bool IsPinned(const std::vector<std::wstring>& pinned, std::wstring_view path)
    {
        return std::any_of(pinned.begin(), pinned.end(), [&](const std::wstring& p) { return SamePath(p, path); });
    }

    bool SetPinned(std::vector<std::wstring>& pinned, const std::wstring& path, bool pin, size_t limit)
    {
        if (path.empty())
            return false;
        if (pin)
        {
            if (!IsPinned(pinned, path))
                AddToTop(pinned, { path }, limit);
            return true;
        }
        std::erase_if(pinned, [&](const std::wstring& p) { return SamePath(p, path); });
        return false;
    }

    std::vector<std::wstring> RecentList(const std::vector<std::wstring>& outputs, const std::vector<std::wstring>& hidden, size_t limit)
    {
        std::vector<std::wstring> recent;
        for (const auto& path : outputs)
        {
            if (recent.size() >= limit)
                break;
            if (path.empty() || IsPinned(recent, path) || IsPinned(hidden, path))
                continue;
            recent.push_back(path);
        }
        return recent;
    }

    void HideRecent(std::vector<std::wstring>& hidden, const std::wstring& path, size_t limit)
    {
        if (!path.empty())
            AddToTop(hidden, { path }, limit);
    }

    bool Visibility::Switch(bool on)
    {
        if (on == m_on)
            return false;
        m_on = on;
        return true;
    }

    size_t Visibility::Put(std::vector<std::wstring>& items, const std::vector<std::wstring>& files, size_t limit)
    {
        if (std::any_of(files.begin(), files.end(), [](const std::wstring& f) { return !f.empty(); }))
            m_on = true;
        return AddToTop(items, files, limit);
    }

    size_t RemoveIf(std::vector<std::wstring>& items, const std::function<bool(const std::wstring&)>& gone)
    {
        const auto before = items.size();
        std::erase_if(items, gone);
        return before - items.size();
    }

    void Selection::Resize(size_t count)
    {
        m_flags.resize(count, false);
        if (m_anchor >= count)
            m_anchor = count ? count - 1 : 0;
    }

    void Selection::Click(size_t index, bool ctrl, bool shift)
    {
        if (index >= m_flags.size())
            return;
        if (shift)
        {
            if (!ctrl)
                std::fill(m_flags.begin(), m_flags.end(), false);
            const size_t lo = std::min(m_anchor, index), hi = std::max(m_anchor, index);
            for (size_t i = lo; i <= hi && i < m_flags.size(); ++i)
                m_flags[i] = true;
            return;
        }
        if (ctrl)
            m_flags[index] = !m_flags[index];
        else
        {
            std::fill(m_flags.begin(), m_flags.end(), false);
            m_flags[index] = true;
        }
        m_anchor = index;
    }

    void Selection::SelectAll()
    {
        std::fill(m_flags.begin(), m_flags.end(), true);
    }

    void Selection::Clear()
    {
        std::fill(m_flags.begin(), m_flags.end(), false);
    }

    size_t Selection::Count() const
    {
        return static_cast<size_t>(std::count(m_flags.begin(), m_flags.end(), true));
    }

    std::vector<size_t> Selection::Indices() const
    {
        std::vector<size_t> out;
        for (size_t i = 0; i < m_flags.size(); ++i)
            if (m_flags[i])
                out.push_back(i);
        return out;
    }

    void Selection::Erase(size_t index)
    {
        if (index >= m_flags.size())
            return;
        m_flags.erase(m_flags.begin() + static_cast<std::ptrdiff_t>(index));
        if (m_anchor > index)
            --m_anchor;
        Resize(m_flags.size());
    }

    std::vector<size_t> Selection::DragSet(size_t pressed) const
    {
        if (IsSelected(pressed))
            return Indices();
        return { pressed };
    }

    std::vector<size_t> Selection::HandleSet() const
    {
        if (Count() > 0)
            return Indices();
        std::vector<size_t> all(m_flags.size());
        for (size_t i = 0; i < all.size(); ++i)
            all[i] = i;
        return all;
    }

    std::wstring CountText(size_t count)
    {
        return count == 1 ? std::wstring(L"1 item") : std::format(L"{} items", count);
    }

    std::wstring CountText(size_t files, size_t folders)
    {
        if (files == 0 && folders == 0)
            return {};
        if (folders == 0)
            return files == 1 ? std::wstring(L"1 file") : std::format(L"{} files", files);
        if (files == 0)
            return folders == 1 ? std::wstring(L"1 folder") : std::format(L"{} folders", folders);
        return CountText(files + folders);
    }
}
