#pragma once
#include "keyboard/WheelLayout.h"

#include <cstdint>
#include <vector>

// Picking a slice with the keyboard while a wheel is open (pure state machine; the overlay applies
// the result, KeyboardController feeds it keys).
//
//   Left / Right   counter-clockwise / clockwise around the current ring, skipping dimmed slices,
//                  wrapping round. With nothing highlighted, Right starts at 12 o'clock and Left
//                  just before it.
//   Up / Down      with two rings: out to the outer ring / in to the inner ring, landing on the slice
//                  nearest in angle. Past the last ring (and always on a one-ring wheel) Up goes to
//                  the previous wheel and Down to the next one, the same way scrolling does.
//   Enter          runs the highlighted slice (or turns the page on "More").
//   Esc            cancels.
namespace sd::keynav
{
    enum class Key { Left, Right, Up, Down };

    struct Move
    {
        int slot = -1;   // the slot to highlight (-1 = none); ignored when `wheel` is set
        int wheel = 0;   // -1: switch to the previous wheel, +1: the next one
    };

    // `selectable[i]` says whether visible slot i can be highlighted (an enabled slice, or "More").
    // `current` is the highlighted slot, -1 for none.
    Move Navigate(const wheel::Layout& layout, const std::vector<bool>& selectable, int current, Key key);

    // The slot a keyboard user lands on after switching wheels: the first selectable one from
    // 12 o'clock on the inner ring, else on the outer ring, else -1.
    int FirstSelectable(const wheel::Layout& layout, const std::vector<bool>& selectable);

    // Auto-repeat for a key that is polled (GetAsyncKeyState) rather than delivered as messages:
    // fires once on the press, then after `delayMs` every `intervalMs` while the key stays down.
    class Repeat
    {
    public:
        explicit Repeat(unsigned delayMs = 400, unsigned intervalMs = 60) : m_delay(delayMs), m_interval(intervalMs) {}
        // True when the key should act now.
        bool Update(bool down, uint64_t nowMs);
        void Reset() { m_down = false; }

    private:
        unsigned m_delay, m_interval;
        bool m_down = false;
        uint64_t m_next = 0;
    };
}
