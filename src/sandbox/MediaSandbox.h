#pragma once
#include "sandbox/Sandbox.h"

#include <string>
#include <vector>

// ffmpeg in the AppContainer (optional, [Sandbox] Media=1). An AppContainer process cannot open the
// user's files by name, so the run is rewritten to work on handles we open for it: the input becomes
// C runtime descriptor 3 ("-fd 3 -i fd:"), the staging output descriptor 4 ("-fd 4 fd:"), and ffmpeg
// is allowed the "fd" protocol only. Descriptor 0 is a second, read-only handle on the output,
// because the MP4 muxer's +faststart reopens its output ("fd:" with no number means descriptor 0)
// to move the index to the front.
//
// Only runs that name exactly one input file and our own staging file (".sdpart") qualify. Anything
// else (a second input, a concat list, subtitles burnt in from a file, two-pass log files, frames
// written into a folder) runs as before, outside the AppContainer, and the log says why.
namespace sd::sandbox
{
    class MediaJail
    {
    public:
        // True when `args` can run on handles; `args()` is then the rewritten command.
        bool Prepare(const std::vector<std::wstring>& args, std::wstring& why);
        const std::vector<std::wstring>& Args() const { return m_args; }
        const std::wstring& Input() const { return m_input; }
        const std::wstring& Output() const { return m_output; }   // empty for a probe (no output)

        // Opens the input (read) and the output (read/write, emptied) and adds them, as descriptors,
        // to `spec`. Call after Prepare; the handles live as long as this object.
        bool Open(LaunchSpec& spec, std::wstring& error);

        // How many runs went through a jail in this process (tests check the sandbox was really used).
        static int OpenCount();

    private:
        std::vector<std::wstring> m_args;
        std::wstring m_input, m_output;
        wil::unique_hfile m_in, m_out, m_back;
    };
}
