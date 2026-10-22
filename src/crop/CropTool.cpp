#include "crop/CropTool.h"

#include <windows.h>

#include <cmath>
#include <format>

namespace sd::crop
{
    using namespace sd::canvas;

    CropTool::CropTool(size_t fileCount) : m_files(fileCount)
    {
        m_single = true;
        m_canDelete = false;
        m_regions.SetMinSize(8);
    }

    SizeD CropTool::Rotated() const { return RotatedSize(m_imageW, m_imageH, m_params.quarterTurns); }

    double CropTool::Ratio() const
    {
        const SizeD r = Rotated();
        return AspectRatio(m_params.aspect, m_params.ratioW, m_params.ratioH, r.w, r.h);
    }

    RectD CropTool::Box() const
    {
        return m_regions.Size() ? m_regions.Items().front().rect : RectD{};
    }

    void CropTool::SetBox(const RectD& box)
    {
        m_regions.Clear();
        m_regions.Add(box);
        m_regions.SelectAll();
        m_lastSize = { Box().w, Box().h };
    }

    // After the picture's size, rotation, straighten angle or aspect changed: the box must stay
    // inside the usable area and keep the preset's shape.
    void CropTool::ApplyBounds(bool resetBox)
    {
        const SizeD r = Rotated();
        const RectD bounds = StraightenBounds(r.w, r.h, m_params.straighten);
        const double ratio = Ratio();
        m_regions.SetAspect(ratio);
        if (resetBox || !m_regions.Size())
        {
            m_regions.SetBounds(bounds);
            SetBox(LargestCentred(bounds, ratio));
            return;
        }
        // Keep the centre; shrink (in shape) only as much as the new bounds need.
        RectD box = Box();
        if (ratio > 0 && std::abs(box.w / box.h - ratio) > 1e-6)
            box = LargestCentred(box, ratio);
        const double k = std::min({ 1.0, bounds.w / box.w, bounds.h / box.h });
        const PointD c = box.Centre();
        box = { c.x - box.w * k / 2, c.y - box.h * k / 2, box.w * k, box.h * k };
        m_regions.SetBounds(bounds);
        SetBox(ClampInside(box, bounds));
    }

    void CropTool::ImageReady(size_t index, int width, int height)
    {
        if (index != 0)
            return;
        const bool first = m_imageW == 0;
        m_imageW = width;
        m_imageH = height;
        ApplyBounds(first);
    }

    void CropTool::Rotate(int quarterTurns)
    {
        m_params.quarterTurns = ((m_params.quarterTurns + quarterTurns) % 4 + 4) % 4;
        ApplyBounds(true);   // the old box means nothing on a turned picture
    }

    void CropTool::RegionsChanged()
    {
        // A typed size holds until the box is resized by hand (a move keeps it).
        const RectD box = Box();
        if (std::abs(box.w - m_lastSize.w) > 1e-6 || std::abs(box.h - m_lastSize.h) > 1e-6)
            m_params.exactSize = false;
        m_lastSize = { box.w, box.h };
    }

    void CropTool::Draw(IPainter& p) const
    {
        if (!m_regions.Size())
            return;
        const auto& theme = p.Theme();
        const RectD box = Box();
        p.ShadeOutside(box, theme.shade);
        if (m_thirds)
        {
            const Color guide = Color{ 1, 1, 1, 0.45f };
            for (int i = 1; i <= 2; ++i)
            {
                p.Line({ box.x + box.w * i / 3, box.y }, { box.x + box.w * i / 3, box.Bottom() }, guide, 1.f);
                p.Line({ box.x, box.y + box.h * i / 3 }, { box.Right(), box.y + box.h * i / 3 }, guide, 1.f);
            }
        }
        // A dark hairline outside a white frame reads on any picture.
        p.StrokeRect(box, Color{ 0, 0, 0, 0.5f }, 3.f);
        p.StrokeRect(box, Color{ 1, 1, 1, 1 }, 1.5f);
        const Grip grips[] = { Grip::NW, Grip::N, Grip::NE, Grip::E, Grip::SE, Grip::S, Grip::SW, Grip::W };
        for (const Grip g : grips)
        {
            const double x = (g == Grip::NW || g == Grip::W || g == Grip::SW) ? box.x : (g == Grip::N || g == Grip::S) ? box.Centre().x : box.Right();
            const double y = (g == Grip::NW || g == Grip::N || g == Grip::NE) ? box.y : (g == Grip::W || g == Grip::E) ? box.Centre().y : box.Bottom();
            p.Grip({ x, y }, m_hot.index == 0 && m_hot.grip == g);
        }
        const RectI px = ToPixels(box, static_cast<int>(Rotated().w), static_cast<int>(Rotated().h));
        p.Tag({ box.x, box.Bottom() + 6 / p.Zoom() }, std::format(L"{} × {}", px.w, px.h), theme.accent, theme.onAccent);
    }

    Response CropTool::Key(unsigned vk, bool shift, bool ctrl)
    {
        if (vk == 'R' && !ctrl)
        {
            Rotate(shift ? -1 : 1);
            return kChanged;
        }
        return RegionTool::Key(vk, shift, ctrl);
    }

    void CropTool::BuildPanel(std::vector<Control>& out) const
    {
        Control c;
        c.kind = ControlKind::Section;
        c.label = L"Aspect ratio";
        out.push_back(c);

        Control aspect;
        aspect.id = "aspect";
        aspect.kind = ControlKind::Choice;
        aspect.chips = true;
        int i = 0;
        for (const auto& preset : AspectPresets())
        {
            aspect.options.emplace_back(preset.label);
            if (m_params.aspect == preset.id)
                aspect.selected = i;
            ++i;
        }
        out.push_back(aspect);
        if (m_params.aspect == "custom")
        {
            Control w;
            w.id = "ratioW";
            w.kind = ControlKind::Number;
            w.label = L"Ratio W";
            w.value = m_params.ratioW;
            w.min = 0.01;
            w.max = 1000;
            w.decimals = 2;
            w.half = true;
            out.push_back(w);
            Control h = w;
            h.id = "ratioH";
            h.label = L"Ratio H";
            h.value = m_params.ratioH;
            out.push_back(h);
        }

        c.label = L"Size";
        out.push_back(c);
        const SizeD r = Rotated();
        const RectI px = ToPixels(Box(), static_cast<int>(r.w), static_cast<int>(r.h));
        Control w;
        w.id = "width";
        w.kind = ControlKind::Number;
        w.label = L"Width";
        w.text = L"px";
        w.value = px.w;
        w.min = 1;
        w.max = std::max(1.0, m_regions.Bounds().w);
        w.half = true;
        out.push_back(w);
        Control h = w;
        h.id = "height";
        h.label = L"Height";
        h.value = px.h;
        h.max = std::max(1.0, m_regions.Bounds().h);
        out.push_back(h);

        c.label = L"Rotate";
        out.push_back(c);
        Control left;
        left.id = "rotateLeft";
        left.kind = ControlKind::Button;
        left.label = L"↺  Left";
        left.half = true;
        out.push_back(left);
        Control right = left;
        right.id = "rotateRight";
        right.label = L"↻  Right";
        out.push_back(right);
        Control straighten;
        straighten.id = "straighten";
        straighten.kind = ControlKind::Slider;
        straighten.label = L"Straighten";
        straighten.text = L"°";
        straighten.value = m_params.straighten;
        straighten.min = -45;
        straighten.max = 45;
        straighten.decimals = 1;
        out.push_back(straighten);

        Control thirds;
        thirds.id = "thirds";
        thirds.kind = ControlKind::Toggle;
        thirds.label = L"Rule-of-thirds grid";
        thirds.on = m_thirds;
        out.push_back(thirds);

        if (m_files > 1)
        {
            c.label = std::format(L"The other {} images", m_files - 1);
            out.push_back(c);
            Control place;
            place.id = "place";
            place.kind = ControlKind::Choice;
            place.options = { L"Centred", L"Same place" };
            place.selected = m_params.samePlace ? 1 : 0;
            out.push_back(place);
            Control note;
            note.kind = ControlKind::Note;
            note.text = m_params.exactSize ? L"Each gets this exact size (smaller if it doesn't fit)."
                                           : L"Each gets this shape, scaled to its size.";
            out.push_back(note);
        }

        Control reset;
        reset.id = "reset";
        reset.kind = ControlKind::Button;
        reset.label = L"Reset";
        reset.icon = 0xE72C;   // Refresh
        out.push_back(reset);
    }

    Response CropTool::OnControl(const ControlEvent& e)
    {
        if (e.id == "aspect")
        {
            const auto& presets = AspectPresets();
            if (e.index < 0 || e.index >= static_cast<int>(presets.size()))
                return {};
            m_params.aspect = presets[static_cast<size_t>(e.index)].id;
            if (m_params.aspect == "custom" && (m_params.ratioW <= 0 || m_params.ratioH <= 0))
                m_params.ratioW = m_params.ratioH = 1;
            ApplyBounds(m_params.aspect != "free");
            return kChanged;
        }
        if (e.id == "ratioW" || e.id == "ratioH")
        {
            (e.id == "ratioW" ? m_params.ratioW : m_params.ratioH) = std::clamp(e.value, 0.01, 1000.0);
            ApplyBounds(true);
            return kChanged;
        }
        if (e.id == "width" || e.id == "height")
        {
            // A typed size, kept centred where the box is and in the preset's shape.
            const RectD bounds = m_regions.Bounds();
            const RectD box = Box();
            const double ratio = Ratio();
            double w = box.w, h = box.h;
            if (e.id == "width")
            {
                w = std::clamp(std::round(e.value), 1.0, bounds.w);
                if (ratio > 0)
                    h = w / ratio;
            }
            else
            {
                h = std::clamp(std::round(e.value), 1.0, bounds.h);
                if (ratio > 0)
                    w = h * ratio;
            }
            const double k = std::min({ 1.0, bounds.w / w, bounds.h / h });
            w *= k;
            h *= k;
            const PointD c = box.Centre();
            SetBox(ClampInside({ c.x - w / 2, c.y - h / 2, w, h }, bounds));
            m_params.exactSize = true;
            return kChanged;
        }
        if (e.id == "rotateLeft" || e.id == "rotateRight")
        {
            Rotate(e.id == "rotateLeft" ? -1 : 1);
            return kChanged;
        }
        if (e.id == "straighten")
        {
            m_params.straighten = std::clamp(std::round(e.value * 10) / 10, -45.0, 45.0);
            ApplyBounds(false);
            return e.final ? kChanged : Response{ true, false, true };
        }
        if (e.id == "thirds")
        {
            m_thirds = e.on;
            return kRedraw;
        }
        if (e.id == "place")
        {
            m_params.samePlace = e.index == 1;
            return kChanged;
        }
        if (e.id == "reset")
        {
            const bool same = m_params.samePlace;
            m_params = {};
            m_params.samePlace = same;
            ApplyBounds(true);
            return kChanged;
        }
        return {};
    }

    nlohmann::json CropTool::State() const
    {
        auto s = Params();
        s["thirds"] = m_thirds;
        return s;
    }

    void CropTool::Restore(const nlohmann::json& state)
    {
        m_params = ParseParams(state);
        m_thirds = state.value("thirds", true);
        const SizeD r = Rotated();
        m_regions.SetBounds(StraightenBounds(r.w, r.h, m_params.straighten));
        m_regions.SetAspect(Ratio());
        SetBox(m_params.box);
    }

    bool CropTool::CanApply(std::wstring* why) const
    {
        const RectD box = Box();
        if (box.w >= 1 && box.h >= 1)
            return true;
        if (why)
            *why = L"Draw a crop box first";
        return false;
    }

    nlohmann::json CropTool::Params() const
    {
        CropParams p = m_params;
        p.box = Box();
        const SizeD r = Rotated();
        p.refW = r.w;
        p.refH = r.h;
        return ToJson(p);
    }
}
