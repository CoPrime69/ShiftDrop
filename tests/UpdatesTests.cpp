#include "FileIo.h"
#include "Hash.h"
#include "SettingsPages.h"
#include "TestUtil.h"
#include "updates/Updates.h"

#include <windows.h>
#include <shellapi.h>

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace sd;
using namespace sd::updates;

namespace
{
    // GitHub's answer to GET /repos/CoPrime69/ShiftDrop/releases/latest for v0.6.0, saved (the author,
    // uploader and most of the notes left out).
    constexpr char kLatest[] = R"json({
  "url": "https://api.github.com/repos/CoPrime69/ShiftDrop/releases/257347315",
  "html_url": "https://github.com/CoPrime69/ShiftDrop/releases/tag/v0.6.0",
  "id": 257347315,
  "tag_name": "v0.6.0",
  "target_commitish": "92f4331",
  "name": "ShiftDrop 0.6.0",
  "draft": false,
  "immutable": false,
  "prerelease": false,
  "created_at": "2026-10-22T09:46:01Z",
  "updated_at": "2026-10-22T09:50:42Z",
  "published_at": "2026-10-22T09:50:42Z",
  "assets": [
    {
      "name": "SHA256SUMS.txt",
      "content_type": "text/plain",
      "state": "uploaded",
      "size": 196,
      "digest": "sha256:d41b11bd7804f5a9490bb6b99a92a312b5437ee8ed304eab0c0b7a40a2cdd8d9",
      "browser_download_url": "https://github.com/CoPrime69/ShiftDrop/releases/download/v0.6.0/SHA256SUMS.txt"
    },
    {
      "name": "ShiftDrop-0.6.0-ffmpeg-sources.tar.xz",
      "size": 31393788,
      "digest": "sha256:f1535d29b0ee4b8da4a6c4a35a0a69694f1e983b06b83ab814f59ab1ce3a77be",
      "browser_download_url": "https://github.com/CoPrime69/ShiftDrop/releases/download/v0.6.0/ShiftDrop-0.6.0-ffmpeg-sources.tar.xz"
    },
    {
      "name": "ShiftDrop-Setup-0.6.0.exe",
      "content_type": "application/x-msdownload",
      "size": 15578809,
      "digest": "sha256:5E3F30C514480EACDB9D9529486C18AA3AB267FC65B0BF50E8B2898BDA8C8E08",
      "browser_download_url": "https://github.com/CoPrime69/ShiftDrop/releases/download/v0.6.0/ShiftDrop-Setup-0.6.0.exe"
    }
  ],
  "tarball_url": "https://api.github.com/repos/CoPrime69/ShiftDrop/tarball/v0.6.0",
  "zipball_url": "https://api.github.com/repos/CoPrime69/ShiftDrop/zipball/v0.6.0",
  "body": "Download **ShiftDrop-Setup-0.6.0.exe** and run it. It installs for your Windows user only and needs no admin rights.\n\n## What's in this build\n\n- CI: publish the release when main reuses the pull-request build (#42)\n- Settings: an accent colour, with presets, a colour of your own or the Windows one (#57)\n- Float shelf stays on screen, takes drops, and has `Clear all` and a [close button](https://github.com/CoPrime69/ShiftDrop/pull/58) (#58)\n\nThe installer isn't code-signed yet, so Windows SmartScreen may warn the first time you run it (\"Windows protected your PC\"): choose **More info**, then **Run anyway**.\n\nSHA-256 of ShiftDrop-Setup-0.6.0.exe: `5e3f30c514480eacdb9d9529486c18aa3ab267fc65b0bf50e8b2898bda8c8e08`. SHA256SUMS.txt lists every file in this release."
})json";

    constexpr int64_t kPublished = 1792662642;   // 2026-10-22T09:50:42Z
    constexpr int64_t kDay = 24 * 3600;

    Version V(const wchar_t* text)
    {
        const auto v = ParseVersion(text);
        EXPECT_TRUE(v) << Narrow(text);
        return v.value_or(Version{});
    }

    Release Newer(const wchar_t* version, int64_t published)
    {
        Release r;
        r.version = V(version);
        r.published = published;
        return r;
    }
}

TEST(Updates, VersionsCompareAsSemver)
{
    EXPECT_GT(Compare(V(L"v0.7.1"), V(L"0.7.0")), 0);
    EXPECT_GT(Compare(V(L"0.10.0"), V(L"0.9.9")), 0);
    EXPECT_GT(Compare(V(L"1.0.0"), V(L"0.99.99")), 0);
    EXPECT_EQ(Compare(V(L"0.7.0"), V(L"V0.7.0")), 0);
    // A main or pull-request build counts as its base version.
    EXPECT_EQ(Compare(V(L"0.6.0+b950d51"), V(L"0.6.0")), 0);
    EXPECT_EQ(ToText(V(L"0.6.0+b950d51")), L"0.6.0");
    // A pre-release comes before its release; numbers by value.
    EXPECT_LT(Compare(V(L"0.7.0-rc.1"), V(L"0.7.0")), 0);
    EXPECT_LT(Compare(V(L"0.7.0-rc.1"), V(L"0.7.0-rc.2")), 0);
    EXPECT_LT(Compare(V(L"0.7.0-rc.2"), V(L"0.7.0-rc.10")), 0);
    EXPECT_LT(Compare(V(L"0.7.0-1"), V(L"0.7.0-alpha")), 0);
    EXPECT_LT(Compare(V(L"0.7.0-rc"), V(L"0.7.0-rc.1")), 0);
    EXPECT_EQ(ToText(V(L"0.7.0-rc.1")), L"0.7.0-rc.1");
    for (const wchar_t* bad : { L"", L"0.7", L"1.2.3.4", L"a.b.c", L"0.7.1-", L"0..1", L"-1.0.0", L"0.7.x", L"latest", L"0.7.1-rc!" })
        EXPECT_FALSE(ParseVersion(bad)) << Narrow(bad);
}

TEST(Updates, RunningVersionIsThisBuilds)
{
    const auto running = ParseVersion(RunningVersion());
    ASSERT_TRUE(running);
    EXPECT_EQ(Compare(RunningBase(), *running), 0);
    EXPECT_TRUE(RunningBase().pre.empty()) << "a build of ShiftDrop is never a pre-release";
}

TEST(Updates, ParsesTheReleaseGitHubSends)
{
    std::wstring error;
    const auto r = ParseRelease(kLatest, &error);
    ASSERT_TRUE(r) << Narrow(error);
    EXPECT_EQ(ToText(r->version), L"0.6.0");
    EXPECT_EQ(r->tag, L"v0.6.0");
    EXPECT_EQ(r->name, L"ShiftDrop 0.6.0");
    EXPECT_EQ(r->published, kPublished);
    EXPECT_EQ(r->setup.name, L"ShiftDrop-Setup-0.6.0.exe");
    EXPECT_EQ(r->setup.url, L"https://github.com/CoPrime69/ShiftDrop/releases/download/v0.6.0/ShiftDrop-Setup-0.6.0.exe");
    EXPECT_EQ(r->setup.size, 15578809u);
    EXPECT_EQ(r->setup.sha256, "5e3f30c514480eacdb9d9529486c18aa3ab267fc65b0bf50e8b2898bda8c8e08") << "GitHub's digest, lower case";
    EXPECT_EQ(r->sums.url, L"https://github.com/CoPrime69/ShiftDrop/releases/download/v0.6.0/SHA256SUMS.txt");
    EXPECT_TRUE(IsReleaseDownload(r->setup.url));
    EXPECT_TRUE(IsReleaseDownload(r->sums.url));
    EXPECT_NE(r->body.find("What's in this build"), std::string::npos);
}

TEST(Updates, RefusesWhatIsNotARelease)
{
    std::string json(kLatest);
    const auto with = [&](const std::string& from, const std::string& to) {
        auto j = json;
        j.replace(j.find(from), from.size(), to);
        return j;
    };
    std::wstring error;
    EXPECT_FALSE(ParseRelease("", &error));
    EXPECT_FALSE(error.empty());
    EXPECT_FALSE(ParseRelease("<html>rate limited</html>"));
    EXPECT_FALSE(ParseRelease(R"({"message":"Not Found"})"));
    EXPECT_FALSE(ParseRelease("[]"));
    EXPECT_FALSE(ParseRelease(with(R"("prerelease": false)", R"("prerelease": true)")));
    EXPECT_FALSE(ParseRelease(with(R"("draft": false)", R"("draft": true)")));
    EXPECT_FALSE(ParseRelease(with(R"("tag_name": "v0.6.0")", R"("tag_name": "v0.7.0-rc.1")")));
    EXPECT_FALSE(ParseRelease(with(R"("tag_name": "v0.6.0")", R"("tag_name": "nightly")")));
    EXPECT_FALSE(ParseRelease(with(R"("published_at": "2026-10-22T09:50:42Z")", R"("published_at": null)")));
    // A value of the wrong type is no release either (and throws nothing).
    EXPECT_FALSE(ParseRelease(with(R"("draft": false)", R"("draft": "no")")));
    // A release without the installer parses, with nothing to download.
    const auto noSetup = ParseRelease(with("ShiftDrop-Setup-0.6.0.exe\",\n      \"content_type", "ShiftDrop-Setup-0.5.0.exe\",\n      \"content_type"));
    ASSERT_TRUE(noSetup);
    EXPECT_TRUE(noSetup->setup.url.empty());
}

TEST(Updates, ReadsGitHubsTimes)
{
    EXPECT_EQ(ParseTime("1970-01-01T00:00:00Z"), 0);
    EXPECT_EQ(ParseTime("2026-10-22T09:50:42Z"), kPublished);
    EXPECT_EQ(ParseTime("2026-10-22T09:50:42.123Z"), kPublished);
    for (const char* bad : { "", "2026-10-22", "2026-10-22T09:50:42", "2026-10-22 09:50:42Z", "2026-13-01T00:00:00Z", "2026-02-30T00:00:00Z", "2026-10-22T25:00:00Z", "x026-10-22T09:50:42Z" })
        EXPECT_FALSE(ParseTime(bad)) << bad;
}

// The daily check offers a release once it is a week old; Check now at once.
TEST(Updates, AReleaseIsOfferedAfterSevenDays)
{
    const Version running = V(L"0.7.0+abc1234");
    const auto r = Newer(L"0.7.1", kPublished);
    EXPECT_EQ(Judge(running, r, kPublished, false), Verdict::Settling);
    EXPECT_EQ(Judge(running, r, kPublished + 6 * kDay, false), Verdict::Settling);
    EXPECT_EQ(Judge(running, r, kPublished + 7 * kDay - 1, false), Verdict::Settling);
    EXPECT_EQ(Judge(running, r, kPublished + 7 * kDay, false), Verdict::Available);
    EXPECT_EQ(Judge(running, r, kPublished + 30 * kDay, false), Verdict::Available);
    EXPECT_EQ(Judge(running, r, kPublished, true), Verdict::Available) << "Check now ignores the 7 days";
    // Not newer: never offered.
    EXPECT_EQ(Judge(V(L"0.7.1"), r, kPublished + 30 * kDay, true), Verdict::UpToDate);
    EXPECT_EQ(Judge(V(L"0.7.1+abc1234"), r, kPublished + 30 * kDay, false), Verdict::UpToDate);
    EXPECT_EQ(Judge(V(L"0.8.0"), r, kPublished + 30 * kDay, true), Verdict::UpToDate);
}

TEST(Updates, FindsTheInstallersSum)
{
    // As the release job writes it: sha256sum's format, "\n" line ends.
    const std::string sums =
        "5e3f30c514480eacdb9d9529486c18aa3ab267fc65b0bf50e8b2898bda8c8e08  ShiftDrop-Setup-0.6.0.exe\n"
        "f1535d29b0ee4b8da4a6c4a35a0a69694f1e983b06b83ab814f59ab1ce3a77be  ShiftDrop-0.6.0-ffmpeg-sources.tar.xz\n";
    EXPECT_EQ(FindSum(sums, L"ShiftDrop-Setup-0.6.0.exe"), "5e3f30c514480eacdb9d9529486c18aa3ab267fc65b0bf50e8b2898bda8c8e08");
    EXPECT_EQ(FindSum(sums, L"ShiftDrop-0.6.0-ffmpeg-sources.tar.xz"), "f1535d29b0ee4b8da4a6c4a35a0a69694f1e983b06b83ab814f59ab1ce3a77be");
    // A missing entry, a name that only starts the same, another version's.
    EXPECT_FALSE(FindSum(sums, L"ShiftDrop-Setup-0.7.0.exe"));
    EXPECT_FALSE(FindSum(sums, L"ShiftDrop-Setup-0.6.0"));
    EXPECT_FALSE(FindSum(sums, L"shiftdrop-setup-0.6.0.exe"));
    // Binary mode, CRLF and upper-case hex are read; a short or non-hex hash is not.
    EXPECT_EQ(FindSum("5E3F30C514480EACDB9D9529486C18AA3AB267FC65B0BF50E8B2898BDA8C8E08 *a.exe\r\n", L"a.exe"),
              "5e3f30c514480eacdb9d9529486c18aa3ab267fc65b0bf50e8b2898bda8c8e08");
    EXPECT_FALSE(FindSum("5e3f30c514480eacdb9d9529486c18aa3ab267fc65b0bf50e8b2898bda8c8e0  a.exe\n", L"a.exe"));
    EXPECT_FALSE(FindSum("ze3f30c514480eacdb9d9529486c18aa3ab267fc65b0bf50e8b2898bda8c8e08  a.exe\n", L"a.exe"));
    EXPECT_FALSE(FindSum("5e3f30c514480eacdb9d9529486c18aa3ab267fc65b0bf50e8b2898bda8c8e08 a.exe\n", L"a.exe"));
    EXPECT_FALSE(FindSum("", L"a.exe"));
}

// The download is refused unless its hash is the one SHA256SUMS.txt lists.
TEST(Updates, AMismatchedDownloadIsRefused)
{
    test::TempDir dir;
    const auto setup = dir / L"ShiftDrop-Setup-0.7.1.exe";
    ASSERT_TRUE(WriteFileBytes(setup, std::string_view("ShiftDrop setup bytes"), WriteMode::New));
    const std::string actual = Sha256File(setup);
    const std::string good = "db2d1e64d70c091389c7fd438f3e23945eb9f72de22c98be1cd38a44dae7f5ee  ShiftDrop-Setup-0.7.1.exe\n";
    const std::string bad = "db2d1e64d70c091389c7fd438f3e23945eb9f72de22c98be1cd38a44dae7f5ef  ShiftDrop-Setup-0.7.1.exe\n";
    ASSERT_TRUE(FindSum(good, FileName(setup)));
    EXPECT_TRUE(SameSum(actual, *FindSum(good, FileName(setup))));
    EXPECT_TRUE(SameSum(actual, "DB2D1E64D70C091389C7FD438F3E23945EB9F72DE22C98BE1CD38A44DAE7F5EE"));
    EXPECT_FALSE(SameSum(actual, *FindSum(bad, FileName(setup))));
    EXPECT_FALSE(SameSum("", ""));
    EXPECT_FALSE(SameSum(actual, ""));
}

TEST(Updates, OnlyThisRepositorysDownloads)
{
    EXPECT_TRUE(IsReleaseDownload(L"https://github.com/CoPrime69/ShiftDrop/releases/download/v0.7.1/ShiftDrop-Setup-0.7.1.exe"));
    for (const wchar_t* bad : { L"http://github.com/CoPrime69/ShiftDrop/releases/download/v0.7.1/ShiftDrop-Setup-0.7.1.exe",
                                L"https://github.com/someone/ShiftDrop/releases/download/v0.7.1/ShiftDrop-Setup-0.7.1.exe",
                                L"https://github.com.evil.example/CoPrime69/ShiftDrop/releases/download/v0.7.1/a.exe",
                                L"https://github.com/CoPrime69/ShiftDrop/releases/download/",
                                L"https://github.com/CoPrime69/ShiftDrop/releases/download/v0.7.1/../../x.exe",
                                L"https://github.com/CoPrime69/ShiftDrop/releases/download/v0.7.1/a.exe?x=1",
                                L"https://github.com/CoPrime69/ShiftDrop/releases/download/v0.7.1/sub/a.exe",
                                L"C:\\Users\\me\\ShiftDrop-Setup-0.7.1.exe" })
        EXPECT_FALSE(IsReleaseDownload(bad)) << Narrow(bad);
}

TEST(Updates, TheNotesAsPlainText)
{
    const auto r = ParseRelease(kLatest);
    ASSERT_TRUE(r);
    const auto notes = PlainNotes(r->body);
    EXPECT_EQ(notes.find(L"**"), std::wstring::npos);
    EXPECT_EQ(notes.find(L"##"), std::wstring::npos);
    EXPECT_EQ(notes.find(L"`"), std::wstring::npos);
    EXPECT_EQ(notes.find(L"](https"), std::wstring::npos);
    EXPECT_NE(notes.find(L"Download ShiftDrop-Setup-0.6.0.exe and run it."), std::wstring::npos) << Narrow(notes);
    EXPECT_NE(notes.find(L"\n\nWhat's in this build\n\n\u2022 CI: publish"), std::wstring::npos) << Narrow(notes);
    EXPECT_NE(notes.find(L"has Clear all and a close button (#58)"), std::wstring::npos) << Narrow(notes);
    // At most so many lines, then an ellipsis; nothing for nothing.
    EXPECT_EQ(PlainNotes("- one\n- two\n- three\n- four", 2), L"\u2022 one\n\u2022 two\n\u2026");
    EXPECT_EQ(PlainNotes("# Title\n\n\n\ntext ![shot](a.png)\n#\n"), L"Title\n\ntext");
    EXPECT_EQ(PlainNotes(""), L"");
}

TEST(Updates, TheSettingDefaultsToAskFirst)
{
    test::TempDir dir;
    const auto ini = dir / L"settings.ini";
    EXPECT_EQ(kDefaultMode, Mode::Ask) << "Prakhar's choice: on, asking first";
    EXPECT_EQ(LoadMode(ini), Mode::Ask) << "a new install";
    for (const auto mode : { Mode::Automatic, Mode::Off, Mode::Ask })
    {
        SaveMode(mode, ini);
        EXPECT_EQ(LoadMode(ini), mode);
    }
    WritePrivateProfileStringW(L"Updates", L"Mode", L"sometimes", ini.c_str());
    EXPECT_EQ(LoadMode(ini), Mode::Ask) << "anything else reads as the default";
    WritePrivateProfileStringW(L"Updates", L"Mode", L"OFF", ini.c_str());
    EXPECT_EQ(LoadMode(ini), Mode::Off);
    // The labels Settings shows, in Mode's order.
    const auto labels = settings::Options(settings::Item::UpdateMode);
    ASSERT_EQ(labels.size(), 3u);
    EXPECT_EQ(std::wstring(labels[static_cast<size_t>(Mode::Automatic)]), L"Install automatically");
    EXPECT_EQ(std::wstring(labels[static_cast<size_t>(Mode::Ask)]), L"Ask me first");
    EXPECT_EQ(std::wstring(labels[static_cast<size_t>(Mode::Off)]), L"Off");

    EXPECT_EQ(LoadLastCheck(ini), 0);
    SaveLastCheck(kPublished, ini);
    EXPECT_EQ(LoadLastCheck(ini), kPublished);
    EXPECT_TRUE(LoadTestFeed(ini).empty());
}

// Off never starts the updater on its own; the others once a day.
TEST(Updates, OffNeverChecksByItself)
{
    for (const int64_t last : { int64_t{ 0 }, kPublished - 400 * kDay, kPublished - kDay, kPublished, kPublished + kDay })
        EXPECT_FALSE(DailyCheckDue(Mode::Off, last, kPublished)) << last;
    for (const auto mode : { Mode::Ask, Mode::Automatic })
    {
        EXPECT_TRUE(DailyCheckDue(mode, 0, kPublished)) << "never checked";
        EXPECT_FALSE(DailyCheckDue(mode, kPublished - kDay + 1, kPublished)) << "less than a day ago";
        EXPECT_FALSE(DailyCheckDue(mode, kPublished, kPublished)) << "just now";
        EXPECT_TRUE(DailyCheckDue(mode, kPublished - kDay, kPublished)) << "a day ago";
        EXPECT_TRUE(DailyCheckDue(mode, kPublished + kDay, kPublished)) << "the clock went back";
    }
}

TEST(Updates, UpdateJsonRoundTrips)
{
    State s;
    s.outcome = Outcome::Downloaded;
    s.checked = kPublished + 8 * kDay;
    s.checkedLocal = L"2026-10-30 14:05:00";
    s.hasRelease = true;
    s.release = *ParseRelease(kLatest);
    s.setupPath = L"C:\\Users\\Zoë\\AppData\\Local\\Temp\\ShiftDrop-update\\0.6.0\\ShiftDrop-Setup-0.6.0.exe";
    s.setupSha256 = "5e3f30c514480eacdb9d9529486c18aa3ab267fc65b0bf50e8b2898bda8c8e08";
    s.fromVersion = L"0.5.0";
    s.told = true;
    const auto back = ParseState(StateJson(s));
    ASSERT_TRUE(back);
    EXPECT_EQ(back->outcome, Outcome::Downloaded);
    EXPECT_EQ(back->checked, s.checked);
    EXPECT_EQ(back->checkedLocal, s.checkedLocal);
    EXPECT_EQ(back->setupPath, s.setupPath);
    EXPECT_EQ(back->setupSha256, s.setupSha256);
    EXPECT_EQ(back->fromVersion, L"0.5.0");
    EXPECT_TRUE(back->told);
    ASSERT_TRUE(back->hasRelease);
    EXPECT_EQ(ToText(back->release.version), L"0.6.0");
    EXPECT_EQ(back->release.name, s.release.name);
    EXPECT_EQ(back->release.body, s.release.body);
    EXPECT_EQ(back->release.published, kPublished);
    EXPECT_EQ(back->release.setup.url, s.release.setup.url);
    EXPECT_EQ(back->release.setup.sha256, s.release.setup.sha256);
    EXPECT_EQ(back->release.sums.url, s.release.sums.url);

    test::TempDir dir;
    const auto path = dir / L"update.json";
    EXPECT_EQ(LoadState(path).outcome, Outcome::None) << "no file yet";
    ASSERT_TRUE(SaveState(s, path));
    EXPECT_EQ(LoadState(path).setupPath, s.setupPath);
    EXPECT_FALSE(PathExists(path + L".tmp"));
    EXPECT_FALSE(ParseState("not json"));
    EXPECT_FALSE(ParseState(R"({"checked":"yesterday"})")) << "a damaged file is as if there were none";
}

TEST(Updates, WhatSettingsAndTheTraySay)
{
    const Version running = V(L"0.7.0");
    State s;
    EXPECT_EQ(ResultLine(s, running), L"Not checked yet");
    s.outcome = Outcome::CheckFailed;
    EXPECT_EQ(ResultLine(s, running), L"Couldn\u2019t reach GitHub");
    EXPECT_FALSE(HasUpdate(s, running));
    s.outcome = Outcome::UpToDate;
    s.hasRelease = true;
    s.release = Newer(L"0.7.0", kPublished);
    EXPECT_EQ(ResultLine(s, running), L"You have the latest version");
    EXPECT_FALSE(HasUpdate(s, running));
    s.outcome = Outcome::Available;
    s.release = Newer(L"0.7.1", kPublished);
    EXPECT_EQ(ResultLine(s, running), L"0.7.1 is available");
    EXPECT_TRUE(HasUpdate(s, running));
    s.outcome = Outcome::Settling;
    EXPECT_NE(ResultLine(s, running).find(L"0.7.1 is out"), std::wstring::npos);
    EXPECT_FALSE(HasUpdate(s, running)) << "not offered before its week is up";
    s.outcome = Outcome::DownloadFailed;
    s.error = L"The download doesn\u2019t match its SHA-256 in SHA256SUMS.txt";
    EXPECT_NE(ResultLine(s, running).find(s.error), std::wstring::npos);
    EXPECT_TRUE(HasUpdate(s, running)) << "it can be tried again";
    // After the update the same file says this build is the latest.
    s.outcome = Outcome::Installed;
    EXPECT_EQ(ResultLine(s, V(L"0.7.1")), L"Updated to 0.7.1. You have the latest version");
    EXPECT_FALSE(HasUpdate(s, V(L"0.7.1")));
}

TEST(Updates, TheInstallCommandLineReadsBack)
{
    const std::wstring setup = L"C:\\Users\\A B\\AppData\\Local\\Temp\\ShiftDrop-update\\0.7.1\\ShiftDrop-Setup-0.7.1.exe";
    const std::wstring app = L"C:\\Users\\A B\\AppData\\Local\\Programs\\ShiftDrop\\ShiftDrop.exe";
    const std::wstring line = L"updater.exe " + InstallArgs(setup, 4242, app);
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(line.c_str(), &argc);
    ASSERT_NE(argv, nullptr);
    std::vector<std::wstring> args(argv, argv + argc);
    LocalFree(argv);
    EXPECT_EQ(args, (std::vector<std::wstring>{ L"updater.exe", kInstallArg, setup, kWaitArg, L"4242", kAppArg, app }));
    // Quotes and trailing backslashes survive too.
    for (const std::wstring& tricky : { std::wstring(L"C:\\a dir\\"), std::wstring(L"say \"hi\""), std::wstring(L"plain"), std::wstring(L"") })
    {
        const std::wstring l = L"x.exe " + Quote(tricky);
        argv = CommandLineToArgvW(l.c_str(), &argc);
        ASSERT_EQ(argc, 2) << Narrow(l);
        EXPECT_EQ(std::wstring(argv[1]), tricky) << Narrow(l);
        LocalFree(argv);
    }
    EXPECT_EQ(SetupArgs(L"C:\\t\\s.log"), L"/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /LOG=C:\\t\\s.log");
    EXPECT_EQ(SetupName(V(L"0.7.1")), L"ShiftDrop-Setup-0.7.1.exe");
}

TEST(Updates, SettingsHasAnUpdatesPage)
{
    using settings::Item;
    using settings::Page;
    EXPECT_EQ(settings::OnPage(Page::Updates), (std::vector<Item>{ Item::UpdateMode, Item::CheckUpdates }));
    EXPECT_EQ(std::wstring(settings::PageTitle(Page::Updates)), L"Updates");
    const auto found = settings::Search(L"update");
    ASSERT_FALSE(found.empty());
    EXPECT_EQ(found.front(), Item::UpdateMode);
    EXPECT_EQ(settings::Search(L"check now"), std::vector<Item>{ Item::CheckUpdates });
    EXPECT_EQ(settings::Search(L"install automatically"), std::vector<Item>{ Item::UpdateMode });
    EXPECT_EQ(settings::Search(L"upgrade").front(), Item::UpdateMode);
}
