#pragma once
#include <cstddef>

namespace sd::platform
{
    // Zeroes `bytes` bytes at `data` in a way the compiler may not optimise away (Secret.h).
    void WipeSecret(void* data, size_t bytes);
}
