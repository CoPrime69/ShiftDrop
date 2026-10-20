// ShiftDrop: drag a file, hold a wheel key (Shift, Ctrl or Alt by default), drop it on a wheel slice.
//
// Threads:
//   UI thread    hidden main window, tray icon, overlay, WH_MOUSE_LL hook, OLE drop target, history
//                window, clipboard file actions (STA)
//   workers      JobQueue threads (MTA) that run recipes and PostMessage results back
//   shell worker one STA thread for IFileOperation work (Move to…)
#include "Brand.h"
#include "DropTarget.h"
#include "FileActions.h"
#include "FileMove.h"
#include "HistoryStore.h"
#include "HistoryWindow.h"
#include "ImagePipeline.h"
#include "JobQueue.h"
#include "Log.h"
#include "Messages.h"
#include "MouseHook.h"
#include "OutputPath.h"
#include "Island.h"
#include "Overlay.h"
#include "PipelineRunner.h"
#include "RecipeStore.h"
#include "Settings.h"
#include "Startup.h"
#include "Tray.h"
#include "Utf.h"
#include "resource.h"

#include <windows.h>
#include <commctrl.h>
#include <ole2.h>
#include <shlobj.h>
#include <wil/resource.h>

#include <algorithm>
#include <atomic>
#include <deque>
#include <filesystem>
#include <format>
#include <memory>
#include <mutex>

namespace sd
{
    namespace
    {
        constexpr wchar_t kMainClass[] = L"" SD_NAME L".Main";
        constexpr wchar_t kMutexName[] = L"Local\\" SD_NAME L".SingleInstance.5C1F0B7E";   // Local\ = per logon session
        constexpr UINT_PTR kTimerPoll = 1;           // modifier pressed while the mouse is not moving
        constexpr UINT_PTR kTimerHideFallback = 2;   // button released but OLE never sent Drop/DragLeave
        constexpr UINT_PTR kTimerWatchdog = 3;       // hook silently removed by Windows
        constexpr size_t kMaxSlices = 8;             // more than this and the labels stop fitting

        // What a finished batch is called in its notification.
        enum class BatchKind { Convert, Tools, Duplicate, Move };

        struct FileOutcome
        {
            std::wstring source;
            ItemResult result;
        };

        struct BatchResult
        {
            BatchKind kind = BatchKind::Convert;
            std::wstring label;    // recipe label, or the destination folder's name for a move
            int64_t runId = 0;     // history run (0 when history is unavailable)
            uint64_t token = 0;    // which batch, so its progress and result reach the top panel
            std::vector<FileOutcome> files;
        };

        struct BatchState
        {
            HWND notify = nullptr;
            Recipe recipe;   // a copy: workers never touch the RecipeStore
            std::mutex mutex;
            BatchResult result;
            std::atomic<size_t> remaining{ 0 };
        };

        struct PendingDrop
        {
            std::vector<std::wstring> files;
            std::string recipeId;
        };

        // "1 file", "3 files".
        std::wstring Count(size_t n, const wchar_t* noun)
        {
            return n == 1 ? std::format(L"1 {}", noun) : std::format(L"{} {}s", n, noun);
        }

        std::wstring FileName(const std::wstring& path)
        {
            return std::filesystem::path(path).filename().wstring();
        }

        bool IsElevated()
        {
            wil::unique_handle token;
            if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
                return false;
            TOKEN_ELEVATION elevation{};
            DWORD size = 0;
            return GetTokenInformation(token.get(), TokenElevation, &elevation, sizeof(elevation), &size) && elevation.TokenIsElevated;
        }

        // Ownership of the batch travels in the LPARAM. If the window is gone (shutting down) the post
        // fails and we take the pointer back so it is freed here.
        void PostBatch(HWND notify, std::unique_ptr<BatchResult> done)
        {
            auto* raw = done.release();
            if (!PostMessageW(notify, msg::BatchDone, 0, reinterpret_cast<LPARAM>(raw)))
                std::unique_ptr<BatchResult> reclaim(raw);
        }

        // Opens Explorer on the folder of the first path with every path from that folder selected.
        // A slice's icon (a Segoe Fluent Icons / MDL2 code point). Single-step conversions show only
        // their format name, which says it best; everything else gets an icon above its label.
        struct Look
        {
            wchar_t icon = 0;
            bool mirror = false;
        };
        [[maybe_unused]] Look SliceLook(const Recipe& recipe)   // unused in the M0 build
        {
            if (recipe.IsSingleEncode() || recipe.steps.empty())
                return {};
            const auto& step = recipe.steps.front();
            const auto& a = step.action;
            if (a == "image.rotate")
            {
                // Rotate (clockwise arrow); counter-clockwise is the same glyph mirrored.
                const auto degrees = step.params.value("degrees", std::string("90"));
                return { 0xE7AD, degrees == "270" };
            }
            if (a == "image.flip") return { 0xE8AB };              // Switch (two arrows)
            if (a == "image.scale") return { 0xE73F };             // BackToWindow (shrink)
            if (a == "image.resize") return { 0xE740 };            // FullScreen (fit)
            if (a == "image.strip_metadata") return { 0xE72E };    // Lock
            if (a == "file.copy") return { 0xE8C8 };               // Copy
            if (a == "file.move") return { 0xE8DE };               // MoveToFolder
            if (a == "file.copy_path") return { 0xE71B };          // Link
            if (a == "file.terminal") return { 0xE756 };           // CommandPrompt
            if (a == "file.duplicate") return { 0xE8A5 };          // Document
            return { 0xE9F5 };                                     // Processing (a generic recipe)
        }

        void RevealInExplorer(const std::vector<std::wstring>& paths)
        {
            if (paths.empty())
                return;
            // ILFree as the deleter: PIDLs are __unaligned pointers on x64, which a plain void* deleter would not accept.
            using Pidl = wil::unique_any<PIDLIST_ABSOLUTE, decltype(&::ILFree), ::ILFree>;
            const auto folder = std::filesystem::path(paths.front()).parent_path().wstring();

            PIDLIST_ABSOLUTE raw = nullptr;
            if (FAILED(SHParseDisplayName(folder.c_str(), nullptr, &raw, 0, nullptr)))
                return;
            Pidl folderPidl(raw);

            std::vector<Pidl> items;
            std::vector<PCUITEMID_CHILD> children;
            for (const auto& p : paths)
            {
                if (std::filesystem::path(p).parent_path().wstring() != folder)
                    continue;
                raw = nullptr;
                if (SUCCEEDED(SHParseDisplayName(p.c_str(), nullptr, &raw, 0, nullptr)))
                {
                    items.emplace_back(raw);
                    // The array wants child IDs relative to the folder: the last ID of each absolute PIDL.
                    children.push_back(reinterpret_cast<PCUITEMID_CHILD>(ILFindLastID(raw)));
                }
            }
            SHOpenFolderAndSelectItems(folderPidl.get(), static_cast<UINT>(children.size()), children.data(), 0);
        }
    }

    class App
    {
    public:
        App(HINSTANCE instance, bool autostart) : m_instance(instance), m_autostart(autostart) {}

        bool Init()
        {
            m_settings = LoadSettings();
            startup::ApplyAtLaunch();
            m_icon.reset(static_cast<HICON>(LoadImageW(m_instance, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON,
                                                       GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR)));
            // The ListView in the history window lives in comctl32 v6 (selected by app.manifest).
            INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES };
            InitCommonControlsEx(&icc);

            WNDCLASSEXW wc{ sizeof(wc) };
            wc.lpfnWndProc = WndProc;
            wc.hInstance = m_instance;
            wc.hIcon = m_icon.get();
            wc.lpszClassName = kMainClass;
            if (!RegisterClassExW(&wc))
                return false;
            // A hidden top-level window rather than HWND_MESSAGE: message-only windows do not receive
            // broadcasts, and we need the "TaskbarCreated" broadcast to restore the tray icon.
            m_hwnd.reset(CreateWindowExW(0, kMainClass, SD_NAME, WS_OVERLAPPED, 0, 0, 0, 0, nullptr, nullptr, m_instance, this));
            if (!m_hwnd)
                return false;
            m_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

            m_recipes.Load(m_registry, RecipesFolder());
            m_codecs = std::make_shared<const ImageCodecs>();
            log::Info(L"codecs: {}", m_codecs->Summary());

            std::wstring dbError;
            m_history = HistoryStore::Open(log::AppDataFolder() + L"\\history.db", dbError);
            if (!m_history)
                log::Error(L"history: {} (conversions still work, but without history or undo)", dbError);
            m_runner = std::make_unique<PipelineRunner>(m_codecs, m_history.get());
            m_runner->RecoverInterrupted();

            const unsigned threads = std::clamp(std::thread::hardware_concurrency() / 2, 1u, 4u);
            m_jobs = std::make_unique<JobQueue>(threads);
            m_shellJobs = std::make_unique<JobQueue>(1, JobQueue::Apartment::Sta);
            log::Info(L"jobs: {} worker thread(s) + 1 shell thread", threads);

            std::vector<WheelRing> rings;
#if !SHIFTDROP_M0
            // Tab icons (Segoe Fluent Icons / MDL2): Sync, Repair (wrench), Folder.
            constexpr wchar_t kRingIcons[kRingCount] = { 0xE895, 0xE90F, 0xE8B7 };
            for (int i = 0; i < kRingCount; ++i)
                rings.push_back(WheelRing{ RingTitle(static_cast<Ring>(i)), m_settings.keys[i] ? m_settings.keyNames[i] : L"",
                                           kRingIcons[i] });
#endif
            m_appearance = LoadAppearance();
            m_island = std::make_unique<Island>(m_instance, rings, [this] { RevealInExplorer(m_lastOutputs); });
            if (!m_island->Create())
                return false;
            m_island->SetAppearance(m_appearance);

            Overlay::Events events;
            events.shown = [this](int ring, HMONITOR monitor) { m_island->ShowSwitcher(ring, monitor); };
            // After a drop the panel turns into the job's progress instead (RunPendingDrop).
            events.hidden = [this] {
                if (m_pendingDrops.empty())
                    m_island->HideSwitcher();
            };
            events.ringChanged = [this](int ring) { m_island->SetRing(ring); };
            m_overlay = std::make_unique<Overlay>(
                m_instance, std::move(rings),
                [this](const std::vector<std::wstring>& files, int ring) { return ContentFor(files, static_cast<Ring>(ring)); },
                [this](std::vector<std::wstring> files, std::wstring actionId) { HandleDrop(std::move(files), std::move(actionId)); },
                std::move(events));
            if (!m_overlay->Create())
                return false;
            m_overlay->SetAppearance(m_appearance);
            m_historyWindow = std::make_unique<HistoryWindow>(m_instance, m_history.get(),
                                                              [](const std::vector<std::wstring>& paths) { RevealInExplorer(paths); });

            m_tray.Add(m_hwnd.get(), m_icon.get(), IdleTip());

            const UINT keys[kRingCount] = { m_settings.keys[0], m_settings.keys[1], m_settings.keys[2] };
            if (!hook::Install(m_hwnd.get(), keys))
            {
                log::Error(L"hook: SetWindowsHookExW(WH_MOUSE_LL) failed ({})", GetLastError());
                m_tray.Balloon(SD_NAME, L"Could not install the mouse hook. See the log.", NIIF_ERROR);
                return false;
            }
            hook::CaptureWheelWhile(m_overlay->ScrollCaptureFlag());
            SetTimer(m_hwnd.get(), kTimerWatchdog, 3000, nullptr);

#if SHIFTDROP_M0
            constexpr const wchar_t* kBuild = L"M0 placeholder build";
#else
            constexpr const wchar_t* kBuild = L"M2 build";
#endif
            if (IsElevated())
            {
                // UIPI: a drag from a normal (medium-integrity) Explorer cannot drop onto an elevated window.
                log::Warn(L"running elevated: drops from normal Explorer windows will be blocked by UIPI");
                m_tray.Balloon(L"" SD_NAME L" is running as administrator",
                               L"Drops from normal Explorer windows will not reach it. Run " SD_NAME L" without elevation.", NIIF_WARNING);
            }
            else if (!m_recipes.Errors().empty())
            {
                // The rest of the recipes still load; the broken file is named so it can be fixed.
                m_tray.Balloon(std::format(L"{} recipe file(s) could not be loaded", m_recipes.Errors().size()),
                               m_recipes.Errors().front() + L"\nSee the log for details.", NIIF_WARNING);
            }
            else if (!m_autostart)
            {
                // Started at login: stay quiet in the tray. Started by hand: say it is ready.
                m_tray.Balloon(L"" SD_NAME L" is running", std::format(L"{}. Drag a file and hold {}.", kBuild, KeyHelp()));
            }
            log::Info(L"ready ({}, {})", kBuild, KeyHelp());
            return true;
        }

        int Run()
        {
            MSG m{};
            while (GetMessageW(&m, nullptr, 0, 0) > 0)
            {
                if (m_historyWindow && m_historyWindow->PreTranslate(m))
                    continue;
                TranslateMessage(&m);
                DispatchMessageW(&m);
            }
            return static_cast<int>(m.wParam);
        }

    private:
        static std::wstring RecipesFolder() { return log::AppDataFolder() + L"\\recipes"; }

        // "Shift (Convert), Ctrl (Tools) or Alt (File actions)", skipping wheels that are turned off.
        std::wstring KeyHelp() const
        {
            std::vector<std::wstring> parts;
            for (int i = 0; i < kRingCount; ++i)
                if (m_settings.keys[i])
                    parts.push_back(std::format(L"{} ({})", m_settings.keyNames[i], RingTitle(static_cast<Ring>(i))));
            std::wstring text;
            for (size_t i = 0; i < parts.size(); ++i)
                text += (i == 0 ? L"" : i + 1 == parts.size() ? L" or " : L", ") + parts[i];
            return text.empty() ? L"no key (every wheel is turned off in settings.ini)" : text;
        }

        std::wstring IdleTip() const
        {
            std::wstring tip = SD_NAME;
            for (int i = 0; i < kRingCount; ++i)
                if (m_settings.keys[i])
                    tip += std::format(L"\n{} = {}", m_settings.keyNames[i], RingTitle(static_cast<Ring>(i)));
            return tip;   // Tray truncates to 127 characters
        }

        static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
        {
            if (msg == WM_NCCREATE)
            {
                auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
                SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
            }
            auto* self = reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
            return self ? self->Handle(hwnd, msg, wParam, lParam) : DefWindowProcW(hwnd, msg, wParam, lParam);
        }

        LRESULT Handle(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
        {
            switch (message)
            {
            case msg::DragBegin:
                SetTimer(hwnd, kTimerPoll, 30, nullptr);
                return 0;
            case msg::DragArm:
                ShowWheel(static_cast<UINT>(wParam), msg::UnpackPoint(lParam));
                return 0;
            case msg::DragEnd:
                OnDragEnd();
                return 0;
            case msg::WheelScroll:
                if (m_overlay)
                    m_overlay->OnScroll(static_cast<short>(LOWORD(wParam)));
                return 0;
            case WM_TIMER:
                OnTimer(wParam);
                return 0;
            case msg::TrayCallback:
                OnTray(wParam, lParam);
                return 0;
            case msg::BatchDone:
                OnBatchDone(std::unique_ptr<BatchResult>(reinterpret_cast<BatchResult*>(lParam)));
                return 0;
            case msg::RunDrop:
                RunPendingDrop();
                return 0;
            case msg::BatchProgress:
                if (wParam == m_islandBatch)
                    m_island->SetProgress(static_cast<size_t>(lParam));
                return 0;
            case msg::AnotherInstance:
                m_tray.Balloon(SD_NAME, L"" SD_NAME L" is already running.");
                return 0;
            case msg::QuitRequest:
                log::Info(L"quit requested by another process (installer or uninstaller)");
                DestroyWindow(hwnd);
                return 0;
            case WM_COMMAND:
                OnCommand(LOWORD(wParam));
                return 0;
            case WM_DESTROY:
                log::Info(L"shutting down");
                hook::Uninstall();
                KillTimer(hwnd, kTimerWatchdog);
                m_jobs.reset();        // stops and joins workers; results still in flight are freed (see PostBatch)
                m_shellJobs.reset();   // waits for a move in progress: IFileOperation can't be cancelled halfway
                m_historyWindow.reset();
                m_overlay.reset();
                m_tray.Remove();
                m_hwnd.release();   // the window is being destroyed by the system right now
                PostQuitMessage(0);
                return 0;
            default:
                if (message == m_taskbarCreated && m_taskbarCreated != 0)
                {
                    log::Info(L"tray: Explorer restarted, re-adding icon");
                    m_tray.Readd();
                    return 0;
                }
                return DefWindowProcW(hwnd, message, wParam, lParam);
            }
        }

        // Takes effect the next time the wheel or the panel appears.
        void ApplyAppearance()
        {
            SaveAppearance(m_appearance);
            m_overlay->SetAppearance(m_appearance);
            m_island->SetAppearance(m_appearance);
            const wchar_t* sizes[] = { L"auto", L"small", L"medium", L"large" };
            log::Info(L"appearance: size {}, style {}", sizes[static_cast<int>(m_appearance.size)],
                      m_appearance.style == Style::Glass ? L"glass" : L"solid");
        }

        void OnCommand(UINT id)
        {
            switch (id)
            {
            case IDM_QUIT:
                DestroyWindow(m_hwnd.get());
                break;
            case IDM_OPEN_LOGS:
                ShellExecuteW(nullptr, L"open", log::Folder().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                break;
            case IDM_OPEN_RECIPES:
                // Recipes are read at startup; the folder exists because Load() seeds it.
                ShellExecuteW(nullptr, L"open", RecipesFolder().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                break;
            case IDM_START_WITH_WINDOWS:
                startup::SetChoice(!startup::Choice());
                break;
            case IDM_SIZE_BASE + 0: case IDM_SIZE_BASE + 1: case IDM_SIZE_BASE + 2: case IDM_SIZE_BASE + 3:
                m_appearance.size = static_cast<WheelSize>(id - IDM_SIZE_BASE);
                ApplyAppearance();
                break;
            case IDM_STYLE_BASE + 0: case IDM_STYLE_BASE + 1:
                m_appearance.style = static_cast<Style>(id - IDM_STYLE_BASE);
                ApplyAppearance();
                break;
            case IDM_HISTORY:
                if (!m_history)
                    m_tray.Balloon(SD_NAME, L"History is unavailable: the history database could not be opened. See the log.", NIIF_WARNING);
                else
                    m_historyWindow->Show();
                break;
            default:
                break;
            }
        }

        // ---- Gesture -------------------------------------------------------------------------------

        // Which wheel a key opens; -1 if none.
        int RingOfKey(UINT vk) const
        {
            for (int i = 0; i < kRingCount; ++i)
                if (vk && m_settings.keys[i] == vk)
                    return i;
            return -1;
        }

        void ShowWheel(UINT vk, POINT pt)
        {
            const int ring = RingOfKey(vk);
            if (ring < 0 || !m_overlay || m_overlay->IsVisible())
                return;
            // The overlay asks ContentFor() once OLE's DragEnter tells it which files are being
            // dragged. Keys already down now must not count as "pressed again" to switch wheels.
            for (int i = 0; i < kRingCount; ++i)
                m_keyDown[i] = IsDown(m_settings.keys[i]);
            m_overlay->ShowProbe(pt, ring);
        }

        void OnDragEnd()
        {
            KillTimer(m_hwnd.get(), kTimerPoll);
            if (!m_overlay || !m_overlay->IsVisible())
                return;
            if (!m_overlay->InDrag())
            {
                // The probe never received DragEnter: this was not an OLE drag (window move, text
                // selection...) or the source is elevated and UIPI blocked it.
                log::Info(L"gesture: released without an OLE drag over the wheel; hiding");
                m_overlay->Hide();
            }
            else
            {
                // The hook sees the button-up before the drag source does. OLE's Drop/DragLeave follows;
                // this timer only covers a source that never sends either.
                SetTimer(m_hwnd.get(), kTimerHideFallback, 750, nullptr);
            }
        }

        static bool IsDown(UINT vk)
        {
            return vk && (GetAsyncKeyState(static_cast<int>(vk)) & 0x8000);
        }

        // While a wheel is open, pressing another wheel's key switches to it (holding the first key
        // or not). Only a fresh press counts, so the key that opened the wheel can stay held.
        void SwitchOnKeyPress()
        {
            for (int i = 0; i < kRingCount; ++i)
            {
                const bool down = IsDown(m_settings.keys[i]);
                if (down && !m_keyDown[i])
                    m_overlay->SetRing(i);
                m_keyDown[i] = down;
            }
        }

        // The first wheel key that is held down right now, or 0.
        UINT HeldKey() const
        {
            for (UINT vk : m_settings.keys)
                if (vk && (GetAsyncKeyState(static_cast<int>(vk)) & 0x8000))
                    return vk;
            return 0;
        }

        void OnTimer(WPARAM id)
        {
            if (id == kTimerPoll)
            {
                if (!hook::IsDragging())
                {
                    KillTimer(m_hwnd.get(), kTimerPoll);
                    return;
                }
                if (m_overlay->IsVisible())
                {
                    SwitchOnKeyPress();
                    return;
                }
                const UINT held = HeldKey();
                if (held && !hook::IsArmed())
                {
                    hook::SetArmed(true);
                    POINT pt{};
                    GetCursorPos(&pt);
                    ShowWheel(held, pt);
                }
                else if (!held && hook::IsArmed() && !m_overlay->IsVisible())
                {
                    // The wheel was dismissed (Esc, left it, not a file drag) and the keys are up again:
                    // pressing one again during the same drag brings a wheel back.
                    hook::SetArmed(false);
                }
            }
            else if (id == kTimerHideFallback)
            {
                KillTimer(m_hwnd.get(), kTimerHideFallback);
                if (m_overlay && m_overlay->IsVisible())
                {
                    log::Warn(L"gesture: no Drop/DragLeave after release; hiding");
                    m_overlay->Hide();
                }
            }
            else if (id == kTimerWatchdog)
            {
                // Windows removes a low-level hook without notice if it ever exceeds LowLevelHooksTimeout.
                // Symptom: the cursor moves but our hook has seen nothing for a while. Then reinstall.
                // (Near a screen edge the hook's point can differ from the clipped cursor; m_watchdogPt
                // stops that from reinstalling more than once per position.)
                POINT cur{};
                GetCursorPos(&cur);
                const POINT last = hook::LastPoint();
                const DWORD idle = GetTickCount() - hook::LastEventTime();
                if ((cur.x != last.x || cur.y != last.y) && idle > 2000 && (cur.x != m_watchdogPt.x || cur.y != m_watchdogPt.y))
                {
                    m_watchdogPt = cur;
                    const bool ok = hook::Reinstall();
                    log::Warn(L"hook: no events for {} ms while the cursor moved (hook removed, or an elevated window "
                              L"is under the cursor); reinstalled: {}", idle, ok);
                }
            }
        }

        // ---- Wheel content and drops ---------------------------------------------------------------

        WheelContent ContentFor(const std::vector<std::wstring>& files, [[maybe_unused]] Ring ring) const
        {
            WheelContent content;
            content.centerText = files.size() == 1 ? std::wstring(L"1 file") : std::format(L"{} files", files.size());
#if !SHIFTDROP_M0
            const char* ringId = RingId(ring);
            std::wstring firstReason;
            bool anyEnabled = false;
            for (const auto& recipe : m_recipes.All())
            {
                if (recipe.ring != ringId)
                    continue;
                if (content.slices.size() == kMaxSlices)
                {
                    log::Warn(L"wheel: more than {} recipes on the {} wheel; '{}' and later ones are not shown",
                              kMaxSlices, RingTitle(ring), recipe.label);
                    break;
                }
                // Mixed selections only get recipes every file supports. A slice is also disabled when
                // it would do nothing for every file (to PNG on PNGs).
                std::wstring reason;
                const bool all = std::all_of(files.begin(), files.end(),
                                             [&](const std::wstring& f) { return m_runner->Accepts(recipe, f, &reason); });
                const bool anyToDo = std::any_of(files.begin(), files.end(),
                                                 [&](const std::wstring& f) { return !m_runner->IsNoOp(recipe, f); });
                if (!all && firstReason.empty())
                    firstReason = reason;
                const auto look = SliceLook(recipe);
                content.slices.push_back(WheelSlice{ recipe.label, Widen(recipe.id), all && anyToDo, look.icon, look.mirror,
                                                     recipe.description.empty() ? recipe.label : recipe.description });
                anyEnabled |= all && anyToDo;
            }
            if (!anyEnabled && !firstReason.empty())
            {
                log::Info(L"wheel: nothing applies: {}", firstReason);
                if (ring != Ring::Files)
                    content.centerText = files.size() == 1 ? L"Not an image" : L"Not all images";
            }
#endif
            return content;
        }

        // Called from inside OLE's IDropTarget::Drop, while Explorer's drag loop waits for us. Queue the
        // work and return at once; RunPendingDrop() does it a moment later.
        void HandleDrop(std::vector<std::wstring> files, std::wstring actionId)
        {
            log::Info(L"drop: {} file(s), action '{}'", files.size(), actionId.empty() ? L"(none)" : actionId);
            for (const auto& f : files)
                log::Info(L"  {}", f);
            m_pendingDrops.push_back(PendingDrop{ std::move(files), Narrow(actionId) });
            PostMessageW(m_hwnd.get(), msg::RunDrop, 0, 0);
        }

        void RunPendingDrop()
        {
            if (m_pendingDrops.empty())
                return;
            auto drop = std::move(m_pendingDrops.front());
            m_pendingDrops.pop_front();

#if SHIFTDROP_M0
            std::wstring list;
            for (const auto& f : drop.files)
                list += FileName(f) + L"\n";
            m_lastOutputs = drop.files;   // clicking the panel reveals the dropped files
            m_island->ShowResult(Island::Outcome::Success, std::format(L"Dropped {}", Count(drop.files.size(), L"file")),
                                 FileName(drop.files.front()));
#else
            const Recipe* recipe = m_recipes.Find(drop.recipeId);
            if (!recipe)
            {
                m_island->HideSwitcher();
                return;
            }
            if (recipe->IsFileAction())
            {
                const auto& action = recipe->steps[0].action;
                if (action == "file.move")
                {
                    m_island->HideSwitcher();   // the folder picker comes first
                    StartMove(*recipe, std::move(drop.files));
                    return;
                }
                if (action != "file.duplicate")
                {
                    // Copy, Copy path, Terminal: instant, nothing written to disk, so no history.
                    const auto r = RunInstantFileAction(action, drop.files, m_hwnd.get());
                    m_island->ShowResult(r.ok ? Island::Outcome::Success : Island::Outcome::Failure, r.message, {});
                    return;
                }
            }
            StartBatch(*recipe, std::move(drop.files));
#endif
        }

        void StartBatch(const Recipe& recipe, std::vector<std::wstring> files)
        {
            auto state = std::make_shared<BatchState>();
            state->notify = m_hwnd.get();
            state->recipe = recipe;
            state->result.kind = recipe.IsFileAction() ? BatchKind::Duplicate : recipe.ring == RingId(Ring::Convert) ? BatchKind::Convert : BatchKind::Tools;
            state->result.label = recipe.label;
            state->result.runId = m_history ? m_history->BeginRun(recipe, static_cast<int>(files.size())) : 0;
            state->result.token = m_islandBatch = m_nextBatch++;
            state->result.files.resize(files.size());
            state->remaining = files.size();
            const auto verb = state->result.kind == BatchKind::Convert ? std::format(L"Converting {} to {}", Count(files.size(), L"file"), recipe.label)
                              : state->result.kind == BatchKind::Duplicate ? std::format(L"Duplicating {}", Count(files.size(), L"file"))
                                                                           : std::format(L"{}: {}", recipe.label, Count(files.size(), L"file"));
            m_island->ShowProgress(verb + L"\u2026", files.size());
            for (size_t i = 0; i < files.size(); ++i)
            {
                state->result.files[i].source = files[i];
                m_jobs->Submit([state, runner = m_runner.get(), history = m_history.get(), i, source = files[i], total = files.size(),
                                token = state->result.token](std::stop_token stop) {
                    auto result = runner->Run(state->recipe, state->result.runId, source, stop);
                    if (result.ok)
                        log::Info(L"job: {} -> {}", source, result.output);
                    else
                        log::Warn(L"job: {} {}: {}", source, result.skipped ? L"skipped" : L"failed", result.error);
                    {
                        std::scoped_lock lock(state->mutex);
                        state->result.files[i].result = std::move(result);
                    }
                    const size_t left = state->remaining.fetch_sub(1) - 1;
                    if (left > 0)
                        PostMessageW(state->notify, msg::BatchProgress, static_cast<WPARAM>(token), static_cast<LPARAM>(total - left));
                    else   // last file of the batch
                    {
                        if (history)
                            history->FinishRun(state->result.runId);
                        std::unique_ptr<BatchResult> done;
                        {
                            std::scoped_lock lock(state->mutex);
                            done = std::make_unique<BatchResult>(std::move(state->result));
                        }
                        PostBatch(state->notify, std::move(done));
                    }
                });
            }
            m_tray.SetTip(std::format(L"" SD_NAME L": {} on {} file(s)...", recipe.label, files.size()));
        }

        void StartMove(const Recipe& recipe, std::vector<std::wstring> files)
        {
            std::wstring folder;
            if (!PickDestinationFolder(m_hwnd.get(), files.size(), folder))
            {
                log::Info(L"move: folder picker cancelled");
                return;
            }
            m_tray.SetTip(std::format(L"" SD_NAME L": moving {} item(s)...", files.size()));
            const uint64_t token = m_islandBatch = m_nextBatch++;
            m_island->ShowProgress(std::format(L"Moving {} to {}\u2026", Count(files.size(), L"item"), FileName(folder).empty() ? folder : FileName(folder)),
                                   files.size());
            m_shellJobs->Submit([notify = m_hwnd.get(), recipe, files = std::move(files), folder,
                                 history = m_history.get(), token](std::stop_token) {
                int64_t runId = 0;
                const auto outcomes = MoveToFolder(recipe, files, folder, history, runId);
                auto done = std::make_unique<BatchResult>();
                done->kind = BatchKind::Move;
                done->label = FileName(folder).empty() ? folder : FileName(folder);   // "D:\" has no file name
                done->runId = runId;
                done->token = token;
                for (const auto& o : outcomes)
                {
                    ItemResult r;
                    r.ok = o.ok;
                    r.skipped = o.skipped;
                    r.output = o.destination;
                    r.error = o.error;
                    done->files.push_back(FileOutcome{ o.source, std::move(r) });
                }
                PostBatch(notify, std::move(done));
            });
        }

        void OnBatchDone(std::unique_ptr<BatchResult> batch)
        {
            m_tray.SetTip(IdleTip());
            size_t ok = 0, skipped = 0, failed = 0;
            std::vector<std::wstring> outputs;
            std::wstring details;
            for (const auto& f : batch->files)
            {
                if (f.result.ok)
                {
                    ++ok;
                    outputs.push_back(f.result.output);
                    details += FileName(f.result.output) + L"\n";
                }
                else
                {
                    ++(f.result.skipped ? skipped : failed);
                    details += std::format(L"{}: {}\n", FileName(f.source), f.result.error);
                }
            }
            if (!outputs.empty())
                m_lastOutputs = outputs;

            const size_t total = batch->files.size();
            const bool all = failed == 0 && skipped == 0;
            std::wstring title;
            switch (batch->kind)
            {
            case BatchKind::Convert:
                title = all ? std::format(L"{} converted to {}", Count(ok, L"file"), batch->label)
                            : std::format(L"{} of {} converted to {}", ok, total, batch->label);
                break;
            case BatchKind::Tools:
                title = all ? std::format(L"{}: {} done", batch->label, Count(ok, L"file"))
                            : std::format(L"{}: {} of {} done", batch->label, ok, total);
                break;
            case BatchKind::Duplicate:
                title = all ? std::format(L"Duplicated {}", Count(ok, L"file")) : std::format(L"Duplicated {} of {}", ok, total);
                break;
            case BatchKind::Move:
                title = all ? std::format(L"Moved {} to {}", Count(ok, L"item"), batch->label)
                            : std::format(L"Moved {} of {} to {}", ok, total, batch->label);
                break;
            }
            if (batch->token == m_islandBatch)
            {
                // The panel: one line on what happened, one on where (or what went wrong first).
                std::wstring sub;
                if (failed || skipped)
                {
                    for (const auto& f : batch->files)
                        if (!f.result.ok)
                        {
                            sub = std::format(L"{}: {}", FileName(f.source), f.result.error);
                            break;
                        }
                }
                else if (batch->kind == BatchKind::Move)
                    sub = L"Undo from History in the tray menu";
                else
                    sub = L"Saved next to the originals \u00b7 Undo from History";
                const auto outcome = ok == 0 ? Island::Outcome::Failure : (failed || skipped) ? Island::Outcome::Partial : Island::Outcome::Success;
                m_island->ShowResult(outcome, title, sub);
            }
            else
            {
                // A later job owns the panel; this one reports the old way.
                if (ok > 0 && m_history)
                    details += L"Undo: tray menu > History";
                m_tray.Balloon(title, details, failed ? NIIF_WARNING : NIIF_INFO);
            }
            log::Info(L"batch (run {}): {} ok, {} skipped, {} failed", batch->runId, ok, skipped, failed);
            if (m_historyWindow)
                m_historyWindow->Refresh();
        }

        // ---- Tray ----------------------------------------------------------------------------------

        void OnTray(WPARAM wParam, LPARAM lParam)
        {
            // NOTIFYICON_VERSION_4: LOWORD(lParam) = event, wParam = anchor point (screen coordinates).
            switch (LOWORD(lParam))
            {
            case WM_CONTEXTMENU:
            case NIN_SELECT:
            case NIN_KEYSELECT:
                m_tray.ShowMenu(POINT{ GET_X_LPARAM(wParam), GET_Y_LPARAM(wParam) }, startup::Choice() && startup::IsEnabled(), m_appearance);
                break;
            case NIN_BALLOONUSERCLICK:
                RevealInExplorer(m_lastOutputs);
                break;
            default:
                break;
            }
        }

        // Members are destroyed in reverse order: the job queues (declared after the runner and the
        // history they use) go first, so no worker outlives what it points at.
        HINSTANCE m_instance;
        bool m_autostart;   // launched by the Run key at login
        Settings m_settings;
        wil::unique_hicon m_icon;
        wil::unique_hwnd m_hwnd;
        UINT m_taskbarCreated = 0;
        Tray m_tray;
        ActionRegistry m_registry;
        RecipeStore m_recipes;
        std::shared_ptr<const ImageCodecs> m_codecs;
        std::unique_ptr<HistoryStore> m_history;
        std::unique_ptr<PipelineRunner> m_runner;
        std::unique_ptr<JobQueue> m_jobs;
        std::unique_ptr<JobQueue> m_shellJobs;
        Appearance m_appearance;
        std::unique_ptr<Island> m_island;     // before the overlay: the overlay's events use it
        std::unique_ptr<Overlay> m_overlay;
        uint64_t m_nextBatch = 1;
        uint64_t m_islandBatch = 0;          // the batch whose progress the top panel shows
        std::unique_ptr<HistoryWindow> m_historyWindow;
        bool m_keyDown[kRingCount]{};   // wheel keys' state at the last poll, to see fresh presses
        std::deque<PendingDrop> m_pendingDrops;
        std::vector<std::wstring> m_lastOutputs;
        POINT m_watchdogPt{};
    };
}

namespace sd
{
    namespace
    {
        // Used by the installer: "--quit" before replacing the files, "--uninstall" before removing them.
        constexpr wchar_t kQuitArg[] = L"--quit";
        constexpr wchar_t kUninstallArg[] = L"--uninstall";

        bool HasArg(const wchar_t* arg)
        {
            int argc = 0;
            wil::unique_hlocal_ptr<PWSTR> argv(CommandLineToArgvW(GetCommandLineW(), &argc));
            if (!argv)
                return false;
            for (int i = 1; i < argc; ++i)
                if (CompareStringOrdinal(argv.get()[i], -1, arg, -1, TRUE) == CSTR_EQUAL)
                    return true;
            return false;
        }

        // Asks the copy running in this logon session to exit and waits for it, so its exe can be
        // replaced or deleted. Returns false only if it is still running after the wait.
        bool QuitRunningInstance()
        {
            HWND other = FindWindowW(kMainClass, nullptr);
            if (!other)
                return true;
            DWORD pid = 0;
            GetWindowThreadProcessId(other, &pid);
            // SYNCHRONIZE is all WaitForSingleObject needs; same user, so no elevation is involved.
            wil::unique_handle process(OpenProcess(SYNCHRONIZE, FALSE, pid));
            PostMessageW(other, msg::QuitRequest, 0, 0);
            // Shutdown waits for a move in progress (IFileOperation can't stop halfway), hence the margin.
            return !process || WaitForSingleObject(process.get(), 15000) == WAIT_OBJECT_0;
        }
    }
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int)
{
    if (sd::HasArg(sd::kQuitArg) || sd::HasArg(sd::kUninstallArg))
    {
        const bool quit = sd::QuitRunningInstance();
        // Uninstalling also removes the Run value, wherever it points; settings and history are kept.
        if (sd::HasArg(sd::kUninstallArg))
            sd::startup::Disable();
        return quit ? 0 : 1;
    }

    const bool autostart = sd::HasArg(sd::startup::kAutostartArg);
    // Single instance per logon session. Read GetLastError immediately: CreateMutexW returns a valid
    // handle to the existing mutex and reports ERROR_ALREADY_EXISTS.
    HANDLE rawMutex = CreateMutexW(nullptr, FALSE, sd::kMutexName);
    const DWORD mutexError = GetLastError();
    wil::unique_handle mutex(rawMutex);
    if (mutexError == ERROR_ALREADY_EXISTS)
    {
        if (HWND other = FindWindowW(sd::kMainClass, nullptr))
            PostMessageW(other, sd::msg::AnotherInstance, 0, 0);
        return 0;
    }

    // OleInitialize (not just CoInitializeEx) is required for RegisterDragDrop; it makes this an STA.
    if (FAILED(OleInitialize(nullptr)))
    {
        MessageBoxW(nullptr, L"OleInitialize failed.", SD_NAME, MB_ICONERROR);
        return 1;
    }
    auto oleUninit = wil::scope_exit([] { OleUninitialize(); });

    sd::log::Init();
    sd::log::Info(L"" SD_NAME L" " SD_VERSION L" starting (pid {}{})", GetCurrentProcessId(), autostart ? L", at login" : L"");

    int code = 1;
    {
        sd::App app(instance, autostart);   // destroyed before OleUninitialize
        if (app.Init())
            code = app.Run();
        else
            sd::log::Error(L"startup failed; see messages above");
    }
    sd::log::Info(L"exit {}", code);
    return code;
}
