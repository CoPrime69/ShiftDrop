#pragma once
#include <cstdint>
#include <vector>

namespace sd::pdf
{
    // Pixels passed between the PDF renderer, OCR and the encoders: 32-bit BGRA, rows top-down with
    // no padding, straight (not premultiplied) alpha.
    struct Bitmap
    {
        uint32_t width = 0;
        uint32_t height = 0;
        std::vector<uint8_t> bgra;

        bool Empty() const { return width == 0 || height == 0 || bgra.size() < static_cast<size_t>(width) * height * 4; }
    };

    // Blends transparent pixels onto white and makes the bitmap opaque (OCR reads dark text on a
    // light page; a transparent PNG's "empty" pixels are often black).
    inline void FlattenOnWhite(Bitmap& b)
    {
        for (size_t i = 0; i + 3 < b.bgra.size(); i += 4)
        {
            const unsigned a = b.bgra[i + 3];
            if (a == 255)
                continue;
            for (size_t c = 0; c < 3; ++c)
                b.bgra[i + c] = static_cast<uint8_t>((b.bgra[i + c] * a + 255u * (255u - a)) / 255u);
            b.bgra[i + 3] = 255;
        }
    }
}
