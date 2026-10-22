#include "photo/PhotoTools.h"

#include <algorithm>
#include <cmath>

namespace sd::photo
{
    using namespace sd::canvas;

    // ---- What the four editors share -----------------------------------------------------------------

    void PhotoTool::SetPicture(size_t index, std::shared_ptr<const Raster> full)
    {
        m_index = index;
        if (!full)
            return;
        m_width = static_cast<int>(full->width);
        m_height = static_cast<int>(full->height);
        m_photo = Picture{ Reduced(full, kPreviewEdge), m_width, m_height };
        Touch();
        PictureReady(index);
    }

    void PhotoTool::ImageReady(size_t index, int width, int height)
    {
        m_index = index;
        m_width = width;
        m_height = height;
        m_photo = {};
        Touch();
        auto full = m_host ? m_host->Picture() : nullptr;
        if (!full)
            return;
        if (std::max(full->width, full->height) <= kPreviewEdge)
        {
            SetPicture(index, std::move(full));
            return;
        }
        // A large photo: the preview works from a copy the size of a big screen, made on a worker.
        m_host->SetStatus(L"Preparing the preview…");
        m_host->RunAsync([this, full, index](std::stop_token) -> std::function<void()> {
            auto reduced = Reduced(full, kPreviewEdge);
            return [this, reduced, index, w = static_cast<int>(full->width), h = static_cast<int>(full->height)] {
                if (index != m_index || w != m_width || h != m_height)
                    return;
                m_photo = Picture{ reduced, w, h };
                Touch();
                m_host->SetStatus({});
                PictureReady(index);
                m_host->Redraw();
            };
        });
    }

    void PhotoTool::UseDevice(unsigned device)
    {
        if (device == m_device)
            return;
        m_device = device;
        m_bitmaps.Clear();
        DeviceChanged();
    }

    std::unique_ptr<ICanvasTool> MakeEditTool(const std::vector<std::wstring>& files, const nlohmann::json& params)
    {
        return std::make_unique<EditTool>(files, params);
    }

    // ---- Edit Photos ----------------------------------------------------------------------------------

    namespace
    {
        const wchar_t* const kGroups[] = { L"Light", L"Colour", L"Detail", L"Effects" };

        double Rounded(double v, int decimals)
        {
            const double k = std::pow(10.0, decimals);
            return std::round(v * k) / k;
        }
    }

    EditTool::EditTool(std::vector<std::wstring> files, const nlohmann::json& params) : PhotoTool(std::move(files)), m_adjust(ParseAdjustments(params)) {}

    void EditTool::PictureReady(size_t)
    {
        m_haze.reset();
        m_cpu = {};
        if (m_photo.Empty())
            return;
        // The dark channel analysis is a small statistical pass (on a 512-pixel copy): done once per
        // picture, so the Dehaze slider moves the preview at once.
        auto raster = m_photo.raster;
        if (!m_host)
        {
            m_haze = std::make_shared<const HazeModel>(AnalyseHaze(*raster));
            return;
        }
        m_host->RunAsync([this, raster](std::stop_token) -> std::function<void()> {
            std::wstring error;
            fx::CompileShaders(error);   // once per process, here rather than on the first preview
            auto haze = std::make_shared<const HazeModel>(AnalyseHaze(*raster));
            return [this, raster, haze] {
                if (m_photo.raster != raster)
                    return;
                m_haze = haze;
                if (m_adjust.dehaze != 0)
                    Touch();
                m_host->Redraw();
            };
        });
    }

    void EditTool::DeviceChanged()
    {
        m_graph = {};
        m_cpu = {};
    }

    bool EditTool::Render(ID2D1DeviceContext* dc, double scale, unsigned device)
    {
        UseDevice(device);
        if (m_photo.Empty())
            return false;
        static const Adjustments none;
        const Adjustments& a = m_compare ? none : m_adjust;
        const HazeModel* haze = m_haze && a.dehaze != 0 ? m_haze.get() : nullptr;
        return SUCCEEDED(DrawAdjusted(dc, device, scale, m_photo, a, haze, m_graph, m_bitmaps, m_cpu));
    }

    Response EditTool::PointerDown(const Pointer&)
    {
        // Press and hold to see the original.
        m_compare = true;
        Touch();
        return kRedraw;
    }

    Response EditTool::PointerUp(const Pointer&) { return PointerCancel(); }

    Response EditTool::PointerCancel()
    {
        if (!m_compare)
            return {};
        m_compare = false;
        Touch();
        return kRedraw;
    }

    Response EditTool::Key(unsigned vk, bool, bool ctrl)
    {
        if (vk == 'A' && !ctrl && !m_photo.Empty())
            return OnControl(ControlEvent{ "preset", 1 });
        return {};
    }

    void EditTool::Draw(IPainter& p) const
    {
        if (m_compare)
            p.Tag({ 12 / p.Zoom(), 12 / p.Zoom() }, L"Original", p.Theme().accent, p.Theme().onAccent);
    }

    void EditTool::BuildPanel(std::vector<Control>& out) const
    {
        Control section;
        section.kind = ControlKind::Section;
        section.label = L"Presets";
        out.push_back(section);

        Control presets;
        presets.id = "preset";
        presets.kind = ControlKind::Choice;
        presets.chips = true;
        presets.options = { L"Original", L"Auto" };
        for (const auto& p : Presets())
            presets.options.emplace_back(p.label);
        if (m_adjust.IsIdentity())
            presets.selected = 0;
        for (size_t i = 0; i < Presets().size(); ++i)
            if (Presets()[i].values == m_adjust)
                presets.selected = static_cast<int>(i) + 2;
        presets.enabled = !m_photo.Empty();
        out.push_back(presets);

        section.label = L"Adjust";
        out.push_back(section);
        Control groups;
        groups.id = "group";
        groups.kind = ControlKind::Choice;
        for (int i = 0; i < 4; ++i)
        {
            groups.options.emplace_back(kGroups[i]);
            if (m_group == kGroups[i])
                groups.selected = i;
        }
        out.push_back(groups);

        for (const auto& s : Sliders())
        {
            if (m_group != s.group)
                continue;
            Control c;
            c.id = s.key;
            c.kind = ControlKind::Slider;
            c.label = s.label;
            c.value = m_adjust.*s.field;
            c.min = s.min;
            c.max = s.max;
            c.decimals = s.decimals;
            if (std::string_view(s.key) == "exposure")
                c.text = L" EV";
            else if (std::string_view(s.key) == "grainSize")
            {
                c.text = L" px";
                c.enabled = m_adjust.grain > 0;
            }
            out.push_back(c);
        }

        Control note;
        note.kind = ControlKind::Note;
        note.text = m_files.size() > 1 ? L"Press and hold on the picture to see the original. Every photo gets these settings."
                                       : L"Press and hold on the picture to see the original.";
        out.push_back(note);

        Control reset;
        reset.id = "reset";
        reset.kind = ControlKind::Button;
        reset.label = L"Reset";
        reset.icon = 0xE72C;   // Refresh
        reset.enabled = !m_adjust.IsIdentity();
        out.push_back(reset);
    }

    Response EditTool::OnControl(const ControlEvent& e)
    {
        if (e.id == "group")
        {
            if (e.index >= 0 && e.index < 4)
                m_group = kGroups[e.index];
            return { true, false, true };
        }
        if (e.id == "preset")
        {
            const double grainSize = m_adjust.grainSize;
            if (e.index == 0)
                m_adjust = {};
            else if (e.index == 1)
            {
                if (m_photo.Empty())
                    return {};
                m_adjust = AutoAdjust(*m_photo.raster);
            }
            else if (e.index >= 2 && static_cast<size_t>(e.index - 2) < Presets().size())
                m_adjust = Presets()[static_cast<size_t>(e.index - 2)].values;
            else
                return {};
            if (m_adjust.grain == 0)
                m_adjust.grainSize = grainSize;
            Touch();
            return kChanged;
        }
        if (e.id == "reset")
        {
            m_adjust = {};
            Touch();
            return kChanged;
        }
        if (const auto* s = FindSlider(e.id))
        {
            m_adjust.*s->field = std::clamp(Rounded(e.value, s->decimals), s->min, s->max);
            Touch();
            return e.final ? kChanged : Response{ true, false, true };
        }
        return {};
    }

    nlohmann::json EditTool::State() const { return ToJson(m_adjust); }

    void EditTool::Restore(const nlohmann::json& state)
    {
        m_adjust = ParseAdjustments(state);
        Touch();
    }

    bool EditTool::CanApply(std::wstring* why) const
    {
        if (!m_adjust.IsIdentity())
            return true;
        if (why)
            *why = L"Move a slider or pick a preset first";
        return false;
    }

    nlohmann::json EditTool::Params() const { return ToJson(m_adjust); }
}
