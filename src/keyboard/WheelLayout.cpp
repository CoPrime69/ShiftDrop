#include "keyboard/WheelLayout.h"

#include <algorithm>
#include <cmath>

namespace sd::wheel
{
    namespace
    {
        void Fill(Layout& layout, size_t visible)
        {
            layout.slots.clear();
            layout.rings = 1;
            if (visible == 0)
                return;
            if (visible <= kRingCapacity)
            {
                for (size_t i = 0; i < visible; ++i)
                    layout.slots.push_back(Slot{ 0, static_cast<int>(i), static_cast<int>(visible) });
                return;
            }
            const size_t inner = (visible + 1) / 2, outer = visible - inner;
            for (size_t i = 0; i < inner; ++i)
                layout.slots.push_back(Slot{ 0, static_cast<int>(i), static_cast<int>(inner) });
            for (size_t i = 0; i < outer; ++i)
                layout.slots.push_back(Slot{ 1, static_cast<int>(i), static_cast<int>(outer) });
            layout.rings = 2;
        }
    }

    Layout Arrange(size_t total, int page)
    {
        Layout layout;
        layout.total = total;
        if (total <= kTwoRingCapacity)
        {
            Fill(layout, total);
            return layout;
        }
        layout.pages = static_cast<int>((total + kPageSize - 1) / kPageSize);
        layout.page = ((page % layout.pages) + layout.pages) % layout.pages;
        layout.first = static_cast<size_t>(layout.page) * kPageSize;
        layout.more = true;
        Fill(layout, std::min(kPageSize, total - layout.first) + 1);
        return layout;
    }

    Band RingBand(int ring)
    {
        return ring == 0 ? Band{ kInnerInDip, kInnerOutDip } : Band{ kOuterInDip, kOuterOutDip };
    }

    int Layout::ContentIndex(int slot) const
    {
        if (slot < 0 || slot >= static_cast<int>(slots.size()) || IsMore(slot))
            return -1;
        return static_cast<int>(first) + slot;
    }

    float Layout::SpanDeg(int slot) const
    {
        return 360.f / static_cast<float>(slots[static_cast<size_t>(slot)].count);
    }

    float Layout::MidDeg(int slot) const
    {
        return static_cast<float>(slots[static_cast<size_t>(slot)].index) * SpanDeg(slot);
    }

    Band Layout::BandOf(int slot) const { return RingBand(slots[static_cast<size_t>(slot)].ring); }

    float Layout::OuterEdge() const { return RingBand(rings - 1).outer; }

    int HitSlot(const Layout& layout, float d, float deg, float reach)
    {
        if (d > layout.OuterEdge() + reach)
            return kOutside;
        if (d < kCentreDip || layout.slots.empty())
            return kCentre;
        // Between the rings the boundary is the middle of the gap, so there is no dead strip.
        const int ring = layout.rings == 2 && d > (kInnerOutDip + kOuterInDip) / 2.f ? 1 : 0;
        int base = 0, count = 0;
        for (const auto& s : layout.slots)
        {
            if (s.ring < ring)
                ++base;
            else if (s.ring == ring)
                ++count;
        }
        if (count == 0)
            return kCentre;
        deg = std::fmod(std::fmod(deg, 360.f) + 360.f, 360.f);
        const float span = 360.f / static_cast<float>(count);
        return base + static_cast<int>(std::floor((deg + span / 2.f) / span)) % count;
    }
}
