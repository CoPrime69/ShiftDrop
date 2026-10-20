#include "keyboard/Hotkey.h"
#include "keyboard/KeyNav.h"
#include "keyboard/ShellVerb.h"
#include "keyboard/WheelLayout.h"
#include "keyboard/WheelRequest.h"

#include <gtest/gtest.h>
#include <wil/resource.h>

using namespace sd;
using keynav::Key;

namespace
{
    std::vector<bool> All(size_t n) { return std::vector<bool>(n, true); }
}

TEST(WheelLayout, OneRingUpToEight)
{
    for (size_t n = 1; n <= 8; ++n)
    {
        const auto l = wheel::Arrange(n);
        EXPECT_EQ(l.rings, 1) << n;
        EXPECT_EQ(l.slots.size(), n);
        EXPECT_FALSE(l.more);
        EXPECT_EQ(l.slots.back().count, static_cast<int>(n));
    }
    EXPECT_TRUE(wheel::Arrange(0).slots.empty());
}

TEST(WheelLayout, TwoRingsSplitEvenly)
{
    const auto twelve = wheel::Arrange(12);
    EXPECT_EQ(twelve.rings, 2);
    ASSERT_EQ(twelve.slots.size(), 12u);
    EXPECT_EQ(twelve.slots[5].ring, 0);
    EXPECT_EQ(twelve.slots[6].ring, 1);
    EXPECT_EQ(twelve.slots[6].index, 0);
    EXPECT_EQ(twelve.slots[0].count, 6);
    EXPECT_EQ(twelve.slots[11].count, 6);
    const auto nine = wheel::Arrange(9);
    EXPECT_EQ(nine.slots[0].count, 5);   // the inner ring gets the odd one
    EXPECT_EQ(nine.slots[8].count, 4);
    EXPECT_FLOAT_EQ(twelve.OuterEdge(), wheel::kOuterOutDip);
    EXPECT_FLOAT_EQ(wheel::Arrange(8).OuterEdge(), wheel::kInnerOutDip);
    // Each label gets at least as much arc as on a full one-ring wheel of 8.
    const float eightArc = wheel::kInnerOutDip * 3.14159f * 2.f / 8.f;
    const float outerArc = wheel::kOuterOutDip * 3.14159f * 2.f / 8.f;   // 16 slices: 8 + 8
    EXPECT_GE(outerArc, eightArc);
}

TEST(WheelLayout, PagesBeyondSixteen)
{
    const auto p0 = wheel::Arrange(20, 0);
    EXPECT_TRUE(p0.more);
    EXPECT_EQ(p0.pages, 2);
    EXPECT_EQ(p0.slots.size(), 16u);   // 15 + More
    EXPECT_TRUE(p0.IsMore(15));
    EXPECT_EQ(p0.ContentIndex(14), 14);
    EXPECT_EQ(p0.ContentIndex(15), -1);
    const auto p1 = wheel::Arrange(20, 1);
    EXPECT_EQ(p1.first, 15u);
    EXPECT_EQ(p1.slots.size(), 6u);   // 5 + More
    EXPECT_EQ(p1.rings, 1);
    EXPECT_EQ(p1.ContentIndex(0), 15);
    EXPECT_EQ(wheel::Arrange(20, 2).page, 0);   // More on the last page goes back to the first
    EXPECT_EQ(wheel::Arrange(20, -1).page, 1);
}

TEST(WheelLayout, HitTestFindsRingsAndSlices)
{
    const auto one = wheel::Arrange(4);
    EXPECT_EQ(wheel::HitSlot(one, 30.f, 0.f, 7.f), wheel::kCentre);
    EXPECT_EQ(wheel::HitSlot(one, 100.f, 0.f, 7.f), 0);
    EXPECT_EQ(wheel::HitSlot(one, 100.f, 90.f, 7.f), 1);
    EXPECT_EQ(wheel::HitSlot(one, 100.f, 350.f, 7.f), 0);   // slice 0 straddles 12 o'clock
    EXPECT_EQ(wheel::HitSlot(one, 100.f, 316.f, 7.f), 0);
    EXPECT_EQ(wheel::HitSlot(one, 100.f, 314.f, 7.f), 3);
    EXPECT_EQ(wheel::HitSlot(one, 134.f, 0.f, 7.f), 0);    // within the hover lift
    EXPECT_EQ(wheel::HitSlot(one, 140.f, 0.f, 7.f), wheel::kOutside);

    const auto two = wheel::Arrange(12);
    EXPECT_EQ(wheel::HitSlot(two, 100.f, 0.f, 7.f), 0);
    EXPECT_EQ(wheel::HitSlot(two, 132.f, 0.f, 7.f), 6);    // past the middle of the gap: outer ring
    EXPECT_EQ(wheel::HitSlot(two, 160.f, 60.f, 7.f), 7);
    EXPECT_EQ(wheel::HitSlot(two, 160.f, -60.f, 7.f), 11);
    EXPECT_EQ(wheel::HitSlot(two, 205.f, 0.f, 7.f), wheel::kOutside);
    EXPECT_EQ(wheel::HitSlot(wheel::Arrange(0), 100.f, 0.f, 7.f), wheel::kCentre);
}

TEST(KeyNav, LeftRightGoRoundSkippingDimmed)
{
    const auto l = wheel::Arrange(5);
    std::vector<bool> sel{ true, false, true, true, false };
    EXPECT_EQ(keynav::Navigate(l, sel, -1, Key::Right).slot, 0);   // from nothing: 12 o'clock
    EXPECT_EQ(keynav::Navigate(l, sel, -1, Key::Left).slot, 3);    // just before it (4 is dimmed)
    EXPECT_EQ(keynav::Navigate(l, sel, 0, Key::Right).slot, 2);    // 1 is dimmed
    EXPECT_EQ(keynav::Navigate(l, sel, 3, Key::Right).slot, 0);    // wraps
    EXPECT_EQ(keynav::Navigate(l, sel, 0, Key::Left).slot, 3);
    EXPECT_EQ(keynav::Navigate(l, std::vector<bool>(5, false), -1, Key::Right).slot, -1);
    std::vector<bool> only{ false, false, true, false, false };
    EXPECT_EQ(keynav::Navigate(l, only, 2, Key::Right).slot, 2);   // nowhere else to go
}

TEST(KeyNav, UpDownSwitchWheelsOnOneRing)
{
    const auto l = wheel::Arrange(6);
    const auto up = keynav::Navigate(l, All(6), 2, Key::Up);
    EXPECT_EQ(up.wheel, -1);
    EXPECT_EQ(keynav::Navigate(l, All(6), 2, Key::Down).wheel, 1);
    EXPECT_EQ(keynav::Navigate(l, All(6), -1, Key::Down).wheel, 1);
}

TEST(KeyNav, UpDownMoveBetweenRings)
{
    const auto l = wheel::Arrange(12);   // 6 inner (every 60°), 6 outer
    EXPECT_EQ(keynav::Navigate(l, All(12), 1, Key::Up).slot, 7);     // 60° inner -> 60° outer
    EXPECT_EQ(keynav::Navigate(l, All(12), 7, Key::Down).slot, 1);
    EXPECT_EQ(keynav::Navigate(l, All(12), 7, Key::Up).wheel, -1);   // past the outer ring
    EXPECT_EQ(keynav::Navigate(l, All(12), 1, Key::Down).wheel, 1);  // past the inner ring
    EXPECT_EQ(keynav::Navigate(l, All(12), -1, Key::Up).slot, 6);    // from nothing: outer ring at 12 o'clock
    EXPECT_EQ(keynav::Navigate(l, All(12), 6, Key::Right).slot, 7);  // stays on the outer ring
    EXPECT_EQ(keynav::Navigate(l, All(12), 11, Key::Right).slot, 6);

    // Uneven rings: 5 inner (72° apart), 4 outer (90° apart). From inner 144° the nearest outer is 180°.
    const auto nine = wheel::Arrange(9);
    EXPECT_EQ(keynav::Navigate(nine, All(9), 2, Key::Up).slot, 7);
    // A dimmed target is skipped for the next nearest.
    auto sel = All(9);
    sel[7] = false;
    EXPECT_EQ(keynav::Navigate(nine, sel, 2, Key::Up).slot, 6);
    // Nothing selectable on the outer ring: Up switches wheels instead.
    std::vector<bool> innerOnly{ true, true, true, true, true, false, false, false, false };
    EXPECT_EQ(keynav::Navigate(nine, innerOnly, 2, Key::Up).wheel, -1);
}

TEST(KeyNav, FirstSelectableAfterASwitch)
{
    EXPECT_EQ(keynav::FirstSelectable(wheel::Arrange(4), { false, false, true, true }), 2);
    EXPECT_EQ(keynav::FirstSelectable(wheel::Arrange(12), { false, false, false, false, false, false, false, true, false, false, false, false }), 7);
    EXPECT_EQ(keynav::FirstSelectable(wheel::Arrange(3), { false, false, false }), -1);
}

TEST(KeyNav, RepeatFiresOnPressThenAfterTheDelay)
{
    keynav::Repeat r(400, 60);
    EXPECT_FALSE(r.Update(false, 0));
    EXPECT_TRUE(r.Update(true, 1000));    // the press
    EXPECT_FALSE(r.Update(true, 1030));
    EXPECT_FALSE(r.Update(true, 1390));
    EXPECT_TRUE(r.Update(true, 1400));    // repeat starts after the delay
    EXPECT_FALSE(r.Update(true, 1430));
    EXPECT_TRUE(r.Update(true, 1460));
    EXPECT_FALSE(r.Update(false, 1490));  // released
    EXPECT_TRUE(r.Update(true, 1500));    // a new press fires at once
}

TEST(Hotkey, ParsesAndPrints)
{
    Hotkey k;
    ASSERT_TRUE(ParseHotkey(L"Ctrl+Shift+Space", k));
    EXPECT_EQ(k, kDefaultExplorerHotkey);
    EXPECT_EQ(HotkeyText(k), L"Ctrl+Shift+Space");
    ASSERT_TRUE(ParseHotkey(L" alt + f2 ", k));
    EXPECT_EQ(k.modifiers, static_cast<UINT>(MOD_ALT));
    EXPECT_EQ(k.vk, static_cast<UINT>(VK_F2));
    EXPECT_EQ(HotkeyText(k), L"Alt+F2");
    ASSERT_TRUE(ParseHotkey(L"control+alt+s", k));
    EXPECT_EQ(HotkeyText(k), L"Ctrl+Alt+S");
    ASSERT_TRUE(ParseHotkey(L"F9", k));   // a function key needs no modifier
    ASSERT_TRUE(ParseHotkey(L"Ctrl+PageDown", k));
    EXPECT_EQ(k.vk, static_cast<UINT>(VK_NEXT));
}

TEST(Hotkey, RefusesWinAndBareKeys)
{
    Hotkey k;
    std::wstring why;
    EXPECT_FALSE(ParseHotkey(L"Win+Space", k, &why));
    EXPECT_NE(why.find(L"Win"), std::wstring::npos);
    EXPECT_FALSE(ParseHotkey(L"Shift+A", k, &why));   // would take over typing capitals
    EXPECT_FALSE(ParseHotkey(L"Space", k));
    EXPECT_FALSE(ParseHotkey(L"Ctrl+", k));
    EXPECT_FALSE(ParseHotkey(L"Ctrl+Banana", k));
    EXPECT_FALSE(ParseHotkey(L"Space+Ctrl", k));
    EXPECT_FALSE(ParseHotkey(L"", k));
    EXPECT_FALSE(ParseHotkey(L"Ctrl+F25", k));
}

TEST(WheelRequest, ArgsAfterWheel)
{
    using namespace wheelrequest;
    EXPECT_TRUE(WheelPaths({ L"--autostart" }).empty());
    const auto paths = WheelPaths({ L"--WHEEL", L"C:\\a b\\photo.jpg", L"D:\\x.png" });
    ASSERT_EQ(paths.size(), 2u);
    EXPECT_EQ(paths[0], L"C:\\a b\\photo.jpg");
    EXPECT_TRUE(HasArg({ L"--Register-Shell" }, kRegisterShellArg));
    EXPECT_FALSE(HasArg({ L"--register" }, kRegisterShellArg));
}

TEST(WheelRequest, CopyDataRoundTrip)
{
    using namespace wheelrequest;
    const std::vector<std::wstring> paths{ L"C:\\one.jpg", L"C:\\two words\\two.png", L"\\\\server\\share\\three.mp4" };
    const auto data = Encode(paths);
    EXPECT_EQ(Decode(data.data(), data.size()), paths);
    EXPECT_TRUE(Decode(nullptr, 5).empty());
    const wchar_t noTail[] = L"C:\\a\0C:\\b";   // 9 characters: the last NUL left out
    EXPECT_EQ(Decode(noTail, 9), (std::vector<std::wstring>{ L"C:\\a", L"C:\\b" }));
    EXPECT_TRUE(Encode({}).empty());
}

TEST(WheelRequest, SelectionDropsDuplicatesAndNonFiles)
{
    const auto clean = wheelrequest::CleanSelection({ L"C:\\A.jpg", L"", L"c:\\a.JPG", L"C:\\b.png" });
    EXPECT_EQ(clean, (std::vector<std::wstring>{ L"C:\\A.jpg", L"C:\\b.png" }));
}

TEST(ShellVerb, RegistersPerUserAndRemovesCleanly)
{
    // A throwaway key instead of HKCU\Software\Classes, so the test never touches Explorer.
    const wchar_t* testKey = L"Software\\ShiftDropTests\\Classes";
    RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\ShiftDropTests");
    wil::unique_hkey classes;
    ASSERT_EQ(RegCreateKeyExW(HKEY_CURRENT_USER, testKey, 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &classes, nullptr), ERROR_SUCCESS);
    EXPECT_FALSE(shellverb::IsRegistered(classes.get()));
    const std::wstring exe = L"C:\\Program Files\\Shift Drop\\ShiftDrop.exe";
    ASSERT_TRUE(shellverb::Register(exe, classes.get()));
    EXPECT_TRUE(shellverb::IsRegistered(classes.get()));

    wchar_t value[512]{};
    DWORD size = sizeof(value);
    ASSERT_EQ(RegGetValueW(classes.get(), L"*\\shell\\ShiftDrop\\command", nullptr, RRF_RT_REG_SZ, nullptr, value, &size), ERROR_SUCCESS);
    EXPECT_EQ(std::wstring(value), L"\"C:\\Program Files\\Shift Drop\\ShiftDrop.exe\" --wheel \"%1\"");
    size = sizeof(value);
    ASSERT_EQ(RegGetValueW(classes.get(), L"Directory\\shell\\ShiftDrop", L"MultiSelectModel", RRF_RT_REG_SZ, nullptr, value, &size), ERROR_SUCCESS);
    EXPECT_EQ(std::wstring(value), L"Player");
    size = sizeof(value);
    ASSERT_EQ(RegGetValueW(classes.get(), L"Directory\\shell\\ShiftDrop", L"MUIVerb", RRF_RT_REG_SZ, nullptr, value, &size), ERROR_SUCCESS);
    EXPECT_EQ(std::wstring(value), L"ShiftDrop\u2026");

    EXPECT_TRUE(shellverb::Unregister(classes.get()));
    EXPECT_FALSE(shellverb::IsRegistered(classes.get()));
    EXPECT_TRUE(shellverb::Unregister(classes.get()));   // nothing left to remove is fine
    wil::unique_hkey gone;
    EXPECT_NE(RegOpenKeyExW(classes.get(), L"*\\shell\\ShiftDrop", 0, KEY_READ, &gone), ERROR_SUCCESS);
    classes.reset();
    RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\ShiftDropTests");
}
