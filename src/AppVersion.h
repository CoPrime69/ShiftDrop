#pragma once

// The version this build shows, from VERSION at the repository root (see Version.h, generated at
// build time by scripts/make-version-header.cmake). Functions rather than the macros so only
// AppVersion.cpp is recompiled when the commit changes, not every file that shows the version.
namespace sd::version
{
    // Semantic Versioning 2.0.0: "0.7.0" for the build that publishes the release; any other build is a
    // pre-release of the next version, "0.7.1-dev.3+af953b2" (3 commits after v0.7.0, at af953b2).
    const wchar_t* Display();
    // "0.7.0": VERSION as written (the release this build is, follows, or leads to).
    const wchar_t* Base();
    // "af953b2": the commit's first 7 characters; empty for the release build and outside git.
    const wchar_t* Commit();
    // The N of dev.N: commits since the last release tag; 0 for the release build.
    unsigned Build();
}
