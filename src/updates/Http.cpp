#include "updates/Http.h"
#include "Brand.h"
#include "Log.h"
#include "updates/Updates.h"

#include <windows.h>
#include <winhttp.h>
#include <wil/resource.h>

#include <format>
#include <functional>
#include <string_view>
#include <vector>

namespace sd::updates::http
{
    namespace
    {
        using Handle = wil::unique_any<HINTERNET, decltype(&::WinHttpCloseHandle), ::WinHttpCloseHandle>;
        using Sink = std::function<bool(const char* data, DWORD size)>;

        std::wstring UserAgent()
        {
            return L"" SD_NAME L"-Updater/" + ToText(Running());
        }

        void Option(HINTERNET h, DWORD option, DWORD value)
        {
            WinHttpSetOption(h, option, &value, sizeof(value));
        }

        // The host that answered: after a redirect, GitHub's file host rather than github.com.
        std::wstring AnsweringHost(HINTERNET request, const std::wstring& asked)
        {
            DWORD size = 0;
            WinHttpQueryOption(request, WINHTTP_OPTION_URL, nullptr, &size);
            std::wstring url(size / sizeof(wchar_t), L'\0');
            if (url.empty() || !WinHttpQueryOption(request, WINHTTP_OPTION_URL, url.data(), &size))
                return asked;
            url.resize(wcsnlen(url.c_str(), url.size()));
            URL_COMPONENTS parts{ sizeof(parts) };
            parts.dwHostNameLength = static_cast<DWORD>(-1);
            if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts) || !parts.lpszHostName)
                return asked;
            return std::wstring(parts.lpszHostName, parts.dwHostNameLength);
        }

        // One try. `again` says whether a failure may pass by itself: no answer, a dropped
        // connection, or an answer such as GitHub's 502/503/504.
        bool FetchOnce(const std::wstring& url, const wchar_t* accept, uint64_t maxBytes, const Sink& sink, std::wstring& error, bool& again)
        {
            again = false;
            URL_COMPONENTS parts{ sizeof(parts) };
            parts.dwHostNameLength = parts.dwUrlPathLength = parts.dwExtraInfoLength = static_cast<DWORD>(-1);
            if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts) || parts.nScheme != INTERNET_SCHEME_HTTPS)
            {
                error = L"Not an https address";
                return false;
            }
            const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
            const std::wstring path = std::wstring(parts.lpszUrlPath, parts.dwUrlPathLength) + std::wstring(parts.lpszExtraInfo, parts.dwExtraInfoLength);

            // The system's proxy settings (WPAD and the user's), as a browser would use them.
            Handle session(WinHttpOpen(UserAgent().c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
            if (!session)
                session.reset(WinHttpOpen(UserAgent().c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
            if (!session)
            {
                error = std::format(L"WinHTTP is unavailable ({})", GetLastError());
                return false;
            }
            // TLS 1.2 or 1.3 only (1.3 where Windows has it), and never from https down to http.
            DWORD tls = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
#ifdef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3
            tls |= WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
#endif
            if (!WinHttpSetOption(session.get(), WINHTTP_OPTION_SECURE_PROTOCOLS, &tls, sizeof(tls)))
                Option(session.get(), WINHTTP_OPTION_SECURE_PROTOCOLS, WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2);
            Option(session.get(), WINHTTP_OPTION_REDIRECT_POLICY, WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP);
            WinHttpSetTimeouts(session.get(), 15000, 15000, 30000, 60000);

            Handle connection(WinHttpConnect(session.get(), host.c_str(), parts.nPort, 0));
            if (!connection)
            {
                error = std::format(L"Couldn't connect to {} ({})", host, GetLastError());
                return false;
            }
            Handle request(WinHttpOpenRequest(connection.get(), L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                              WINHTTP_FLAG_SECURE | WINHTTP_FLAG_REFRESH));
            if (!request)
            {
                error = std::format(L"Couldn't open a request ({})", GetLastError());
                return false;
            }
            // Nothing about the user goes with it: no cookies, and no Windows sign-in to any server or proxy.
            Option(request.get(), WINHTTP_OPTION_DISABLE_FEATURE, WINHTTP_DISABLE_COOKIES);
            Option(request.get(), WINHTTP_OPTION_AUTOLOGON_POLICY, WINHTTP_AUTOLOGON_SECURITY_LEVEL_HIGH);
            const std::wstring headers = std::format(L"Accept: {}\r\n", accept);
            if (!WinHttpSendRequest(request.get(), headers.c_str(), static_cast<DWORD>(headers.size()), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
                !WinHttpReceiveResponse(request.get(), nullptr))
            {
                const DWORD code = GetLastError();
                again = true;
                error = code == ERROR_WINHTTP_NAME_NOT_RESOLVED || code == ERROR_WINHTTP_CANNOT_CONNECT || code == ERROR_WINHTTP_TIMEOUT
                            ? std::format(L"Couldn’t reach {} ({})", host, code)
                            : std::format(L"The request to {} failed ({})", host, code);
                return false;
            }
            DWORD status = 0, size = sizeof(status);
            WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size,
                                WINHTTP_NO_HEADER_INDEX);
            if (status != 200)
            {
                error = std::format(L"{} answered {}", AnsweringHost(request.get(), host), status);
                again = IsWorthRetrying(status);
                return false;
            }
            std::vector<char> buffer(64 * 1024);
            uint64_t total = 0;
            for (;;)
            {
                DWORD read = 0;
                if (!WinHttpReadData(request.get(), buffer.data(), static_cast<DWORD>(buffer.size()), &read))
                {
                    error = std::format(L"The download from {} broke off ({})", host, GetLastError());
                    again = true;
                    return false;
                }
                if (read == 0)
                    return true;
                total += read;
                if (total > maxBytes)
                {
                    error = std::format(L"The answer from {} is larger than expected", host);
                    return false;
                }
                if (!sink(buffer.data(), read))
                {
                    error = L"Couldn’t save the download";
                    return false;
                }
            }
        }

        // FetchOnce, tried again after a short wait when the failure may pass by itself (GitHub
        // answering 502/503/504 now and then, a connection that dropped). `restart` empties what the
        // sink got from a failed try.
        bool Fetch(const std::wstring& url, const wchar_t* accept, uint64_t maxBytes, const Sink& sink, const std::function<bool()>& restart,
                   std::wstring& error)
        {
            for (int attempt = 0;; ++attempt)
            {
                bool again = false;
                if (FetchOnce(url, accept, maxBytes, sink, error, again))
                    return true;
                if (!again || attempt + 1 >= kTries || !restart())
                    return false;
                log::Warn(L"updater: {}; trying again", error);
                Sleep(kRetryWaitMs[attempt]);
            }
        }
    }

    bool Get(const std::wstring& url, const wchar_t* accept, uint64_t maxBytes, std::string& body, std::wstring& error)
    {
        body.clear();
        return Fetch(
            url, accept, maxBytes,
            [&](const char* data, DWORD size) {
                body.append(data, size);
                return true;
            },
            [&] {
                body.clear();
                return true;
            },
            error);
    }

    bool Download(const std::wstring& url, const std::wstring& path, uint64_t maxBytes, std::wstring& error)
    {
        wil::unique_hfile file(CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (!file)
        {
            error = std::format(L"Couldn’t create {} ({})", path, GetLastError());
            return false;
        }
        const bool ok = Fetch(
            url, L"application/octet-stream", maxBytes,
            [&](const char* data, DWORD size) {
                DWORD written = 0;
                return WriteFile(file.get(), data, size, &written, nullptr) && written == size;
            },
            [&] { return SetFilePointer(file.get(), 0, nullptr, FILE_BEGIN) == 0 && SetEndOfFile(file.get()); },
            error);
        file.reset();
        if (!ok)
            DeleteFileW(path.c_str());
        return ok;
    }
}
