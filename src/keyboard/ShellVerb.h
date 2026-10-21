#pragma once
#include <windows.h>

#include <string>

// The "ShiftDrop…" entry in Explorer's right-click menu for files and folders.
//
// A classic shell verb, registered per user under HKCU\Software\Classes (no admin rights):
//   *\shell\ShiftDrop and Directory\shell\ShiftDrop
//     (Default), MUIVerb = "ShiftDrop…"; Icon = the exe; MultiSelectModel = Player
//     command\(Default) = "<exe>" --wheel "%1"
// MultiSelectModel=Player keeps the entry for selections of any size (without it Explorer hides a
// verb when more than 15 items are selected) and starts one process per item; each forwards its path
// to the running copy (WheelRequest.h). On Windows 11 classic verbs are listed under
// "Show more options" (Shift+F10); the compact menu only takes packaged extensions.
namespace sd::shellverb
{
    // `classes` is the key the verbs go under: null for HKCU\Software\Classes (tests pass their own).
    bool Register(const std::wstring& exe, HKEY classes = nullptr);
    bool Unregister(HKEY classes = nullptr);
    bool IsRegistered(HKEY classes = nullptr);
    std::wstring CommandFor(const std::wstring& exe);
}
