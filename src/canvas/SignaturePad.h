#pragma once
#include "canvas/Raster.h"

#include <windows.h>

#include <functional>
#include <memory>
#include <string>

// Fill and sign's signature pad (app only): a small window over the picture editor to write a
// signature with the mouse or a pen, or to take it from a photo or scan of one (the paper made
// see-through if the person wants). Nothing leaves the PC.
namespace sd::canvas
{
    // `done` gets the ink on a transparent background, cropped to it, and whether to remember it; it
    // isn't called when the pad is cancelled. `owner` is disabled while the pad is open. False when the
    // window could not be made.
    bool OpenSignaturePad(HWND owner, bool fromPicture, std::function<void(std::shared_ptr<const Raster> ink, bool remember)> done);

    // The Open dialog for one picture, as the picture editor and the pad show it.
    bool PickPictureFile(HWND owner, std::wstring& path);
}
