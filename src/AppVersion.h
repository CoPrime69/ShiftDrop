#pragma once

// The version this build shows, from VERSION at the repository root (see Version.h, generated at
// build time). Functions rather than the macros so only AppVersion.cpp is recompiled when the commit
// changes, not every file that shows the version.
namespace sd::version
{
    // "0.6.0" for the build that publishes the release, "0.6.0+b950d51" (the commit) for any other.
    const wchar_t* Display();
    // "0.6.0": the release this build is or comes from.
    const wchar_t* Base();
    // "b950d51": the commit's first 7 characters; empty for the release build and outside git.
    const wchar_t* Commit();
}
