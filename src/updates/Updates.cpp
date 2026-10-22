#include "updates/Updates.h"
#include "Brand.h"
#include "FileIo.h"
#include "Log.h"
#include "Settings.h"
#include "Strings.h"
#include "Utf.h"

#include <windows.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cwctype>
#include <format>
#include <vector>

namespace sd::updates
{
    // The one place that reads the build's version. When the generated version header lands, this
    // returns its display string instead (the version and, for a build that isn't the release, its
    // commit); RunningBase() already drops that part.
    std::wstring RunningVersion()
    {
        return SD_VERSION;
    }

    Version RunningBase()
    {
        return ParseVersion(RunningVersion()).value_or(Version{});
    }

    namespace
    {
        // A whole non-negative number with no sign or spaces, small enough for an int.
        bool Number(std::wstring_view text, int& out)
        {
            if (text.empty() || text.size() > 9 || !std::all_of(text.begin(), text.end(), [](wchar_t c) { return c >= L'0' && c <= L'9'; }))
                return false;
            out = 0;
            for (const wchar_t c : text)
                out = out * 10 + (c - L'0');
            return true;
        }

        bool AllDigits(std::wstring_view s)
        {
            return !s.empty() && std::all_of(s.begin(), s.end(), [](wchar_t c) { return c >= L'0' && c <= L'9'; });
        }

        std::vector<std::wstring_view> Split(std::wstring_view s, wchar_t at)
        {
            std::vector<std::wstring_view> parts;
            for (size_t start = 0;;)
            {
                const size_t next = s.find(at, start);
                parts.push_back(s.substr(start, next == std::wstring_view::npos ? std::wstring_view::npos : next - start));
                if (next == std::wstring_view::npos)
                    return parts;
                start = next + 1;
            }
        }
    }

    std::optional<Version> ParseVersion(std::wstring_view text)
    {
        text = Trim(text);
        if (!text.empty() && (text.front() == L'v' || text.front() == L'V'))
            text.remove_prefix(1);
        if (const size_t plus = text.find(L'+'); plus != std::wstring_view::npos)
            text = text.substr(0, plus);   // build metadata: never part of the order
        Version v;
        if (const size_t dash = text.find(L'-'); dash != std::wstring_view::npos)
        {
            v.pre = std::wstring(text.substr(dash + 1));
            text = text.substr(0, dash);
            if (v.pre.empty() || !std::all_of(v.pre.begin(), v.pre.end(), [](wchar_t c) { return std::iswalnum(c) || c == L'.' || c == L'-'; }))
                return std::nullopt;
        }
        const auto parts = Split(text, L'.');
        if (parts.size() != 3 || !Number(parts[0], v.major) || !Number(parts[1], v.minor) || !Number(parts[2], v.patch))
            return std::nullopt;
        return v;
    }

    int Compare(const Version& a, const Version& b)
    {
        if (a.major != b.major)
            return a.major < b.major ? -1 : 1;
        if (a.minor != b.minor)
            return a.minor < b.minor ? -1 : 1;
        if (a.patch != b.patch)
            return a.patch < b.patch ? -1 : 1;
        if (a.pre == b.pre)
            return 0;
        if (a.pre.empty() || b.pre.empty())
            return a.pre.empty() ? 1 : -1;   // 0.7.0-rc.1 < 0.7.0
        // Dot-separated identifiers: numbers by value and before words; words in ASCII order; a
        // shorter list first when the rest is equal.
        const auto x = Split(a.pre, L'.'), y = Split(b.pre, L'.');
        for (size_t i = 0; i < std::min(x.size(), y.size()); ++i)
        {
            const bool nx = AllDigits(x[i]), ny = AllDigits(y[i]);
            if (nx && ny)
            {
                if (x[i].size() != y[i].size())
                    return x[i].size() < y[i].size() ? -1 : 1;
            }
            else if (nx != ny)
                return nx ? -1 : 1;
            if (const int c = x[i].compare(y[i]); c != 0)
                return c < 0 ? -1 : 1;
        }
        return x.size() == y.size() ? 0 : x.size() < y.size() ? -1 : 1;
    }

    std::wstring ToText(const Version& v)
    {
        auto text = std::format(L"{}.{}.{}", v.major, v.minor, v.patch);
        if (!v.pre.empty())
            text += L"-" + v.pre;
        return text;
    }

    // ---- The release ------------------------------------------------------------------------------------

    std::wstring SetupName(const Version& v)
    {
        return L"" SD_NAME L"-Setup-" + ToText(v) + L".exe";
    }

    std::optional<int64_t> ParseTime(std::string_view iso)
    {
        // "YYYY-MM-DDTHH:MM:SSZ" (GitHub writes UTC with a Z); fractions of a second are allowed.
        const auto num = [&](size_t at, size_t n, int& out) {
            if (at + n > iso.size())
                return false;
            const auto r = std::from_chars(iso.data() + at, iso.data() + at + n, out);
            return r.ec == std::errc{} && r.ptr == iso.data() + at + n;
        };
        int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0;
        if (iso.size() < 20 || iso[4] != '-' || iso[7] != '-' || (iso[10] != 'T' && iso[10] != 't') || iso[13] != ':' || iso[16] != ':' ||
            !num(0, 4, y) || !num(5, 2, mo) || !num(8, 2, d) || !num(11, 2, h) || !num(14, 2, mi) || !num(17, 2, s))
            return std::nullopt;
        size_t end = 19;
        if (end < iso.size() && iso[end] == '.')
            for (++end; end < iso.size() && iso[end] >= '0' && iso[end] <= '9';)
                ++end;
        if (end + 1 != iso.size() || (iso[end] != 'Z' && iso[end] != 'z'))
            return std::nullopt;
        const std::chrono::year_month_day day{ std::chrono::year{ y }, std::chrono::month{ static_cast<unsigned>(mo) }, std::chrono::day{ static_cast<unsigned>(d) } };
        if (!day.ok() || h > 23 || mi > 59 || s > 60)
            return std::nullopt;
        const auto days = std::chrono::sys_days{ day }.time_since_epoch().count();
        return static_cast<int64_t>(days) * 86400 + h * 3600 + mi * 60 + s;
    }

    bool IsReleaseDownload(std::wstring_view url)
    {
        constexpr std::wstring_view kPrefix = L"https://github.com/CoPrime69/" SD_NAME L"/releases/download/";
        if (!url.starts_with(kPrefix) || url.size() == kPrefix.size())
            return false;
        // v0.7.1/ShiftDrop-Setup-0.7.1.exe: a tag and a file name, nothing that climbs or adds a query.
        const auto rest = url.substr(kPrefix.size());
        return rest.find(L"..") == std::wstring_view::npos && rest.find_first_of(L"?#%\\") == std::wstring_view::npos &&
               std::count(rest.begin(), rest.end(), L'/') == 1;
    }

    namespace
    {
        std::wstring Text(const nlohmann::json& j, const char* key)
        {
            const auto it = j.find(key);
            return it != j.end() && it->is_string() ? Widen(it->get<std::string>()) : std::wstring();
        }

        bool IsHex64(std::string_view s)
        {
            return s.size() == 64 && std::all_of(s.begin(), s.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); });
        }
    }

    namespace
    {
        std::optional<Release> ReleaseOf(std::string_view json, std::wstring* error)
        {
            const auto fail = [&](const wchar_t* why) -> std::optional<Release> {
                if (error)
                    *error = why;
                return std::nullopt;
            };
            const auto j = nlohmann::json::parse(json, nullptr, false);
            if (j.is_discarded() || !j.is_object())
                return fail(L"GitHub's answer was not a release");
            Release r;
            r.tag = Text(j, "tag_name");
            const auto version = ParseVersion(r.tag);
            if (!version)
                return fail(L"The latest release has no version tag");
            r.version = *version;
            if (j.value("draft", false) || j.value("prerelease", false) || !r.version.pre.empty())
                return fail(L"The latest release is a pre-release");
            r.name = Text(j, "name");
            if (r.name.empty())
                r.name = L"" SD_NAME L" " + ToText(r.version);
            r.htmlUrl = Text(j, "html_url");
            if (const auto it = j.find("body"); it != j.end() && it->is_string())
                r.body = it->get<std::string>();
            const auto published = j.find("published_at");
            const auto when = published != j.end() && published->is_string() ? ParseTime(published->get<std::string>()) : std::nullopt;
            if (!when)
                return fail(L"The latest release has no publish date");
            r.published = *when;
            const auto setupName = SetupName(r.version);
            if (const auto assets = j.find("assets"); assets != j.end() && assets->is_array())
                for (const auto& a : *assets)
                {
                    if (!a.is_object())
                        continue;
                    Asset asset{ Text(a, "name"), Text(a, "browser_download_url"), 0, {} };
                    if (const auto size = a.find("size"); size != a.end() && size->is_number_unsigned())
                        asset.size = size->get<uint64_t>();
                    // "digest": "sha256:<hex>" (newer releases have it).
                    if (const auto digest = Text(a, "digest"); digest.starts_with(L"sha256:") && IsHex64(Narrow(digest.substr(7))))
                        asset.sha256 = AsciiLower(Narrow(digest.substr(7)));
                    if (asset.name == setupName)
                        r.setup = std::move(asset);
                    else if (asset.name == kSumsName)
                        r.sums = std::move(asset);
                }
            return r;
        }
    }

    std::optional<Release> ParseRelease(std::string_view json, std::wstring* error)
    {
        // A value of the wrong type (a number where text should be) throws: that is no release either.
        try
        {
            return ReleaseOf(json, error);
        }
        catch (const nlohmann::json::exception&)
        {
            if (error)
                *error = L"GitHub's answer was not a release";
            return std::nullopt;
        }
    }

    // ---- Is it offered? ---------------------------------------------------------------------------------

    Verdict Judge(const Version& running, const Release& release, int64_t now, bool manual)
    {
        if (Compare(release.version, running) <= 0)
            return Verdict::UpToDate;
        if (!manual && now - release.published < kSettleSeconds)
            return Verdict::Settling;
        return Verdict::Available;
    }

    // ---- SHA256SUMS.txt ---------------------------------------------------------------------------------

    std::optional<std::string> FindSum(std::string_view sums, std::wstring_view name)
    {
        const std::string wanted = Narrow(name);
        while (!sums.empty())
        {
            const size_t eol = sums.find('\n');
            std::string_view line = sums.substr(0, eol);
            sums = eol == std::string_view::npos ? std::string_view() : sums.substr(eol + 1);
            if (!line.empty() && line.back() == '\r')
                line.remove_suffix(1);
            // "<hash>  <name>" (text mode) or "<hash> *<name>" (binary mode).
            if (line.size() < 66 || !IsHex64(line.substr(0, 64)) || line[64] != ' ' || (line[65] != ' ' && line[65] != '*'))
                continue;
            if (line.substr(66) == wanted)
                return AsciiLower(line.substr(0, 64));
        }
        return std::nullopt;
    }

    bool SameSum(std::string_view actual, std::string_view expected)
    {
        return IsHex64(actual) && IsHex64(expected) && AsciiLower(actual) == AsciiLower(expected);
    }

    // ---- The notes ----------------------------------------------------------------------------------

    namespace
    {
        // One line of Markdown as text: links and pictures keep their words, emphasis and code marks go.
        std::wstring Inline(std::wstring_view s)
        {
            std::wstring out;
            for (size_t i = 0; i < s.size(); ++i)
            {
                const wchar_t c = s[i];
                const bool picture = c == L'!' && i + 1 < s.size() && s[i + 1] == L'[';
                if (c == L'[' || picture)
                {
                    // [text](url) -> text; ![alt](url) -> nothing.
                    const size_t open = picture ? i + 1 : i;
                    const size_t close = s.find(L']', open);
                    if (close != std::wstring_view::npos && close + 1 < s.size() && s[close + 1] == L'(')
                        if (const size_t end = s.find(L')', close); end != std::wstring_view::npos)
                        {
                            if (!picture)
                                out += Inline(s.substr(open + 1, close - open - 1));
                            i = end;
                            continue;
                        }
                }
                if (c == L'*' || c == L'`' || (c == L'_' && i + 1 < s.size() && s[i + 1] == L'_'))
                {
                    if (c == L'_')
                        ++i;
                    continue;
                }
                out += c;
            }
            return out;
        }
    }

    std::wstring PlainNotes(std::string_view markdown, size_t maxLines)
    {
        const std::wstring text = Widen(markdown);
        std::vector<std::wstring> lines;
        bool blank = false;
        for (auto raw : Split(text, L'\n'))
        {
            if (!raw.empty() && raw.back() == L'\r')
                raw.remove_suffix(1);
            std::wstring_view line = Trim(raw);
            if (line.empty())
            {
                blank = !lines.empty();
                continue;
            }
            std::wstring out;
            if (line.starts_with(L"#"))
                line = Trim(line.substr(std::min(line.find_first_not_of(L'#'), line.size())));
            else if (line.starts_with(L"- ") || line.starts_with(L"* ") || line.starts_with(L"+ "))
            {
                out = L"• ";
                line = Trim(line.substr(2));
            }
            out += Inline(line);
            const auto kept = Trim(out);
            if (kept.empty() || kept == L"\u2022")
                continue;
            if (blank)
                lines.emplace_back();
            blank = false;
            lines.emplace_back(kept);
        }
        std::wstring result;
        for (size_t i = 0; i < lines.size(); ++i)
        {
            if (i == maxLines)
            {
                result += L"…";
                break;
            }
            result += lines[i];
            if (i + 1 < lines.size())
                result += L"\n";
        }
        return result;
    }

    // ---- The setting --------------------------------------------------------------------------------

    namespace
    {
        constexpr const wchar_t* kModes[] = { L"auto", L"ask", L"off" };
        constexpr wchar_t kSection[] = L"Updates";

        int64_t ReadInt64(const std::wstring& ini, const wchar_t* key)
        {
            wchar_t value[32]{};
            GetPrivateProfileStringW(kSection, key, L"0", value, ARRAYSIZE(value), ini.c_str());
            int64_t n = 0;
            const std::string text = Narrow(value);
            const auto r = std::from_chars(text.data(), text.data() + text.size(), n);
            return r.ec == std::errc{} && r.ptr == text.data() + text.size() && n > 0 ? n : 0;
        }
    }

    Mode LoadMode(const std::wstring& ini)
    {
        return static_cast<Mode>(IniChoice(ini, kSection, L"Mode", kModes, static_cast<int>(kDefaultMode)));
    }

    void SaveMode(Mode mode, const std::wstring& ini)
    {
        WritePrivateProfileStringW(kSection, L"Mode", kModes[static_cast<int>(mode)], ini.c_str());
    }

    int64_t LoadLastCheck(const std::wstring& ini)
    {
        return ReadInt64(ini, L"LastCheck");
    }

    void SaveLastCheck(int64_t when, const std::wstring& ini)
    {
        WritePrivateProfileStringW(kSection, L"LastCheck", std::to_wstring(when).c_str(), ini.c_str());
    }

    bool DailyCheckDue(Mode mode, int64_t lastCheck, int64_t now)
    {
        if (mode == Mode::Off)
            return false;
        return lastCheck <= 0 || now < lastCheck || now - lastCheck >= kCheckEverySeconds;
    }

    std::wstring LoadTestFeed(const std::wstring& ini)
    {
        std::wstring value(1024, L'\0');
        value.resize(GetPrivateProfileStringW(kSection, L"TestFeed", L"", value.data(), static_cast<DWORD>(value.size()), ini.c_str()));
        return std::wstring(Trim(value));
    }

    // ---- update.json --------------------------------------------------------------------------------

    namespace
    {
        constexpr const char* kOutcomes[] = { "none", "up-to-date", "available", "settling", "check-failed",
                                              "downloaded", "download-failed", "installed", "install-failed" };

        nlohmann::json AssetJson(const Asset& a)
        {
            return { { "name", Narrow(a.name) }, { "url", Narrow(a.url) }, { "size", a.size }, { "sha256", a.sha256 } };
        }

        Asset AssetOf(const nlohmann::json& j)
        {
            Asset a;
            if (!j.is_object())
                return a;
            a.name = Text(j, "name");
            a.url = Text(j, "url");
            a.size = j.value("size", uint64_t{ 0 });
            a.sha256 = j.value("sha256", std::string());
            return a;
        }
    }

    std::wstring StatePath()
    {
        return log::AppDataFolder() + L"\\update.json";
    }

    std::string StateJson(const State& s)
    {
        nlohmann::json j = {
            { "outcome", kOutcomes[static_cast<size_t>(s.outcome)] },
            { "error", Narrow(s.error) },
            { "checked", s.checked },
            { "checkedLocal", Narrow(s.checkedLocal) },
            { "setupPath", Narrow(s.setupPath) },
            { "setupSha256", s.setupSha256 },
            { "fromVersion", Narrow(s.fromVersion) },
            { "told", s.told },
        };
        if (s.hasRelease)
        {
            const auto& r = s.release;
            j["release"] = { { "version", Narrow(ToText(r.version)) }, { "tag", Narrow(r.tag) }, { "name", Narrow(r.name) },
                             { "url", Narrow(r.htmlUrl) }, { "body", r.body }, { "published", r.published },
                             { "setup", AssetJson(r.setup) }, { "sums", AssetJson(r.sums) } };
        }
        return j.dump(2);
    }

    std::optional<State> ParseState(std::string_view json)
    try
    {
        const auto j = nlohmann::json::parse(json, nullptr, false);
        if (j.is_discarded() || !j.is_object())
            return std::nullopt;
        State s;
        const auto outcome = j.value("outcome", std::string());
        for (size_t i = 0; i < std::size(kOutcomes); ++i)
            if (outcome == kOutcomes[i])
                s.outcome = static_cast<Outcome>(i);
        s.error = Text(j, "error");
        s.checked = j.value("checked", int64_t{ 0 });
        s.checkedLocal = Text(j, "checkedLocal");
        s.setupPath = Text(j, "setupPath");
        s.setupSha256 = j.value("setupSha256", std::string());
        s.fromVersion = Text(j, "fromVersion");
        s.told = j.value("told", false);
        if (const auto r = j.find("release"); r != j.end() && r->is_object())
            if (const auto version = ParseVersion(Text(*r, "version")))
            {
                s.hasRelease = true;
                s.release.version = *version;
                s.release.tag = Text(*r, "tag");
                s.release.name = Text(*r, "name");
                s.release.htmlUrl = Text(*r, "url");
                s.release.body = r->value("body", std::string());
                s.release.published = r->value("published", int64_t{ 0 });
                s.release.setup = AssetOf(r->value("setup", nlohmann::json::object()));
                s.release.sums = AssetOf(r->value("sums", nlohmann::json::object()));
            }
        return s;
    }
    catch (const nlohmann::json::exception&)
    {
        return std::nullopt;   // not ours, or damaged: as if there were none
    }

    bool SaveState(const State& state, const std::wstring& path)
    {
        // Written whole to a temporary name, then moved over the old one: a reader never sees half a file.
        const std::wstring temp = path + L".tmp";
        std::wstring error;
        if (!WriteFileBytes(temp, StateJson(state), WriteMode::Any, &error) ||
            !MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        {
            log::Warn(L"updates: could not write {} ({})", path, error.empty() ? std::to_wstring(GetLastError()) : error);
            DeleteFileW(temp.c_str());
            return false;
        }
        return true;
    }

    State LoadState(const std::wstring& path)
    {
        std::string bytes;
        if (!ReadFileBytes(path, bytes, nullptr, 4u << 20))
            return {};
        return ParseState(bytes).value_or(State{});
    }

    std::wstring ResultLine(const State& s, const Version& running)
    {
        const bool newer = s.hasRelease && Compare(s.release.version, running) > 0;
        const std::wstring v = s.hasRelease ? ToText(s.release.version) : L"";
        switch (s.outcome)
        {
        case Outcome::None: return L"Not checked yet";
        case Outcome::CheckFailed: return L"Couldn’t reach GitHub";
        case Outcome::Settling:
            return newer ? std::format(L"{} is out. It is offered a week after its release; Check now offers it today.", v)
                         : L"You have the latest version";
        case Outcome::DownloadFailed:
            return newer ? std::format(L"{} is available. The last download failed: {}", v, s.error) : L"You have the latest version";
        case Outcome::InstallFailed:
            return newer ? std::format(L"{} is available. It couldn’t be installed last time.", v) : L"You have the latest version";
        case Outcome::Installed:
            return newer ? std::format(L"{} is available", v) : std::format(L"Updated to {}. You have the latest version", ToText(running));
        case Outcome::UpToDate:
        case Outcome::Available:
        case Outcome::Downloaded:
            break;
        }
        return newer ? std::format(L"{} is available", v) : L"You have the latest version";
    }

    bool HasUpdate(const State& s, const Version& running)
    {
        if (!s.hasRelease || Compare(s.release.version, running) <= 0)
            return false;
        switch (s.outcome)
        {
        case Outcome::Available:
        case Outcome::Downloaded:
        case Outcome::DownloadFailed:
        case Outcome::InstallFailed:
            return true;
        default:
            return false;
        }
    }

    // ---- The updater's command line -------------------------------------------------------------------

    std::wstring Quote(std::wstring_view arg)
    {
        // CommandLineToArgvW's rules: backslashes are literal unless they come before a quote, where
        // each one is doubled; a quote is escaped with a backslash.
        if (!arg.empty() && arg.find_first_of(L" \t\"") == std::wstring_view::npos)
            return std::wstring(arg);
        std::wstring out = L"\"";
        size_t slashes = 0;
        for (const wchar_t c : arg)
        {
            if (c == L'\\')
            {
                ++slashes;
                continue;
            }
            out.append(c == L'"' ? slashes * 2 + 1 : slashes, L'\\');
            slashes = 0;
            out += c;
        }
        out.append(slashes * 2, L'\\');
        return out + L"\"";
    }

    std::wstring InstallArgs(const std::wstring& setup, unsigned long pid, const std::wstring& appExe)
    {
        return std::format(L"{} {} {} {} {} {}", kInstallArg, Quote(setup), kWaitArg, pid, kAppArg, Quote(appExe));
    }

    std::wstring SetupArgs(const std::wstring& logPath)
    {
        return L"/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /LOG=" + Quote(logPath);
    }
}
