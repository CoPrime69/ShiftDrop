#pragma once

namespace sd::docs
{
    struct Paper
    {
        float widthPt = 595.28f;    // A4 portrait, in points (1/72 inch)
        float heightPt = 841.89f;
        bool letter = false;        // US Letter instead of A4
    };

    // The user's paper size from their region settings: US Letter in the US, Canada, Mexico and a
    // few others (Windows' LOCALE_IPAPERSIZE says 1), A4 everywhere else.
    Paper DefaultPaper();
    Paper A4Paper();
    Paper LetterPaper();
}
