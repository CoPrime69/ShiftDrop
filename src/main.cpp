// ShiftDrop: drag a file, hold the trigger key (Shift by default), drop it on a wheel slice.
//
// Threads:
//   UI thread   hidden main window, tray icon, overlay, WH_MOUSE_LL hook, OLE drop target (STA)
//   workers     JobQueue threads (MTA) that run converters and PostMessage results back
#include "Brand.h"
#include "DropTarget.h"
#include "JobQueue.h"
#include "Log.h"
#include "Messages.h"
#include "MouseHook.h"
#include "OutputPath.h"
#include "Overlay.h"
#include "Settings.h"
#include "Tray.h"
#include "WicConverter.h"
#include "resource.h"

#include <windows.h>
#include <ole2.h>
#include <shlobj.h>
#include <wil/resource.h>

#include <algorithm>
#include <atomic>
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

        struct FileOutcome
        {
            std::wstring source;
            JobResult result;
        };

        struct BatchResult
        {
            std::wstring label;
            std::vector<FileOutcome> files;
        };

        struct BatchState
        {
            HWND notify = nullptr;
            std::mutex mutex;
            BatchResult result;
            std::atomic<size_t> remaining{ 0 };
        };

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

        // Opens Explorer on the folder of the first path with every path from that folder selected.
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
        explicit App(HINSTANCE instance) : m_instance(instance) {}

        bool Init()
        {
            m_settings = LoadSettings();
            m_icon.reset(static_cast<HICON>(LoadImageW(m_instance, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON,
                                                       GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR)));

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

            m_wic = std::make_shared<WicConverter>();
            log::Info(L"wic: {}", m_wic->CodecSummary());
            const unsigned threads = std::clamp(std::thread::hardware_concurrency() / 2, 1u, 4u);
            m_jobs = std::make_unique<JobQueue>(threads);
            log::Info(L"jobs: {} worker thread(s)", threads);

            m_overlay = std::make_unique<Overlay>(
                m_instance,
                [this](const std::vector<std::wstring>& files) { return ContentFor(files); },
                [this](std::vector<std::wstring> files, std::wstring actionId) { HandleDrop(std::move(files), std::move(actionId)); });
            if (!m_overlay->Create())
                return false;

            m_tray.Add(m_hwnd.get(), m_icon.get(), std::format(L"" SD_NAME L": hold {} while dragging a file", m_settings.triggerName));

            if (!hook::Install(m_hwnd.get(), m_settings.triggerVk))
            {
                log::Error(L"hook: SetWindowsHookExW(WH_MOUSE_LL) failed ({})", GetLastError());
                m_tray.Balloon(SD_NAME, L"Could not install the mouse hook. See the log.", NIIF_ERROR);
                return false;
            }
            SetTimer(m_hwnd.get(), kTimerWatchdog, 3000, nullptr);

#if SHIFTDROP_M0
            constexpr const wchar_t* kBuild = L"M0 placeholder build";
#else
            constexpr const wchar_t* kBuild = L"M1 build";
#endif
            if (IsElevated())
            {
                // UIPI: a drag from a normal (medium-integrity) Explorer cannot drop onto an elevated window.
                log::Warn(L"running elevated: drops from normal Explorer windows will be blocked by UIPI");
                m_tray.Balloon(L"" SD_NAME L" is running as administrator",
                               L"Drops from normal Explorer windows will not reach it. Run " SD_NAME L" without elevation.", NIIF_WARNING);
            }
            else
            {
                m_tray.Balloon(L"" SD_NAME L" is running",
                               std::format(L"{}. Drag a file, hold {}, drop it on the wheel.", kBuild, m_settings.triggerName));
            }
            log::Info(L"ready ({}, trigger {})", kBuild, m_settings.triggerName);
            return true;
        }

        int Run()
        {
            MSG m{};
            while (GetMessageW(&m, nullptr, 0, 0) > 0)
            {
                TranslateMessage(&m);
                DispatchMessageW(&m);
            }
            return static_cast<int>(m.wParam);
        }

    private:
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
                ShowWheel(msg::UnpackPoint(lParam));
                return 0;
            case msg::DragEnd:
                OnDragEnd();
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
            case msg::AnotherInstance:
                m_tray.Balloon(SD_NAME, L"" SD_NAME L" is already running.");
                return 0;
            case WM_COMMAND:
                if (LOWORD(wParam) == IDM_QUIT)
                    DestroyWindow(hwnd);
                else if (LOWORD(wParam) == IDM_OPEN_LOGS)
                    ShellExecuteW(nullptr, L"open", log::Folder().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                return 0;
            case WM_DESTROY:
                log::Info(L"shutting down");
                hook::Uninstall();
                KillTimer(hwnd, kTimerWatchdog);
                m_jobs.reset();   // stops and joins workers; results still in flight are freed (see Submit)
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

        // ---- Gesture -------------------------------------------------------------------------------

        void ShowWheel(POINT pt)
        {
            if (m_overlay && !m_overlay->IsVisible())
                m_overlay->ShowProbe(pt);
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

        void OnTimer(WPARAM id)
        {
            if (id == kTimerPoll)
            {
                if (!hook::IsDragging())
                {
                    KillTimer(m_hwnd.get(), kTimerPoll);
                    return;
                }
                const bool keyDown = (GetAsyncKeyState(static_cast<int>(m_settings.triggerVk)) & 0x8000) != 0;
                if (keyDown && !hook::IsArmed())
                {
                    hook::SetArmed(true);
                    POINT pt{};
                    GetCursorPos(&pt);
                    ShowWheel(pt);
                }
                else if (!keyDown && hook::IsArmed() && !m_overlay->IsVisible())
                {
                    // The wheel was dismissed (Esc, left it, not a file drag) and the key is up again:
                    // pressing it again during the same drag brings the wheel back.
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

        WheelContent ContentFor(const std::vector<std::wstring>& files) const
        {
            WheelContent content;
            const bool allImages = std::all_of(files.begin(), files.end(),
                                               [&](const std::wstring& f) { return m_wic->supports(LowerExtension(f)); });
            content.centerText = files.size() == 1 ? L"1 file" : std::format(L"{} files", files.size());
#if !SHIFTDROP_M0
            if (!allImages)
                content.centerText = files.size() == 1 ? L"Not an image" : L"Not all images";
            // Mixed selections only get actions every file supports. A slice is also disabled when every
            // file is already in that format.
            for (const auto& action : m_wic->actions())
            {
                const bool anyToDo = std::any_of(files.begin(), files.end(), [&](const std::wstring& f) {
                    return !IsAlreadyTargetFormat(action.id, LowerExtension(f));
                });
                content.slices.push_back(WheelSlice{ action.label, action.id, allImages && anyToDo });
            }
#else
            (void)allImages;
#endif
            return content;
        }

        void HandleDrop(std::vector<std::wstring> files, std::wstring actionId)
        {
            log::Info(L"drop: {} file(s), action '{}'", files.size(), actionId.empty() ? L"(none)" : actionId);
            for (const auto& f : files)
                log::Info(L"  {}", f);

#if SHIFTDROP_M0
            (void)actionId;
            std::wstring list;
            for (const auto& f : files)
                list += FileName(f) + L"\n";
            m_lastOutputs = files;   // clicking the notification reveals the dropped files
            m_tray.Balloon(std::format(L"Dropped {} file(s)", files.size()), list);
#else
            std::wstring label = actionId;
            for (const auto& a : m_wic->actions())
                if (a.id == actionId)
                    label = a.label;

            auto state = std::make_shared<BatchState>();
            state->notify = m_hwnd.get();
            state->result.label = label;
            state->result.files.resize(files.size());
            state->remaining = files.size();
            for (size_t i = 0; i < files.size(); ++i)
            {
                state->result.files[i].source = files[i];
                m_jobs->Submit([state, wic = m_wic, i, job = Job{ files[i], actionId }](std::stop_token stop) {
                    auto result = wic->run(job, nullptr, stop);
                    if (result.ok)
                        log::Info(L"job: {} -> {}", job.source, result.output);
                    else
                        log::Warn(L"job: {} {}: {}", job.source, result.skipped ? L"skipped" : L"failed", result.error);
                    {
                        std::scoped_lock lock(state->mutex);
                        state->result.files[i].result = std::move(result);
                    }
                    if (state->remaining.fetch_sub(1) == 1)   // last file of the batch
                    {
                        std::unique_ptr<BatchResult> done;
                        {
                            std::scoped_lock lock(state->mutex);
                            done = std::make_unique<BatchResult>(std::move(state->result));
                        }
                        // Ownership travels in the LPARAM. If the window is gone (shutting down) the post
                        // fails and we take the pointer back so it is freed here.
                        auto* raw = done.release();
                        if (!PostMessageW(state->notify, msg::BatchDone, 0, reinterpret_cast<LPARAM>(raw)))
                            std::unique_ptr<BatchResult> reclaim(raw);
                    }
                });
            }
            m_tray.SetTip(std::format(L"" SD_NAME L": converting {} file(s) to {}...", files.size(), label));
#endif
        }

        void OnBatchDone(std::unique_ptr<BatchResult> batch)
        {
            m_tray.SetTip(std::format(L"" SD_NAME L": hold {} while dragging a file", m_settings.triggerName));
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

            const auto title = failed == 0 && skipped == 0
                                   ? std::format(L"Converted {} file(s) to {}", ok, batch->label)
                                   : std::format(L"{} of {} converted to {}", ok, batch->files.size(), batch->label);
            m_tray.Balloon(title, details, failed ? NIIF_WARNING : NIIF_INFO);
            log::Info(L"batch: {} ok, {} skipped, {} failed", ok, skipped, failed);
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
                m_tray.ShowMenu(POINT{ GET_X_LPARAM(wParam), GET_Y_LPARAM(wParam) });
                break;
            case NIN_BALLOONUSERCLICK:
                RevealInExplorer(m_lastOutputs);
                break;
            default:
                break;
            }
        }

        HINSTANCE m_instance;
        Settings m_settings;
        wil::unique_hicon m_icon;
        wil::unique_hwnd m_hwnd;
        UINT m_taskbarCreated = 0;
        Tray m_tray;
        std::shared_ptr<const WicConverter> m_wic;
        std::unique_ptr<JobQueue> m_jobs;
        std::unique_ptr<Overlay> m_overlay;
        std::vector<std::wstring> m_lastOutputs;
        POINT m_watchdogPt{};
    };
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int)
{
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
    sd::log::Info(L"" SD_NAME L" " SD_VERSION L" starting (pid {})", GetCurrentProcessId());

    int code = 1;
    {
        sd::App app(instance);   // destroyed before OleUninitialize
        if (app.Init())
            code = app.Run();
        else
            sd::log::Error(L"startup failed; see messages above");
    }
    sd::log::Info(L"exit {}", code);
    return code;
}
