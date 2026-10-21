#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Subtitle tracks: SubRip (.srt) and WebVTT (.vtt) in and out, and plain text out.
// Pure: no Windows calls, so the parser and writers are tested directly.
namespace sd::subtitles
{
    struct Cue
    {
        int64_t startMs = 0;
        int64_t endMs = 0;
        std::string id;     // the SRT index or VTT identifier as written (may be empty)
        std::string text;   // UTF-8, lines joined by "\n", markup as in the source
    };

    enum class Format
    {
        Srt,
        Vtt,
    };

    struct Track
    {
        Format source = Format::Srt;
        std::vector<Cue> cues;
    };

    // "01:02:03,456", "01:02:03.456", "02:03.456" (no hours), "1:02:03,5" (short fraction) and
    // "00:00:01:000" (colon before the milliseconds). Surrounding spaces are ignored.
    bool ParseTimestamp(std::string_view s, int64_t& ms);
    std::string FormatSrtTime(int64_t ms);   // 01:02:03,456
    std::string FormatVttTime(int64_t ms);   // 01:02:03.456

    // Parses SRT or WebVTT text (already decoded to UTF-8). Tolerates CRLF or CR line ends, a BOM,
    // missing or wrong indices, missing blank lines between cues, VTT headers, NOTE, STYLE and
    // REGION blocks, cue settings after the end time, and end times before start times.
    // Cues come back in start order, empty ones dropped.
    Track Parse(std::string_view text);

    // Markup conversion. SRT keeps <b> <i> <u> and <font>; VTT keeps <b> <i> <u>, <c.class>,
    // <v Speaker>, <lang>, <ruby>/<rt> and inline timestamps, and escapes & and <.
    std::string SrtToVttText(std::string_view text);
    std::string VttToSrtText(std::string_view text);
    // All markup removed (SRT tags, VTT tags, {\an8}-style override codes) and entities decoded.
    std::string PlainCueText(std::string_view text, Format source);

    std::string WriteSrt(const Track& track);   // UTF-8 with BOM, CRLF, indices renumbered from 1
    std::string WriteVtt(const Track& track);   // UTF-8, "WEBVTT" header, LF
    // Text only: one line per cue (its lines merged with spaces), markup removed, and a line that
    // repeats the line before it (roll-up captions) left out. CRLF, no BOM.
    std::string WriteText(const Track& track);
}
