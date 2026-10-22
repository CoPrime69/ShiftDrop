#include "AppVersion.h"

#include "Version.h"

namespace sd::version
{
    const wchar_t* Display()
    {
        return SD_VERSION;
    }

    const wchar_t* Base()
    {
        return SD_VERSION_BASE;
    }

    const wchar_t* Commit()
    {
        return SD_VERSION_COMMIT;
    }
}
