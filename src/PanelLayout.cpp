#include "PanelLayout.h"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <optional>

namespace sd::panel
{
    float MaxContentWidth(float scale, float workWidth)
    {
        return std::max(0.f, std::min(kMaxWidthDip * scale, workWidth - 2.f * kScreenRoomDip * scale));
    }

    Switcher FitSwitcher(const std::vector<float>& names, float hint, float scale, float available)
    {
        const float s = scale, count = static_cast<float>(names.size());
        const float text = std::accumulate(names.begin(), names.end(), 0.f);
        // Gaps at t = 0 are full size, at t = 1 the smallest; the width is linear in t.
        const auto gap = [s](float full, float least, float t) { return (full + (least - full) * t) * s; };
        const auto width = [&](float t, bool withHint, float nameScale) {
            float w = count * (2.f * gap(kTabPadDip, kTabPadMinDip, t) + kIconDip * s + gap(kIconGapDip, kIconGapMinDip, t)) + text * nameScale;
            if (withHint)
                w += gap(kHintGapDip, kHintGapMinDip, t) + hint + gap(kHintEndDip, kHintEndMinDip, t);
            return w;
        };
        // The least shrinking that fits, if shrinking the gaps is enough.
        const auto fit = [&](bool withHint) -> std::optional<float> {
            const float full = width(0.f, withHint, 1.f), least = width(1.f, withHint, 1.f);
            if (full <= available)
                return 0.f;
            if (least > available)
                return std::nullopt;
            return (full - available) / (full - least);
        };

        Switcher out;
        float t = 1.f;
        out.hint = hint > 0.f;
        if (const auto withHint = out.hint ? fit(true) : std::nullopt)
            t = *withHint;
        else if (const auto bare = fit(false))
        {
            out.hint = false;
            t = *bare;
        }
        else
        {
            // Not even the tabs fit with the smallest gaps: shorten every name by the same share.
            out.hint = false;
            const float fixed = width(1.f, false, 0.f);
            out.nameScale = text > 0.f ? std::clamp((available - fixed) / text, 0.f, 1.f) : 1.f;
        }
        out.pad = gap(kTabPadDip, kTabPadMinDip, t);
        out.iconGap = gap(kIconGapDip, kIconGapMinDip, t);
        if (out.hint)
        {
            out.hintGap = gap(kHintGapDip, kHintGapMinDip, t);
            out.hintEnd = gap(kHintEndDip, kHintEndMinDip, t);
        }
        for (const float name : names)
            out.tabs.push_back(2.f * out.pad + kIconDip * s + out.iconGap + name * out.nameScale);
        out.width = std::accumulate(out.tabs.begin(), out.tabs.end(), 0.f) + (out.hint ? out.hintGap + hint + out.hintEnd : 0.f);
        return out;
    }

    void Approach(float& value, float target, float dtMs, float tauMs, float snap)
    {
        value += (target - value) * (1.f - std::exp(-dtMs / tauMs));
        if (std::abs(target - value) < snap)
            value = target;
    }
}
