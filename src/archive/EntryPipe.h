#pragma once
#include "archive/Entries.h"

#include <windows.h>

#include <functional>

// The worker -> parent stream: the sandboxed worker parses the archive and sends each entry over an
// anonymous pipe; the parent validates everything again and does all the writing. Frames are
// [type: 1 byte][length: 4 bytes LE][payload], and the parent treats the stream as hostile too: a
// worker that was taken over by a crafted archive can send anything, so every length is bounded.
//
//   'E' entry header   type u8, size i64, mtime i64, atime i64, name (u32 count + UTF-16), link (same)
//   'D' data           up to kMaxChunk bytes of the current file entry
//   'Z' end of entry
//   'P' progress       archive bytes consumed, u64
//   'X' finished       status u8, message (u32 count + UTF-16)
namespace sd::archive
{
    constexpr uint32_t kMaxChunk = 1u << 20;
    constexpr uint32_t kMaxNameChars = 32768;

    class PipeWriter : public IEntrySink
    {
    public:
        explicit PipeWriter(HANDLE pipe) : m_pipe(pipe) {}
        Result Begin(const Entry& entry) override;
        Result Data(const void* bytes, size_t size) override;
        Result End() override;
        bool Progress(uint64_t consumed);
        bool Finish(const Result& result);

    private:
        bool Frame(char type, const void* payload, size_t size);
        HANDLE m_pipe;
    };

    // Reads frames until 'X' or the pipe closes, driving `sink`. A closed pipe before 'X' (the worker
    // was killed, crashed or hit its memory limit) is Status::Worker.
    Result PumpPipe(HANDLE pipe, IEntrySink& sink, const std::function<void(uint64_t consumed)>& progress);
}
