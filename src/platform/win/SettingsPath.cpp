// Where settings.ini lives on Windows: in %LOCALAPPDATA%\ShiftDrop, with the history and the logs.
#include "Log.h"
#include "Settings.h"

namespace sd
{
    std::wstring SettingsPath()
    {
        return log::AppDataFolder() + L"\\settings.ini";
    }
}
