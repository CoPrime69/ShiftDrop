#pragma once
#include <cstdint>
#include <string>

// Numbers as the app writes them for people: file sizes, counts and durations. One rule each, so a
// size reads the same on the wheel, the shelf, the top panel and in the inspector.
namespace sd
{
    // "1 byte", "512 bytes", "860 KB", "1.2 MB", "48 MB", "3.0 GB" (1 KB = 1024 bytes, like Explorer).
    // KB and MB switch to the next unit at 1000, so a size never reads "1023 KB".
    std::wstring SizeText(uint64_t bytes);
    // SizeText plus the exact count: "3.3 MB (3,456,789 bytes)"; "512 bytes" alone.
    std::wstring ExactSizeText(uint64_t bytes);
    // 54321 -> "54,321".
    std::wstring Thousands(uint64_t n);
    // A duration rounded to the millisecond: "0:01:23.456", or "00:01:23.456" with `padHours`.
    // A negative time reads as 0.
    std::wstring Timecode(int64_t us, bool padHours);
    // How long something took, to the second: "38 s", "4 min 12 s", "1 h 5 min" (under a second: "1 s").
    std::wstring DurationText(int64_t ms);
}
