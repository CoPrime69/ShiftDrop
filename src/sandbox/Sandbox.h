#pragma once
#include <windows.h>
#include <wil/resource.h>

#include <cstdint>
#include <string>
#include <vector>

// Sealed engines: work on untrusted input (archives, and optionally ffmpeg) runs in a child
// process inside an AppContainer with no capabilities. Such a process has no network access and can
// open none of the user's files by name; it works only on the handles we pass it.
//
// When AppContainer is unavailable (an older or locked-down Windows, or Wine), the child still runs
// as a separate low-priority process in a kill-on-close job with the same limits, and the log says so.
namespace sd::sandbox
{
    // The AppContainer profile all sandboxed children share ("ShiftDrop.Sandbox"). Created on first
    // use for this Windows user; RemoveProfile (uninstall) deletes it again.
    struct Container
    {
        PSID sid = nullptr;      // owned by this module; valid for the process lifetime
        std::wstring sidText;    // "S-1-15-2-...", for logs
        std::wstring why;        // why it is unavailable, when sid is null
        bool Available() const { return sid != nullptr; }
    };
    const Container& AppContainer();
    void RemoveProfile();

    // Lets the AppContainer read and run `path` (our worker exe or ffmpeg.exe, or a folder of them).
    // A process can only start in an AppContainer from an image the container may read. With
    // `inherit` the grant also applies to everything in the folder. Idempotent; false on failure.
    bool GrantRead(const std::wstring& path, bool inherit = false);

    struct Limits
    {
        uint64_t memoryBytes = 0;   // committed memory per process; 0 = no limit
        uint64_t cpuSeconds = 0;    // user-mode CPU time per process; 0 = no limit
        bool noChildren = true;     // the child cannot start processes of its own
        bool noUi = true;           // no clipboard, desktop switching, global atoms or other processes' windows
    };

    struct LaunchSpec
    {
        std::wstring exe;
        std::wstring commandLine;        // the whole command line, program name first
        // Handles the child receives, already inheritable. Exactly these are inherited (and the std
        // handles below, which must be among them or null).
        std::vector<HANDLE> inherit;
        HANDLE stdIn = nullptr, stdOut = nullptr, stdErr = nullptr;
        // Optional C runtime file descriptors for the child (index = fd), passed the way the CRT's own
        // spawn does (STARTUPINFO.lpReserved2). ffmpeg reads "fd:" from these. Null entries are closed.
        std::vector<HANDLE> crtFds;
        Limits limits;
        bool appContainer = true;        // false: a plain child (still in the job, with the limits)
        std::wstring workingDir;         // the child's current folder (a plain child only; empty: ours)
        bool hideWindow = false;         // starts its windows hidden (a GUI program run headless: LibreOffice)
        bool mitigations = false;        // extra process mitigations (no extension points, no remote images)
        DWORD priority = BELOW_NORMAL_PRIORITY_CLASS;
    };

    struct Process
    {
        wil::unique_handle process;
        wil::unique_handle job;          // kill-on-close: resetting it ends the child
        bool sandboxed = false;          // runs in the AppContainer
        std::wstring error;
        explicit operator bool() const { return static_cast<bool>(process); }
    };

    // Starts the child suspended, puts it in its job, then lets it run, so it can never outlive us,
    // even for a moment. When the AppContainer is wanted but unavailable, falls back to a plain child
    // and logs why once.
    Process Launch(const LaunchSpec& spec);

    // The STARTUPINFO.lpReserved2 block that hands `fds` to a child's C runtime (exposed for tests).
    std::vector<unsigned char> CrtFdBlock(const std::vector<HANDLE>& fds);

    // The archive worker: <exe folder>\engines\ShiftDropWorker.exe, or %SHIFTDROP_WORKER% if set.
    // Tests point it at their own exe with SetWorkerPath. Empty when there is none.
    std::wstring WorkerPath();
    void SetWorkerPath(std::wstring path);

    // Settings ([Sandbox] in settings.ini). Archives=1 (default): archive parsing in the AppContainer
    // worker. Media=0 (default): ffmpeg runs outside it. Media=1 puts it inside, which works with the
    // CPU encoders (the tests prove it), but GPU encoders (NVENC, Quick Sync, AMF) and -hwaccel need
    // driver access a container with no capabilities may not get, and CI has no GPU to check them
    // on, so it stays off by default (docs/ARCHITECTURE.md, "Sandbox").
    bool ArchiveSandboxEnabled();
    bool MediaSandboxEnabled();
    void SetMediaSandbox(bool on);      // tests, and a future settings window
    void SetArchiveSandbox(bool on);
}
