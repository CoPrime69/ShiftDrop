#include "archive/EntryPipe.h"

#include <cstring>
#include <string>
#include <vector>

namespace sd::archive
{
    namespace
    {
        bool WriteAll(HANDLE h, const void* data, size_t size)
        {
            const auto* p = static_cast<const char*>(data);
            while (size > 0)
            {
                DWORD written = 0;
                const DWORD chunk = static_cast<DWORD>(std::min<size_t>(size, 1u << 20));
                if (!WriteFile(h, p, chunk, &written, nullptr) || written == 0)
                    return false;
                p += written;
                size -= written;
            }
            return true;
        }

        bool ReadAll(HANDLE h, void* data, size_t size)
        {
            auto* p = static_cast<char*>(data);
            while (size > 0)
            {
                DWORD read = 0;
                if (!ReadFile(h, p, static_cast<DWORD>(std::min<size_t>(size, 1u << 20)), &read, nullptr) || read == 0)
                    return false;
                p += read;
                size -= read;
            }
            return true;
        }

        template <class T>
        void Put(std::vector<char>& out, T value)
        {
            const auto at = out.size();
            out.resize(at + sizeof(T));
            std::memcpy(out.data() + at, &value, sizeof(T));
        }

        void PutText(std::vector<char>& out, const std::wstring& text)
        {
            const auto count = static_cast<uint32_t>(std::min<size_t>(text.size(), kMaxNameChars));
            Put(out, count);
            const auto at = out.size();
            out.resize(at + count * sizeof(wchar_t));
            std::memcpy(out.data() + at, text.data(), count * sizeof(wchar_t));
        }

        // Bounds-checked reading of one frame's payload.
        class Cursor
        {
        public:
            explicit Cursor(const std::vector<char>& data) : m_data(data) {}
            template <class T>
            bool Get(T& value)
            {
                if (m_at + sizeof(T) > m_data.size())
                    return false;
                std::memcpy(&value, m_data.data() + m_at, sizeof(T));
                m_at += sizeof(T);
                return true;
            }
            bool GetText(std::wstring& text)
            {
                uint32_t count = 0;
                if (!Get(count) || count > kMaxNameChars || m_at + size_t(count) * sizeof(wchar_t) > m_data.size())
                    return false;
                text.resize(count);
                std::memcpy(text.data(), m_data.data() + m_at, size_t(count) * sizeof(wchar_t));
                m_at += size_t(count) * sizeof(wchar_t);
                return true;
            }
            bool AtEnd() const { return m_at == m_data.size(); }

        private:
            const std::vector<char>& m_data;
            size_t m_at = 0;
        };

        Result Malformed()
        {
            return Fail(Status::Worker, L"the archive worker sent malformed data");
        }
    }

    bool PipeWriter::Frame(char type, const void* payload, size_t size)
    {
        char header[5];
        header[0] = type;
        const auto length = static_cast<uint32_t>(size);
        std::memcpy(header + 1, &length, sizeof(length));
        return WriteAll(m_pipe, header, sizeof(header)) && (size == 0 || WriteAll(m_pipe, payload, size));
    }

    Result PipeWriter::Begin(const Entry& entry)
    {
        std::vector<char> p;
        Put(p, static_cast<uint8_t>(entry.type));
        Put(p, entry.size);
        Put(p, entry.mtime);
        Put(p, entry.atime);
        PutText(p, entry.name);
        PutText(p, entry.link);
        return Frame('E', p.data(), p.size()) ? Result{} : Fail(Status::Io, L"pipe closed");
    }

    Result PipeWriter::Data(const void* bytes, size_t size)
    {
        const auto* p = static_cast<const char*>(bytes);
        while (size > 0)
        {
            const size_t chunk = std::min<size_t>(size, kMaxChunk);
            if (!Frame('D', p, chunk))
                return Fail(Status::Io, L"pipe closed");
            p += chunk;
            size -= chunk;
        }
        return {};
    }

    Result PipeWriter::End()
    {
        return Frame('Z', nullptr, 0) ? Result{} : Fail(Status::Io, L"pipe closed");
    }

    bool PipeWriter::Progress(uint64_t consumed)
    {
        return Frame('P', &consumed, sizeof(consumed));
    }

    bool PipeWriter::Finish(const Result& result)
    {
        std::vector<char> p;
        Put(p, static_cast<uint8_t>(result.status));
        PutText(p, result.message);
        return Frame('X', p.data(), p.size());
    }

    Result PumpPipe(HANDLE pipe, IEntrySink& sink, const std::function<void(uint64_t)>& progress)
    {
        std::vector<char> payload;
        bool inEntry = false, fileEntry = false;
        for (;;)
        {
            char header[5];
            if (!ReadAll(pipe, header, sizeof(header)))
                return Fail(Status::Worker, L"the archive worker stopped before the end (damaged archive, or it ran out of memory)");
            uint32_t length = 0;
            std::memcpy(&length, header + 1, sizeof(length));
            if (length > kMaxChunk + 1024)
                return Malformed();
            payload.resize(length);
            if (length > 0 && !ReadAll(pipe, payload.data(), length))
                return Fail(Status::Worker, L"the archive worker stopped in the middle of an entry");
            Cursor in(payload);
            switch (header[0])
            {
            case 'E':
            {
                if (inEntry)
                    return Malformed();
                Entry e;
                uint8_t type = 0;
                if (!in.Get(type) || type > static_cast<uint8_t>(EntryType::Other) || !in.Get(e.size) || !in.Get(e.mtime) || !in.Get(e.atime) ||
                    !in.GetText(e.name) || !in.GetText(e.link) || !in.AtEnd())
                    return Malformed();
                e.type = static_cast<EntryType>(type);
                inEntry = true;
                fileEntry = e.type == EntryType::File;
                if (auto r = sink.Begin(e); !r.ok())
                    return r;
                break;
            }
            case 'D':
                if (!inEntry || !fileEntry)
                    return Malformed();
                if (auto r = sink.Data(payload.data(), payload.size()); !r.ok())
                    return r;
                break;
            case 'Z':
                if (!inEntry || length != 0)
                    return Malformed();
                inEntry = false;
                if (auto r = sink.End(); !r.ok())
                    return r;
                break;
            case 'P':
            {
                uint64_t consumed = 0;
                if (!in.Get(consumed) || !in.AtEnd())
                    return Malformed();
                if (progress)
                    progress(consumed);
                break;
            }
            case 'X':
            {
                uint8_t status = 0;
                Result r;
                if (!in.Get(status) || status > static_cast<uint8_t>(Status::Worker) || !in.GetText(r.message) || !in.AtEnd())
                    return Malformed();
                r.status = static_cast<Status>(status);
                if (r.ok() && inEntry)
                    return Malformed();
                return r;
            }
            default:
                return Malformed();
            }
        }
    }
}
