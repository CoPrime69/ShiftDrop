#include "TrayIcon.h"

#include <gtest/gtest.h>

using namespace sd;

TEST(TrayIcon, FollowsTheTaskbarTheme)
{
    EXPECT_EQ(trayicon::For(false, 96).resource, IDI_TRAY_DARK);
    EXPECT_EQ(trayicon::For(true, 96).resource, IDI_TRAY_LIGHT);
    EXPECT_NE(IDI_TRAY_DARK, IDI_TRAY_LIGHT);
    EXPECT_NE(IDI_TRAY_LIGHT, IDI_APPICON);
}

TEST(TrayIcon, SlotIsTheSmallIconAtTheDpi)
{
    EXPECT_EQ(trayicon::SlotSize(96), 16);
    EXPECT_EQ(trayicon::SlotSize(120), 20);
    EXPECT_EQ(trayicon::SlotSize(144), 24);
    EXPECT_EQ(trayicon::SlotSize(168), 28);
    EXPECT_EQ(trayicon::SlotSize(192), 32);
    EXPECT_EQ(trayicon::SlotSize(0), 16);   // no DPI known: 100%
}

TEST(TrayIcon, EachScaleGetsItsOwnDrawing)
{
    // 100%, 125%, 150% and 200%: the drawing made for that size, on both taskbars.
    for (const bool light : { false, true })
    {
        EXPECT_EQ(trayicon::For(light, 96).size, 16);
        EXPECT_EQ(trayicon::For(light, 120).size, 20);
        EXPECT_EQ(trayicon::For(light, 144).size, 24);
        EXPECT_EQ(trayicon::For(light, 192).size, 32);
    }
}

TEST(TrayIcon, BetweenSizesTheShellOnlyScalesDown)
{
    EXPECT_EQ(trayicon::For(false, 108).size, 20);   // 112.5%: 18 px slot
    EXPECT_EQ(trayicon::For(false, 168).size, 32);   // 175%: 28 px slot
    EXPECT_EQ(trayicon::For(true, 240).size, 32);    // 250%: past the largest drawing
    EXPECT_EQ(trayicon::For(true, 0).size, 16);
    static_assert(trayicon::For(true, 144) == trayicon::Pick{ IDI_TRAY_LIGHT, 24 });
}
