// ShiftDrop: drag a file, press Shift, scroll to the section you want, drop it on a wheel slice.
//
// Threads:
//   UI thread    hidden main window, tray icon, overlay, WH_MOUSE_LL hook, OLE drop target, history
//                window, clipboard file actions (STA)
//   workers      JobQueue threads (MTA) that run recipes and PostMessage results back
//   media worker one JobQueue thread for video: ffmpeg already uses every core, so files go one at a
//                time (each in its own ffmpeg process, see Ffmpeg.h)
//   audio workers JobQueue threads for sound-only ffmpeg runs, which use one core each, so several
//                files convert at once
//   shell worker one STA thread for IFileOperation work (Move to…, Rename…)
//   shelf worker one STA thread for the Float shelf: thumbnails, missing-file checks, clipboard pictures
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
#include "VirtualDrop.h"
#include "resource.h"
#include "inspect/InspectorWindow.h"
#include "keyboard/KeyboardController.h"
#include "keyboard/ShellVerb.h"
#include "keyboard/WheelRequest.h"
#include "preview/PreviewCard.h"
#include "preview/Prediction.h"
#include "sandbox/Sandbox.h"
#include "Features.h"
#include "SettingsWindow.h"
#include "ocr/Ocr.h"
#include "rename/RenameWindow.h"
#include "shelf/ShelfWindow.h"
#include "toast/Toasts.h"

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
        constexpr size_t kMaxSlices = 60;            // two rings hold 16; past that the wheel pages (keyboard/WheelLayout.h)

        // What a finished batch is called in its notification.
        enum class BatchKind { Convert, Tools, Duplicate, Move, Rename };

        struct FileOutcome
        {
            std::wstring source;
            ItemResult result;
        };

        struct BatchResult
        {
            BatchKind kind = BatchKind::Convert;
            std::wstring label;    // recipe label, or the destination folder's name for a move
            std::wstring savedTo;  // files dragged out of an app: the folder the outputs went to
            int64_t runId = 0;     // history run (0 when history is unavailable)
            uint64_t token = 0;    // which batch, so its progress and result reach the top panel
            bool cancelled = false;
            std::vector<FileOutcome> files;
        };

        struct BatchState
        {
            HWND notify = nullptr;
            Recipe recipe;   // a copy: workers never touch the RecipeStore
            std::mutex mutex;
            BatchResult result;
            std::atomic<size_t> remaining{ 0 };
            std::stop_source cancel;          // the top panel's ×; also requested when the app shuts down
            std::atomic<int> lastPermille{ -1 };   // throttles progress posts to whole tenths of a percent
            std::shared_ptr<VirtualDrop> virtualDrop;   // dragged out of an app: its saved copies last as long as the batch
            std::vector<double> done;              // each file's own progress, 0..1 (under mutex)

            // Sets file i's progress and returns the whole batch's, in files.
            double Progress(size_t i, double fraction)
            {
                std::scoped_lock lock(mutex);
                done[i] = fraction;
                double sum = 0;
                for (const double d : done)
                    sum += d;
                return sum;
            }
        };

        struct PendingDrop
        {
            std::vector<std::wstring> files;
            std::string recipeId;
            std::shared_ptr<VirtualDrop> virtualDrop;   // files dragged out of a browser or a mail app
            std::wstring error;                         // why none of them could be saved
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

        // "Desktop" for a user's Desktop folder; a drive's root has no file name, so it is shown whole.
        std::wstring FolderName(const std::wstring& folder)
        {
            const auto name = FileName(folder);
            return name.empty() ? folder : name;
        }

        // File actions that hand the files themselves to something else (the clipboard, the Float
        // shelf, the share sheet, a terminal): for files dragged out of an app they get a copy kept in
        // the output folder, since the drop folder is deleted when the job ends.
        [[maybe_unused]] bool HandsFilesOn(const Recipe& recipe)   // unused in the M0 build
        {
            if (recipe.steps.size() != 1)
                return false;
            const auto& action = recipe.steps[0].action;
            return (recipe.IsFileAction() && action != "file.duplicate" && action != "file.move") || action == "share.files";
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
            if (recipe.icon)
                return { recipe.icon };   // the recipe names its own (feature modules, user recipes)
            if (recipe.IsSingleEncode() || recipe.IsMedia() || recipe.steps.empty())
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
            if (a == "file.rename") return { 0xE8AC };             // Rename
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

        // A click on a finished batch's toast (Toasts.h): show the outputs, or open the one output.
        // Paths that are gone by now (moved, undone) are left out; with none left, their folder opens.
        void OnToastClick(const toast::Activation& click)
        {
            std::vector<std::wstring> present;
            for (const auto& p : click.paths)
                if (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES)
                    present.push_back(p);
            if (click.verb == toast::Verb::Open && !present.empty())
                ShellExecuteW(nullptr, nullptr, present.front().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            else if (!present.empty())
                RevealInExplorer(present);
            else if (const auto folder = std::filesystem::path(click.paths.front()).parent_path().wstring();
                     GetFileAttributesW(folder.c_str()) != INVALID_FILE_ATTRIBUTES)
                ShellExecuteW(nullptr, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
    }

    class App
    {
    public:
        // `wheelPaths`: started by the right-click entry ("--wheel <path>"); the wheel opens on them.
        App(HINSTANCE instance, bool autostart, std::vector<std::wstring> wheelPaths)
            : m_instance(instance), m_autostart(autostart), m_startWheel(std::move(wheelPaths))
        {
        }

        bool Init()
        {
            m_settings = LoadSettings();
            m_preferences = LoadPreferences();
            ApplyPreferences();
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
            m_media = std::make_shared<const FfmpegEngine>(FfmpegEngine::Locate());
            if (m_media->Available())
                log::Info(L"media: ffmpeg at {}", m_media->Path());
            else
                log::Warn(L"media: engines\\ffmpeg.exe not found next to " SD_NAME L".exe; video and audio slices are disabled");
            m_runner = std::make_unique<PipelineRunner>(m_codecs, m_history.get(), m_media);
            m_runner->RecoverInterrupted();
            // Files dragged out of a browser or a mail app: where their outputs go, and the copies a
            // crash may have left behind.
            SetVirtualOutputChoice(LoadVirtualOutput());
            RemoveStaleVirtualDrops(VirtualDropRoot());

            const unsigned threads = JobQueue::PoolThreads();
            m_jobs = std::make_unique<JobQueue>(threads);
            m_mediaJobs = std::make_unique<JobQueue>(1);
            m_audioJobs = std::make_unique<JobQueue>(threads);
            m_shellJobs = std::make_unique<JobQueue>(1, JobQueue::Apartment::Sta);
            log::Info(L"jobs: {0} worker thread(s) + 1 video thread + {0} audio thread(s) + 1 shell thread", threads);
            // The H.264 encoder test (a few tiny encodes, about a second) runs now, on the media thread,
            // instead of delaying the first video conversion.
            if (m_media->Available())
                m_mediaJobs->Submit([media = m_media](std::stop_token) { media->H264(); });

            m_features = LoadFeatures();
            std::vector<WheelRing> rings = BuildRings();
            m_appearance = LoadAppearance();
            {
                wchar_t tag[64]{};
                GetPrivateProfileStringW(L"Ocr", L"Language", L"", tag, ARRAYSIZE(tag), SettingsPath().c_str());
                if (tag[0] && CompareStringOrdinal(tag, -1, L"auto", -1, TRUE) != CSTR_EQUAL)
                    ocr::SetLanguage(tag);
            }
            m_island = std::make_unique<Island>(m_instance, rings, [this] { RevealInExplorer(m_lastOutputs); });
            if (!m_island->Create())
                return false;
            m_island->SetAppearance(m_appearance);
            // The Float shelf: the File actions wheel's Float slice, and where finished results can be carried.
            m_shelf = std::make_unique<Shelf>(m_instance, log::AppDataFolder() + L"\\shelf.json");
            if (!m_shelf->Create())
                log::Error(L"shelf: unavailable; the Float slice and carrying results are off");
            m_shelf->SetAppearance(m_appearance);

            Overlay::Events events;
            events.shown = [this](int ring, HMONITOR monitor) { m_island->ShowSwitcher(ring, monitor); };
            // After a drop the panel turns into the job's progress instead (RunPendingDrop).
            events.hidden = [this] {
                if (m_pendingDrops.empty())
                    m_island->HideSwitcher();
            };
            events.ringChanged = [this](int ring) { m_island->SetRing(ring); };
            // The predicted-output card follows the highlighted slice (M4).
            events.highlighted = [this](const WheelHighlight& h) {
                if (m_preview)
                    m_preview->Update(h, h.actionId.empty() ? nullptr : m_recipes.Find(Narrow(h.actionId)));
            };
            m_overlay = std::make_unique<Overlay>(
                m_instance, std::move(rings),
                [this](const std::vector<std::wstring>& files, int ring) { return ContentFor(files, SectionAt(ring)); },
                [this](std::vector<std::wstring> files, std::wstring actionId) { HandleDrop(std::move(files), std::move(actionId)); },
                std::move(events));
            if (!m_overlay->Create())
                return false;
            m_overlay->SetAppearance(m_appearance);
            m_settingsWindow = std::make_unique<SettingsWindow>(
                m_instance, [this](const Features& f) { ApplyFeatures(f); },
                [this](const Preferences& p) {
                    m_preferences = p;
                    SavePreferences(m_preferences);
                    ApplyPreferences();
                });
            m_renameWindow = std::make_unique<RenameWindow>(m_instance);
            m_historyWindow = std::make_unique<HistoryWindow>(m_instance, m_history.get(),
                                                              [](const std::vector<std::wstring>& paths) { RevealInExplorer(paths); });

            m_tray.Add(m_hwnd.get(), m_icon.get(), IdleTip());
            // Real toasts need the installer's Start menu shortcut; without it, balloons as before.
            m_toasts = toast::Start(OnToastClick);

            // M4: the predicted-output card, the inspector window, keyboard picking and the wheel on
            // Explorer's selection. None of them is needed for the drag gesture, so a failure only logs.
            inspect::InstallWindow();
            m_preview = std::make_unique<PreviewCard>(m_instance, m_media);
            m_preview->SetAppearance(m_appearance);
            if (!m_preview->Create())
                m_preview.reset();
            KeyboardController::Events keyEvents;
            keyEvents.notice = [this](const std::wstring& title, const std::wstring& text) { m_tray.Balloon(title, text); };
            m_keyboard = std::make_unique<KeyboardController>(m_instance, *m_overlay, SectionKeys(), std::move(keyEvents));
            if (!m_keyboard->Create())
                m_keyboard.reset();
            else if (!m_startWheel.empty())
                m_keyboard->Queue(m_startWheel);

            UINT keys[kRingCount]{};
            std::copy(m_settings.keys.begin(), m_settings.keys.end(), keys);
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
            constexpr const wchar_t* kBuild = L"Version " SD_VERSION;
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
            else if (!m_autostart && m_startWheel.empty())
            {
                // Started at login: stay quiet in the tray. Started by hand: say it is ready.
                m_tray.Balloon(L"" SD_NAME L" is running", std::format(L"{}. Drag a file, press {}, and scroll for more sections.", kBuild, KeyHelp()));
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
                if (m_settingsWindow && m_settingsWindow->PreTranslate(m))
                    continue;
                if (m_renameWindow && m_renameWindow->PreTranslate(m))
                    continue;
                TranslateMessage(&m);
                DispatchMessageW(&m);
            }
            return static_cast<int>(m.wParam);
        }

    private:
        static std::wstring RecipesFolder() { return log::AppDataFolder() + L"\\recipes"; }

        // "Shift" by default; a wheel given its own key in settings.ini is named after it, as in
        // "Shift or Ctrl (Tools)".
        std::wstring KeyHelp() const
        {
            std::vector<std::wstring> parts;
            for (int i = 0; i < kRingCount; ++i)
                if (m_settings.keys[i])
                    parts.push_back(i == 0 ? m_settings.keyNames[i] : std::format(L"{} ({})", m_settings.keyNames[i], RingTitle(static_cast<Ring>(i))));
            std::wstring text;
            for (size_t i = 0; i < parts.size(); ++i)
                text += (i == 0 ? L"" : i + 1 == parts.size() ? L" or " : L", ") + parts[i];
            return text.empty() ? L"no key (every wheel is turned off in settings.ini)" : text;
        }

        std::wstring IdleTip() const
        {
            std::wstring tip = SD_NAME;
            tip += std::format(L"\nDrag a file, press {}", KeyHelp());
            tip += L"\nScroll to switch sections";
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
                    m_island->SetProgress(static_cast<double>(lParam) / 1000.0);
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
                m_mediaJobs.reset();   // kills a running ffmpeg (its job object) and deletes its staging file
                m_audioJobs.reset();
                m_shellJobs.reset();   // waits for a move in progress: IFileOperation can't be cancelled halfway
                m_historyWindow.reset();
                m_keyboard.reset();    // before the overlay it drives
                m_preview.reset();     // joins the prediction worker
                m_shelf.reset();       // revokes its drop target; OLE is still initialised
                m_overlay.reset();
                toast::Stop();
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
            if (m_preview)
                m_preview->SetAppearance(m_appearance);
            if (m_shelf)
                m_shelf->SetAppearance(m_appearance);
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
            case IDM_HIDE_CURSOR:
                m_appearance.hideCursor = !m_appearance.hideCursor;
                ApplyAppearance();
                break;
            case IDM_EXPLORER_HOTKEY:
                if (m_keyboard)
                    m_keyboard->ToggleHotkey();
                break;
            case IDM_SHELL_MENU:
                if (m_keyboard)
                    m_keyboard->ToggleShellMenu();
                break;
            case IDM_HISTORY:
                if (!m_history)
                    m_tray.Balloon(SD_NAME, L"History is unavailable: the history database could not be opened. See the log.", NIIF_WARNING);
                else
                    m_historyWindow->Show();
                break;
            case IDM_SETTINGS:
                if (m_settingsWindow)
                    m_settingsWindow->Show(BuildSettingsTree(), m_features, m_preferences);
                break;
            case IDM_SHOW_SHELF:
                if (m_shelf)
                    m_shelf->Reveal();
                break;
            case IDM_VIRTUAL_BASE + 0: case IDM_VIRTUAL_BASE + 1: case IDM_VIRTUAL_BASE + 2:
            {
                const auto choice = static_cast<VirtualOutput>(id - IDM_VIRTUAL_BASE);
                SetVirtualOutputChoice(choice);
                SaveVirtualOutput(choice);
                const wchar_t* names[] = { L"the Desktop", L"Downloads", L"a folder asked for each time" };
                log::Info(L"files dragged out of apps: saved to {}", names[static_cast<int>(choice)]);
                break;
            }
            default:
                if (id >= IDM_OCR_BASE && id <= IDM_OCR_LAST)
                {
                    // 0 = Automatic, else the language at that place in the list the menu showed.
                    const size_t i = id - IDM_OCR_BASE;
                    const std::wstring tag = i == 0 || i > m_ocrLanguages.size() ? L"" : m_ocrLanguages[i - 1].tag;
                    ocr::SetLanguage(tag);
                    WritePrivateProfileStringW(L"Ocr", L"Language", tag.empty() ? L"auto" : tag.c_str(), SettingsPath().c_str());
                    log::Info(L"ocr: language {}", tag.empty() ? L"automatic" : tag);
                }
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
            const int key = RingOfKey(vk);
            if (key < 0 || !m_overlay || m_overlay->IsVisible())
                return;
            // Where that section sits on the wheel; a section turned off in Settings opens the first one.
            const int ring = std::max(0, SectionIndex(static_cast<Ring>(key)));
            // The overlay asks ContentFor() once OLE's DragEnter tells it which files are being
            // dragged. Keys already down now must not count as "pressed again" to switch wheels.
            for (int i = 0; i < kRingCount; ++i)
                m_keyDown[i] = IsDown(m_settings.keys[i]);
            for (int d = 0; d < kDigitKeys; ++d)
                m_digitDown[d] = IsDown('1' + d);
            m_overlay->ShowProbe(pt, ring);
        }

        void OnDragEnd()
        {
            KillTimer(m_hwnd.get(), kTimerPoll);
            if (!m_overlay || !m_overlay->IsVisible() || m_overlay->InClickMode())
                return;   // a wheel opened without a drag closes by itself (KeyboardController)
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
                if (down && !m_keyDown[i] && SectionIndex(static_cast<Ring>(i)) >= 0)
                    m_overlay->SetRing(SectionIndex(static_cast<Ring>(i)));
                m_keyDown[i] = down;
            }
            // Shift+1, Shift+2...: straight to that section, for anyone without a mouse wheel. The
            // drag source's modal loop keeps the keystroke from reaching any window.
            for (int d = 0; d < kDigitKeys; ++d)
            {
                const bool down = IsDown('1' + d);
                if (down && !m_digitDown[d])
                    m_overlay->SetRing(d);
                m_digitDown[d] = down;
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
                    if (m_keyboard)
                        m_keyboard->PollDrag();   // arrows and Enter pick a slice (M4)
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
            // Files dragged out of a browser or a mail app are not on disk until they are dropped, so
            // until then their names alone decide; the job checks the rest once they are saved.
            const auto dragged = files.empty() ? nullptr : VirtualDrop::Find(files.front());
            const bool unsaved = dragged && !dragged->Saved();
            const auto accepts = [&](const Recipe& recipe, const std::wstring& f, std::wstring* why) {
                if (!unsaved)
                    return m_runner->Accepts(recipe, f, why);
                if (recipe.IsMedia() && !m_media->Available())
                {
                    *why = L"the media engine is missing; reinstall to convert video and audio";
                    return false;
                }
                if (m_runner->Applies(recipe, f))
                    return true;
                *why = L"not for this kind of file";
                return false;
            };
            // Each wheel shows the recipes meant for these files: video formats for videos, image formats
            // for photos. With nothing meant for all of them (a photo and a video), the ones meant for
            // some; with nothing meant for any (a PDF), all of them, disabled, so the wheel still says
            // what it does.
            std::vector<const Recipe*> forAll, forAny, onRing;
            for (const auto& recipe : m_recipes.All())
            {
                if (recipe.ring != ringId || !m_features.RecipeOn(recipe) || !m_runner->AppliesToDrop(recipe, files.size()))
                    continue;
                onRing.push_back(&recipe);
                const auto applies = [&](const std::wstring& f) { return m_runner->Applies(recipe, f); };
                if (std::all_of(files.begin(), files.end(), applies))
                    forAll.push_back(&recipe);
                else if (std::any_of(files.begin(), files.end(), applies))
                    forAny.push_back(&recipe);
            }
            const bool meantForThese = !forAll.empty();
            if (forAll.empty())
                forAll.insert(forAll.end(), forAny.begin(), forAny.end());
            const auto& shown = forAll.empty() ? onRing : forAll;

            // Reduce size: the centre shows how big the files are now.
            uint64_t totalBytes = 0;
            if (ring == Ring::Reduce)
            {
                for (const auto& f : files)
                {
                    WIN32_FILE_ATTRIBUTE_DATA data{};
                    if (GetFileAttributesExW(f.c_str(), GetFileExInfoStandard, &data) && !(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                        totalBytes += (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
                }
                if (totalBytes > 0)
                    content.centerText = preview::SizeText(totalBytes);
            }

            std::wstring firstReason;
            bool anyEnabled = false;
            for (const Recipe* recipe : shown)
            {
                if (content.slices.size() == kMaxSlices)
                {
                    log::Warn(L"wheel: more than {} recipes fit these files on the {} wheel; '{}' and later ones are not shown",
                              kMaxSlices, RingTitle(ring), recipe->label);
                    break;
                }
                // Mixed selections only get recipes every file supports. A slice is also disabled when
                // it would do nothing for every file (to PNG on PNGs).
                std::wstring reason;
                const bool all = std::all_of(files.begin(), files.end(),
                                             [&](const std::wstring& f) { return accepts(*recipe, f, &reason); });
                const bool anyToDo = std::any_of(files.begin(), files.end(),
                                                 [&](const std::wstring& f) { return !m_runner->IsNoOp(*recipe, f); });
                if (!all && firstReason.empty())
                    firstReason = reason;
                const auto look = SliceLook(*recipe);
                // A slice dimmed for a reason (HEIC without Windows' codec) carries that reason as its detail.
                std::wstring detail = !all && !reason.empty() ? reason : recipe->description.empty() ? recipe->label : recipe->description;
                if (!all && !reason.empty())
                    log::Info(L"wheel: '{}' dimmed: {}", recipe->label, reason);
                content.slices.push_back(WheelSlice{ recipe->label, Widen(recipe->id), all && anyToDo, look.icon, look.mirror, std::move(detail) });
                if (ring == Ring::Reduce && totalBytes > 0)
                {
                    // Hovered, the centre shows the size it comes out at over the size it has now.
                    const int percent = ReducePercent(*recipe);
                    if (percent > 0)
                    {
                        content.slices.back().headline = L"\u2248 " + preview::SizeText(totalBytes * static_cast<uint64_t>(percent) / 100);
                        content.slices.back().detail = L"was " + preview::SizeText(totalBytes);
                    }
                }
                anyEnabled |= all && anyToDo;
            }
            if (!anyEnabled && !firstReason.empty())
            {
                log::Info(L"wheel: nothing applies: {}", firstReason);
                if (ring != Ring::Files)
                    content.centerText = NothingHereText(files);
                content.explains = meantForThese;   // e.g. video formats for a video, but no engine
            }
#endif
            return content;
        }

        // ---- Sections and Settings ------------------------------------------------------------------

        // The wheel's sections that are on, in scroll order; m_sections[i] is what ring i of the
        // overlay and the top panel shows.
        std::vector<WheelRing> BuildRings()
        {
            std::vector<WheelRing> rings;
            m_sections.clear();
#if !SHIFTDROP_M0
            // Tab icons (Segoe Fluent Icons / MDL2): Sync, Repair (wrench), BackToWindow (arrows in), Folder.
            constexpr wchar_t kRingIcons[kRingCount] = { 0xE895, 0xE90F, 0xE73F, 0xE8B7 };
            for (int i = 0; i < kRingCount; ++i)
            {
                const auto ring = static_cast<Ring>(i);
                if (!m_features.SectionOn(ring))
                    continue;
                m_sections.push_back(ring);
                rings.push_back(WheelRing{ RingTitle(ring), m_settings.keys[i] ? m_settings.keyNames[i] : L"", kRingIcons[i],
                                           ring == Ring::Convert });
            }
#endif
            return rings;
        }

        Ring SectionAt(int index) const
        {
            return index >= 0 && static_cast<size_t>(index) < m_sections.size() ? m_sections[static_cast<size_t>(index)] : Ring::Convert;
        }

        // Where a section sits on the wheel, or -1 when it is turned off.
        int SectionIndex(Ring ring) const
        {
            const auto it = std::find(m_sections.begin(), m_sections.end(), ring);
            return it == m_sections.end() ? -1 : static_cast<int>(it - m_sections.begin());
        }

        // The wheel keys by place on the wheel, for the click-mode wheel (KeyboardController).
        std::array<UINT, kRingCount> SectionKeys() const
        {
            std::array<UINT, kRingCount> keys{};
            for (size_t i = 0; i < m_sections.size() && i < keys.size(); ++i)
                keys[i] = m_settings.keys[static_cast<size_t>(m_sections[i])];
            return keys;
        }

        // Settings changed what is on. Sections take effect on the next wheel (the tray menu or the
        // Settings window has the focus, so no wheel is open); slices on the next ContentFor.
        void ApplyFeatures(const Features& features)
        {
            const bool sectionsChanged = features.sections != m_features.sections;
            m_features = features;
            SaveFeatures(m_features);
            if (!sectionsChanged)
                return;
            auto rings = BuildRings();
            if (m_island)
                m_island->SetRings(rings);
            if (m_overlay)
                m_overlay->SetRings(std::move(rings));
            if (m_keyboard)
                m_keyboard->SetRingKeys(SectionKeys());
        }

        // The Settings window's other choices: the theme takes effect the next time a window opens,
        // output names with the next output, the terminal with the next Terminal slice.
        void ApplyPreferences()
        {
            theme::SetChoice(m_preferences.theme);
            SetOutputNaming(m_preferences.naming);
            SetTerminalChoice(m_preferences.terminal, m_preferences.terminalCommand);
        }

        // Where outputs went, for the result's second line: "next to the originals", "to Desktop".
        std::wstring SavedWhere(const std::wstring& savedTo) const
        {
            const bool subfolder = m_preferences.naming == OutputNaming::Subfolder;
            if (savedTo.empty())
                return subfolder ? std::format(L"in a \u201c{}\u201d folder next to the originals", kOutputSubfolder) : L"next to the originals";
            return subfolder ? std::format(L"to {}\\{}", FolderName(savedTo), kOutputSubfolder) : std::format(L"to {}", FolderName(savedTo));
        }

        // Every slice, by section and by the kind of file it is for, for the Settings window.
        SettingsTree BuildSettingsTree() const
        {
            SettingsTree tree;
            struct Kind
            {
                const wchar_t* title;
                const wchar_t* sample;   // a name its slices apply to (by extension; the file need not exist)
            };
            static constexpr Kind kKinds[] = {
                { L"Photos", L"x.jpg" },          { L"Video", L"x.mp4" },          { L"Audio", L"x.mp3" },
                { L"PDF", L"x.pdf" },             { L"Documents", L"x.docx" },     { L"Spreadsheets", L"x.xlsx" },
                { L"Subtitles", L"x.srt" },       { L"Archives", L"x.zip" },
            };
            for (int i = 0; i < kRingCount; ++i)
            {
                const auto ring = static_cast<Ring>(i);
                SettingsTree::Section section{ ring, RingTitle(ring), {} };
                std::vector<SettingsTree::Group> groups;
                const auto groupFor = [&](const std::wstring& title) -> SettingsTree::Group& {
                    for (auto& g : groups)
                        if (g.title == title)
                            return g;
                    return groups.emplace_back(SettingsTree::Group{ title, {} });
                };
                for (const auto& recipe : m_recipes.All())
                {
                    if (recipe.ring != RingId(ring))
                        continue;
                    std::vector<bool> applies;
                    for (const auto& k : kKinds)
                        applies.push_back(m_runner->Applies(recipe, k.sample));
                    std::wstring title = L"Other";
                    if (recipe.IsFileAction() || std::all_of(applies.begin(), applies.begin() + 4, [](bool b) { return b; }))
                        title = L"Any file";
                    else if (applies[1] && applies[2])
                        title = L"Video and audio";
                    else
                        for (size_t k = 0; k < applies.size(); ++k)
                            if (applies[k])
                            {
                                title = kKinds[k].title;
                                break;
                            }
                    groupFor(title).slices.push_back(SettingsTree::Slice{ recipe.id, recipe.label, recipe.description });
                }
                section.groups = std::move(groups);
                tree.sections.push_back(std::move(section));
            }
            return tree;
        }

        // A Reduce size recipe's target, as a percentage of the source's size (10 for "90% smaller").
        static int ReducePercent(const Recipe& recipe)
        {
            for (const auto& step : recipe.steps)
                if (const int percent = step.params.value("percent", 0); percent > 0)
                    return percent;
            return 0;
        }

        // The wheel's centre when none of its slices can take the dragged files.
        std::wstring NothingHereText(const std::vector<std::wstring>& files) const
        {
            bool video = false, audio = false, other = false;
            for (const auto& f : files)
            {
                switch (MediaKindOfExtension(LowerExtension(f)))
                {
                case MediaKind::Video: video = true; break;
                case MediaKind::Audio: audio = true; break;
                default: other = true; break;
                }
            }
            if ((video || audio) && !m_media->Available())
                return L"Media engine missing";
            if (int(video) + int(audio) + int(other) > 1)
                return L"Mixed files";
            if (video)
                return L"Not for videos";
            if (audio)
                return L"Not for audio";
            for (const auto& f : files)   // a HEIC on a PC without Windows' HEIF codec says what to install
                if (auto label = m_codecs->MissingCodecLabel(LowerExtension(f)); !label.empty())
                    return label;
            return files.size() == 1 ? L"Not an image" : L"Not all images";
        }

        // Called from inside OLE's IDropTarget::Drop, while Explorer's drag loop waits for us. Queue the
        // work and return at once; RunPendingDrop() does it a moment later.
        void HandleDrop(std::vector<std::wstring> files, std::wstring actionId)
        {
            log::Info(L"drop: {} file(s), action '{}'", files.size(), actionId.empty() ? L"(none)" : actionId);
            for (const auto& f : files)
                log::Info(L"  {}", f);
            PendingDrop drop{ std::move(files), Narrow(actionId), nullptr, {} };
            // Dragged out of a browser or a mail app: saved now, while the source still holds them.
            if (auto dragged = drop.files.empty() ? nullptr : VirtualDrop::Find(drop.files.front()))
            {
                drop.files = SaveDragged(*dragged, drop.error);
                drop.virtualDrop = std::move(dragged);
            }
            m_pendingDrops.push_back(std::move(drop));
            PostMessageW(m_hwnd.get(), msg::RunDrop, 0, 0);
        }

        // Saves the files of a drag out of an app into its drop folder. This runs inside the drop, so
        // the UI thread is busy until it is done: a save that takes a while shows its progress in the
        // top panel by running just the panel's animation.
        std::vector<std::wstring> SaveDragged(VirtualDrop& dragged, std::wstring& error)
        {
            const ULONGLONG started = GetTickCount64();
            ULONGLONG lastFrame = 0;
            bool shown = false;
            const size_t count = dragged.Paths().size();
            auto saved = dragged.Save([&](uint64_t done, uint64_t total) {
                const ULONGLONG now = GetTickCount64();
                if (now - started < 300 || now - lastFrame < 15)
                    return;
                lastFrame = now;
                if (!shown)
                    m_island->ShowProgress(std::format(L"Saving {}\u2026", Count(count, L"dropped file")), 1);
                shown = true;
                if (total > 0)
                    m_island->SetProgress(std::min(1.0, static_cast<double>(done) / static_cast<double>(total)));
                m_island->Pump();
            }, error);
            log::Info(L"drop: saved {} of {} dragged file(s) in {} ms", saved.size(), count, GetTickCount64() - started);
            return saved;
        }

        // Before a slice runs on files dragged out of an app: a folder for the outputs when the
        // setting asks every time, and a copy kept there for what hands the files themselves on.
        // False when there is nothing to run.
        bool PrepareDragged(PendingDrop& drop, const Recipe& recipe)
        {
            if (drop.files.empty())
            {
                m_island->ShowResult(Island::Outcome::Failure, L"The dropped file could not be saved", drop.error);
                return false;
            }
            if (recipe.IsFileAction() && recipe.steps[0].action == "file.move")
                return true;   // Move to… asks for its folder itself
            auto& dragged = *drop.virtualDrop;
            if (dragged.AsksFolder())
            {
                std::wstring folder;
                m_island->HideSwitcher();   // the folder picker comes first
                if (!PickDestinationFolder(m_hwnd.get(), drop.files.size(), folder, L"Save"))
                {
                    log::Info(L"drop: no folder picked for the dragged file(s); nothing saved");
                    return false;
                }
                dragged.SetOutputFolder(folder);
            }
            if (HandsFilesOn(recipe))
            {
                std::wstring error;
                drop.files = KeepDragged(drop.files, dragged.OutputFolder(), error);
                if (drop.files.empty())
                {
                    m_island->ShowResult(Island::Outcome::Failure, L"The dropped file could not be saved", error);
                    return false;
                }
                drop.virtualDrop.reset();   // the kept copies are ordinary files now
            }
            return true;
        }

        // Moves saved copies of dragged files into `folder`, never replacing anything there.
        static std::vector<std::wstring> KeepDragged(const std::vector<std::wstring>& files, const std::wstring& folder, std::wstring& error)
        {
            std::vector<std::wstring> kept;
            for (const auto& f : files)
            {
                std::wstring path;
                if (const HRESULT hr = KeepVirtualFile(f, folder, path); FAILED(hr))
                {
                    log::Warn(L"drop: could not save {} in {}: {}", f, folder, HResultText(hr));
                    if (error.empty())
                        error = std::format(L"{}: {}", FileName(f), HResultText(hr));
                    continue;
                }
                log::Info(L"drop: saved {}", path);
                kept.push_back(std::move(path));
            }
            return kept;
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
            if (drop.virtualDrop && !PrepareDragged(drop, *recipe))
                return;
            if (recipe->IsFileAction())
            {
                const auto& action = recipe->steps[0].action;
                if (action == "file.move")
                {
                    m_island->HideSwitcher();   // the folder picker comes first
                    StartMove(*recipe, std::move(drop.files), drop.virtualDrop != nullptr);
                    return;
                }
                if (action == "file.rename")
                {
                    m_island->HideSwitcher();   // the Rename window comes first
                    StartRename(*recipe, std::move(drop.files));
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
            if (const auto* engine = m_runner->EngineFor(*recipe); engine && engine->NeedsEditor(*recipe))
            {
                // Trim, crop, annotate...: the engine's window asks first, then the batch runs with the
                // user's choices in the recipe's params.
                m_island->HideSwitcher();
                engine->OpenEditor(m_hwnd.get(), *recipe, drop.files,
                                   [this, dragged = drop.virtualDrop](Recipe adjusted, std::vector<std::wstring> files) {
                                       StartBatch(adjusted, std::move(files), dragged);
                                   });
                return;
            }
            StartBatch(*recipe, std::move(drop.files), std::move(drop.virtualDrop));
#endif
        }

        // `dragged`: the files were dragged out of an app and saved in its drop folder, which the
        // batch keeps until its last file is done.
        void StartBatch(const Recipe& recipe, std::vector<std::wstring> files, std::shared_ptr<VirtualDrop> dragged = nullptr)
        {
            auto state = std::make_shared<BatchState>();
            state->notify = m_hwnd.get();
            state->recipe = recipe;
            if (dragged)
                state->result.savedTo = dragged->OutputFolder();
            state->virtualDrop = std::move(dragged);
            state->result.kind = recipe.IsFileAction() ? BatchKind::Duplicate : recipe.ring == RingId(Ring::Convert) ? BatchKind::Convert : BatchKind::Tools;
            state->result.label = recipe.label;
            state->result.runId = m_history ? m_history->BeginRun(recipe, static_cast<int>(files.size())) : 0;
            state->result.token = m_islandBatch = m_nextBatch++;
            state->result.files.resize(files.size());
            state->done.assign(files.size(), 0.0);
            state->remaining = files.size();
            const auto verb = state->result.kind == BatchKind::Convert ? std::format(L"Converting {} to {}", Count(files.size(), L"file"), recipe.label)
                              : state->result.kind == BatchKind::Duplicate ? std::format(L"Duplicating {}", Count(files.size(), L"file"))
                                                                           : std::format(L"{}: {}", recipe.label, Count(files.size(), L"file"));
            // The panel's × stops this batch: the file in progress is abandoned (its staging file
            // deleted) and the rest are skipped. Files already finished stay.
            m_island->ShowProgress(verb + L"\u2026", files.size(), [cancel = state->cancel]() mutable { cancel.request_stop(); });
            // Video and audio go through the media queue, one file at a time; images use every worker;
            // shell work goes to the STA thread. A batch action (join, merge, zip) is one job for all files.
            const JobLane lane = m_runner->Lane(recipe);
            JobQueue& queue = lane == JobLane::Media ? *m_mediaJobs : lane == JobLane::Audio ? *m_audioJobs : lane == JobLane::Shell ? *m_shellJobs : *m_jobs;
            if (const auto* engine = m_runner->EngineFor(recipe); engine && engine->IsBatch(recipe))
            {
                state->result.files.resize(1);
                state->result.files[0].source = files.front();
                state->remaining = 1;
                queue.Submit([state, runner = m_runner.get(), history = m_history.get(), files, token = state->result.token](std::stop_token stop) {
                    std::stop_callback onShutdown(stop, [&] { state->cancel.request_stop(); });
                    const auto progress = [&](double fraction) {
                        const auto at = static_cast<int>(fraction * 1000.0);
                        if (at != state->lastPermille.exchange(at))
                            PostMessageW(state->notify, msg::BatchProgress, static_cast<WPARAM>(token), static_cast<LPARAM>(at));
                    };
                    auto result = runner->RunBatch(state->recipe, state->result.runId, files, state->cancel.get_token(), progress);
                    if (result.ok)
                        log::Info(L"job: {} file(s) -> {}", files.size(), result.output);
                    else
                        log::Warn(L"job: batch of {} {}: {}", files.size(), result.skipped ? L"skipped" : L"failed", result.error);
                    if (history)
                        history->FinishRun(state->result.runId);
                    auto done = std::make_unique<BatchResult>(std::move(state->result));
                    done->files[0].result = std::move(result);
                    done->cancelled = state->cancel.stop_requested();
                    PostBatch(state->notify, std::move(done));
                });
                m_island->ShowProgress(std::format(L"{}: {}\u2026", recipe.label, Count(files.size(), L"file")), 1,
                                       [cancel = state->cancel]() mutable { cancel.request_stop(); });
                return;
            }
            for (size_t i = 0; i < files.size(); ++i)
            {
                state->result.files[i].source = files[i];
                queue.Submit([state, runner = m_runner.get(), history = m_history.get(), i, source = files[i], total = files.size(),
                              token = state->result.token](std::stop_token stop) {
                    // Shutting down stops the batch the same way the × does.
                    std::stop_callback onShutdown(stop, [&] { state->cancel.request_stop(); });
                    // Within-file progress (media only): every file's own fraction added up (several run
                    // at once on the audio lane), in thousandths of a file, posted when it moves by a
                    // tenth of a percent of the batch.
                    const auto progress = [&](double fraction) {
                        const auto at = static_cast<int>(state->Progress(i, fraction) * 1000.0);
                        if (at / static_cast<int>(total) != state->lastPermille.exchange(at / static_cast<int>(total)))
                            PostMessageW(state->notify, msg::BatchProgress, static_cast<WPARAM>(token), static_cast<LPARAM>(at));
                    };
                    auto result = runner->Run(state->recipe, state->result.runId, source, state->cancel.get_token(), progress);
                    if (result.ok)
                        log::Info(L"job: {} -> {}", source, result.output);
                    else
                        log::Warn(L"job: {} {}: {}", source, result.skipped ? L"skipped" : L"failed", result.error);
                    {
                        std::scoped_lock lock(state->mutex);
                        state->result.files[i].result = std::move(result);
                    }
                    const double batchDone = state->Progress(i, 1.0);
                    const size_t left = state->remaining.fetch_sub(1) - 1;
                    if (left > 0)
                        PostMessageW(state->notify, msg::BatchProgress, static_cast<WPARAM>(token), static_cast<LPARAM>(batchDone * 1000));
                    else   // last file of the batch
                    {
                        if (history)
                            history->FinishRun(state->result.runId);
                        std::unique_ptr<BatchResult> done;
                        {
                            std::scoped_lock lock(state->mutex);
                            state->result.cancelled = state->cancel.stop_requested();
                            done = std::make_unique<BatchResult>(std::move(state->result));
                        }
                        PostBatch(state->notify, std::move(done));
                    }
                });
            }
            m_tray.SetTip(std::format(L"" SD_NAME L": {} on {} file(s)...", recipe.label, files.size()));
        }

        // `dragged`: files dragged out of an app and saved in a drop folder. They are kept in the folder
        // picked: a new file there, with no history entry (there is no original place to move back to).
        void StartMove(const Recipe& recipe, std::vector<std::wstring> files, bool dragged = false)
        {
            std::wstring folder;
            if (!PickDestinationFolder(m_hwnd.get(), files.size(), folder))
            {
                log::Info(L"move: folder picker cancelled");
                return;
            }
            if (dragged)
            {
                std::wstring error;
                const auto kept = KeepDragged(files, folder, error);
                if (!kept.empty())
                    m_lastOutputs = kept;
                const auto title = kept.size() == files.size() ? std::format(L"Saved {} to {}", Count(kept.size(), L"file"), FolderName(folder))
                                                               : std::format(L"Saved {} of {} to {}", kept.size(), files.size(), FolderName(folder));
                const auto outcome = kept.empty() ? Island::Outcome::Failure : error.empty() ? Island::Outcome::Success : Island::Outcome::Partial;
                m_island->ShowResult(outcome, title, error, m_shelf && !kept.empty() ? m_shelf->CarryActions(kept) : Island::ResultActions{});
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

        // The Rename window asks for the names; they are then given on the shell thread, as one undoable
        // history run.
        void StartRename(const Recipe& recipe, std::vector<std::wstring> files)
        {
            m_renameWindow->Show(std::move(files), [this, recipe](std::vector<Rename> renames) {
                const uint64_t token = m_islandBatch = m_nextBatch++;
                m_island->ShowProgress(std::format(L"Renaming {}\u2026", Count(renames.size(), L"item")), renames.size());
                m_shellJobs->Submit([notify = m_hwnd.get(), recipe, renames = std::move(renames), history = m_history.get(), token](std::stop_token) {
                    int64_t runId = 0;
                    const auto outcomes = RenameItems(recipe, renames, history, runId);
                    auto done = std::make_unique<BatchResult>();
                    done->kind = BatchKind::Rename;
                    done->label = recipe.label;
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
                    if (!f.result.output.empty())   // empty: the result is not a file (Copy text)
                        outputs.push_back(f.result.output);
                    details += (f.result.output.empty() ? f.result.message : FileName(f.result.output)) + L"\n";
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
            case BatchKind::Rename:
                title = all ? std::format(L"Renamed {}", Count(ok, L"item")) : std::format(L"Renamed {} of {}", ok, total);
                break;
            }
            // An engine's own line for a single result ("Copied 214 words", "4.2 MB to 1.1 MB").
            if (total == 1 && ok == 1 && !batch->files[0].result.message.empty())
                title = batch->files[0].result.message;
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
                else if (batch->kind == BatchKind::Move || batch->kind == BatchKind::Rename)
                    sub = L"Undo from History in the tray menu";
                else if (!outputs.empty())
                    sub = std::format(L"Saved {} \u00b7 Undo from History", SavedWhere(batch->savedTo));
                auto outcome = ok == 0 ? Island::Outcome::Failure : (failed || skipped) ? Island::Outcome::Partial : Island::Outcome::Success;
                if (batch->cancelled)
                {
                    // Asked for, so not a failure: say what is left on disk.
                    outcome = Island::Outcome::Partial;
                    title = ok == 0 ? std::wstring(L"Cancelled") : std::format(L"Cancelled after {} of {}", ok, total);
                    sub = ok == 0 ? std::wstring(L"Nothing was saved") : L"Finished files are saved " + SavedWhere(batch->savedTo);
                }
                // Outputs can be carried on from the panel: dragged into an app, floated, copied.
                const bool carry = m_shelf && !outputs.empty();
                m_island->ShowResult(outcome, title, sub, carry ? m_shelf->CarryActions(outputs) : Island::ResultActions{});
            }
            else
            {
                // A later job owns the panel: this one says so in a toast (with Open folder and Open),
                // or a balloon where toasts are not available.
                if (ok > 0 && m_history)
                    details += L"Undo: tray menu > History";
                if (!m_toasts || !toast::Show(toast::Content{ title, details, outputs }))
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
            {
                TrayExtras extras;
                extras.shelf = m_shelf != nullptr;
                // A text-recognition language choice, only when there is more than one to pick from.
                m_ocrLanguages = ocr::Languages();
                if (m_ocrLanguages.size() > 1)
                {
                    TrayChoice choice{ L"Text recognition language", IDM_OCR_BASE, { L"Automatic" }, 0 };
                    const auto current = ocr::LanguageChoice();
                    for (size_t i = 0; i < m_ocrLanguages.size() && IDM_OCR_BASE + i + 1 <= IDM_OCR_LAST; ++i)
                    {
                        choice.items.push_back(m_ocrLanguages[i].name.empty() ? m_ocrLanguages[i].tag : m_ocrLanguages[i].name);
                        if (CompareStringOrdinal(m_ocrLanguages[i].tag.c_str(), -1, current.c_str(), -1, TRUE) == CSTR_EQUAL)
                            choice.checked = static_cast<int>(i) + 1;
                    }
                    extras.choices.push_back(std::move(choice));
                }
                // Where files dragged out of a browser or a mail app are saved (VirtualDrop.h).
                extras.choices.push_back(TrayChoice{ L"Save browser drops to", IDM_VIRTUAL_BASE,
                                                     { L"Desktop", L"Downloads", L"Ask every time" }, static_cast<int>(VirtualOutputChoice()) });
                if (m_keyboard)
                {
                    extras.toggles.push_back({ IDM_EXPLORER_HOTKEY, std::format(L"Wheel on Explorer's selection ({})", m_keyboard->HotkeyLabel()), m_keyboard->HotkeyOn() });
                    extras.toggles.push_back({ IDM_SHELL_MENU, L"\u201c" SD_NAME L"\u2026\u201d in Explorer's right-click menu", KeyboardController::ShellMenuOn() });
                }
                m_tray.ShowMenu(POINT{ GET_X_LPARAM(wParam), GET_Y_LPARAM(wParam) }, startup::Choice() && startup::IsEnabled(), m_appearance, extras);
                break;
            }
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
        Preferences m_preferences;           // the Settings window's choices besides the slices
        wil::unique_hicon m_icon;
        wil::unique_hwnd m_hwnd;
        UINT m_taskbarCreated = 0;
        Tray m_tray;
        ActionRegistry m_registry;
        RecipeStore m_recipes;
        std::shared_ptr<const ImageCodecs> m_codecs;
        std::unique_ptr<HistoryStore> m_history;
        std::shared_ptr<const FfmpegEngine> m_media;
        std::unique_ptr<PipelineRunner> m_runner;
        std::unique_ptr<JobQueue> m_jobs;
        std::unique_ptr<JobQueue> m_mediaJobs;
        std::unique_ptr<JobQueue> m_audioJobs;
        std::unique_ptr<JobQueue> m_shellJobs;
        Appearance m_appearance;
        std::unique_ptr<Island> m_island;     // before the overlay: the overlay's events use it
        std::unique_ptr<Overlay> m_overlay;
        Features m_features;                 // what Settings turned off
        std::vector<Ring> m_sections;        // the sections on the wheel, in order (BuildRings)
        std::unique_ptr<SettingsWindow> m_settingsWindow;
        std::unique_ptr<RenameWindow> m_renameWindow;
        std::vector<ocr::Language> m_ocrLanguages;   // as the tray menu last listed them
        std::unique_ptr<Shelf> m_shelf;      // after the island: results hand it their outputs
        uint64_t m_nextBatch = 1;
        uint64_t m_islandBatch = 0;          // the batch whose progress the top panel shows
        std::unique_ptr<HistoryWindow> m_historyWindow;
        std::unique_ptr<PreviewCard> m_preview;            // M4: after the overlay, which reports highlights to it
        std::unique_ptr<KeyboardController> m_keyboard;    // M4: drives the overlay
        std::vector<std::wstring> m_startWheel;            // --wheel paths this process was started with
        bool m_keyDown[kRingCount]{};
        static constexpr int kDigitKeys = 9;   // 1 to 9
        bool m_digitDown[kDigitKeys]{};       // the same for the section number keys   // wheel keys' state at the last poll, to see fresh presses
        std::deque<PendingDrop> m_pendingDrops;
        std::vector<std::wstring> m_lastOutputs;
        bool m_toasts = false;               // toast::Start succeeded
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
    // The installer adds and removes the right-click entry (per user, no admin rights).
    if (sd::HasArg(sd::wheelrequest::kRegisterShellArg))
        return sd::shellverb::Register(sd::shellverb::CurrentExe()) ? 0 : 1;
    if (sd::HasArg(sd::wheelrequest::kUnregisterShellArg))
        return sd::shellverb::Unregister() ? 0 : 1;
    if (sd::HasArg(sd::kQuitArg) || sd::HasArg(sd::kUninstallArg))
    {
        const bool quit = sd::QuitRunningInstance();
        // Uninstalling also removes the Run value, wherever it points, and the right-click entry;
        // settings and history are kept.
        if (sd::HasArg(sd::kUninstallArg))
        {
            sd::startup::Disable();
            sd::shellverb::Unregister();
            sd::toast::Unregister();
            sd::sandbox::RemoveProfile();   // the sandbox's AppContainer profile (S6)
        }
        return quit ? 0 : 1;
    }
    // A click on a toast after ShiftDrop quit: COM starts this exe to take it, and nothing else runs.
    if (sd::HasArg(sd::toast::kActivatedArg) || sd::HasArg(L"-Embedding"))
    {
        if (FAILED(OleInitialize(nullptr)))
            return 1;
        auto oleUninit = wil::scope_exit([] { OleUninitialize(); });
        sd::log::Init();
        return sd::toast::ServeActivation(sd::OnToastClick);
    }
    // "--wheel <paths>" from Explorer's right-click entry: one process per selected item.
    auto wheelPaths = sd::wheelrequest::WheelPaths(sd::wheelrequest::CommandLineArgs());

    const bool autostart = sd::HasArg(sd::startup::kAutostartArg);
    // Single instance per logon session. Read GetLastError immediately: CreateMutexW returns a valid
    // handle to the existing mutex and reports ERROR_ALREADY_EXISTS.
    HANDLE rawMutex = CreateMutexW(nullptr, FALSE, sd::kMutexName);
    const DWORD mutexError = GetLastError();
    wil::unique_handle mutex(rawMutex);
    if (mutexError == ERROR_ALREADY_EXISTS)
    {
        // Hand the paths to the running copy, which opens one wheel for all of them.
        if (!wheelPaths.empty())
            return sd::KeyboardController::Forward(wheelPaths) ? 0 : 1;
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
        sd::App app(instance, autostart, std::move(wheelPaths));   // destroyed before OleUninitialize
        if (app.Init())
            code = app.Run();
        else
            sd::log::Error(L"startup failed; see messages above");
    }
    sd::log::Info(L"exit {}", code);
    return code;
}
