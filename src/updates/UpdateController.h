#pragma once
#include "updates/Updates.h"

#include <windows.h>

#include <functional>
#include <string>
#include <utility>

namespace sd::updates
{
    // Check for updates, inside ShiftDrop.exe: when to start ShiftDrop-Updater.exe and what to do with
    // what it found (updates::State in update.json). The app itself never goes online.
    //
    // - Once a day (unless Settings › Updates says Off) it starts "--check": Ask me first then shows a
    //   toast and the tray menu's "Update to 0.7.1…"; Install automatically also downloads it and
    //   installs it as soon as no job is running.
    // - Check now: the same without the 7 days' wait; what it finds is offered at once.
    // - Update now (the window, the toast, the tray item): "--download", then, once no job is running and
    //   no editor or question is open, "--install" from the download folder while ShiftDrop quits. The
    //   updater starts ShiftDrop again: the new one, or the old one if Setup failed.
    // UI thread only.
    class UpdateController
    {
    public:
        struct Hooks
        {
            HWND window = nullptr;                // gets msg::UpdaterDone and the timer
            UINT_PTR timer = 0;                   // a timer id of that window's, for this
            std::function<bool()> busy;           // a job, a drag, an editor or a question is under way
            std::function<void(const std::wstring& title, const std::wstring& text, bool warning)> notify;   // a balloon
            std::function<bool(const std::wstring& title, const std::wstring& text)> toast;                    // an update toast; false: none
            std::function<void()> changed;        // what Settings shows changed
            std::function<void()> quit;           // quit now: the updater takes over
        };
        explicit UpdateController(Hooks hooks);
        ~UpdateController();
        UpdateController(const UpdateController&) = delete;
        UpdateController& operator=(const UpdateController&) = delete;

        // At startup: says how an update that just ran went, and arms the daily check.
        void Start();
        void OnTimer();
        // msg::UpdaterDone: the updater started for `job` has exited.
        void OnUpdaterDone(WPARAM job);

        Mode CurrentMode() const { return m_mode; }
        void SetMode(Mode mode);
        void CheckNow();
        void Offer();        // the update window: the release, its notes, Update now and Later
        void UpdateNow();

        bool Checking() const { return m_job != Job::None; }
        // Settings › Updates: the line under Check now, and when it last checked.
        std::wstring StatusLine() const;
        std::wstring CheckedLine() const;
        // The tray menu's "Update to 0.7.1…", or empty when there is nothing to offer.
        std::wstring TrayText() const;
        // There is an update to install and nothing is downloading or waiting to install it.
        bool CanUpdate() const;

    private:
        enum class Job : WPARAM { None, DailyCheck, ManualCheck, Download };
        bool Launch(Job job, const std::wstring& exe, const std::wstring& args);
        void Finished(Job job, DWORD code);
        void InstallWhenIdle();
        void Arm(UINT ms);

        Hooks m_hooks;
        Mode m_mode = kDefaultMode;
        State m_state;
        Version m_running;
        Job m_job = Job::None;
        HANDLE m_process = nullptr;   // the updater running for m_job
        HANDLE m_wait = nullptr;      // RegisterWaitForSingleObject on it
        std::pair<HWND, WPARAM> m_waitContext;   // what its callback posts, and where
        bool m_offering = false;         // the update window is open
        bool m_installPending = false;   // downloaded: install once nothing is going on
        bool m_toldWaiting = false;      // ...and the user was told it waits for a job
    };
}
