#include "shelf/ShelfWindow.h"
#include "Brand.h"
#include "DropDescription.h"
#include "DropTarget.h"
#include "ErrorText.h"
#include "FileActions.h"
#include "FileMove.h"
#include "FormatText.h"
#include "Log.h"
#include "PanelLayout.h"
#include "PathUtil.h"
#include "UiThread.h"
#include "Utf.h"
#include "VirtualDrop.h"
#include "shelf/Carry.h"

#include <commctrl.h>
#include <shellapi.h>
#include <shlguid.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <windowsx.h>
#include <wrl/implements.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <format>

using Microsoft::WRL::ComPtr;
using namespace sd::theme;
using sd::shelf::Box;
using sd::shelf::Edge;
using sd::shelf::Hit;
using sd::shelf::Part;

namespace sd
{
    namespace
    {
        constexpr wchar_t kClassName[] = L"" SD_NAME L".Shelf";

        constexpr UINT kMsgThumb = WM_APP + 1;       // lParam: ThumbResult*, owned by the receiver
        constexpr UINT kMsgCheck = WM_APP + 2;       // lParam: CheckResult*
        constexpr UINT kMsgClipboard = WM_APP + 3;   // lParam: ClipboardJob*
        constexpr UINT kMsgKeep = WM_APP + 4;        // lParam: KeepJob*
        constexpr UINT kMsgRecent = WM_APP + 5;      // lParam: RecentResult*
        constexpr UINT_PTR kTimerFrame = 1;
        constexpr UINT_PTR kTimerFold = 2;
        constexpr UINT_PTR kTimerCheck = 3;

        constexpr float kPadDip = 26.f;       // shadow room around the panel
        constexpr float kShowMs = 240.f;
        constexpr float kHideMs = 180.f;
        constexpr float kOpenTauMs = 55.f;    // folding and unfolding
        constexpr float kSizeTauMs = 60.f;
        constexpr float kSlideTauMs = 70.f;
        constexpr float kScrollTauMs = 50.f;
        constexpr UINT kPeekMs = 1800;        // after files arrive, a pinned shelf stays open this long
        constexpr UINT kFoldMs = 450;         // after the mouse leaves
        constexpr UINT kCheckMs = 4000;       // files that disappeared are dropped this often
        constexpr ULONGLONG kNoteMs = 1800;

        // Segoe Fluent Icons / MDL2 Assets.
        constexpr wchar_t kGlyphPin = 0xE718;
        constexpr wchar_t kGlyphUnpin = 0xE77A;
        constexpr wchar_t kGlyphReveal = 0xE838;   // FolderOpen
        constexpr wchar_t kGlyphRemove = 0xE711;   // Cancel
        constexpr wchar_t kGlyphClose = 0xE8BB;    // ChromeClose
        constexpr wchar_t kGlyphFolder = 0xE8B7;   // Folder
        constexpr wchar_t kGlyphOpenFile = 0xE8E5; // OpenFile
        constexpr wchar_t kGlyphDown = 0xE70D, kGlyphUp = 0xE70E;   // ChevronDown, ChevronUp

        enum MenuId : UINT { kMenuOpen = 1, kMenuReveal, kMenuCopy, kMenuRemove, kMenuPin, kMenuClear, kMenuClose, kMenuPinItem };

        // The header buttons Tab moves through, left to right (Clear all only while there are files).
        constexpr Part kHeaderButtons[] = { Part::DropDown, Part::Pin, Part::Clear, Part::Close };
        // Tooltip ids of a row's buttons (the header buttons' ids are their Part).
        constexpr UINT_PTR kRowTipBase = 100;

        constexpr const wchar_t* kTitleShelf = L"Float shelf";
        constexpr const wchar_t* kTitleDropDown = L"Pinned and recent";

        // The paths of a list of the shelf's items.
        template <class ItemT>
        std::vector<std::wstring> PathsIn(const std::vector<ItemT>& items)
        {
            std::vector<std::wstring> paths;
            for (const auto& item : items)
                paths.push_back(item.path);
            return paths;
        }

        // The shelf's colours, from the same palette as the wheel and the top panel (theme::Palette):
        // a floating surface that follows the light or dark app theme.
        struct Colors
        {
            D2D1_COLOR_F surface, line, text, dim, hover, button, accent, danger, onDanger, shadow;
        };
        Colors ColorsFor(const Palette& p, Style style)
        {
            Colors c{};
            // Glass has no blur behind the shelf: the panel's tint, a little stronger, keeps it legible.
            c.surface = style == Style::Glass ? WithAlpha(p.panel, std::min(1.f, 0.94f / std::max(0.01f, p.panel.a))) : p.panel;
            c.line = p.panelLine;
            c.text = p.panelText;
            c.dim = p.panelTextDim;
            c.hover = p.hover;
            c.button = p.keycap;
            c.accent = p.accentMark;
            c.danger = p.error;
            c.onDanger = Rgb(accent::TextOn(RgbOf(p.error)));
            c.shadow = p.shadow;
            return c;
        }

        // Only a definite "not there" drops an item; an unplugged network drive or a locked file keeps it.
        bool IsGoneError(DWORD error)
        {
            return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
        }
    }

    struct Shelf::ThumbResult
    {
        std::wstring path;
        bool exists = true, folder = false;
        unsigned long long size = 0;
        Pixels pixels;
    };

    struct Shelf::CheckResult
    {
        std::vector<std::wstring> gone;
    };

    // History's latest outputs that still exist, newest first (before Recent's own filtering).
    struct Shelf::RecentResult
    {
        std::vector<std::wstring> outputs;
    };

    struct Shelf::ClipboardJob
    {
        std::vector<std::wstring> files;
        DWORD sequence = 0;   // the clipboard as we left it; anything newer wins
        shelf::ClipboardImage image;
        bool ok = false;
    };

    // Files dragged out of a browser or a mail app onto the shelf, saved in the drag's drop folder and
    // waiting to be kept somewhere lasting (the drop folder goes with `drop`).
    struct Shelf::KeepJob
    {
        std::shared_ptr<VirtualDrop> drop;
        std::vector<std::wstring> saved;
        std::wstring error;
    };

    Shelf::Shelf(HINSTANCE instance, std::wstring storePath) : m_instance(instance), m_storePath(std::move(storePath))
    {
    }

    Shelf::~Shelf()
    {
        if (m_hwnd)
            Save();   // on exit, so it comes back where it was left
        *m_alive = false;
        SetInstantFileAction("file.float", nullptr);
        m_worker.reset();   // joins: nothing posts to the window after this
        if (m_registered && m_hwnd)
            RevokeDragDrop(m_hwnd.get());
        m_hwnd.reset();
    }

    bool Shelf::Create()
    {
        // Not WS_EX_NOACTIVATE: a click gives it the keyboard. WS_EX_TOOLWINDOW keeps it off the
        // taskbar and out of Alt+Tab.
        m_hwnd.reset(CreatePanel(m_instance, kClassName, WndProc, WS_EX_TOPMOST | WS_EX_TOOLWINDOW, L"" SD_NAME L" Float shelf", this,
                                 IDC_ARROW, CS_DBLCLKS));
        if (!m_hwnd || !InitGraphics())
        {
            log::Error(L"shelf: could not create the window");
            return false;
        }
        m_worker = std::make_unique<JobQueue>(1, JobQueue::Apartment::Sta);   // shell thumbnails want an STA

        // The app's drop target, as the wheel has: CF_HDROP and a browser's or mail app's virtual files.
        m_dropTarget = Microsoft::WRL::Make<DropTarget>(m_hwnd.get(), static_cast<IDropSink*>(this), MakeDragImageHelper());
        if (const HRESULT hr = RegisterDragDrop(m_hwnd.get(), m_dropTarget.Get()); SUCCEEDED(hr))
            m_registered = true;
        else
            log::Warn(L"shelf: RegisterDragDrop failed ({}); files reach it only through the wheel", HexCode(hr));

        // The header buttons' names, as tooltips (the text is asked for when one shows: TTN_GETDISPINFO).
        m_tips = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, CW_USEDEFAULT,
                                 CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, m_hwnd.get(), nullptr, m_instance, nullptr);
        if (m_tips)
        {
            std::vector<UINT_PTR> ids;
            for (const Part part : kHeaderButtons)
                ids.push_back(static_cast<UINT_PTR>(part));
            for (size_t k = 0; k < std::size(shelf::kRowButtons); ++k)
                ids.push_back(kRowTipBase + k);   // the hovered row's buttons
            for (const UINT_PTR id : ids)
            {
                TTTOOLINFOW tool{ sizeof(tool) };
                tool.uFlags = TTF_SUBCLASS;   // it watches the shelf's mouse messages itself
                tool.hwnd = m_hwnd.get();
                tool.uId = id;
                tool.lpszText = LPSTR_TEXTCALLBACKW;
                SendMessageW(m_tips, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tool));
            }
        }

        SetInstantFileAction("file.float", [this](const std::vector<std::wstring>& files, HWND) {
            Add(files);
            const auto what = files.size() == 1 ? FileName(files[0]) : std::format(L"{} items", files.size());
            return FileActionResult{ true, std::format(L"{} on the Float shelf", what) };
        });
        Restore();
        return true;
    }

    void Shelf::SetAppearance(const Appearance& appearance)
    {
        m_appearance = appearance;
        if (m_visible)
        {
            Refresh();
            Animate();
        }
    }

    // ---- Items -----------------------------------------------------------------------------------

    void Shelf::Restore()
    {
        const auto saved = shelf::Load(m_storePath);
        m_edge = saved.edge;
        m_place = saved.place;
        if (m_place)
            m_edge = m_place->edge;   // placed on first show (Refresh), on whichever monitor fits
        else if (saved.x && saved.y)
        {
            // A file from before "place": its pixels, remembered as a place once shown.
            m_x = static_cast<float>(*saved.x);
            m_y = static_cast<float>(*saved.y);
            m_placed = true;
        }
        for (const auto& path : saved.items)
            m_items.push_back(Item{ path, FileName(path) });
        for (const auto& path : saved.pinned)
            m_pinned.push_back(Item{ path, FileName(path) });
        m_hidden = saved.hidden;
        m_selection.Resize(m_items.size());
        m_shown = shelf::Visibility(saved.shown);
        log::Info(L"shelf: {} item(s) and {} pinned restored, {}", m_items.size(), m_pinned.size(), m_shown.On() ? L"on" : L"switched off");
        if (!m_shown.On())
            return;
        // Back where it was, empty or not; a pinned shelf comes back folded so it doesn't cover
        // anything at login.
        m_open = m_edge == Edge::None ? 1.f : 0.f;
        m_wantOpen = m_edge == Edge::None;
        Show(false);
        CheckFiles();
    }

    void Shelf::Save() const
    {
        shelf::SavedShelf saved;
        saved.items = PathsIn(m_items);
        saved.pinned = PathsIn(m_pinned);
        saved.hidden = m_hidden;
        saved.edge = m_edge;
        saved.shown = m_shown.On();
        if (m_placed)
        {
            saved.x = static_cast<int>(std::lround(m_x));
            saved.y = static_cast<int>(std::lround(m_y));
        }
        saved.place = m_place;
        shelf::Save(m_storePath, saved);
    }

    void Shelf::RememberPlace()
    {
        m_place = shelf::PlaceOf(Expanded(), m_work, m_edge, m_device);
    }

    HMONITOR Shelf::SavedMonitor() const
    {
        struct Found
        {
            std::vector<HMONITOR> monitors;
            std::vector<shelf::Screen> screens;
        } found;
        EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR monitor, HDC, LPRECT, LPARAM data) -> BOOL {
            auto& f = *reinterpret_cast<Found*>(data);
            MONITORINFOEXW mi{};
            mi.cbSize = sizeof(mi);
            if (GetMonitorInfoW(monitor, &mi))
            {
                f.monitors.push_back(monitor);
                f.screens.push_back({ Box{ static_cast<float>(mi.rcWork.left), static_cast<float>(mi.rcWork.top), static_cast<float>(mi.rcWork.right),
                                           static_cast<float>(mi.rcWork.bottom) },
                                      mi.szDevice });
            }
            return TRUE;
        }, reinterpret_cast<LPARAM>(&found));
        if (!m_place || found.monitors.empty())
            return MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
        return found.monitors[shelf::PickScreen(found.screens, *m_place)];
    }

    void Shelf::Add(const std::vector<std::wstring>& files)
    {
        if (files.empty() || !m_hwnd)
            return;
        std::vector<std::wstring> paths;
        for (const auto& item : m_items)
            paths.push_back(item.path);
        const size_t fresh = m_shown.Put(paths, files);   // also switches a closed shelf back on
        // Rebuild in the new order, keeping the thumbnails of items that were already here.
        std::vector<Item> items;
        for (const auto& path : paths)
        {
            const auto it = std::find_if(m_items.begin(), m_items.end(), [&](const Item& i) { return shelf::SamePath(i.path, path); });
            items.push_back(it != m_items.end() ? std::move(*it) : Item{ path, FileName(path) });
        }
        m_items = std::move(items);
        m_selection = shelf::Selection();
        m_selection.Resize(m_items.size());
        m_focus = 0;
        m_rowButton = -1;
        m_scrollTarget = 0.f;
        m_dropDown = false;   // what just arrived is what to see
        log::Info(L"shelf: {} added ({} new), {} on the shelf", files.size(), fresh, m_items.size());
        Save();
        Show(true);
        RequestThumbnails();
    }

    void Shelf::SwitchOn(bool on)
    {
        if (!m_hwnd)
            return;
        const bool changed = m_shown.Switch(on);
        log::Info(L"shelf: switched {} ({} item(s))", on ? L"on" : L"off", m_items.size());
        if (changed)
            Save();
        if (!on)
        {
            m_keyPart = Part::None;
            Hide();
            return;
        }
        Show(false);
        WantOpen(true);
        if (m_edge != Edge::None)
            ScheduleFold(kPeekMs);   // a pinned shelf folds back as usual once the pointer is elsewhere
    }

    std::vector<std::wstring> Shelf::PathsOf(const std::vector<size_t>& indices) const
    {
        std::vector<std::wstring> paths;
        for (size_t i : indices)
            if (i < m_items.size())
                paths.push_back(m_items[i].path);
        return paths;
    }

    void Shelf::RemoveAt(std::vector<size_t> indices)
    {
        std::sort(indices.begin(), indices.end(), std::greater<>());
        indices.erase(std::unique(indices.begin(), indices.end()), indices.end());
        for (size_t i : indices)
            if (i < m_items.size())
            {
                m_items.erase(m_items.begin() + static_cast<std::ptrdiff_t>(i));
                m_selection.Erase(i);
            }
        const auto entries = Entries();
        m_focus = std::min(m_focus, entries.empty() ? 0 : entries.size() - 1);
        m_scrollTarget = std::min(m_scrollTarget, shelf::MaxScroll(m_metrics, Shape(entries)));
        m_hover = {};
        if (m_items.empty() && m_keyPart == Part::Clear)
            m_keyPart = Part::Pin;   // Clear goes with the last file
        Save();
        Animate();   // an empty shelf stays: it is where files can be dropped
    }

    void Shelf::Clear()
    {
        log::Info(L"shelf: cleared {} item(s)", m_items.size());
        m_items.clear();
        m_selection = shelf::Selection();
        m_hover = {};
        if (m_keyPart == Part::Clear)
            m_keyPart = Part::Pin;
        m_scrollTarget = 0.f;
        Save();
        Animate();   // stays on screen, empty, ready for a drop
    }

    void Shelf::RequestThumbnails()
    {
        if (!m_worker || m_thumbPx == 0)
            return;
        for (auto* list : { &m_items, &m_pinned, &m_recent })
        for (auto& item : *list)
        {
            if (item.requested)
                continue;
            item.requested = true;
            m_worker->Submit([hwnd = m_hwnd.get(), path = item.path, px = m_thumbPx](std::stop_token stop) {
                auto result = std::make_unique<ThumbResult>();
                result->path = path;
                WIN32_FILE_ATTRIBUTE_DATA data{};
                if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data))
                {
                    result->folder = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
                    result->size = FileSizeOf(data);
                    if (!stop.stop_requested())
                        result->pixels = ShellThumbnail(path, px);
                }
                else
                {
                    result->exists = !IsGoneError(GetLastError());
                }
                PostOwned(hwnd, kMsgThumb, std::move(result));
            });
        }
    }

    void Shelf::OnThumb(std::unique_ptr<ThumbResult> r)
    {
        const auto same = [&](const Item& i) { return shelf::SamePath(i.path, r->path); };
        if (!r->exists)
        {
            // Gone from every list: the shelf's, Pinned (saved) and Recent.
            log::Info(L"shelf: {} is gone; removed", r->path);
            const size_t pinned = std::erase_if(m_pinned, same);
            std::erase_if(m_recent, same);
            if (const auto it = std::find_if(m_items.begin(), m_items.end(), same); it != m_items.end())
                RemoveAt({ static_cast<size_t>(it - m_items.begin()) });   // saves
            else if (pinned)
                Save();
            m_focus = std::min(m_focus, Entries().empty() ? 0 : Entries().size() - 1);
            Animate();
            return;
        }
        // The same file can be on the shelf, pinned and recent: each gets the picture.
        for (auto* list : { &m_items, &m_pinned, &m_recent })
            for (auto& item : *list)
            {
                if (!same(item))
                    continue;
                item.folder = r->folder;
                const auto parent = std::filesystem::path(item.path).parent_path().filename().wstring();
                const auto what = r->folder ? std::wstring(L"Folder") : SizeText(r->size);
                item.detail = parent.empty() ? what : std::format(L"{} · {}", what, parent);
                if (!r->pixels.px.empty())
                {
                    item.thumb = r->pixels;
                    item.bitmap.Reset();
                }
            }
        Animate();
    }

    void Shelf::CheckFiles()
    {
        if (m_checking || (m_items.empty() && m_pinned.empty()) || !m_worker)
            return;
        m_checking = true;
        std::vector<std::wstring> paths = PathsIn(m_items);
        for (const auto& item : m_pinned)
            paths.push_back(item.path);
        m_worker->Submit([hwnd = m_hwnd.get(), paths = std::move(paths)](std::stop_token) {
            auto result = std::make_unique<CheckResult>();
            for (const auto& p : paths)
                if (GetFileAttributesW(p.c_str()) == INVALID_FILE_ATTRIBUTES && IsGoneError(GetLastError()))
                    result->gone.push_back(p);
            PostOwned(hwnd, kMsgCheck, std::move(result));
        });
    }

    void Shelf::OnCheck(std::unique_ptr<CheckResult> r)
    {
        m_checking = false;
        size_t unpinned = 0;
        for (const auto& gone : r->gone)
            unpinned += std::erase_if(m_pinned, [&](const Item& i) { return shelf::SamePath(i.path, gone); });
        if (unpinned)
        {
            log::Info(L"shelf: {} pinned item(s) were moved or deleted; unpinned", unpinned);
            Save();
            Animate();
        }
        std::vector<size_t> indices;
        for (const auto& gone : r->gone)
            for (size_t i = 0; i < m_items.size(); ++i)
                if (shelf::SamePath(m_items[i].path, gone))
                    indices.push_back(i);
        if (indices.empty())
            return;
        log::Info(L"shelf: {} item(s) were moved or deleted; removed", indices.size());
        RemoveAt(std::move(indices));
    }

    // ---- Carry -----------------------------------------------------------------------------------

    Island::ResultActions Shelf::CarryActions(std::vector<std::wstring> outputs)
    {
        Island::ResultActions actions{};
        if (outputs.empty())
            return actions;
        const std::weak_ptr<bool> alive = m_alive;
        actions.files = outputs.size();
        // Open and Show in folder need no shelf: the shell does them.
        actions.open = [outputs] { shelf::OpenFiles(outputs); };
        actions.reveal = [outputs] { shelf::ShowInFolder(outputs); };
        RefreshRecent();   // a conversion just finished: it is the newest of Recent conversions
        actions.drag = [alive, this, outputs] {
            if (!alive.expired())
                shelf::DragOut(m_hwnd.get(), outputs);
        };
        actions.toShelf = [alive, this, outputs] {
            if (alive.expired())
                return std::wstring();
            Add(outputs);
            return std::wstring(L"On the Float shelf, drag it from there any time");
        };
        actions.copy = [alive, this, outputs] {
            return alive.expired() ? std::wstring() : Copy(outputs);
        };
        return actions;
    }

    std::wstring Shelf::Copy(const std::vector<std::wstring>& files)
    {
        if (files.empty())
            return {};
        if (!shelf::PutOnClipboard(m_hwnd.get(), files, nullptr))
            return L"Could not open the clipboard";
        log::Info(L"shelf: {} file(s) on the clipboard", files.size());
        if (files.size() != 1 || !shelf::IsClipboardImage(files[0]) || !m_worker)
            return files.size() == 1 ? std::wstring(L"Copied, paste it into a folder or an app")
                                     : std::format(L"Copied {} files, paste them into a folder or an app", files.size());
        // The picture follows a moment later (decoding can take a while for a big photo); until then
        // a paste gets the file, which is what most apps want anyway.
        auto job = std::make_unique<ClipboardJob>();
        job->files = files;
        job->sequence = GetClipboardSequenceNumber();
        m_worker->Submit([hwnd = m_hwnd.get(), job = std::shared_ptr<ClipboardJob>(std::move(job))](std::stop_token) mutable {
            std::wstring error;
            job->ok = SUCCEEDED(shelf::MakeClipboardImage(job->files[0], job->image, error));
            if (!job->ok)
                log::Info(L"shelf: no picture for the clipboard: {}", error);
            PostOwned(hwnd, kMsgClipboard, std::make_unique<ClipboardJob>(std::move(*job)));
        });
        return L"Copied, paste the picture into a chat or the file into a folder";
    }

    // ---- Pinned and Recent conversions (the header's drop-down) -----------------------------------

    std::vector<Shelf::Entry> Shelf::Entries() const
    {
        std::vector<Entry> entries;
        if (!m_dropDown)
        {
            for (size_t i = 0; i < m_items.size(); ++i)
                entries.push_back({ List::Shelf, i });
            return entries;
        }
        for (const List list : { List::Pinned, List::Recent })
        {
            entries.push_back({ list, 0, true });
            for (size_t i = 0; i < ItemsOf(list).size(); ++i)
                entries.push_back({ list, i });
        }
        return entries;
    }

    shelf::ListShape Shelf::Shape(const std::vector<Entry>& entries) const
    {
        if (!m_dropDown)
        {
            auto shape = shelf::ShelfShape(m_items.size());
            shape.dropDown = DropDownWidth();
            return shape;
        }
        shelf::ListShape shape;
        for (const auto& e : entries)
            shape.rows.push_back(e.label ? shelf::RowKind::Label : shelf::RowKind::Item);
        shape.dropDown = DropDownWidth();
        return shape;
    }

    float Shelf::DropDownWidth() const
    {
        // From the title's start: the Float icon's room, the title, a gap and the chevron (DrawPanel).
        if (!m_titleFont)
            return 0.f;
        return TextWidth(m_dropDown ? kTitleDropDown : kTitleShelf, m_titleFont.Get()) + 40.f * m_scale;
    }

    shelf::Layout Shelf::CurrentLayout() const
    {
        return shelf::LayOut(Content(), m_metrics, Shape(Entries()), m_scroll);
    }

    std::vector<Shelf::Item>& Shelf::ItemsOf(List list)
    {
        return list == List::Pinned ? m_pinned : list == List::Recent ? m_recent : m_items;
    }

    const std::vector<Shelf::Item>& Shelf::ItemsOf(List list) const
    {
        return list == List::Pinned ? m_pinned : list == List::Recent ? m_recent : m_items;
    }

    const Shelf::Item* Shelf::ItemAt(const std::vector<Entry>& entries, size_t entry) const
    {
        if (entry >= entries.size() || entries[entry].label)
            return nullptr;
        const auto& items = ItemsOf(entries[entry].list);
        return entries[entry].index < items.size() ? &items[entries[entry].index] : nullptr;
    }

    bool Shelf::IsPinned(const std::wstring& path) const
    {
        return std::any_of(m_pinned.begin(), m_pinned.end(), [&](const Item& i) { return shelf::SamePath(i.path, path); });
    }

    void Shelf::SetPinned(const std::wstring& path, bool pin)
    {
        auto paths = PathsIn(m_pinned);
        shelf::SetPinned(paths, path, pin);
        // In the new order, keeping the pictures of items that were pinned already (or are on the
        // shelf or recent: the same file).
        std::vector<Item> pinned;
        for (const auto& p : paths)
        {
            const Item* known = nullptr;
            for (const auto* list : { &m_pinned, &m_items, &m_recent })
                for (const auto& item : *list)
                    if (!known && shelf::SamePath(item.path, p))
                        known = &item;
            pinned.push_back(known ? *known : Item{ p, FileName(p) });
        }
        m_pinned = std::move(pinned);
        log::Info(L"shelf: {} {}, {} pinned", FileName(path), pin ? L"pinned" : L"unpinned", m_pinned.size());
        Save();
        RequestThumbnails();
        Animate();
    }

    void Shelf::RunButton(Part part, size_t entry)
    {
        const auto entries = Entries();
        const Item* item = ItemAt(entries, entry);
        if (!item)
            return;
        const Entry e = entries[entry];
        const std::wstring path = item->path;   // the lists change below
        // On the shelf, Open and Show in folder take the selection when the row is part of it.
        const auto files = e.list == List::Shelf ? PathsOf(m_selection.DragSet(e.index)) : std::vector<std::wstring>{ path };
        switch (part)
        {
        case Part::Open: Open(files); break;
        case Part::Reveal: shelf::ShowInFolder(files); break;
        case Part::PinItem: SetPinned(path, !IsPinned(path)); break;
        case Part::Remove:
            // × takes it off this list only: the shelf's, Pinned, or Recent (remembered, so History's
            // entry and the file stay but the list no longer shows it).
            if (e.list == List::Shelf)
                RemoveAt({ e.index });
            else if (e.list == List::Pinned)
                SetPinned(path, false);
            else
            {
                shelf::HideRecent(m_hidden, path);
                m_recent.erase(m_recent.begin() + static_cast<std::ptrdiff_t>(e.index));
                log::Info(L"shelf: {} taken off Recent conversions", FileName(path));
                Save();
            }
            m_hover = {};
            m_focus = std::min(m_focus, std::max<size_t>(Entries().size(), 1) - 1);
            m_rowButton = -1;
            Animate();
            break;
        default: break;
        }
    }

    void Shelf::ToggleDropDown()
    {
        m_dropDown = !m_dropDown;
        m_focus = m_dropDown ? 1 : 0;   // the first item under the Pinned heading, or the shelf's first
        m_rowButton = -1;
        m_hover = {};
        m_scrollTarget = m_scroll = 0.f;
        if (m_keyPart == Part::Clear)
            m_keyPart = Part::DropDown;
        if (m_dropDown)
            RefreshRecent();
        log::Info(L"shelf: {}", m_dropDown ? L"Pinned and recent shown" : L"back to the shelf");
        Animate();
    }

    void Shelf::RefreshRecent()
    {
        if (!m_worker || !m_recentSource || !m_hwnd)
            return;
        if (m_recentBusy)
        {
            m_recentAgain = true;
            return;
        }
        m_recentBusy = true;
        // Enough to fill the list after the hidden ones and files that are gone are left out.
        const size_t want = shelf::kRecentCount + std::min<size_t>(m_hidden.size(), 200) + 20;
        m_worker->Submit([hwnd = m_hwnd.get(), source = m_recentSource, want](std::stop_token) {
            auto result = std::make_unique<RecentResult>();
            for (auto& path : source(want))
                if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES)
                    result->outputs.push_back(std::move(path));
            PostOwned(hwnd, kMsgRecent, std::move(result));
        });
    }

    void Shelf::OnRecent(std::unique_ptr<RecentResult> r)
    {
        m_recentBusy = false;
        std::vector<Item> recent;
        for (const auto& path : shelf::RecentList(r->outputs, m_hidden, shelf::kRecentCount))
        {
            const auto it = std::find_if(m_recent.begin(), m_recent.end(), [&](const Item& i) { return shelf::SamePath(i.path, path); });
            recent.push_back(it != m_recent.end() ? std::move(*it) : Item{ path, FileName(path) });
        }
        m_recent = std::move(recent);
        if (m_dropDown)
            m_focus = std::min(m_focus, Entries().size() - 1);
        RequestThumbnails();
        Animate();
        if (std::exchange(m_recentAgain, false))
            RefreshRecent();
    }

    // ---- Drop target -----------------------------------------------------------------------------

    bool Shelf::OnDragEnter(const std::vector<std::wstring>& files, POINT)
    {
        if (m_draggingOut || files.empty())
            return false;   // our own drag out is never taken back
        m_dropHover = true;
        KillTimer(m_hwnd.get(), kTimerFold);
        WantOpen(true);
        Animate();
        return true;
    }

    DWORD Shelf::OnDragOver(POINT, DWORD allowed)
    {
        return SafeEffect(allowed);   // copy, like the wheel: MOVE would make the source delete the file
    }

    void Shelf::OnDragLeave()
    {
        m_dropHover = false;
        ScheduleFold(kFoldMs);
        Animate();
    }

    void Shelf::OnDrop(const std::vector<std::wstring>& files, POINT)
    {
        m_dropHover = false;
        // Dragged out of a browser or a mail app: saved now, while the source still holds them, and
        // kept a moment later, after the drop (a folder may have to be picked; the source is waiting).
        if (auto dragged = files.empty() ? nullptr : VirtualDrop::Find(files.front()))
        {
            auto job = std::make_unique<KeepJob>();
            job->saved = dragged->Save({}, job->error);
            job->drop = std::move(dragged);
            PostOwned(m_hwnd.get(), kMsgKeep, std::move(job));
            Animate();
            return;
        }
        // Adding only lists the paths and asks for thumbnails, so the source is not kept waiting.
        Add(files);
    }

    void Shelf::OnKeep(std::unique_ptr<KeepJob> job)
    {
        // Kept where files dragged out of apps go (Settings: the Desktop, Downloads, or asked each time).
        std::vector<std::wstring> kept;
        std::wstring error = job->error;
        if (!job->saved.empty())
        {
            std::wstring folder = job->drop->OutputFolder();
            if (job->drop->AsksFolder() && !PickDestinationFolder(m_hwnd.get(), job->saved.size(), folder, L"Save"))
            {
                log::Info(L"shelf: no folder picked for the dropped file(s); nothing kept");
                return;
            }
            kept = KeepVirtualFiles(job->saved, folder, error);
        }
        if (kept.empty())
        {
            ShowNote(L"The dropped file could not be saved");
            log::Warn(L"shelf: dropped file(s) not kept: {}", error);
            return;
        }
        Add(kept);
    }

    void Shelf::ShowNote(std::wstring note)
    {
        m_note = std::move(note);
        m_noteUntil = GetTickCount64() + kNoteMs;
        Animate();
    }

    // ---- Showing, hiding, folding -------------------------------------------------------------

    void Shelf::UseMonitor(HMONITOR monitor)
    {
        m_monitor = monitor;
        MONITORINFOEXW mi{};
        mi.cbSize = sizeof(mi);
        GetMonitorInfoW(monitor, &mi);
        m_device = mi.szDevice;
        m_work = Box{ static_cast<float>(mi.rcWork.left), static_cast<float>(mi.rcWork.top), static_cast<float>(mi.rcWork.right),
                      static_cast<float>(mi.rcWork.bottom) };
        m_screen = Box{ static_cast<float>(mi.rcMonitor.left), static_cast<float>(mi.rcMonitor.top), static_cast<float>(mi.rcMonitor.right),
                        static_cast<float>(mi.rcMonitor.bottom) };
        const float scale = ScaleFor(monitor, m_appearance.size);
        if (scale != m_scale || m_fontScale == 0.f)
        {
            // Keep the panel's height in step with the new scale so it doesn't animate from the old one.
            m_h = m_h > 0.f ? m_h * scale / m_scale : 0.f;
            m_scale = scale;
            m_metrics = shelf::Metrics(scale);
            EnsureTextFormats();
        }
        // Thumbnails at the size they are drawn; asked for again only if they would be too small.
        const auto px = static_cast<unsigned>(std::ceil(m_metrics.thumb));
        if (px > m_thumbPx)
        {
            m_thumbPx = px;
            for (auto& item : m_items)
                item.requested = false;
            RequestThumbnails();
        }
    }

    void Shelf::Refresh()
    {
        const bool place = !m_placed;
        if (place)
        {
            // Where the user left it, on that monitor or the nearest one; the first time (nothing
            // saved) on the screen the mouse is on.
            POINT cursor{};
            GetCursorPos(&cursor);
            UseMonitor(m_place ? SavedMonitor() : MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST));
        }
        else
        {
            const POINT anchor{ static_cast<LONG>(m_x + 10.f), static_cast<LONG>(m_y + 10.f) };
            UseMonitor(MonitorFromPoint(anchor, MONITOR_DEFAULTTONEAREST));
        }
        m_palette = Current(m_appearance.style);
        m_kit.SetPalette(theme::EditorColors());
        m_motion = AnimationsOn();
        if (m_h <= 0.f)
            m_h = shelf::PanelHeight(m_metrics, Shape(Entries()));
        if (place)
        {
            // With nothing saved, shelf::Place's default: pinned to the right edge, a fifth of the way down.
            const shelf::Place to = m_place.value_or(shelf::Place{});
            const Box b = shelf::PlaceIn(to, m_work, m_metrics.width, m_h);
            m_edge = to.edge;
            m_x = b.left;
            m_y = b.top;
            m_placed = true;
        }
        // Back inside the (possibly changed) work area. Clamping here never changes the saved place:
        // only the user's own moves do.
        const Box rest = Expanded();
        m_x = rest.left;
        m_y = rest.top;
        if (!m_place && !m_moving)
            RememberPlace();
    }

    Box Shelf::Expanded() const
    {
        const Box b{ m_x, m_y, m_x + m_metrics.width, m_y + m_h };
        return m_moving ? b : shelf::Dock(b, m_work, m_edge);
    }

    void Shelf::Appearing(float& dx, float& dy) const
    {
        // A pinned shelf slides out of its edge, a floating one rises a little.
        const float away = (1.f - EaseOutCubic(m_presence)) * 18.f * m_scale;
        shelf::Outward(m_edge, dx, dy);
        if (m_edge == Edge::None)
            dy = 0.5f;
        dx *= away;
        dy *= away;
    }

    Box Shelf::Content() const
    {
        float dx = 0.f, dy = 0.f;
        Appearing(dx, dy);
        return Expanded().Offset(m_slideX + dx, m_slideY + dy);
    }

    Box Shelf::Shown() const
    {
        const Box open = Expanded().Offset(m_slideX, m_slideY);
        Box b = open;
        if (m_edge != Edge::None)
            b = shelf::Lerp(shelf::Tab(open, m_work, m_edge, m_metrics.tabThickness, m_metrics.tabLength), open, m_open);
        float dx = 0.f, dy = 0.f;
        Appearing(dx, dy);
        return b.Offset(dx, dy);
    }

    void Shelf::Show(bool peek)
    {
        if (!m_hwnd)
            return;
        if (!m_visible || m_hiding)
        {
            const bool fresh = !m_visible;
            Refresh();
            m_visible = true;
            m_hiding = false;
            if (fresh)
            {
                m_presence = m_motion ? 0.f : 1.f;
                m_h = shelf::PanelHeight(m_metrics, Shape(Entries()));
                m_slideX = m_slideY = 0.f;
                m_scroll = m_scrollTarget;
            }
            SetTimer(m_hwnd.get(), kTimerCheck, kCheckMs, nullptr);
            Render();
            ShowWindow(m_hwnd.get(), SW_SHOWNOACTIVATE);
        }
        if (peek)
        {
            WantOpen(true);
            ScheduleFold(kPeekMs);
        }
        Animate();
    }

    void Shelf::Hide()
    {
        if (!m_visible)
            return;
        KillTimer(m_hwnd.get(), kTimerCheck);
        KillTimer(m_hwnd.get(), kTimerFold);
        m_hiding = true;
        if (!m_motion)
            m_presence = 0.f;
        Animate();
    }

    void Shelf::WantOpen(bool open)
    {
        if (m_edge == Edge::None)
            open = true;
        if (open == m_wantOpen)
            return;
        m_wantOpen = open;
        if (!open)
            m_hover = {};
        Animate();
    }

    void Shelf::ScheduleFold(UINT delayMs)
    {
        if (m_edge != Edge::None && m_hwnd)
            SetTimer(m_hwnd.get(), kTimerFold, delayMs, nullptr);
    }

    void Shelf::Animate()
    {
        if (!m_hwnd || !m_visible)
            return;
        m_frames.Start(m_hwnd.get(), kTimerFrame);
        Render();
    }

    void Shelf::OnFrame()
    {
        const ULONGLONG now = GetTickCount64();
        const float dt = m_frames.Tick(now);
        // Without motion everything is where it is going at once.
        const auto approach = [dt, this](float& value, float target, float tau, float close) {
            if (m_motion)
                panel::Approach(value, target, dt, tau, close);
            else
                value = target;
        };
        const float height = shelf::PanelHeight(m_metrics, Shape(Entries()));
        approach(m_open, m_wantOpen ? 1.f : 0.f, kOpenTauMs, 0.002f);
        approach(m_h, height, kSizeTauMs, 0.3f);
        approach(m_slideX, 0.f, kSlideTauMs, 0.3f);
        approach(m_slideY, 0.f, kSlideTauMs, 0.3f);
        approach(m_scroll, m_scrollTarget, kScrollTauMs, 0.3f);
        if (!m_note.empty() && now >= m_noteUntil)
            m_note.clear();

        if (m_hiding)
        {
            m_presence = m_motion ? std::max(0.f, m_presence - dt / kHideMs) : 0.f;
            if (m_presence <= 0.f)
            {
                m_hiding = false;
                m_visible = false;
                ShowWindow(m_hwnd.get(), SW_HIDE);
                m_frames.Stop(m_hwnd.get(), kTimerFrame);
                return;
            }
        }
        else
        {
            m_presence = m_motion ? std::min(1.f, m_presence + dt / kShowMs) : 1.f;
        }
        Render();

        const bool settled = !m_hiding && m_presence >= 1.f && m_open == (m_wantOpen ? 1.f : 0.f) && m_h == height && m_slideX == 0.f &&
                             m_slideY == 0.f && m_scroll == m_scrollTarget && m_note.empty();
        if (settled)
            m_frames.Stop(m_hwnd.get(), kTimerFrame);
    }

    // ---- Mouse and keyboard ----------------------------------------------------------------------

    POINT Shelf::ToScreen(LPARAM lParam) const
    {
        return POINT{ GET_X_LPARAM(lParam) + m_origin.x, GET_Y_LPARAM(lParam) + m_origin.y };
    }

    Hit Shelf::HitAt(POINT pt) const
    {
        if (Folded() || m_hiding || !m_visible)
            return {};
        return shelf::HitTest(CurrentLayout(), static_cast<float>(pt.x), static_cast<float>(pt.y));
    }

    void Shelf::OnMouseMove(POINT pt)
    {
        if (!m_tracking)
        {
            TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, m_hwnd.get(), 0 };
            m_tracking = TrackMouseEvent(&tme) != FALSE;
        }
        m_mouseInside = true;
        KillTimer(m_hwnd.get(), kTimerFold);
        if (!m_wantOpen)
            WantOpen(true);

        if (m_pressed)
        {
            const bool beyond = std::abs(pt.x - m_pressPt.x) > GetSystemMetrics(SM_CXDRAG) || std::abs(pt.y - m_pressPt.y) > GetSystemMetrics(SM_CYDRAG);
            // The title is the drop-down's button, and part of the header: dragged, it moves the shelf.
            if ((m_press.part == Part::Header || m_press.part == Part::DropDown) && !m_moving && beyond)
            {
                // Picked up by the header: it follows the mouse freely and pins (or not) when let go.
                const Box b = Expanded();
                m_moving = true;
                m_slideX = m_slideY = 0.f;
                m_grabX = static_cast<float>(m_pressPt.x) - b.left;
                m_grabY = static_cast<float>(m_pressPt.y) - b.top;
                m_edge = Edge::None;
            }
            if (m_moving)
            {
                m_x = static_cast<float>(pt.x) - m_grabX;
                m_y = static_cast<float>(pt.y) - m_grabY;
                if (const HMONITOR monitor = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST); monitor != m_monitor)
                {
                    const float before = m_metrics.width;
                    UseMonitor(monitor);   // a different DPI changes the size; keep the grab point inside
                    m_grabX *= m_metrics.width / before;
                    m_grabY *= m_metrics.width / before;
                    m_x = static_cast<float>(pt.x) - m_grabX;
                    m_y = static_cast<float>(pt.y) - m_grabY;
                }
                Animate();
                return;
            }
            if ((m_press.part == Part::Item || m_press.part == Part::Footer) && beyond)
            {
                if (m_dropDown)
                {
                    // A Pinned or recent item goes out alone, like any file.
                    const auto entries = Entries();
                    if (const Item* item = ItemAt(entries, m_press.index))
                        StartDragOut({ item->path });
                    return;
                }
                StartDragOut(PathsOf(m_press.part == Part::Item ? m_selection.DragSet(m_press.index) : m_selection.HandleSet()));
                return;
            }
        }
        const Hit hover = HitAt(pt);
        if (!(hover == m_hover))
        {
            m_hover = hover;
            Animate();
        }
    }

    void Shelf::OnButtonDown(POINT pt, WPARAM)
    {
        if (Folded())
        {
            WantOpen(true);   // a click on the tab opens it
            return;
        }
        m_pressed = true;
        m_press = HitAt(pt);
        m_pressPt = pt;
        SetCapture(m_hwnd.get());
    }

    void Shelf::OnButtonUp(POINT pt, WPARAM keys)
    {
        const bool pressed = m_pressed, moving = m_moving;
        m_pressed = false;
        if (moving)
            EndMove();   // before ReleaseCapture, whose WM_CAPTURECHANGED would end it too
        ReleaseCapture();
        if (!pressed || moving)
            return;
        const Hit hit = HitAt(pt);
        if (!(hit == m_press))
            return;
        switch (hit.part)
        {
        case Part::DropDown:
        case Part::Pin:
        case Part::Clear:
        case Part::Close:
            Press(hit.part);
            break;
        case Part::Remove:
        case Part::Reveal:
        case Part::Open:
        case Part::PinItem:
            RunButton(hit.part, hit.index);
            break;
        case Part::Item:
            if (!m_dropDown)
                m_selection.Click(hit.index, (keys & MK_CONTROL) != 0, (keys & MK_SHIFT) != 0);
            m_focus = hit.index;
            m_rowButton = -1;
            m_keyPart = Part::None;
            Animate();
            break;
        case Part::None:
            m_selection.Clear();
            Animate();
            break;
        default:
            break;
        }
    }

    void Shelf::OnDoubleClick(POINT pt)
    {
        const Hit hit = HitAt(pt);
        if (hit.part == Part::Item)
            RunButton(Part::Open, hit.index);
    }

    void Shelf::Open(const std::vector<std::wstring>& files)
    {
        // As the top panel's Open: the file, the first of a few, or the folder of many.
        shelf::OpenFiles(files);
    }

    void Shelf::StartDragOut(const std::vector<std::wstring>& files)
    {
        m_pressed = false;
        ReleaseCapture();
        if (files.empty())
            return;
        m_draggingOut = true;
        shelf::DragOut(m_hwnd.get(), files);   // modal until dropped or cancelled
        m_draggingOut = false;
        POINT cursor{};
        GetCursorPos(&cursor);
        m_hover = HitAt(cursor);
        if (!Shown().Contains(static_cast<float>(cursor.x), static_cast<float>(cursor.y)))
        {
            m_mouseInside = false;
            ScheduleFold(kFoldMs);
        }
        CheckFiles();
        Animate();
    }

    void Shelf::EndMove()
    {
        m_moving = false;
        const Box dropped{ m_x, m_y, m_x + m_metrics.width, m_y + m_h };
        const Edge edge = shelf::SnapEdge(dropped, m_work, m_metrics.snap);
        const Box rest = shelf::Dock(dropped, m_work, edge);
        m_edge = edge;
        m_x = rest.left;
        m_y = rest.top;
        if (m_motion)
        {
            m_slideX = dropped.left - rest.left;
            m_slideY = dropped.top - rest.top;
        }
        log::Info(L"shelf: moved, pinned to {}", Widen(shelf::EdgeName(edge)));
        RememberPlace();
        Save();
        Animate();
    }

    void Shelf::Press(Part part)
    {
        switch (part)
        {
        case Part::DropDown: ToggleDropDown(); break;
        case Part::Pin: TogglePin(); break;
        case Part::Clear: Clear(); break;
        case Part::Close: SwitchOn(false); break;   // as unticking "Show Float shelf" in the tray
        default: break;
        }
    }

    void Shelf::MoveButtonFocus(bool back)
    {
        // The stops in Tab order: the list (when there is one), then the header buttons.
        std::vector<Part> stops;
        const auto entries = Entries();
        if (std::any_of(entries.begin(), entries.end(), [](const Entry& e) { return !e.label; }))
            stops.push_back(Part::None);
        for (const Part part : kHeaderButtons)
            if (part != Part::Clear || (!m_items.empty() && !m_dropDown))
                stops.push_back(part);
        const auto at = std::find(stops.begin(), stops.end(), m_keyPart);
        const size_t i = at == stops.end() ? 0 : static_cast<size_t>(at - stops.begin());
        const size_t n = stops.size();
        m_keyPart = at == stops.end() ? stops.front() : stops[(i + (back ? n - 1 : 1)) % n];
        Animate();
    }

    void Shelf::TogglePin()
    {
        const Box before = Expanded();
        if (m_edge == Edge::None)
        {
            // To the nearest edge (a side one when it is a tie).
            m_edge = shelf::SnapEdge(before, m_work, 1e9f);
            const Box rest = shelf::Dock(before, m_work, m_edge);
            m_x = rest.left;
            m_y = rest.top;
        }
        else
        {
            // Off the edge and a little inward, floating where it is.
            float dx = 0.f, dy = 0.f;
            shelf::Outward(m_edge, dx, dy);
            m_edge = Edge::None;
            const Box rest = shelf::ClampInto(before.Offset(-dx * 24.f * m_scale, -dy * 24.f * m_scale), m_work);
            m_x = rest.left;
            m_y = rest.top;
        }
        const Box after = Expanded();
        if (m_motion)
        {
            m_slideX = before.left - after.left;
            m_slideY = before.top - after.top;
        }
        m_wantOpen = true;
        RememberPlace();
        Save();
        Animate();
    }

    void Shelf::OnContextMenu(POINT pt, bool fromKeyboard)
    {
        if (Folded())
            return;
        const auto entries = Entries();
        Hit hit = !fromKeyboard ? HitAt(pt) : ItemAt(entries, m_focus) && m_keyPart == Part::None ? Hit{ Part::Item, m_focus } : Hit{};
        if (fromKeyboard)
        {
            const auto layout = CurrentLayout();
            pt = POINT{ static_cast<LONG>(layout.title.left), static_cast<LONG>(layout.header.bottom) };
            for (const auto& r : layout.rows)
                if (hit.part == Part::Item && r.index == m_focus)
                    pt = POINT{ static_cast<LONG>(r.name.left), static_cast<LONG>(r.row.bottom) };
        }
        const bool row = hit.part == Part::Item || hit.part == Part::Reveal || hit.part == Part::Remove || hit.part == Part::Open || hit.part == Part::PinItem;
        const Item* item = row ? ItemAt(entries, hit.index) : nullptr;
        std::vector<size_t> targets;   // the shelf's items the menu is for
        if (item)
        {
            m_focus = hit.index;
            if (!m_dropDown)
            {
                if (!m_selection.IsSelected(hit.index))
                    m_selection.Click(hit.index, false, false);
                targets = m_selection.Indices();
            }
            Animate();
        }
        const auto files = !item ? std::vector<std::wstring>{} : m_dropDown ? std::vector<std::wstring>{ item->path } : PathsOf(targets);
        const List list = item ? entries[hit.index].list : List::Shelf;

        wil::unique_hmenu menu(CreatePopupMenu());
        if (!menu)
            return;
        if (item)
        {
            const bool one = files.size() == 1;
            const bool pinned = IsPinned(item->path);
            AppendMenuW(menu.get(), MF_STRING, kMenuOpen, one ? L"&Open" : std::format(L"&Open {} items", files.size()).c_str());
            AppendMenuW(menu.get(), MF_STRING, kMenuReveal, L"Show in &folder");
            AppendMenuW(menu.get(), MF_STRING, kMenuCopy, L"&Copy\tCtrl+C");
            AppendMenuW(menu.get(), MF_STRING, kMenuPinItem, pinned ? L"U&npin\tP" : L"Pi&n\tP");
            const wchar_t* remove = list == List::Shelf ? L"&Remove from the shelf\tDel" : list == List::Pinned ? L"&Remove from Pinned\tDel" : L"&Remove from Recent\tDel";
            AppendMenuW(menu.get(), MF_STRING, kMenuRemove, remove);
            AppendMenuW(menu.get(), MF_SEPARATOR, 0, nullptr);
        }
        AppendMenuW(menu.get(), MF_STRING, kMenuPin, m_edge == Edge::None ? L"&Pin to the nearest edge" : L"Un&pin from the edge");
        AppendMenuW(menu.get(), MF_STRING | (m_items.empty() ? MF_GRAYED : 0), kMenuClear, L"C&lear all");
        AppendMenuW(menu.get(), MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu.get(), MF_STRING, kMenuClose, L"Cl&ose the shelf");

        m_menu = true;
        SetForegroundWindow(m_hwnd.get());   // so the menu closes when you click elsewhere
        const UINT cmd = static_cast<UINT>(TrackPopupMenu(menu.get(), TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, pt.x, pt.y, 0, m_hwnd.get(), nullptr));
        m_menu = false;
        switch (cmd)
        {
        case kMenuOpen: Open(files); break;
        case kMenuReveal: shelf::ShowInFolder(files); break;
        case kMenuCopy: ShowNote(Copy(files)); break;
        case kMenuPinItem: RunButton(Part::PinItem, hit.index); break;
        case kMenuRemove:
            if (m_dropDown)
                RunButton(Part::Remove, hit.index);
            else
                RemoveAt(targets);
            break;
        case kMenuPin: TogglePin(); break;
        case kMenuClear: Clear(); break;
        case kMenuClose: SwitchOn(false); return;
        default: break;
        }
        POINT cursor{};
        GetCursorPos(&cursor);
        if (!Shown().Contains(static_cast<float>(cursor.x), static_cast<float>(cursor.y)))
            ScheduleFold(kFoldMs);
    }

    void Shelf::ScrollIntoView(size_t index)
    {
        const auto shape = Shape(Entries());
        const float top = shelf::RowTop(m_metrics, shape, index), h = shelf::RowHeight(m_metrics, shape, index);
        // A heading comes into view with the item under it.
        const float above = index > 0 && shape.rows[index - 1] == shelf::RowKind::Label ? m_metrics.label : 0.f;
        const float view = static_cast<float>(m_metrics.maxRows) * m_metrics.row;
        if (top - above < m_scrollTarget)
            m_scrollTarget = top - above;
        else if (top + h > m_scrollTarget + view)
            m_scrollTarget = top + h - view;
        m_scrollTarget = std::clamp(m_scrollTarget, 0.f, shelf::MaxScroll(m_metrics, shape));
    }

    void Shelf::MoveFocus(int delta, bool extend)
    {
        // Through the items, over the headings.
        const auto entries = Entries();
        std::vector<size_t> items;
        for (size_t i = 0; i < entries.size(); ++i)
            if (!entries[i].label)
                items.push_back(i);
        if (items.empty())
            return;
        auto at = std::lower_bound(items.begin(), items.end(), m_focus);
        long long k = at == items.end() ? static_cast<long long>(items.size()) - 1 : at - items.begin();
        if (at != items.end() && *at != m_focus && delta > 0)
            --k;   // the focus was on a heading: the first step lands on the item under it
        k = std::clamp(k + delta, 0ll, static_cast<long long>(items.size()) - 1);
        m_focus = items[static_cast<size_t>(k)];
        m_rowButton = -1;
        if (!m_dropDown)
            m_selection.Click(m_focus, false, extend);
        ScrollIntoView(m_focus);
        Animate();
    }

    void Shelf::OnKey(UINT vk)
    {
        WantOpen(true);
        const bool ctrl = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0;
        if (vk == VK_TAB)
        {
            m_rowButton = -1;
            MoveButtonFocus(shift);
            return;
        }
        if (m_keyPart != Part::None && (vk == VK_RETURN || vk == VK_SPACE))
        {
            Press(m_keyPart);
            return;
        }
        const auto entries = Entries();
        const Item* focused = ItemAt(entries, m_focus);
        if (!focused)
        {
            if (std::any_of(entries.begin(), entries.end(), [](const Entry& e) { return !e.label; }) && (vk == VK_UP || vk == VK_DOWN || vk == VK_HOME || vk == VK_END))
            {
                m_keyPart = Part::None;
                MoveFocus(vk == VK_UP || vk == VK_HOME ? -1 : 1, false);
                return;
            }
            if (vk == VK_ESCAPE && m_edge != Edge::None)
                WantOpen(false);
            return;
        }
        if (vk == VK_UP || vk == VK_DOWN || vk == VK_HOME || vk == VK_END || vk == VK_LEFT || vk == VK_RIGHT)
            m_keyPart = Part::None;   // back in the list
        const auto chosen = [&] { return m_dropDown ? std::vector<size_t>{} : m_selection.Count() ? m_selection.Indices() : std::vector<size_t>{ m_focus }; };
        const auto chosenFiles = [&] { return m_dropDown ? std::vector<std::wstring>{ focused->path } : PathsOf(chosen()); };
        const int buttons = static_cast<int>(std::size(shelf::kRowButtons));
        switch (vk)
        {
        case VK_UP: MoveFocus(-1, shift); break;
        case VK_DOWN: MoveFocus(1, shift); break;
        case VK_HOME: MoveFocus(-static_cast<int>(entries.size()), shift); break;
        case VK_END: MoveFocus(static_cast<int>(entries.size()), shift); break;
        case VK_LEFT:
        case VK_RIGHT:
            // Through the focused row's buttons (pin, Open, Show in folder, ×) and back to the row.
            m_rowButton = vk == VK_RIGHT ? std::min(m_rowButton + 1, buttons - 1) : std::max(m_rowButton - 1, -1);
            Animate();
            break;
        case VK_RETURN:
        case VK_SPACE:
            if (m_rowButton >= 0)
                RunButton(shelf::kRowButtons[m_rowButton], m_focus);
            else if (vk == VK_RETURN)
                Open(chosenFiles());
            break;
        case VK_DELETE:
            if (m_dropDown)
                RunButton(Part::Remove, m_focus);
            else
                RemoveAt(chosen());
            break;
        case 'P':
            if (!ctrl)
                RunButton(Part::PinItem, m_focus);
            break;
        case VK_ESCAPE:
            m_selection.Clear();
            m_rowButton = -1;
            if (m_edge != Edge::None)
                WantOpen(false);
            Animate();
            break;
        case 'A':
            if (ctrl && !m_dropDown)
            {
                m_selection.SelectAll();
                Animate();
            }
            break;
        case 'C':
            if (ctrl)
                ShowNote(Copy(m_dropDown ? chosenFiles() : PathsOf(m_selection.HandleSet())));
            break;
        default:
            break;
        }
    }

    void Shelf::OnWheel(int delta)
    {
        const float max = shelf::MaxScroll(m_metrics, Shape(Entries()));
        if (max <= 0.f)
            return;
        m_scrollTarget = std::clamp(m_scrollTarget - static_cast<float>(delta) / WHEEL_DELTA * m_metrics.row, 0.f, max);
        m_hover = {};
        Animate();
    }

    LRESULT CALLBACK Shelf::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        if (msg == WM_NCCREATE)
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams));
        auto* self = reinterpret_cast<Shelf*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        // m_hwnd is still empty during CreateWindowExW; nothing here needs it before then.
        if (!self || !self->m_hwnd)
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        return self->Handle(msg, wParam, lParam);
    }

    LRESULT Shelf::Handle(UINT msg, WPARAM wParam, LPARAM lParam)
    {
        const HWND hwnd = m_hwnd.get();
        switch (msg)
        {
        case WM_NCHITTEST:
            return HTCLIENT;   // alpha-0 pixels are already excluded by the layered-window hit test
        case WM_MOUSEACTIVATE:
            // Opening the folded tab shouldn't take the keyboard from the app you are in.
            return Folded() ? MA_NOACTIVATE : MA_ACTIVATE;
        case WM_ACTIVATE:
            m_active = LOWORD(wParam) != WA_INACTIVE;
            Animate();
            return 0;
        case WM_MOUSEMOVE:
            OnMouseMove(ToScreen(lParam));
            return 0;
        case WM_MOUSELEAVE:
            m_tracking = false;
            m_mouseInside = false;
            m_hover = {};
            if (!m_pressed && !m_menu && !m_dropHover)
                ScheduleFold(kFoldMs);
            Animate();
            return 0;
        case WM_LBUTTONDOWN:
            OnButtonDown(ToScreen(lParam), wParam);
            return 0;
        case WM_LBUTTONUP:
            OnButtonUp(ToScreen(lParam), wParam);
            return 0;
        case WM_LBUTTONDBLCLK:
            OnDoubleClick(ToScreen(lParam));
            return 0;
        case WM_CAPTURECHANGED:
            if (m_moving)
                EndMove();
            m_pressed = false;
            return 0;
        case WM_CONTEXTMENU:
            // lParam is -1 for Shift+F10 or the menu key.
            OnContextMenu(POINT{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) }, lParam == -1);
            return 0;
        case WM_MOUSEWHEEL:
            OnWheel(GET_WHEEL_DELTA_WPARAM(wParam));
            return 0;
        case WM_KEYDOWN:
            OnKey(static_cast<UINT>(wParam));
            return 0;
        case WM_CLOSE:
            SwitchOn(false);   // Alt+F4 closes it like its × (DefWindowProc would destroy the window)
            return 0;
        case WM_ENDSESSION:
            if (wParam)
                Save();   // signing out or shutting down: the process may end without another message
            return 0;
        case WM_NOTIFY:
        {
            const auto* header = reinterpret_cast<const NMHDR*>(lParam);
            if (header->hwndFrom != m_tips || header->code != TTN_GETDISPINFOW)
                return DefWindowProcW(hwnd, msg, wParam, lParam);
            auto* info = reinterpret_cast<NMTTDISPINFOW*>(lParam);
            if (header->idFrom >= kRowTipBase)
            {
                // The hovered row's buttons.
                const auto entries = Entries();
                const Item* item = ItemAt(entries, m_hover.index);
                const size_t k = header->idFrom - kRowTipBase;
                const List list = item ? entries[m_hover.index].list : List::Shelf;
                const wchar_t* tips[] = { item && IsPinned(item->path) ? L"Unpin (P)" : L"Pin, to keep it in Pinned (P)", L"Open (Enter)", L"Show in folder",
                                          list == List::Shelf    ? L"Remove from the shelf (the file stays where it is)"
                                          : list == List::Pinned ? L"Remove from Pinned (the file stays where it is)"
                                                                 : L"Remove from Recent (the file and its History entry stay)" };
                info->lpszText = const_cast<wchar_t*>(k < std::size(tips) ? tips[k] : L"");
                return 0;
            }
            switch (static_cast<Part>(header->idFrom))
            {
            case Part::DropDown:
                info->lpszText = const_cast<wchar_t*>(m_dropDown ? L"Back to the shelf" : L"Pinned items and recent conversions");
                break;
            case Part::Pin: info->lpszText = const_cast<wchar_t*>(m_edge == Edge::None ? L"Pin to the nearest edge" : L"Unpin"); break;
            case Part::Clear: info->lpszText = const_cast<wchar_t*>(L"Take everything off the shelf (the files stay where they are)"); break;
            case Part::Close: info->lpszText = const_cast<wchar_t*>(L"Close the shelf (the tray icon brings it back)"); break;
            default: break;
            }
            return 0;
        }
        case WM_SETCURSOR:
        {
            POINT cursor{};
            GetCursorPos(&cursor);
            const Hit hit = HitAt(cursor);
            const auto* id = hit.part == Part::Header ? IDC_SIZEALL
                             : hit.part == Part::Pin || hit.part == Part::Clear || hit.part == Part::Close || hit.part == Part::Reveal ||
                                       hit.part == Part::Remove || hit.part == Part::Open || hit.part == Part::PinItem || hit.part == Part::DropDown
                                   ? IDC_HAND
                                   : IDC_ARROW;
            SetCursor(LoadCursorW(nullptr, id));
            return TRUE;
        }
        case WM_TIMER:
            if (wParam == kTimerFrame)
                OnFrame();
            else if (wParam == kTimerCheck)
                CheckFiles();
            else if (wParam == kTimerFold)
            {
                KillTimer(hwnd, kTimerFold);
                POINT cursor{};
                GetCursorPos(&cursor);
                const bool over = Shown().Contains(static_cast<float>(cursor.x), static_cast<float>(cursor.y)) && m_mouseInside;
                if (!m_dropHover && !m_pressed && !m_menu && !over)
                    WantOpen(false);
            }
            return 0;
        case WM_DISPLAYCHANGE:
        case WM_SETTINGCHANGE:
            // Resolution, taskbar, DPI or light/dark theme changed. Placed again from where the user
            // left it, so a monitor that went away for a moment (sleep, a cable) gets it back.
            if ((msg == WM_DISPLAYCHANGE || wParam == SPI_SETWORKAREA) && m_place && !m_moving)
                m_placed = false;   // hidden, it is placed when shown
            if (m_visible)
            {
                Refresh();
                Animate();
            }
            return 0;
        case kMsgThumb:
            OnThumb(std::unique_ptr<ThumbResult>(reinterpret_cast<ThumbResult*>(lParam)));
            return 0;
        case kMsgCheck:
            OnCheck(std::unique_ptr<CheckResult>(reinterpret_cast<CheckResult*>(lParam)));
            return 0;
        case kMsgKeep:
            OnKeep(std::unique_ptr<KeepJob>(reinterpret_cast<KeepJob*>(lParam)));
            return 0;
        case kMsgRecent:
            OnRecent(std::unique_ptr<RecentResult>(reinterpret_cast<RecentResult*>(lParam)));
            return 0;
        case kMsgClipboard:
        {
            std::unique_ptr<ClipboardJob> job(reinterpret_cast<ClipboardJob*>(lParam));
            // Only if nothing else was copied meanwhile; then the files go back on with the picture.
            if (job->ok && GetClipboardSequenceNumber() == job->sequence)
            {
                const bool ok = shelf::PutOnClipboard(hwnd, job->files, &job->image);
                log::Info(L"shelf: picture {}x{} added to the clipboard: {}", job->image.width, job->image.height, ok);
            }
            return 0;
        }
        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        }
    }

    // ---- Drawing ---------------------------------------------------------------------------------

    bool Shelf::EnsureTextFormats()
    {
        if (m_titleFont && m_fontScale == m_scale)
            return true;
        m_fontScale = m_scale;
        constexpr auto L = DWRITE_TEXT_ALIGNMENT_LEADING, C = DWRITE_TEXT_ALIGNMENT_CENTER;
        m_titleFont = MakeFormat(m_textFamily, 13.f, DWRITE_FONT_WEIGHT_SEMI_BOLD, L);
        m_nameFont = MakeFormat(m_textFamily, 12.5f, DWRITE_FONT_WEIGHT_NORMAL, L);
        m_detailFont = MakeFormat(m_textFamily, 11.f, DWRITE_FONT_WEIGHT_NORMAL, L);
        m_iconFont = MakeFormat(m_iconFamily, 13.f, DWRITE_FONT_WEIGHT_NORMAL, C);
        m_countFont = MakeFormat(m_textFamily, 12.f, DWRITE_FONT_WEIGHT_NORMAL, L);
        m_footerFont = MakeFormat(m_textFamily, 12.f, DWRITE_FONT_WEIGHT_SEMI_BOLD, L);
        m_badgeFont = MakeFormat(m_textFamily, 11.f, DWRITE_FONT_WEIGHT_BOLD, C);
        // The empty shelf's line, centred and wrapping (the one-line formats end in "...").
        m_hintFont = MakeFormat(m_textFamily, 13.f, DWRITE_FONT_WEIGHT_NORMAL, C);
        if (m_hintFont)
        {
            m_hintFont->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
            m_hintFont->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
            const DWRITE_TRIMMING none{ DWRITE_TRIMMING_GRANULARITY_NONE, 0, 0 };
            m_hintFont->SetTrimming(&none, nullptr);
        }
        return m_titleFont && m_nameFont && m_detailFont && m_iconFont && m_countFont && m_footerFont && m_badgeFont && m_hintFont;
    }

    void Shelf::Text(const std::wstring& text, IDWriteTextFormat* format, const Box& box, const D2D1_COLOR_F& color, float alpha)
    {
        if (box.Width() > 0.f)
            Text(text, format, D2D1::RectF(box.left, box.top, box.right, box.bottom), color, alpha);
    }

    void Shelf::Glyph(wchar_t glyph, IDWriteTextFormat* format, const Box& box, const D2D1_COLOR_F& color, float alpha)
    {
        const wchar_t text[2]{ glyph, 0 };
        Text(text, format, box, color, alpha);
    }

    ControlKit::ControlState Shelf::StateOf(Part part, bool focused) const
    {
        const bool hot = m_hover.part == part;
        return ControlKit::ControlState{ true, hot, hot && m_pressed && m_press.part == part, focused };
    }

    void Shelf::IconButton(const Box& box, wchar_t glyph, ControlKit::ControlState state)
    {
        // The kit draws in DIPs: scaled about the shelf's screen-pixel transform for the call.
        D2D1::Matrix3x2F base;
        m_rt->GetTransform(&base);
        m_rt->SetTransform(D2D1::Matrix3x2F::Scale(m_scale, m_scale) * base);
        m_kit.DrawIconButton(D2D1::RectF(box.left / m_scale, box.top / m_scale, box.right / m_scale, box.bottom / m_scale), glyph, false, state);
        m_rt->SetTransform(base);
    }

    void Shelf::PushButton(const Box& box, const std::wstring& text, ControlKit::ControlState state)
    {
        D2D1::Matrix3x2F base;
        m_rt->GetTransform(&base);
        m_rt->SetTransform(D2D1::Matrix3x2F::Scale(m_scale, m_scale) * base);
        m_kit.DrawButton(D2D1::RectF(box.left / m_scale, box.top / m_scale, box.right / m_scale, box.bottom / m_scale), text, 0, false, state);
        m_rt->SetTransform(base);
    }

    void Shelf::DrawThumb(Item& item, const Box& box, float alpha)
    {
        const auto colors = ColorsFor(m_palette, m_appearance.style);
        const float radius = 6.f * m_scale;
        if (!item.thumb.px.empty() && (!item.bitmap || item.bitmapGeneration != m_canvas->DeviceGeneration()))
        {
            item.bitmap = ToBitmap(m_rt, item.thumb);
            item.bitmapGeneration = m_canvas->DeviceGeneration();
        }
        if (!item.bitmap)
        {
            // No picture of its own: a folder's icon, or the brand's file mark.
            if (item.folder)
            {
                m_rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(box.left, box.top, box.right, box.bottom), radius, radius),
                                           Brush(colors.button, alpha).Get());
                Glyph(kGlyphFolder, m_iconFont.Get(), box, colors.dim, alpha);
            }
            else
                DrawFileMark(D2D1::RectF(box.left, box.top, box.right, box.bottom), m_palette, false, alpha);
            return;
        }
        // Fit inside the square, keeping the picture's shape; a bitmap brush gives rounded corners
        // without a clip layer per row.
        const float w = static_cast<float>(item.thumb.w), h = static_cast<float>(item.thumb.h);
        const float k = std::min(box.Width() / w, box.Height() / h);
        const float dw = w * k, dh = h * k;
        const float x = box.left + (box.Width() - dw) / 2.f, y = box.top + (box.Height() - dh) / 2.f;
        ComPtr<ID2D1BitmapBrush> brush;
        m_rt->CreateBitmapBrush(item.bitmap.Get(),
                                D2D1::BitmapBrushProperties(D2D1_EXTEND_MODE_CLAMP, D2D1_EXTEND_MODE_CLAMP, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR),
                                D2D1::BrushProperties(alpha), &brush);
        if (!brush)
            return;
        brush->SetTransform(D2D1::Matrix3x2F::Scale(k, k) * D2D1::Matrix3x2F::Translation(x, y));
        const float r = std::min(radius, std::min(dw, dh) / 2.f);
        m_rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x, y, x + dw, y + dh), r, r), brush.Get());
    }

    void Shelf::DrawPanel(const shelf::Layout& l, float alpha)
    {
        const auto c = ColorsFor(m_palette, m_appearance.style);
        const float s = m_scale;
        const size_t n = m_items.size();

        // Header: the Float icon, the title as the drop-down's button ("Float shelf" or "Pinned and
        // recent", with a chevron) and the count ("3 files"), then Pin, Clear all and Close in the
        // control kit's look (the keyboard's button with the kit's focus ring).
        const auto focused = [&](Part part) { return m_active && m_keyPart == part; };
        const float cy = (l.header.top + l.header.bottom) / 2.f;
        const float ix = l.title.left - 4.f * s;
        if (l.dropDown.Width() > 0.f)
        {
            // The kit's button look without its glyph: a hover fill and the focus ring around the title.
            const auto state = StateOf(Part::DropDown, focused(Part::DropDown));
            if (state.hot || state.focused || m_dropDown)
                IconButton(l.dropDown, 0, state);
        }
        Glyph(kGlyphPin, m_iconFont.Get(), Box{ ix, cy - 9.f * s, ix + 16.f * s, cy + 9.f * s }, c.text, alpha);
        const Box title{ ix + 24.f * s, l.title.top, l.title.right, l.title.bottom };
        const std::wstring name = m_dropDown ? kTitleDropDown : kTitleShelf;
        const float tw = TextWidth(name, m_titleFont.Get());
        Text(name, m_titleFont.Get(), title, c.text, alpha);
        const float chevron = title.left + tw + 4.f * s;
        Glyph(m_dropDown ? kGlyphUp : kGlyphDown, m_iconFont.Get(),
              Box{ chevron, cy - 7.f * s, chevron + 12.f * s, cy + 7.f * s }, c.dim, alpha);
        const size_t folders = static_cast<size_t>(std::count_if(m_items.begin(), m_items.end(), [](const Item& i) { return i.folder; }));
        const float countLeft = std::max(l.dropDown.right, title.left + tw) + 6.f * s;
        const Box count{ countLeft, title.top, title.right, title.bottom };
        if (count.Width() > 20.f * s)
            Text(m_dropDown ? std::format(L"{} pinned", m_pinned.size()) : shelf::CountText(n - folders, folders), m_countFont.Get(), count, c.dim, alpha);
        IconButton(l.pin, m_edge == Edge::None ? kGlyphPin : kGlyphUnpin, StateOf(Part::Pin, focused(Part::Pin)));
        if (l.clear.Width() > 0.f)
            PushButton(l.clear, L"Clear all", StateOf(Part::Clear, focused(Part::Clear)));
        IconButton(l.close, kGlyphClose, StateOf(Part::Close, focused(Part::Close)));
        m_rt->DrawLine(D2D1::Point2F(l.panel.left + m_metrics.inset, l.header.bottom - 0.5f), D2D1::Point2F(l.panel.right - m_metrics.inset, l.header.bottom - 0.5f),
                       Brush(c.line, alpha).Get(), 1.f);

        // Rows, clipped to the list so scrolled rows slide under the header and footer.
        m_rt->PushAxisAlignedClip(D2D1::RectF(l.list.left, l.list.top, l.list.right, l.list.bottom), D2D1_ANTIALIAS_MODE_ALIASED);
        const float rr = 6.f * s;
        const auto entries = Entries();
        for (const auto& r : l.rows)
        {
            if (r.index >= entries.size())
                continue;
            const Entry& e = entries[r.index];
            if (e.label)
            {
                // "Pinned", "Recent conversions", and what an empty one is for.
                const bool none = ItemsOf(e.list).empty();
                const std::wstring heading = e.list == List::Pinned ? (none ? L"Pinned · pin an item to keep it here" : L"Pinned")
                                                                    : (none ? L"Recent conversions · none yet" : L"Recent conversions");
                Text(heading, m_footerFont.Get(), r.name, c.dim, alpha);
                continue;
            }
            auto& item = ItemsOf(e.list)[e.index];
            const bool keyRow = m_active && m_keyPart == Part::None && r.index == m_focus;
            const auto onRow = [](Part p) { return p == Part::Item || p == Part::Reveal || p == Part::Remove || p == Part::Open || p == Part::PinItem; };
            const bool hovered = m_hover.index == r.index && onRow(m_hover.part);
            const bool selected = !m_dropDown && m_selection.IsSelected(r.index);
            const auto rowRect = D2D1::RoundedRect(D2D1::RectF(r.row.left, r.row.top + 1.f * s, r.row.right, r.row.bottom - 1.f * s), rr, rr);
            if (selected)
            {
                // The kit's selection: a light accent wash and a short accent bar on the left.
                m_rt->FillRoundedRectangle(rowRect, Brush(c.accent, 0.12f * alpha).Get());
                const float my = (r.row.top + r.row.bottom) / 2.f;
                m_rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.row.left, my - 10.f * s, r.row.left + 3.f * s, my + 10.f * s), 1.5f * s, 1.5f * s),
                                           Brush(c.accent, alpha).Get());
            }
            else if (hovered)
                m_rt->FillRoundedRectangle(rowRect, Brush(c.hover, alpha).Get());
            if (keyRow && !selected && m_rowButton < 0)
                m_rt->DrawRoundedRectangle(rowRect, Brush(c.dim, 0.6f * alpha).Get(), 1.f);
            DrawThumb(item, r.thumb, alpha);
            // The buttons show on the hovered row and the keyboard's; elsewhere the name takes their room.
            const bool buttons = hovered || keyRow;
            const bool pinned = IsPinned(item.path);
            const float textRight = buttons ? r.name.right : pinned ? r.pin.left - 2.f * s : r.remove.right;
            Text(item.name, m_nameFont.Get(), Box{ r.name.left, r.name.top, textRight, r.name.bottom }, c.text, alpha);
            Text(item.detail.empty() ? std::wstring(L"…") : item.detail, m_detailFont.Get(), Box{ r.detail.left, r.detail.top, textRight, r.detail.bottom }, c.dim, alpha);
            if (buttons)
            {
                for (size_t k = 0; k < std::size(shelf::kRowButtons); ++k)
                {
                    const Part part = shelf::kRowButtons[k];
                    const wchar_t glyph = part == Part::PinItem ? (pinned ? kGlyphUnpin : kGlyphPin) : part == Part::Open ? kGlyphOpenFile
                                          : part == Part::Reveal                ? kGlyphReveal
                                                                                : kGlyphRemove;
                    auto state = StateOf(part, keyRow && m_rowButton == static_cast<int>(k));
                    state.hot = state.hot && m_hover.index == r.index;
                    IconButton(shelf::ButtonBox(r, part), glyph, state);
                }
            }
            else if (pinned)
                Glyph(kGlyphPin, m_iconFont.Get(), r.pin, c.accent, alpha);   // pinned: its pin, in the accent
        }
        // Scroll position, when there is more than fits.
        const auto shape = Shape(entries);
        if (const float max = shelf::MaxScroll(m_metrics, shape); max > 0.f)
        {
            const float track = l.list.Height() - 8.f * s;
            const float thumbLen = std::max(track * l.list.Height() / std::max(1.f, shelf::ContentHeight(m_metrics, shape)), 18.f * s);
            const float y = l.list.top + 4.f * s + (track - thumbLen) * (m_scroll / max);
            const float x = l.panel.right - 4.f * s;
            m_rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x - 1.5f * s, y, x + 1.5f * s, y + thumbLen), 1.5f * s, 1.5f * s),
                                       Brush(c.dim, 0.5f * alpha).Get());
        }
        m_rt->PopAxisAlignedClip();

        if (l.drop.Width() > 0.f)
        {
            DrawDropZone(l.drop, alpha);   // no footer: nothing to drag out yet
            return;
        }
        if (l.footer.Width() <= 0.f)
            return;   // the drop-down's lists: their items are dragged one by one

        // The handle at the bottom: drag it to carry all (or the selected) items out at once.
        const bool footerHover = m_hover.part == Part::Footer;
        const auto& f = l.footer;
        const float fr = f.Height() / 2.f;
        m_rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(f.left, f.top, f.right, f.bottom), fr, fr),
                                   Brush(c.accent, (footerHover ? 0.30f : 0.16f) * alpha).Get());
        // A little fanned stack of the first thumbnails.
        const float t = f.Height() - 12.f * s;
        const float fy = (f.top + f.bottom) / 2.f;
        const size_t stack = std::min<size_t>(n, 3);
        const auto base = D2D1::Matrix3x2F::Translation(-static_cast<float>(m_origin.x), -static_cast<float>(m_origin.y));
        for (size_t k = stack; k-- > 0;)
        {
            const float angle = (static_cast<float>(k) - static_cast<float>(stack - 1) / 2.f) * 9.f;
            const float x = f.left + 10.f * s + static_cast<float>(k) * 4.f * s;
            const Box tb{ x, fy - t / 2.f, x + t, fy + t / 2.f };
            m_rt->SetTransform(D2D1::Matrix3x2F::Rotation(angle, D2D1::Point2F(tb.left + t / 2.f, tb.top + t / 2.f)) * base);
            m_rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(tb.left - 1.f, tb.top - 1.f, tb.right + 1.f, tb.bottom + 1.f), 5.f * s, 5.f * s),
                                       Brush(c.surface, alpha).Get());
            DrawThumb(m_items[k], tb, alpha);
        }
        m_rt->SetTransform(base);
        const size_t selected = m_selection.Count();
        const auto label = !m_note.empty() ? m_note
                           : selected > 0  ? std::format(L"Drag {} selected into an app", selected)
                           : n == 1        ? std::wstring(L"Drag it into an app")
                                           : std::format(L"Drag all {} into an app", n);
        const float lx = f.left + 10.f * s + t + static_cast<float>(stack > 0 ? stack - 1 : 0) * 4.f * s + 12.f * s;
        Text(label, m_footerFont.Get(), Box{ lx, f.top, f.right - 12.f * s, f.bottom }, c.text, alpha);
    }

    void Shelf::DrawDropZone(const Box& box, float alpha)
    {
        // An empty shelf: a dashed outline saying where files go, filled while a drag is over it.
        const auto c = ColorsFor(m_palette, m_appearance.style);
        const float s = m_scale;
        if (!m_dashes)
        {
            const float dashes[] = { 4.f, 3.f };   // in stroke widths, so they scale with it
            m_d2d->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT,
                                                                 D2D1_LINE_JOIN_ROUND, 10.f, D2D1_DASH_STYLE_CUSTOM),
                                     dashes, 2, &m_dashes);
        }
        const float w = std::max(1.f, std::round(1.5f * s));
        const float r = 6.f * s;
        const auto outline = D2D1::RoundedRect(D2D1::RectF(box.left + w / 2.f, box.top + w / 2.f, box.right - w / 2.f, box.bottom - w / 2.f), r, r);
        if (m_dropHover)
            m_rt->FillRoundedRectangle(outline, Brush(c.accent, 0.12f * alpha).Get());
        m_rt->DrawRoundedRectangle(outline, Brush(m_dropHover ? c.accent : m_palette.pageLine, alpha).Get(), w, m_dashes.Get());

        // A blank file over the line, centred together; a note ("could not be saved") takes the line's place.
        const std::wstring line = m_note.empty() ? std::wstring(L"Drag files here to keep them for later.") : m_note;
        const float fw = 38.f * s, fh = 47.f * s, gap = 12.f * s, lh = 36.f * s;
        const float top = std::round((box.top + box.bottom - fh - gap - lh) / 2.f);
        const float cx = (box.left + box.right) / 2.f;
        DrawFileMark(D2D1::RectF(cx - fw / 2.f, top, cx + fw / 2.f, top + fh), m_palette, m_dropHover, 0.75f * alpha);
        const float tw = std::min(180.f * s, box.Width() - 20.f * s);
        Text(line, m_hintFont.Get(), Box{ cx - tw / 2.f, top + fh + gap, cx + tw / 2.f, top + fh + gap + lh }, m_dropHover ? c.text : c.dim, alpha);
    }

    void Shelf::DrawTab(const Box& tab, float alpha)
    {
        if (alpha <= 0.f)
            return;
        const auto c = ColorsFor(m_palette, m_appearance.style);
        const float s = m_scale;
        if (m_items.empty())
        {
            Glyph(kGlyphPin, m_iconFont.Get(), tab, c.dim, alpha);   // the Float icon: an empty shelf, here to drop on
            return;
        }
        const bool vertical = m_edge == Edge::Left || m_edge == Edge::Right;
        const float cx = (tab.left + tab.right) / 2.f, cy = (tab.top + tab.bottom) / 2.f;
        const float t = 18.f * s, gap = 14.f * s;
        // The newest item's thumbnail and the count on an accent badge.
        const Box thumb = vertical ? Box{ cx - t / 2.f, cy - gap - t / 2.f, cx + t / 2.f, cy - gap + t / 2.f }
                                   : Box{ cx - gap - t / 2.f, cy - t / 2.f, cx - gap + t / 2.f, cy + t / 2.f };
        DrawThumb(m_items.front(), thumb, alpha);
        const auto number = std::to_wstring(m_items.size());
        const float bw = std::max(18.f * s, TextWidth(number, m_badgeFont.Get()) + 8.f * s), bh = 18.f * s;
        const float bx = vertical ? cx : cx + gap, by = vertical ? cy + gap : cy;
        const Box badge{ bx - bw / 2.f, by - bh / 2.f, bx + bw / 2.f, by + bh / 2.f };
        m_rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(badge.left, badge.top, badge.right, badge.bottom), bh / 2.f, bh / 2.f),
                                   Brush(m_palette.accent, alpha).Get());
        Text(number, m_badgeFont.Get(), badge, m_palette.textOnAccent, alpha);
    }

    void Shelf::Render()
    {
        if (!m_hwnd || !m_canvas || !m_visible || !EnsureTextFormats())
            return;
        const float s = m_scale;
        const Box shown = Shown();
        const float pad = std::round(kPadDip * s);
        Box area = shown.Inflated(pad);
        if (!m_moving)
            area = shelf::Intersect(area, m_screen);   // never spill onto the next monitor
        m_origin = POINT{ static_cast<LONG>(std::floor(area.left)), static_cast<LONG>(std::floor(area.top)) };
        m_size = SIZE{ static_cast<LONG>(std::ceil(area.right)) - m_origin.x, static_cast<LONG>(std::ceil(area.bottom)) - m_origin.y };
        m_rt = m_canvas->Begin(m_size);
        if (!m_rt)
            return;
        m_kit.SetTarget(nullptr);   // brushes made again: the target may be new
        m_kit.SetTarget(m_rt);
        // Draw in screen pixels; the transform moves them into the window.
        const auto base = D2D1::Matrix3x2F::Translation(-static_cast<float>(m_origin.x), -static_cast<float>(m_origin.y));
        m_rt->SetTransform(base);
        const auto c = ColorsFor(m_palette, m_appearance.style);
        const bool pinned = m_edge != Edge::None && !m_moving;

        // Pinned, the outer side runs past the edge and is clipped there, so it sits flush with
        // square corners like part of the screen's frame.
        if (pinned)
            m_rt->PushAxisAlignedClip(D2D1::RectF(m_work.left, m_work.top, m_work.right, m_work.bottom), D2D1_ANTIALIAS_MODE_ALIASED);
        const float radius = m_edge == Edge::None ? m_metrics.radius : m_metrics.tabThickness / 2.f + (m_metrics.radius - m_metrics.tabThickness / 2.f) * m_open;
        Box body = shown;
        if (pinned)
        {
            float dx = 0.f, dy = 0.f;
            shelf::Outward(m_edge, dx, dy);
            const float e = radius + 2.f;
            body = Box{ body.left + std::min(0.f, dx) * e, body.top + std::min(0.f, dy) * e, body.right + std::max(0.f, dx) * e,
                        body.bottom + std::max(0.f, dy) * e };
        }
        const auto rounded = D2D1::RoundedRect(D2D1::RectF(body.left, body.top, body.right, body.bottom), radius, radius);

        // Soft shadow: a few growing, fading copies (as the top panel).
        for (int k = 6; k >= 1; --k)
        {
            const float g = 2.6f * s * static_cast<float>(k);
            m_rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(body.left - g, body.top - g + 4.f * s, body.right + g, body.bottom + g + 4.f * s), radius + g, radius + g),
                                       Brush(c.shadow, 0.05f).Get());
        }
        m_rt->FillRoundedRectangle(rounded, Brush(c.surface).Get());
        m_rt->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(body.left + 0.5f, body.top + 0.5f, body.right - 0.5f, body.bottom - 0.5f), radius - 0.5f, radius - 0.5f),
                                   Brush(m_dropHover ? c.accent : c.line).Get(), m_dropHover ? 2.f * s : 1.f);

        // Content fades in once the panel has mostly unfolded; the tab's fades out first.
        const float contentAlpha = EaseOutCubic(std::clamp((m_open - 0.55f) / 0.45f, 0.f, 1.f));
        const float tabAlpha = std::clamp(1.f - m_open * 2.5f, 0.f, 1.f);
        const auto layout = CurrentLayout();
        PlaceTips(contentAlpha > 0.f && !Folded() && !m_hiding ? &layout : nullptr);
        if (contentAlpha > 0.f)
        {
            ComPtr<ID2D1RoundedRectangleGeometry> clip;
            ComPtr<ID2D1Layer> layer;
            const bool clipped = SUCCEEDED(m_d2d->CreateRoundedRectangleGeometry(rounded, &clip)) && SUCCEEDED(m_rt->CreateLayer(&layer));
            // The mask is in the same (screen) coordinates as the drawing; the world transform applies to both.
            // The layer fades the content in as a whole, the kit's buttons too.
            if (clipped)
                m_rt->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), clip.Get(), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                                      D2D1::IdentityMatrix(), contentAlpha),
                                layer.Get());
            DrawPanel(layout, clipped ? 1.f : contentAlpha);
            if (m_dropHover)
                m_rt->FillRoundedRectangle(rounded, Brush(c.accent, 0.08f).Get());
            if (clipped)
                m_rt->PopLayer();
        }
        if (m_edge != Edge::None)
            DrawTab(shown, tabAlpha);
        if (pinned)
            m_rt->PopAxisAlignedClip();

        m_canvas->End(m_hwnd.get(), m_origin, EaseOutCubic(m_presence));
        m_kit.SetTarget(nullptr);
        m_rt = nullptr;
    }

    void Shelf::PlaceTips(const shelf::Layout* layout)
    {
        if (!m_tips)
            return;
        const auto place = [&](UINT_PTR id, const Box* b) {
            TTTOOLINFOW tool{ sizeof(tool) };
            tool.hwnd = m_hwnd.get();
            tool.uId = id;
            // Window (client) pixels: the layout is in screen pixels and the window starts at m_origin.
            if (b && b->Width() > 0.f)
                tool.rect = RECT{ static_cast<LONG>(b->left) - m_origin.x, static_cast<LONG>(b->top) - m_origin.y,
                                  static_cast<LONG>(std::ceil(b->right)) - m_origin.x, static_cast<LONG>(std::ceil(b->bottom)) - m_origin.y };
            SendMessageW(m_tips, TTM_NEWTOOLRECTW, 0, reinterpret_cast<LPARAM>(&tool));
        };
        for (const Part part : kHeaderButtons)
        {
            const Box* b = !layout ? nullptr
                           : part == Part::DropDown ? &layout->dropDown
                           : part == Part::Pin      ? &layout->pin
                           : part == Part::Clear    ? &layout->clear
                                                    : &layout->close;
            place(static_cast<UINT_PTR>(part), b);
        }
        // The hovered row's buttons.
        const shelf::RowBox* row = nullptr;
        if (layout)
            for (const auto& r : layout->rows)
                if (r.kind == shelf::RowKind::Item && r.index == m_hover.index &&
                    (m_hover.part == Part::Item || m_hover.part == Part::Reveal || m_hover.part == Part::Remove || m_hover.part == Part::Open ||
                     m_hover.part == Part::PinItem))
                    row = &r;
        for (size_t k = 0; k < std::size(shelf::kRowButtons); ++k)
            place(kRowTipBase + k, row ? &shelf::ButtonBox(*row, shelf::kRowButtons[k]) : nullptr);
    }
}
