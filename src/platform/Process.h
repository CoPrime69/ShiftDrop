#pragma once
#include <cstdint>

namespace sd::platform
{
    // This process's id, for names that must not clash with another running copy's.
    uint32_t CurrentProcessId();
}
