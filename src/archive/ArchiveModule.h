#pragma once
#include "Engine.h"

#include <memory>

// Archives: Zip / TAR.GZ / 7z of any files and folders, Extract, and Convert between formats,
// through libarchive (BSD-2). Every archive that is read (Extract, Convert) is parsed by the sandboxed
// worker (src/sandbox); writing happens in the app, from the user's own files or from entries the app
// has already checked.
//
// Where the slices are:
//   File actions (Alt):   Zip — any files and folders, one ZIP of everything.
//   Convert (Shift), on a folder:    ZIP, TAR.GZ, 7z — one archive of everything dropped.
//   Convert (Shift), on an archive:  Extract, then ZIP, TAR.GZ, 7z (the archive's own format dimmed).
namespace sd::archive
{
    FeatureModule Module();
    std::unique_ptr<IEngine> MakeEngine(const EngineServices& services);
}
