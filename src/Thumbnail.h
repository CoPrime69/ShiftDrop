#pragma once
#include "Theme.h"

#include <string>

namespace sd
{
    // The shell's picture for a file, as Explorer shows it: the image itself for photos (from the
    // thumbnail cache when it has one), otherwise the file type's icon. At most about px × px, as
    // premultiplied pixels; empty on failure.
    //
    // Blocking (it may read and decode the file), so call it off the UI thread, on a thread that has
    // initialised COM.
    theme::Pixels LoadThumbnail(const std::wstring& path, int px);
}
