#pragma once

#define IDI_APPICON     101
// The tray glyph for a dark and for a light taskbar (see TrayIcon.h)
#define IDI_TRAY_DARK   102
#define IDI_TRAY_LIGHT  103

// Tray menu commands
#define IDM_OPEN_LOGS   40001
#define IDM_QUIT        40002
#define IDM_HISTORY     40003
#define IDM_OPEN_RECIPES 40004
#define IDM_START_WITH_WINDOWS 40005
// Keyboard and Explorer
#define IDM_EXPLORER_HOTKEY 40030
#define IDM_SHELL_MENU  40031
// Settings window, Float shelf and the OCR language submenu: base + the index in
// ocr::Languages() + 1, base + 0 = Automatic
#define IDM_SETTINGS    40033
#define IDM_SHOW_SHELF  40032
#define IDM_OCR_BASE    40040
#define IDM_OCR_LAST    40069
// "Save browser drops to": base + the VirtualOutput value (see Settings.h)
#define IDM_VIRTUAL_BASE 40090
// Size and Style submenus: base + the WheelSize / Style value (see Settings.h)
#define IDM_SIZE_BASE   40010
#define IDM_STYLE_BASE  40020
// Check for updates: the tray's "Update to 0.7.1...", Settings > Updates' Check now, and its choice:
// base + the updates::Mode value (see updates/Updates.h)
#define IDM_UPDATE      40100
#define IDM_CHECK_UPDATES 40101
#define IDM_UPDATE_NOW  40102   // Settings > Updates' Update now: no question, it was the answer
#define IDM_UPDATE_MODE_BASE 40110
