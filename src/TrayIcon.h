#pragma once
#include "resource.h"

#include <iterator>

namespace sd::trayicon
{
    // The tray glyph is drawn for the taskbar it sits on: white on a dark taskbar, near-black on a light
    // one, each hand-fitted at these sizes (src/TrayDarkTaskbar.ico, src/TrayLightTaskbar.ico).
    inline constexpr int kSizes[] = { 16, 20, 24, 32 };

    struct Pick
    {
        int resource = IDI_TRAY_DARK;   // the icon resource to load
        int size = 16;                  // the image in it, in pixels
        friend constexpr bool operator==(const Pick&, const Pick&) = default;
    };

    // The notification area's icon size at a DPI: Windows' small icon, 16 px at 96 DPI.
    constexpr int SlotSize(unsigned dpi) { return static_cast<int>(((dpi ? dpi : 96u) * 16u + 48u) / 96u); }

    // The drawing for the taskbar's theme (`lightTaskbar`: SystemUsesLightTheme is 1) and its DPI: the
    // smallest one at least as big as the slot, so the shell only ever scales down (28 px at 175% uses
    // the 32 px drawing), and the largest past 200%.
    constexpr Pick For(bool lightTaskbar, unsigned dpi)
    {
        const int slot = SlotSize(dpi);
        int size = kSizes[std::size(kSizes) - 1];
        for (const int s : kSizes)
        {
            if (s >= slot)
            {
                size = s;
                break;
            }
        }
        return { lightTaskbar ? IDI_TRAY_LIGHT : IDI_TRAY_DARK, size };
    }
}
