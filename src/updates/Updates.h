#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

// Check for updates: what both ShiftDrop.exe and ShiftDrop-Updater.exe need to agree on. The app never
// goes online; it starts the updater (installed next to it) and reads what the updater found from
// update.json. Everything here is plain logic over text, files and settings.ini, so it is unit-tested
// and links into the app without any networking code (scripts/check-imports.ps1 checks that).
//
//   ShiftDrop.exe                                       ShiftDrop-Updater.exe
//   once a day (not Off) ──── --check [--download] ──►  GET the latest release, apply the 7 days
//   Settings › Check now ──── --check --manual ──────►  the same, without the 7 days
//   Update now ────────────── --download ────────────►  installer + SHA256SUMS.txt, hash checked
//   idle, then quits ───────── --install (from %TEMP%) ► waits for it, runs Setup silently, starts it
//   reads update.json  ◄────────────────────────────── writes update.json, exits with a Result
namespace sd::updates
{
    // ---- Versions ---------------------------------------------------------------------------------

    // A Semantic Versioning 2.0.0 version: major.minor.patch, an optional "-pre-release" and an
    // optional "+build", which is dropped (it never takes part in the order). A main or pull-request
    // build is a pre-release of the next version, "0.7.1-dev.3+af953b2", so it sorts above the release
    // it follows (0.7.0) and below the one it leads to (0.7.1).
    struct Version
    {
        int major = 0, minor = 0, patch = 0;
        std::wstring pre;   // "dev.3" of "0.7.1-dev.3+af953b2"; empty for a release
    };
    // "0.7.1", "v0.7.1", "0.7.1-dev.3+af953b2", "1.0.0-rc.1"; nothing for anything SemVer 2.0.0 rejects
    // (leading zeros in a number, an empty or non-ASCII identifier, four numbers...).
    std::optional<Version> ParseVersion(std::wstring_view text);
    // Negative, 0 or positive, by SemVer's precedence: numbers in order, then a pre-release before its
    // release, then the pre-release identifiers one by one (numbers by value and before words, words
    // in ASCII order, fewer identifiers first when the rest is equal).
    int Compare(const Version& a, const Version& b);
    std::wstring ToText(const Version& v);   // "0.7.1", "0.7.1-dev.3" (no build part)

    // The version of this build as About shows it, and the version it is for updates (the same without
    // its +build). RunningVersion() is the one place that reads the build's version (see Updates.cpp).
    std::wstring RunningVersion();
    Version Running();

    // ---- The release --------------------------------------------------------------------------------

    struct Asset
    {
        std::wstring name, url;
        uint64_t size = 0;
        std::string sha256;   // GitHub's own "digest" of the file, lower-case hex, when it gives one
    };
    struct Release
    {
        Version version;
        std::wstring tag, name, htmlUrl;
        std::string body;            // the notes as written (Markdown)
        int64_t published = 0;       // published_at, seconds since 1970 (UTC)
        Asset setup, sums;           // ShiftDrop-Setup-<version>.exe and SHA256SUMS.txt; empty url: missing
    };

    // GitHub's answer to GET /repos/CoPrime69/ShiftDrop/releases/latest. Nothing (with `error`) when
    // it isn't one: not JSON, no version tag, a draft or a pre-release, no published time.
    std::optional<Release> ParseRelease(std::string_view json, std::wstring* error = nullptr);
    std::wstring SetupName(const Version& v);   // "ShiftDrop-Setup-0.7.1.exe", as the release job names it
    inline constexpr wchar_t kSumsName[] = L"SHA256SUMS.txt";
    // "2026-10-22T09:50:42Z" as seconds since 1970; nothing when it isn't that shape.
    std::optional<int64_t> ParseTime(std::string_view iso);

    // Where the updater may go: the releases API and this repository's release downloads, over
    // https. (GitHub then redirects a download to its file host; WinHTTP follows that, https only.)
    inline constexpr wchar_t kLatestUrl[] = L"https://api.github.com/repos/CoPrime69/ShiftDrop/releases/latest";
    bool IsReleaseDownload(std::wstring_view url);
    // An answer that may well be different a moment later (a timeout, too many requests, a server
    // error such as GitHub's 502/503/504), so worth asking again.
    bool IsWorthRetrying(unsigned long status);
    // How many times a request is tried, and the wait before each retry.
    inline constexpr int kTries = 3;
    inline constexpr unsigned long kRetryWaitMs[] = { 2000, 5000 };

    // ---- Is it offered? -----------------------------------------------------------------------------

    // The daily check offers a release only once it has been out this long, so a bad release can be
    // pulled before it reaches everyone. Check now doesn't wait.
    inline constexpr int64_t kSettleSeconds = 7 * 24 * 3600;
    enum class Verdict
    {
        UpToDate,    // not newer than this build
        Available,   // newer, and offered
        Settling,    // newer, but out for less than kSettleSeconds (daily check only)
    };
    Verdict Judge(const Version& running, const Release& release, int64_t now, bool manual);

    // ---- SHA256SUMS.txt -----------------------------------------------------------------------------

    // The hash `sums` (sha256sum's format: "<64 hex>  <name>" per line, "*" before a binary name)
    // lists for `name`, lower-case; nothing when it has no well-formed line for it.
    std::optional<std::string> FindSum(std::string_view sums, std::wstring_view name);
    // Does `actual` (Sha256File's answer) equal `expected`, ignoring case? Never for an empty one.
    bool SameSum(std::string_view actual, std::string_view expected);

    // ---- The notes ----------------------------------------------------------------------------------

    // The release notes as plain text for the update window: headings, bold, code and links lose their
    // Markdown, "- " items become "• " lines, blank runs collapse. At most `maxLines` lines, then "…".
    std::wstring PlainNotes(std::string_view markdown, size_t maxLines = 14);

    // ---- The setting --------------------------------------------------------------------------------

    // Settings › Updates › When an update is available: [Updates] Mode=auto|ask|off.
    enum class Mode { Automatic, Ask, Off };
    // Prakhar's choice (2026-10-22): on, asking first. A new install and any unknown value read as it.
    inline constexpr Mode kDefaultMode = Mode::Ask;
    Mode LoadMode(const std::wstring& settingsIni);
    void SaveMode(Mode mode, const std::wstring& settingsIni);

    // When ShiftDrop last started the daily check, seconds since 1970 ([Updates] LastCheck); 0: never.
    int64_t LoadLastCheck(const std::wstring& settingsIni);
    void SaveLastCheck(int64_t when, const std::wstring& settingsIni);
    inline constexpr int64_t kCheckEverySeconds = 24 * 3600;
    // Does the running app start the daily check now? Never when Off; once a day otherwise (and again
    // if the clock went back past the last one).
    bool DailyCheckDue(Mode mode, int64_t lastCheck, int64_t now);

    // For testing an update before a real one exists ([Updates] TestFeed): a release JSON file on this
    // PC, read instead of GitHub's. Its assets may then be files on this PC too. Empty: none.
    std::wstring LoadTestFeed(const std::wstring& settingsIni);

    // ---- What the updater found: %LOCALAPPDATA%\ShiftDrop\update.json -------------------------------

    enum class Outcome
    {
        None,
        UpToDate,        // checked: nothing newer
        Available,       // checked: `release` is newer (and offered)
        Settling,        // checked: `release` is newer but less than 7 days old
        CheckFailed,     // couldn't reach GitHub, or its answer was not a release
        Downloaded,      // `setupPath` holds the verified installer
        DownloadFailed,  // the download or its hash check failed (`error` says which)
        Installed,       // Setup finished; ShiftDrop was started again
        InstallFailed,   // Setup failed; the old ShiftDrop was started again
    };
    struct State
    {
        Outcome outcome = Outcome::None;
        std::wstring error;          // a short reason for a failure, for people
        int64_t checked = 0;         // when the last check ran, seconds since 1970
        std::wstring checkedLocal;   // the same in local time, "2026-10-22 14:05:00" (history::WhenText reads it)
        bool hasRelease = false;
        Release release;             // the newest release the last check saw
        std::wstring setupPath;      // Downloaded: the verified installer
        std::string setupSha256;     // ...and its SHA-256, checked again just before it runs
        std::wstring fromVersion;    // Installed / InstallFailed: the version that was running before
        bool told = false;           // ...and ShiftDrop has said so since
    };
    std::wstring StatePath();   // %LOCALAPPDATA%\ShiftDrop\update.json
    std::string StateJson(const State& state);
    std::optional<State> ParseState(std::string_view json);
    bool SaveState(const State& state, const std::wstring& path);
    State LoadState(const std::wstring& path);   // a fresh State when there is none

    // Settings › Updates' line under Check now, for `state` and this build: "You have the latest
    // version", "0.7.1 is available", "Couldn't reach GitHub"...
    std::wstring ResultLine(const State& state, const Version& running);
    // Is there an update to offer from what the last check saw (the tray's "Update to 0.7.1…")?
    bool HasUpdate(const State& state, const Version& running);

    // ---- The updater's command line and exit codes --------------------------------------------------

    inline constexpr wchar_t kUpdaterExe[] = L"ShiftDrop-Updater.exe";
    inline constexpr wchar_t kCheckArg[] = L"--check";         // look for a newer release
    inline constexpr wchar_t kManualArg[] = L"--manual";       // ...asked for: no 7 days
    inline constexpr wchar_t kDownloadArg[] = L"--download";   // fetch and verify what update.json offers
    inline constexpr wchar_t kInstallArg[] = L"--install";     // --install <setup> --wait <pid> --app <ShiftDrop.exe>
    inline constexpr wchar_t kWaitArg[] = L"--wait";
    inline constexpr wchar_t kAppArg[] = L"--app";
    enum class Result : int
    {
        Ok = 0,        // up to date, downloaded, or installed
        Available = 1, // --check: a release is offered (with --download: and it is downloaded)
        Failed = 2,    // update.json's error says what
        BadArgs = 3,
    };
    // A command-line argument, quoted for CreateProcess (CommandLineToArgvW reads it back).
    std::wstring Quote(std::wstring_view arg);
    // The --install command line, after the exe's own quoted path.
    std::wstring InstallArgs(const std::wstring& setup, unsigned long pid, const std::wstring& appExe);
    // The installer's silent switches: no wizard, no message boxes, no restart (per user, no admin).
    std::wstring SetupArgs(const std::wstring& logPath);
}
