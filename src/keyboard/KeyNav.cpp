#include "keyboard/KeyNav.h"

#include <cmath>

namespace sd::keynav
{
    namespace
    {
        bool Selectable(const std::vector<bool>& selectable, int slot)
        {
            return slot >= 0 && static_cast<size_t>(slot) < selectable.size() && selectable[static_cast<size_t>(slot)];
        }

        // Visible slot numbers of one ring, in clockwise order.
        std::vector<int> RingSlots(const wheel::Layout& layout, int ring)
        {
            std::vector<int> out;
            for (size_t i = 0; i < layout.slots.size(); ++i)
                if (layout.slots[i].ring == ring)
                    out.push_back(static_cast<int>(i));
            return out;
        }

        // From position `from` in `ring` (exclusive), the next selectable slot stepping by `step`.
        int Step(const std::vector<int>& ring, const std::vector<bool>& selectable, int from, int step)
        {
            const int n = static_cast<int>(ring.size());
            for (int k = 1; k <= n; ++k)
            {
                const int at = ring[static_cast<size_t>(((from + step * k) % n + n) % n)];
                if (Selectable(selectable, at))
                    return at;
            }
            return -1;
        }

        float AngleDistance(float a, float b)
        {
            const float d = std::fmod(std::fabs(a - b), 360.f);
            return d > 180.f ? 360.f - d : d;
        }

        // The selectable slot of `ring` whose middle is nearest `degrees` (ties: the earlier one).
        int Nearest(const wheel::Layout& layout, const std::vector<bool>& selectable, int ring, float degrees)
        {
            int best = -1;
            float bestD = 1e9f;
            for (const int s : RingSlots(layout, ring))
            {
                if (!Selectable(selectable, s))
                    continue;
                const float d = AngleDistance(layout.MidDeg(s), degrees);
                if (d < bestD - 0.01f)
                {
                    best = s;
                    bestD = d;
                }
            }
            return best;
        }
    }

    int FirstSelectable(const wheel::Layout& layout, const std::vector<bool>& selectable)
    {
        for (int ring = 0; ring < layout.rings; ++ring)
            for (const int s : RingSlots(layout, ring))
                if (Selectable(selectable, s))
                    return s;
        return -1;
    }

    Move Navigate(const wheel::Layout& layout, const std::vector<bool>& selectable, int current, Key key)
    {
        if (current >= static_cast<int>(layout.slots.size()))
            current = -1;
        const int ring = current >= 0 ? layout.slots[static_cast<size_t>(current)].ring : 0;
        switch (key)
        {
        case Key::Left:
        case Key::Right:
        {
            const int step = key == Key::Right ? 1 : -1;
            if (current < 0)
            {
                // Right starts at 12 o'clock (index 0 is checked first), Left just before it.
                for (int r = 0; r < layout.rings; ++r)
                {
                    const auto slots = RingSlots(layout, r);
                    if (const int s = Step(slots, selectable, key == Key::Right ? -1 : 0, step); s >= 0)
                        return Move{ s, 0 };
                }
                return Move{ -1, 0 };
            }
            const auto slots = RingSlots(layout, ring);
            const int pos = layout.slots[static_cast<size_t>(current)].index;
            const int next = Step(slots, selectable, pos, step);
            return Move{ next >= 0 ? next : current, 0 };
        }
        case Key::Up:
        case Key::Down:
        {
            const int target = key == Key::Up ? ring + 1 : ring - 1;
            // From nothing highlighted, Up goes out to the outer ring and Down switches wheels.
            const bool inward = key == Key::Down;
            if (layout.rings == 2 && target >= 0 && target < 2 && !(current < 0 && inward))
            {
                const float at = current >= 0 ? layout.MidDeg(current) : 0.f;
                if (const int s = Nearest(layout, selectable, current < 0 ? 1 : target, at); s >= 0)
                    return Move{ s, 0 };
            }
            return Move{ -1, key == Key::Up ? -1 : 1 };
        }
        }
        return Move{ current, 0 };
    }

    bool Repeat::Update(bool down, uint64_t now)
    {
        if (!down)
        {
            m_down = false;
            return false;
        }
        if (!m_down)
        {
            m_down = true;
            m_next = now + m_delay;
            return true;
        }
        if (now >= m_next)
        {
            m_next = now + m_interval;
            return true;
        }
        return false;
    }
}
