#pragma once
#include <cstdint>

namespace sd::platform
{
    // Milliseconds from a fixed point that only moves forward (on Windows, since the PC started), for
    // timing how long something took. Not a date.
    uint64_t MonotonicMilliseconds();

    // The local date and time, field by field (the fields of Windows' SYSTEMTIME).
    struct LocalDateTime
    {
        uint16_t year = 0, month = 0, day = 0;
        uint16_t hour = 0, minute = 0, second = 0;
    };
    LocalDateTime LocalTimeNow();
}
