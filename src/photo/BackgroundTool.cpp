#include "photo/PhotoTools.h"
#include "PathUtil.h"

#include <algorithm>
#include <cmath>

namespace sd::photo
{
    using namespace sd::canvas;

    std::unique_ptr<ICanvasTool> MakeBackgroundTool(const std::vector<std::wstring>& files, const nlohmann::json& params)
    {
        return std::make_unique<BackgroundTool>(files, params);
    }

    namespace
    {
        // Five colours and a custom chip fit beside a label.
        const uint32_t kColours[] = { 0xFFFFFF, 0x1F1F1F, 0x6A5ACD, 0xF08A5D, 0x2BB3A3 };
        const wchar_t* const kFills[] = { L"Solid", L"Gradient", L"Blur", L"Picture" };

        Control ColourRow(const std::string& id, const wchar_t* label, uint32_t value, uint32_t custom)
        {
            Control c;
            c.id = id;
            c.kind = ControlKind::Swatches;
            c.label = label;
            c.custom = true;
            for (size_t i = 0; i < std::size(kColours); ++i)
            {
                c.swatches.push_back(Color::Rgb(kColours[i]));
                if (kColours[i] == value)
                    c.selected = static_cast<int>(i);
            }
            c.swatches.push_back(custom ? Color::Rgb(custom) : Color{ 0, 0, 0, 0 });
            if (c.selected < 0 && custom == value)
                c.selected = static_cast<int>(std::size(kColours));
            return c;
        }

        Control SliderControl(const std::string& id, const wchar_t* label, double value, double lo, double hi, const wchar_t* unit)
        {
            Control c;
            c.id = id;
            c.kind = ControlKind::Slider;
            c.label = label;
            c.value = value;
            c.min = lo;
            c.max = hi;
            c.text = unit;
            return c;
        }
    }

    BackgroundTool::BackgroundTool(std::vector<std::wstring> files, const nlohmann::json& params)
        : PhotoTool(std::move(files)), m_params(ParseBackground(params))
    {
        if (std::find(std::begin(kColours), std::end(kColours), m_params.color1) == std::end(kColours))
            m_custom1 = m_params.color1;
        if (std::find(std::begin(kColours), std::end(kColours), m_params.color2) == std::end(kColours))
            m_custom2 = m_params.color2;
    }

    void BackgroundTool::PictureReady(size_t)
    {
        if (m_params.fill == Fill::Image)
            LoadBackgroundImage(m_params.image);   // a recipe's own picture
    }

    SizeD BackgroundTool::Size() const
    {
        if (m_width <= 0 || m_height <= 0)
            return {};
        const auto layout = LayoutBackground(m_width, m_height, m_params);
        return { double(layout.width), double(layout.height) };
    }

    bool BackgroundTool::Render(ID2D1DeviceContext* dc, double scale, unsigned device)
    {
        UseDevice(device);
        if (m_photo.Empty())
            return false;
        const auto layout = LayoutBackground(m_width, m_height, m_params);
        const Picture none;
        const Picture& back = m_params.fill == Fill::Image && m_backPath == m_params.image ? m_backImage : none;
        return SUCCEEDED(DrawBackground(dc, device, scale, m_photo, m_params, layout, back, m_bitmaps, m_cache));
    }

    void BackgroundTool::SetBackgroundImage(const std::wstring& path, Picture picture)
    {
        m_backPath = path;
        m_backImage = std::move(picture);
        m_params.image = path;
        m_params.fill = Fill::Image;
        m_cache = {};
        Touch();
    }

    void BackgroundTool::LoadBackgroundImage(const std::wstring& path)
    {
        if (path.empty() || path == m_backPath)
            return;
        if (!m_host)
            return;
        m_host->SetStatus(L"Opening the picture…");
        m_host->RunAsync([this, path](std::stop_token) -> std::function<void()> {
            auto r = std::make_shared<Raster>();
            std::wstring error;
            const bool ok = SUCCEEDED(LoadRaster(path, *r, error, 2048));
            return [this, path, r, ok, error] {
                if (ok)
                {
                    const bool first = m_params.image != path;
                    SetBackgroundImage(path, Picture::Of(r));
                    m_host->SetStatus({});
                    if (first)
                        m_host->Changed();
                }
                else
                    m_host->SetStatus(L"Can't open that picture: " + error);
                m_host->Redraw();
            };
        });
    }

    void BackgroundTool::BuildPanel(std::vector<Control>& out) const
    {
        Control section;
        section.kind = ControlKind::Section;
        section.label = L"Background";
        out.push_back(section);
        Control fill;
        fill.id = "fill";
        fill.kind = ControlKind::Choice;
        fill.options.assign(std::begin(kFills), std::end(kFills));
        fill.selected = static_cast<int>(m_params.fill);
        out.push_back(fill);

        switch (m_params.fill)
        {
        case Fill::Solid: out.push_back(ColourRow("color1", L"Colour", m_params.color1, m_custom1)); break;
        case Fill::Gradient:
            out.push_back(ColourRow("color1", L"From", m_params.color1, m_custom1));
            out.push_back(ColourRow("color2", L"To", m_params.color2, m_custom2));
            out.push_back(SliderControl("angle", L"Direction", m_params.angle, 0, 360, L"°"));
            break;
        case Fill::Image:
        {
            Control pick;
            pick.id = "pick";
            pick.kind = ControlKind::Button;
            pick.label = L"Choose another picture";
            pick.icon = 0xEB9F;   // Photo
            out.push_back(pick);
            Control name;
            name.kind = ControlKind::Note;
            name.text = FileName(m_params.image);
            out.push_back(name);
            out.push_back(SliderControl("blur", L"Blur", m_params.blur, 0, 100, L""));
            break;
        }
        case Fill::Blur: out.push_back(SliderControl("blur", L"Blur", m_params.blur, 0, 100, L"")); break;
        }

        section.label = L"Shape";
        out.push_back(section);
        Control aspect;
        aspect.id = "aspect";
        aspect.kind = ControlKind::Choice;
        aspect.chips = true;
        int i = 0;
        for (const auto& c : AspectChoices())
        {
            aspect.options.emplace_back(c.label);
            if (m_params.aspect == c.id)
                aspect.selected = i;
            ++i;
        }
        const bool fit = m_params.aspect == kFitToPicture;
        aspect.options.emplace_back(L"Fit to picture");
        if (fit)
            aspect.selected = i;
        out.push_back(aspect);
        // Fit to picture adds no margin: no spacing, and no room for a shadow.
        if (!fit)
            out.push_back(SliderControl("padding", L"Spacing", m_params.padding, 0, 40, L"%"));
        out.push_back(SliderControl("radius", L"Rounded corners", m_params.radius, 0, 100, L""));
        if (!fit)
            out.push_back(SliderControl("shadow", L"Shadow", m_params.shadow, 0, 100, L""));

        if (m_width > 0)
        {
            const auto layout = LayoutBackground(m_width, m_height, m_params);
            Control note;
            note.kind = ControlKind::Note;
            note.text = std::to_wstring(layout.width) + L" × " + std::to_wstring(layout.height) + L" pixels" +
                        (m_files.size() > 1 ? L" · every picture gets this background" : L"");
            out.push_back(note);
        }

        Control reset;
        reset.id = "reset";
        reset.kind = ControlKind::Button;
        reset.label = L"Reset";
        reset.icon = 0xE72C;   // Refresh
        out.push_back(reset);
    }

    Response BackgroundTool::OnControl(const ControlEvent& e)
    {
        const Response dragging{ true, false, true };
        if (e.id == "fill")
        {
            if (e.index < 0 || e.index > 3)
                return {};
            if (static_cast<Fill>(e.index) == Fill::Image && (m_params.image.empty() || m_backPath != m_params.image))
                return OnControl(ControlEvent{ "pick" });
            m_params.fill = static_cast<Fill>(e.index);
            Touch();
            return kChanged;
        }
        if (e.id == "pick")
        {
            std::wstring path;
            if (!m_host || !m_host->PickImage(path))
                return {};
            if (path == m_backPath)
            {
                m_params.fill = Fill::Image;
                m_params.image = path;
                Touch();
                return kChanged;
            }
            LoadBackgroundImage(path);
            return {};
        }
        if (e.id == "color1" || e.id == "color2")
        {
            uint32_t& custom = e.id == "color1" ? m_custom1 : m_custom2;
            uint32_t rgb;
            if (e.index >= 0 && e.index < static_cast<int>(std::size(kColours)))
                rgb = kColours[e.index];
            else if (e.index == static_cast<int>(std::size(kColours)) && (e.rgb || custom))
            {
                if (e.rgb)
                    custom = e.rgb;
                rgb = custom;
            }
            else
                return {};
            (e.id == "color1" ? m_params.color1 : m_params.color2) = rgb;
            Touch();
            return kChanged;
        }
        if (e.id == "aspect")
        {
            const auto& choices = AspectChoices();
            if (e.index < 0 || e.index > static_cast<int>(choices.size()))
                return {};
            // After the shapes comes Fit to picture.
            m_params.aspect = e.index == static_cast<int>(choices.size()) ? kFitToPicture : choices[static_cast<size_t>(e.index)].id;
            Touch();
            return kChanged;
        }
        double* value = e.id == "angle" ? &m_params.angle : e.id == "blur" ? &m_params.blur : e.id == "padding" ? &m_params.padding
                      : e.id == "radius"  ? &m_params.radius
                      : e.id == "shadow"  ? &m_params.shadow
                                          : nullptr;
        if (value)
        {
            const double hi = e.id == "angle" ? 360 : e.id == "padding" ? 40 : 100;
            *value = std::clamp(std::round(e.value), 0.0, hi);
            Touch();
            return e.final ? kChanged : dragging;
        }
        if (e.id == "reset")
        {
            m_params = {};
            Touch();
            return kChanged;
        }
        return {};
    }

    nlohmann::json BackgroundTool::State() const { return ToJson(m_params); }

    void BackgroundTool::Restore(const nlohmann::json& state)
    {
        m_params = ParseBackground(state);
        Touch();
    }

    nlohmann::json BackgroundTool::Params() const { return ToJson(m_params); }
}
