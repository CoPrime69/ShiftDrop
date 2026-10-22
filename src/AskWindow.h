#pragma once
#include "Ask.h"

#include <windows.h>

namespace sd
{
    // The question window (App Windows page, Questions): what a job or an editor asks the user, in
    // the shared kit's look. The question in bold, a line under it, then a password field (the
    // archive's password, with "That password didn't work" on a retry), the options to pick one from
    // (Normalize's loudness), or nothing; the buttons at the bottom, the first in the accent colour
    // and Cancel last. Standard controls drawn by the kit (DialogWindow): Tab, Enter, Esc, focus rings,
    // screen readers, light and dark, the accent and per-monitor DPI as in Settings.
    //
    // ask::InstallUi takes it: every question the app asks (Ask.h) is shown in it.
    ask::Presenter QuestionWindows(HINSTANCE instance);
}
