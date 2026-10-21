#include "docs/Paper.h"

#include <windows.h>

namespace sd::docs
{
    Paper A4Paper() { return Paper{ 595.28f, 841.89f, false }; }
    Paper LetterPaper() { return Paper{ 612.f, 792.f, true }; }

    Paper DefaultPaper()
    {
        DWORD size = 0;
        // LOCALE_RETURN_NUMBER writes a DWORD: 1 = Letter, 5 = Legal, 8 = A3, 9 = A4.
        if (GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_IPAPERSIZE | LOCALE_RETURN_NUMBER, reinterpret_cast<LPWSTR>(&size),
                            sizeof(size) / sizeof(wchar_t)) &&
            (size == 1 || size == 5))
            return LetterPaper();
        return A4Paper();
    }
}
