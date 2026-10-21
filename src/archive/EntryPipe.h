#pragma once
#include "archive/Entries.h"

#include <windows.h>

#include <functional>

// The worker <-> parent pipe: the sandboxed worker parses the archive and sends each entry over it;
// the parent validates everything again and does all the writing. Frames are
// [type: 1 byte][length: 4 bytes LE][payload], and the parent treats the stream as hostile too: a
// worker that was taken over by a crafted archive can send anything, so every length is bounded.
//
//   'E' entry header   type u8, size i64, mtime i64, atime i64, name (u32 count + UTF-16), link (same)
//   'D' data           up to kMaxChunk bytes of the current file entry
//   'Z' end of entry
//   'P' progress       archive bytes consumed, u64
//   'X' finished       status u8, message (u32 count + UTF-16)
//   'K' password       the worker needs one: u8 1 when the last one was wrong, else 0. The only frame
//                      that goes back the other way answers it: 'K' with the password (u32 count +
//                      UTF-16), count 0 for none. The password exists only in that frame, in the
//                      worker's memory while it reads, and nowhere on disk or in a log.
namespace sd::archive
{
    constexpr uint32_t kMaxChunk = 1u << 20;
    constexpr uint32_t kMaxNameChars = 32768;
    constexpr uint32_t kMaxPasswordChars = static_cast<uint32_t>(Secret::kCapacity);

    class PipeWriter : public IEntrySink
    {
    public:
        explicit PipeWriter(HANDLE pipe) : m_pipe(pipe) {}
        Result Begin(const Entry& entry) override;
        Result Data(const void* bytes, size_t size) override;
        Result End() override;
        bool Progress(uint64_t consumed);
        bool Finish(const Result& result);
        // Asks the parent for the password and waits for its answer (the pipe goes both ways).
        bool AskPassword(bool wrong, Secret& password);

    private:
        bool Frame(char type, const void* payload, size_t size);
        HANDLE m_pipe;
    };

    // Reads frames until 'X' or the pipe closes, driving `sink`. A closed pipe before 'X' (the worker
    // was killed, crashed or hit its memory limit) is Status::Worker. A 'K' frame is answered on the
    // same pipe with what `password` gives (nothing when it is empty or says no).
    Result PumpPipe(HANDLE pipe, IEntrySink& sink, const std::function<void(uint64_t consumed)>& progress, const PasswordFn& password = {});
}
