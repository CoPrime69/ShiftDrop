#include "photo/PhotoTools.h"
#include "Log.h"

#include <algorithm>
#include <filesystem>
#include <format>
#include <random>

namespace sd::photo
{
    using namespace sd::canvas;

    std::unique_ptr<ICanvasTool> MakeCollageTool(const std::vector<std::wstring>& files, const nlohmann::json& params)
    {
        return std::make_unique<CollageTool>(files, params);
    }

    namespace
    {
        // Six colours and a custom chip fill the panel.
        const uint32_t kBackgrounds[] = { 0xFFFFFF, 0xF2EFE9, 0xD9D9D9, 0x6B6B6B, 0x1F1F1F, 0x000000 };
        const wchar_t* const kKinds[] = { L"Grid", L"Row", L"Column", L"Featured" };
        constexpr UINT kCollagePreview = 1024;   // each photo's copy for the editor
    }

    CollageTool::CollageTool(std::vector<std::wstring> files, const nlohmann::json& params) : PhotoTool(std::move(files)), m_params(ParseCollage(params))
    {
        m_params.order = NormalisedOrder(m_params.order, m_files.size());
        if (std::find(std::begin(kBackgrounds), std::end(kBackgrounds), m_params.background) == std::end(kBackgrounds))
            m_custom = m_params.background;
    }

    void CollageTool::SetPictures(std::vector<Picture> pictures)
    {
        m_pictures = std::move(pictures);
        m_loading = false;
        Relayout();
        Touch();
    }

    void CollageTool::PictureReady(size_t)
    {
        if (m_loading || Loaded() || !m_host)
            return;
        // Every photo at a size fit for the screen, on a worker (a few at a time).
        m_loading = true;
        m_host->SetStatus(std::format(L"Opening {} photos…", m_files.size()));
        m_host->RunAsync([this, files = m_files](std::stop_token stop) -> std::function<void()> {
            std::vector<Picture> pictures(files.size());
            std::vector<std::wstring> errors(files.size());
            ParallelFor(static_cast<unsigned>(files.size()), [&](unsigned begin, unsigned end) {
                const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
                for (unsigned i = begin; i < end && !stop.stop_requested(); ++i)
                {
                    auto r = std::make_shared<Raster>();
                    if (SUCCEEDED(LoadRaster(files[i], *r, errors[i], kCollagePreview)))
                        pictures[i] = Picture::Of(std::move(r));
                    else if (errors[i].empty())
                        errors[i] = L"can't be opened";
                }
                if (SUCCEEDED(co))
                    CoUninitialize();
            });
            return [this, pictures = std::move(pictures), errors = std::move(errors), files] {
                std::wstring problem;
                for (size_t i = 0; i < files.size(); ++i)
                    if (pictures[i].Empty())
                    {
                        log::Warn(L"collage: cannot open {}: {}", files[i], errors[i]);
                        if (problem.empty())
                            problem = std::filesystem::path(files[i]).filename().wstring() + L": " + errors[i];
                    }
                SetPictures(pictures);
                m_host->SetStatus(problem);
                m_host->Redraw();
            };
        });
    }

    void CollageTool::Relayout()
    {
        m_params.order = NormalisedOrder(m_params.order, m_files.size());
        if (!Loaded())
        {
            m_layout = {};
            return;
        }
        std::vector<SizeD> sizes;
        for (const auto& p : m_pictures)
            sizes.push_back(p.Empty() ? SizeD{ 1, 1 } : SizeD{ double(p.width), double(p.height) });
        m_layout = LayoutCollage(sizes, m_params);
    }

    SizeD CollageTool::Size() const
    {
        if (Loaded() && m_layout.width > 0)
            return { double(m_layout.width), double(m_layout.height) };
        return { double(m_width), double(m_height) };
    }

    bool CollageTool::Render(ID2D1DeviceContext* dc, double scale, unsigned device)
    {
        UseDevice(device);
        if (!Loaded() || m_layout.cells.empty())
            return false;
        return SUCCEEDED(DrawCollage(dc, device, scale, m_pictures, m_params, m_layout, m_bitmaps));
    }

    void CollageTool::Draw(IPainter& p) const
    {
        if (m_dragCell < 0 || static_cast<size_t>(m_dragCell) >= m_layout.cells.size())
            return;
        const auto& theme = p.Theme();
        const auto rectOf = [&](int c) {
            const auto& r = m_layout.cells[static_cast<size_t>(c)].rect;
            return RectD{ double(r.x), double(r.y), double(r.w), double(r.h) };
        };
        p.FillRect(rectOf(m_dragCell), theme.shade);
        p.StrokeRect(rectOf(m_dragCell), theme.accent, 2.f, true);
        if (m_overCell >= 0 && m_overCell != m_dragCell)
            p.StrokeRect(rectOf(m_overCell), theme.accent, 3.f);
    }

    CursorShape CollageTool::CursorAt(const Pointer& p) const
    {
        if (m_dragCell >= 0)
            return CursorShape::Move;
        return CellAt(m_layout, p.at) >= 0 ? CursorShape::Hand : CursorShape::Arrow;
    }

    Response CollageTool::PointerDown(const Pointer& p)
    {
        m_dragCell = m_overCell = Loaded() ? CellAt(m_layout, p.at) : -1;
        return m_dragCell >= 0 ? kRedraw : Response{};
    }

    Response CollageTool::PointerMove(const Pointer& p)
    {
        if (m_dragCell < 0)
            return {};
        const int over = CellAt(m_layout, p.at);
        if (over == m_overCell)
            return {};
        m_overCell = over;
        return kRedraw;
    }

    Response CollageTool::PointerUp(const Pointer&)
    {
        const int from = std::exchange(m_dragCell, -1), to = std::exchange(m_overCell, -1);
        if (from < 0)
            return {};
        if (to < 0 || to == from || static_cast<size_t>(std::max(from, to)) >= m_params.order.size())
            return kRedraw;
        std::swap(m_params.order[static_cast<size_t>(from)], m_params.order[static_cast<size_t>(to)]);
        Relayout();
        Touch();
        return kChanged;
    }

    Response CollageTool::PointerCancel()
    {
        m_dragCell = m_overCell = -1;
        return kRedraw;
    }

    void CollageTool::BuildPanel(std::vector<Control>& out) const
    {
        Control section;
        section.kind = ControlKind::Section;
        section.label = L"Layout";
        out.push_back(section);
        Control kind;
        kind.id = "layout";
        kind.kind = ControlKind::Choice;
        kind.options.assign(std::begin(kKinds), std::end(kKinds));
        kind.selected = static_cast<int>(m_params.kind);
        out.push_back(kind);

        const bool shaped = m_params.kind == CollageKind::Grid || m_params.kind == CollageKind::Featured;
        if (shaped)
        {
            Control aspect;
            aspect.id = "aspect";
            aspect.kind = ControlKind::Choice;
            int i = 0;
            for (const auto& c : AspectChoices())
            {
                aspect.options.emplace_back(c.label);
                if (m_params.aspect == c.id)
                    aspect.selected = i;
                ++i;
            }
            out.push_back(aspect);
        }
        else
        {
            Control note;
            note.kind = ControlKind::Note;
            note.text = L"A row or a column shows every photo whole, so its shape follows theirs.";
            out.push_back(note);
        }

        Control size;
        size.id = "size";
        size.kind = ControlKind::Number;
        size.label = L"Longer side";
        size.text = L"px";
        size.value = m_params.size;
        size.min = 256;
        size.max = 16384;
        out.push_back(size);

        Control slider;
        slider.kind = ControlKind::Slider;
        slider.id = "spacing";
        slider.label = L"Spacing";
        slider.value = m_params.spacing;
        slider.min = 0;
        slider.max = 100;
        out.push_back(slider);
        slider.id = "radius";
        slider.label = L"Rounded corners";
        slider.value = m_params.radius;
        out.push_back(slider);

        section.label = L"Background";
        out.push_back(section);
        Control colours;
        colours.id = "background";
        colours.kind = ControlKind::Swatches;
        colours.custom = true;
        for (size_t i = 0; i < std::size(kBackgrounds); ++i)
        {
            colours.swatches.push_back(Color::Rgb(kBackgrounds[i]));
            if (kBackgrounds[i] == m_params.background)
                colours.selected = static_cast<int>(i);
        }
        colours.swatches.push_back(m_custom ? Color::Rgb(m_custom) : Color{ 0, 0, 0, 0 });
        if (colours.selected < 0 && m_custom == m_params.background)
            colours.selected = static_cast<int>(std::size(kBackgrounds));
        out.push_back(colours);
        Control fit;
        fit.id = "fit";
        fit.kind = ControlKind::Toggle;
        fit.label = L"Show whole photos (no cropping)";
        fit.on = m_params.fit;
        fit.enabled = shaped;
        out.push_back(fit);

        section.label = L"Order";
        out.push_back(section);
        Control shuffle;
        shuffle.id = "shuffle";
        shuffle.kind = ControlKind::Button;
        shuffle.label = L"Shuffle";
        shuffle.icon = 0xE8B1;   // Shuffle
        shuffle.half = true;
        out.push_back(shuffle);
        Control reverse = shuffle;
        reverse.id = "reverse";
        reverse.label = L"Reverse";
        reverse.icon = 0xE8CB;   // Sort
        out.push_back(reverse);
        Control note;
        note.kind = ControlKind::Note;
        note.text = m_params.kind == CollageKind::Featured ? L"Drag a photo onto another to swap them; the first is the large one."
                                                           : L"Drag a photo onto another to swap them.";
        if (m_layout.width > 0)
            note.text += std::format(L" The collage is {} × {} pixels.", m_layout.width, m_layout.height);
        out.push_back(note);
    }

    Response CollageTool::OnControl(const ControlEvent& e)
    {
        const auto changed = [this] {
            Relayout();
            Touch();
            return kChanged;
        };
        if (e.id == "layout")
        {
            if (e.index < 0 || e.index > 3)
                return {};
            m_params.kind = static_cast<CollageKind>(e.index);
            return changed();
        }
        if (e.id == "aspect")
        {
            const auto& choices = AspectChoices();
            if (e.index < 0 || e.index >= static_cast<int>(choices.size()))
                return {};
            m_params.aspect = choices[static_cast<size_t>(e.index)].id;
            return changed();
        }
        if (e.id == "size")
        {
            m_params.size = static_cast<int>(std::clamp(std::round(e.value), 256.0, 16384.0));
            return changed();
        }
        if (e.id == "spacing" || e.id == "radius")
        {
            (e.id == "spacing" ? m_params.spacing : m_params.radius) = std::clamp(std::round(e.value), 0.0, 100.0);
            Relayout();
            Touch();
            return e.final ? kChanged : Response{ true, false, true };
        }
        if (e.id == "background")
        {
            if (e.index >= 0 && e.index < static_cast<int>(std::size(kBackgrounds)))
                m_params.background = kBackgrounds[e.index];
            else if (e.index == static_cast<int>(std::size(kBackgrounds)) && (e.rgb || m_custom))
            {
                if (e.rgb)
                    m_custom = e.rgb;
                m_params.background = m_custom;
            }
            else
                return {};
            return changed();
        }
        if (e.id == "fit")
        {
            m_params.fit = e.on;
            return changed();
        }
        if (e.id == "shuffle")
        {
            static std::mt19937 random{ std::random_device{}() };
            const auto before = m_params.order;
            for (int tries = 0; tries < 8 && m_params.order == before; ++tries)
                std::shuffle(m_params.order.begin(), m_params.order.end(), random);
            return changed();
        }
        if (e.id == "reverse")
        {
            std::reverse(m_params.order.begin(), m_params.order.end());
            return changed();
        }
        return {};
    }

    nlohmann::json CollageTool::State() const { return ToJson(m_params); }

    void CollageTool::Restore(const nlohmann::json& state)
    {
        m_params = ParseCollage(state);
        Relayout();
        Touch();
    }

    bool CollageTool::CanApply(std::wstring* why) const
    {
        if (m_files.size() >= 2 && Loaded())
            return true;
        if (why)
            *why = m_files.size() < 2 ? L"A collage needs two or more pictures" : L"Opening the photos…";
        return false;
    }

    nlohmann::json CollageTool::Params() const { return ToJson(m_params); }
}
