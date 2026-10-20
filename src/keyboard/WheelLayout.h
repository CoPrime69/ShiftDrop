#pragma once
#include <cstddef>
#include <vector>

// Where each slice of a wheel sits (pure math, shared by the overlay's drawing and hit-testing and by
// the keyboard model).
//
// One ring holds up to 8 slices, which is where its labels stop fitting. 9 to 16 slices take two
// rings, split as evenly as possible (the inner ring gets the odd one). Beyond 16 the wheel pages:
// 15 slices plus a "More" slice that turns to the next 15 (and from the last page back to the first).
//
// Angles are in degrees, 0 at 12 o'clock, clockwise. Radii are in DIPs.
namespace sd::wheel
{
    inline constexpr size_t kRingCapacity = 8;
    inline constexpr size_t kTwoRingCapacity = 16;
    inline constexpr size_t kPageSize = kTwoRingCapacity - 1;   // a paged wheel's last slot is "More"

    inline constexpr float kCentreDip = 52.f;     // centre disc = dead zone: dropping here cancels
    inline constexpr float kInnerInDip = 58.f;    // inner ring
    inline constexpr float kInnerOutDip = 128.f;
    inline constexpr float kOuterInDip = 134.f;   // outer ring (only with more than 8 slices)
    inline constexpr float kOuterOutDip = 192.f;

    struct Slot
    {
        int ring = 0;    // 0 = inner, 1 = outer
        int index = 0;   // position in its ring: 0 at 12 o'clock, then clockwise
        int count = 1;   // slots in that ring
    };

    struct Band
    {
        float inner = 0.f, outer = 0.f;
    };

    struct Layout
    {
        std::vector<Slot> slots;   // the visible slots: the inner ring first, then the outer one
        size_t first = 0;          // content index shown by slots[0]
        size_t total = 0;          // slices in the content
        bool more = false;         // the last slot is "More" (the wheel pages)
        int page = 0, pages = 1;
        int rings = 1;

        bool IsMore(int slot) const { return more && slot == static_cast<int>(slots.size()) - 1; }
        // The content index a slot shows; -1 for "More" and out-of-range slots.
        int ContentIndex(int slot) const;
        float SpanDeg(int slot) const;
        float MidDeg(int slot) const;
        Band BandOf(int slot) const;
        float OuterEdge() const;   // outer edge of the outermost ring, DIPs
    };

    // `page` wraps, so "More" can always ask for page + 1.
    Layout Arrange(size_t total, int page = 0);
    Band RingBand(int ring);

    inline constexpr int kOutside = -1;
    inline constexpr int kCentre = -2;
    // The visible slot under a point `distanceDip` from the centre at `degrees`; kCentre inside the
    // dead zone (or when there are no slots), kOutside more than `reachDip` beyond the outer edge.
    int HitSlot(const Layout& layout, float distanceDip, float degrees, float reachDip);
}
