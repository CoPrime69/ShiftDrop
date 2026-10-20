#include "sandbox/MediaSandbox.h"

#include <array>
#include <atomic>
#include <cwctype>

namespace sd::sandbox
{
    namespace
    {
        std::atomic<int> g_opened{ 0 };

        std::wstring Lower(std::wstring s)
        {
            for (auto& c : s)
                c = static_cast<wchar_t>(std::towlower(c));
            return s;
        }

        bool IsLocalFile(const std::wstring& path)
        {
            // "C:\..." or "\\server\share\..." with no device or protocol prefix.
            const bool drive = path.size() > 3 && std::iswalpha(path[0]) && path[1] == L':' && (path[2] == L'\\' || path[2] == L'/');
            if (!drive || path.find(L':', 2) != std::wstring::npos)
                return false;
            const DWORD a = GetFileAttributesW(path.c_str());
            return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
        }

        // An argument that names something on disk ffmpeg would open by itself: a path, or a filter
        // reading a file (subtitles=C\:/..., movie=...).
        bool MentionsAPath(const std::wstring& arg)
        {
            const auto lower = Lower(arg);
            for (const wchar_t* p : { L":\\", L":/", L"\\:", L"\\\\", L"//", L"movie=", L"subtitles=", L"ass=", L"lut3d=", L"haldclut", L"file=" })
                if (lower.find(p) != std::wstring::npos)
                    return true;
            return false;
        }

        // Options whose value ffmpeg opens as a file, and formats that read or write more than one file.
        constexpr std::array kFileOptions{ L"-passlogfile", L"-attach", L"-dump_attachment", L"-filter_script", L"-filter_complex_script",
                                           L"-sdp_file", L"-vstats_file", L"-stats_enc_pre", L"-stats_enc_post", L"-stats_mux_pre", L"-pass" };
        constexpr std::array kMultiFileFormats{ L"concat", L"image2", L"segment", L"stream_segment", L"hls", L"dash", L"tee", L"lavfi", L"fifo" };
    }

    bool MediaJail::Prepare(const std::vector<std::wstring>& args, std::wstring& why)
    {
        m_args.clear();
        m_input.clear();
        m_output.clear();
        size_t inputAt = 0;
        int inputs = 0;
        bool noStdin = false;
        for (size_t i = 0; i < args.size(); ++i)
        {
            const auto& a = args[i];
            if (a == L"-i")
            {
                ++inputs;
                inputAt = i;
            }
            noStdin |= a == L"-nostdin";
            for (const auto* o : kFileOptions)
                if (a == o)
                {
                    why = a + L" uses a file of its own";
                    return false;
                }
            if (a.starts_with(L"-/"))
            {
                why = L"an option value is read from a file";
                return false;
            }
            if (a == L"-f" && i + 1 < args.size())
                for (const auto* f : kMultiFileFormats)
                    if (Lower(args[i + 1]) == f)
                    {
                        why = L"the " + args[i + 1] + L" format reads or writes other files";
                        return false;
                    }
        }
        if (inputs != 1 || inputAt + 1 >= args.size())
        {
            why = inputs == 0 ? L"no input file" : L"more than one input";
            return false;
        }
        if (!noStdin)
        {
            why = L"no -nostdin";   // descriptor 0 is the output's read-back handle, never a keyboard
            return false;
        }
        m_input = args[inputAt + 1];
        if (!IsLocalFile(m_input))
        {
            why = L"the input is not a local file";
            return false;
        }
        const bool probe = inputAt + 2 == args.size();
        if (!probe)
        {
            m_output = args.back();
            // Only ever our own staging file: whatever the arguments say, a sandboxed ffmpeg can write
            // nowhere else.
            if (!Lower(m_output).ends_with(L".sdpart") || !IsLocalFile(m_output))
            {
                why = L"the output is not a staging file";
                return false;
            }
            bool muxerNamed = false;
            for (size_t i = inputAt + 2; i + 1 < args.size(); ++i)
                muxerNamed |= args[i] == L"-f";
            if (!muxerNamed)
            {
                why = L"the output format is not named";
                return false;
            }
        }
        for (size_t i = 0; i < args.size(); ++i)
        {
            if (i == inputAt + 1 || (!probe && i + 1 == args.size()))
                continue;
            if (MentionsAPath(args[i]))
            {
                why = L"an argument names a file (" + args[i].substr(0, 60) + L")";
                return false;
            }
        }

        // The rewrite. -protocol_whitelist (an input option) allows "fd" and nothing else.
        bool whitelisted = false;
        for (size_t i = 0; i < args.size(); ++i)
        {
            if (args[i] == L"-protocol_whitelist" && i + 1 < args.size() && i < inputAt)
            {
                m_args.insert(m_args.end(), { L"-protocol_whitelist", L"fd" });
                whitelisted = true;
                ++i;
            }
            else if (i == inputAt)
            {
                if (!whitelisted)
                    m_args.insert(m_args.end(), { L"-protocol_whitelist", L"fd" });
                m_args.insert(m_args.end(), { L"-fd", L"3", L"-i", L"fd:" });
                ++i;
            }
            else if (!probe && i + 1 == args.size())
                m_args.insert(m_args.end(), { L"-fd", L"4", L"fd:" });
            else
                m_args.push_back(args[i]);
        }
        return true;
    }

    bool MediaJail::Open(LaunchSpec& spec, std::wstring& error)
    {
        SECURITY_ATTRIBUTES inherit{ sizeof(inherit), nullptr, TRUE };
        m_in.reset(CreateFileW(m_input.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, &inherit, OPEN_EXISTING,
                               FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
        if (!m_in)
        {
            error = L"cannot open the input for the sandbox";
            return false;
        }
        HANDLE back = INVALID_HANDLE_VALUE, out = INVALID_HANDLE_VALUE;
        if (!m_output.empty())
        {
            // Emptied: through a descriptor ffmpeg's -y cannot truncate what an earlier attempt left.
            m_out.reset(CreateFileW(m_output.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, &inherit, TRUNCATE_EXISTING, 0, nullptr));
            m_back.reset(CreateFileW(m_output.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, OPEN_EXISTING, 0, nullptr));
            if (!m_out || !m_back)
            {
                error = L"cannot open the output for the sandbox";
                return false;
            }
            back = m_back.get();
            out = m_out.get();
        }
        spec.inherit.push_back(m_in.get());
        if (m_out)
            spec.inherit.insert(spec.inherit.end(), { m_out.get(), m_back.get() });
        // Descriptors 1 and 2 stay closed in the table: the C runtime then takes them from the
        // STARTUPINFO std handles (our progress and log pipes).
        spec.crtFds = { back, INVALID_HANDLE_VALUE, INVALID_HANDLE_VALUE, m_in.get(), out };
        ++g_opened;
        return true;
    }

    int MediaJail::OpenCount()
    {
        return g_opened.load();
    }
}
