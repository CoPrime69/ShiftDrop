#include "updates/UpdateController.h"
#include "Ask.h"
#include "Brand.h"
#include "HistoryView.h"
#include "Log.h"
#include "Messages.h"
#include "PathUtil.h"
#include "Settings.h"

#include <chrono>
#include <filesystem>
#include <format>

namespace sd::updates
{
    namespace
    {
        constexpr UINT kFirstCheckMs = 60 * 1000;     // after startup, so a sign-in isn't slowed down
        constexpr UINT kTickMs = 10 * 60 * 1000;      // then "is the daily check due?" every 10 minutes
        constexpr UINT kIdleTickMs = 3 * 1000;        // waiting for a job to finish before installing
        constexpr int kUpdateNow = 1;

        int64_t Now()
        {
            return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        }

        std::wstring Folder(const std::wstring& path)
        {
            return std::filesystem::path(path).parent_path().wstring();
        }

        // Next to ShiftDrop.exe, as the installer puts it (and every build does).
        std::wstring UpdaterPath()
        {
            return Folder(ThisExe()) + L"\\" + kUpdaterExe;
        }

        // A window where quitting now would lose something: an editor, Rename, a question, the wheel.
        bool WorkWindowOpen()
        {
            bool open = false;
            EnumThreadWindows(GetCurrentThreadId(), [](HWND hwnd, LPARAM found) -> BOOL {
                wchar_t name[64]{};
                GetClassNameW(hwnd, name, ARRAYSIZE(name));
                for (const wchar_t* busy : { L"" SD_NAME L".Editor", L"" SD_NAME L".Rename", L"" SD_NAME L".Question", L"" SD_NAME L".Overlay" })
                    if (IsWindowVisible(hwnd) && wcscmp(name, busy) == 0)
                    {
                        *reinterpret_cast<bool*>(found) = true;
                        return FALSE;
                    }
                return TRUE;
            }, reinterpret_cast<LPARAM>(&open));
            return open;
        }

        void CALLBACK OnExit(PVOID context, BOOLEAN)
        {
            const auto* hooks = static_cast<const std::pair<HWND, WPARAM>*>(context);
            PostMessageW(hooks->first, msg::UpdaterDone, hooks->second, 0);
        }
    }

    UpdateController::UpdateController(Hooks hooks) : m_hooks(std::move(hooks))
    {
    }

    UpdateController::~UpdateController()
    {
        // Waits for a callback under way; the updater itself goes on (a check just writes update.json).
        if (m_wait)
            UnregisterWaitEx(m_wait, INVALID_HANDLE_VALUE);
        if (m_process)
            CloseHandle(m_process);
    }

    void UpdateController::Start()
    {
        m_running = Running();
        m_mode = LoadMode(SettingsPath());
        m_state = LoadState(StatePath());
        // The updater ran Setup and started this copy: say how it went, once.
        if ((m_state.outcome == Outcome::Installed || m_state.outcome == Outcome::InstallFailed) && !m_state.told)
        {
            const std::wstring v = m_state.hasRelease ? ToText(m_state.release.version) : L"";
            if (m_state.outcome == Outcome::Installed && m_state.hasRelease && Compare(m_state.release.version, m_running) == 0)
                m_hooks.notify(std::format(L"" SD_NAME L" is updated to {}", v), L"Your settings and history are as you left them.", false);
            else if (m_state.outcome == Outcome::Installed)   // an installer that holds another version (a test feed's)
                m_hooks.notify(std::format(L"Setup for {} finished", v), std::format(L"This is " SD_NAME L" {}.", RunningVersion()), false);
            else
                m_hooks.notify(std::format(L"Couldn’t install " SD_NAME L" {}", v),
                               std::format(L"{}{}" SD_NAME L" {} is still installed.", m_state.error, m_state.error.empty() ? L"" : L". ", ToText(m_running)), true);
            m_state.told = true;
            SaveState(m_state, StatePath());
        }
        log::Info(L"updates: {} ({}), mode {}", ResultLine(m_state, m_running), m_state.checkedLocal.empty() ? L"never checked" : m_state.checkedLocal,
                  m_mode == Mode::Automatic ? L"install automatically" : m_mode == Mode::Ask ? L"ask first" : L"off");
        Arm(kFirstCheckMs);
    }

    void UpdateController::Arm(UINT ms)
    {
        SetTimer(m_hooks.window, m_hooks.timer, ms, nullptr);
    }

    void UpdateController::OnTimer()
    {
        if (m_installPending)
        {
            InstallWhenIdle();
            return;
        }
        Arm(kTickMs);
        if (m_job != Job::None)
            return;
        // settings.ini may have been edited by hand since.
        m_mode = LoadMode(SettingsPath());
        const int64_t now = Now();
        if (!DailyCheckDue(m_mode, LoadLastCheck(SettingsPath()), now))
            return;
        // Noted before it runs, so a check that fails (or crashes) still counts as today's.
        SaveLastCheck(now, SettingsPath());
        const std::wstring args = m_mode == Mode::Automatic ? std::format(L"{} {}", kCheckArg, kDownloadArg) : std::wstring(kCheckArg);
        Launch(Job::DailyCheck, UpdaterPath(), args);
    }

    void UpdateController::SetMode(Mode mode)
    {
        if (mode == m_mode)
            return;
        m_mode = mode;
        SaveMode(mode, SettingsPath());
        log::Info(L"updates: mode {}", mode == Mode::Automatic ? L"install automatically" : mode == Mode::Ask ? L"ask first" : L"off");
    }

    bool UpdateController::Launch(Job job, const std::wstring& exe, const std::wstring& args)
    {
        if (m_job != Job::None)
            return false;
        std::wstring line = Quote(exe) + L" " + args;
        STARTUPINFOW si{ sizeof(si) };
        PROCESS_INFORMATION pi{};
        // A check runs in the background at low priority; nothing it does needs to be quick.
        const DWORD priority = job == Job::DailyCheck ? BELOW_NORMAL_PRIORITY_CLASS : 0;
        if (!CreateProcessW(exe.c_str(), line.data(), nullptr, nullptr, FALSE, priority, nullptr, nullptr, &si, &pi))
        {
            const DWORD error = GetLastError();
            log::Error(L"updates: couldn't start {} ({})", exe, error);
            m_state.outcome = job == Job::Download ? Outcome::DownloadFailed : Outcome::CheckFailed;
            m_state.error = std::format(L"{} is missing or couldn’t start", kUpdaterExe);
            if (m_hooks.changed)
                m_hooks.changed();
            return false;
        }
        CloseHandle(pi.hThread);
        m_process = pi.hProcess;
        m_job = job;
        m_waitContext = { m_hooks.window, static_cast<WPARAM>(job) };
        if (!RegisterWaitForSingleObject(&m_wait, m_process, OnExit, &m_waitContext, INFINITE, WT_EXECUTEONLYONCE))
        {
            m_wait = nullptr;
            log::Warn(L"updates: couldn't wait for the updater ({})", GetLastError());
        }
        log::Info(L"updates: started {} {}", kUpdaterExe, args);
        if (m_hooks.changed)
            m_hooks.changed();
        return true;
    }

    void UpdateController::OnUpdaterDone(WPARAM job)
    {
        if (static_cast<Job>(job) != m_job || !m_process)
            return;
        if (m_wait)
            UnregisterWaitEx(m_wait, nullptr);   // it has fired: this only frees it
        m_wait = nullptr;
        DWORD code = static_cast<DWORD>(Result::Failed);
        GetExitCodeProcess(m_process, &code);
        CloseHandle(m_process);
        m_process = nullptr;
        m_job = Job::None;
        Finished(static_cast<Job>(job), code);
    }

    void UpdateController::Finished(Job job, DWORD code)
    {
        m_state = LoadState(StatePath());
        const std::wstring v = m_state.hasRelease ? ToText(m_state.release.version) : L"";
        log::Info(L"updates: the updater exited {}: {}", code, ResultLine(m_state, m_running));
        const bool available = code == static_cast<DWORD>(Result::Available) && HasUpdate(m_state, m_running);
        switch (job)
        {
        case Job::DailyCheck:
            if (available && m_mode == Mode::Automatic && m_state.outcome == Outcome::Downloaded)
            {
                m_installPending = true;
                m_toldWaiting = true;   // nobody asked: it waits without a word
                InstallWhenIdle();
            }
            else if (available && m_mode == Mode::Ask)
            {
                const std::wstring title = std::format(L"" SD_NAME L" {} is available", v);
                const std::wstring text = std::format(L"You have {}. It takes a moment and keeps your settings.", ToText(m_running));
                if (!m_hooks.toast || !m_hooks.toast(title, text))
                    m_hooks.notify(title, std::format(L"Choose “Update to {}…” in the tray menu.", v), false);
            }
            break;
        case Job::ManualCheck:
            if (available)
            {
                if (m_mode == Mode::Automatic)
                    UpdateNow();
                else
                    Offer();
            }
            break;
        case Job::Download:
            if (code == static_cast<DWORD>(Result::Ok) && m_state.outcome == Outcome::Downloaded)
            {
                m_installPending = true;
                m_toldWaiting = false;
                InstallWhenIdle();
            }
            else
                m_hooks.notify(std::format(L"Couldn’t download " SD_NAME L" {}", v), m_state.error.empty() ? L"See the log for details." : m_state.error, true);
            break;
        case Job::None:
            break;
        }
        if (m_hooks.changed)
            m_hooks.changed();
    }

    void UpdateController::CheckNow()
    {
        Launch(Job::ManualCheck, UpdaterPath(), std::format(L"{} {}", kCheckArg, kManualArg));
    }

    void UpdateController::Offer()
    {
        if (m_offering || m_installPending || m_job == Job::Download || !HasUpdate(m_state, m_running))
            return;
        // The question window (AskWindow): the release, its notes, Update now and Later.
        ask::Question q;
        q.title = L"" SD_NAME L" update";
        q.heading = std::format(L"{} is available", m_state.release.name);
        q.text = std::format(L"You have {}. Updating restarts " SD_NAME L" and keeps your settings and history.", ToText(m_running));
        if (const auto notes = PlainNotes(m_state.release.body); !notes.empty())
            q.text += L"\n\n" + notes;
        q.choices = { { kUpdateNow, L"Update now", L"Downloads it, checks it against the release’s SHA-256 and installs it" } };
        q.cancel = L"Later";
        m_offering = true;
        const int answer = ask::ChooseNow(nullptr, q);
        m_offering = false;
        log::Info(L"updates: {} offered: {}", ToText(m_state.release.version), answer == kUpdateNow ? L"update now" : L"later");
        if (answer == kUpdateNow)
            UpdateNow();
    }

    void UpdateController::UpdateNow()
    {
        if (m_job != Job::None || m_installPending || !HasUpdate(m_state, m_running))
            return;
        const std::wstring v = ToText(m_state.release.version);
        // Already downloaded and checked (Install automatically, or an earlier Update now).
        if (m_state.outcome == Outcome::Downloaded && IsFile(m_state.setupPath))
        {
            m_installPending = true;
            m_toldWaiting = false;
            InstallWhenIdle();
            return;
        }
        if (Launch(Job::Download, UpdaterPath(), kDownloadArg))
            m_hooks.notify(std::format(L"Downloading " SD_NAME L" {}", v), L"" SD_NAME L" restarts by itself once it is installed.", false);
    }

    void UpdateController::InstallWhenIdle()
    {
        const std::wstring v = m_state.hasRelease ? ToText(m_state.release.version) : L"";
        if ((m_hooks.busy && m_hooks.busy()) || WorkWindowOpen())
        {
            if (!m_toldWaiting)
                m_hooks.notify(std::format(L"" SD_NAME L" {} is ready", v), L"It installs as soon as the current job is done and no editor is open.", false);
            m_toldWaiting = true;
            Arm(kIdleTickMs);
            return;
        }
        m_installPending = false;
        Arm(kTickMs);
        // The updater copy in the download folder runs Setup: the one in the install folder is replaced.
        const std::wstring updater = Folder(m_state.setupPath) + L"\\" + kUpdaterExe;
        std::wstring line = Quote(updater) + L" " + InstallArgs(m_state.setupPath, GetCurrentProcessId(), ThisExe());
        STARTUPINFOW si{ sizeof(si) };
        PROCESS_INFORMATION pi{};
        if (!IsFile(updater) || !CreateProcessW(updater.c_str(), line.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi))
        {
            log::Error(L"updates: couldn't start the install from {} ({})", updater, GetLastError());
            m_hooks.notify(std::format(L"Couldn’t install " SD_NAME L" {}", v), L"The download is incomplete. Try Check now in Settings.", true);
            if (m_hooks.changed)
                m_hooks.changed();
            return;
        }
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        log::Info(L"updates: installing {}; quitting so Setup can replace the files", v);
        m_hooks.quit();
    }

    std::wstring UpdateController::StatusLine() const
    {
        const std::wstring v = m_state.hasRelease ? ToText(m_state.release.version) : L"";
        if (m_job == Job::DailyCheck || m_job == Job::ManualCheck)
            return L"Checking…";
        if (m_job == Job::Download)
            return std::format(L"Downloading {}…", v);
        if (m_installPending)
            return std::format(L"{} is ready. It installs once the current job is done.", v);
        return ResultLine(m_state, m_running);
    }

    std::wstring UpdateController::CheckedLine() const
    {
        if (m_state.checkedLocal.empty())
            return L"Never checked";
        return L"Last checked: " + history::WhenText(m_state.checkedLocal, history::LocalNow());
    }

    bool UpdateController::CanUpdate() const
    {
        return !m_installPending && m_job != Job::Download && HasUpdate(m_state, m_running);
    }

    std::wstring UpdateController::TrayText() const
    {
        if (!CanUpdate())
            return {};
        return std::format(L"Update to {}…", ToText(m_state.release.version));
    }
}
