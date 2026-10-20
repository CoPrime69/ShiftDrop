#include "redact/RedactTool.h"
#include "Utf.h"
#include "redact/Vision.h"

#include <windows.h>

#include <format>
#include <map>

namespace sd::redact
{
    using namespace sd::canvas;

    namespace
    {
        RectD Inflate(const RectD& r, double left, double top, double right, double bottom, double width, double height)
        {
            const RectD grown{ r.x - left, r.y - top, r.w + left + right, r.h + top + bottom };
            return Intersect(grown, { 0, 0, width, height });
        }

        double Overlap(const RectD& a, const RectD& b)
        {
            const RectD i = Intersect(a, b);
            const double inter = i.w * i.h;
            const double uni = a.w * a.h + b.w * b.h - inter;
            return uni > 0 ? inter / uni : 0;
        }

        std::wstring Plural(PiiKind kind, size_t n)
        {
            const wchar_t* one = kind == PiiKind::Email ? L"email address" : kind == PiiKind::Phone ? L"phone number"
                                 : kind == PiiKind::Card                    ? L"card number"
                                                                            : L"IBAN";
            const wchar_t* many = kind == PiiKind::Email ? L"email addresses" : kind == PiiKind::Phone ? L"phone numbers"
                                  : kind == PiiKind::Card                     ? L"card numbers"
                                                                              : L"IBANs";
            return std::format(L"{} {}", n, n == 1 ? one : many);
        }

        const Grip kGrips[] = { Grip::NW, Grip::N, Grip::NE, Grip::E, Grip::SE, Grip::S, Grip::SW, Grip::W };

        PointD GripAt(const RectD& r, Grip g)
        {
            const double x = (g == Grip::NW || g == Grip::W || g == Grip::SW) ? r.x : (g == Grip::N || g == Grip::S) ? r.Centre().x : r.Right();
            const double y = (g == Grip::NW || g == Grip::N || g == Grip::NE) ? r.y : (g == Grip::W || g == Grip::E) ? r.Centre().y : r.Bottom();
            return { x, y };
        }
    }

    nlohmann::json CoversToJson(const std::vector<std::wstring>& files, const std::vector<std::vector<CoverSpec>>& covers)
    {
        auto list = nlohmann::json::array();
        for (size_t i = 0; i < files.size(); ++i)
        {
            auto c = nlohmann::json::array();
            if (i < covers.size())
                for (const auto& s : covers[i])
                    c.push_back({ { "x", s.rect.x }, { "y", s.rect.y }, { "w", s.rect.w }, { "h", s.rect.h }, { "kind", static_cast<int>(s.kind) }, { "rgb", s.rgb } });
            list.push_back({ { "path", Narrow(files[i]) }, { "covers", std::move(c) } });
        }
        return { { "files", std::move(list) } };
    }

    std::vector<CoverSpec> CoversFor(const nlohmann::json& params, const std::wstring& file)
    {
        std::vector<CoverSpec> out;
        if (!params.is_object() || !params.contains("files") || !params["files"].is_array())
            return out;
        const std::string wanted = Narrow(file);
        for (const auto& f : params["files"])
        {
            if (!f.is_object() || f.value("path", std::string{}) != wanted || !f.contains("covers"))
                continue;
            for (const auto& c : f["covers"])
                out.push_back({ { c.value("x", 0.0), c.value("y", 0.0), c.value("w", 0.0), c.value("h", 0.0) },
                                static_cast<CoverKind>(std::clamp(c.value("kind", 0), 0, 2)), c.value("rgb", 0u) });
        }
        return out;
    }

    std::vector<Cover> ToPixelCovers(const std::vector<CoverSpec>& covers, int width, int height)
    {
        std::vector<Cover> out;
        for (const auto& c : covers)
        {
            // Outward to whole pixels: a cover never leaves a sliver of what it hides at its edge.
            if (Intersect(c.rect, { 0, 0, static_cast<double>(width), static_cast<double>(height) }).Empty())
                continue;   // entirely off the picture
            const double l = std::floor(c.rect.x), t = std::floor(c.rect.y);
            const RectI px = ToPixels({ l, t, std::ceil(c.rect.Right()) - l, std::ceil(c.rect.Bottom()) - t }, width, height);
            if (px.w > 0 && px.h > 0)
                out.push_back(Cover{ px, c.kind, c.rgb });
        }
        return out;
    }

    std::vector<RectD> TextLineBoxes(const std::vector<OcrLine>& lines, double width, double height)
    {
        std::vector<RectD> out;
        for (const auto& line : lines)
        {
            if (line.words.empty())
                continue;
            RectD box = line.words.front().box;
            for (const auto& w : line.words)
            {
                const double l = std::min(box.x, w.box.x), t = std::min(box.y, w.box.y);
                box = { l, t, std::max(box.Right(), w.box.Right()) - l, std::max(box.Bottom(), w.box.Bottom()) - t };
            }
            const double pad = box.h * 0.2;
            out.push_back(Inflate(box, pad, pad, pad, pad, width, height));
        }
        return out;
    }

    RectD FaceCover(const RectD& face, double width, double height)
    {
        return Inflate(face, face.w * 0.2, face.h * 0.35, face.w * 0.2, face.h * 0.2, width, height);
    }

    RectD PiiCover(const RectD& box, double width, double height)
    {
        const double pad = box.h * 0.2;
        return Inflate(box, pad, pad, pad, pad, width, height);
    }

    RedactTool::RedactTool(std::vector<std::wstring> files, Mode mode) : m_files(std::move(files)), m_mode(mode), m_state(m_files.size())
    {
        m_regions.SetMinSize(4);
    }

    std::wstring RedactTool::Title() const { return m_mode == Mode::Review ? L"Review before sharing" : L"Redact"; }

    std::wstring RedactTool::ApplyLabel() const
    {
        if (m_mode == Mode::Review)
            return m_files.size() > 1 ? L"Save share-safe copies" : L"Save share-safe copy";
        return m_files.size() > 1 ? L"Redact all" : L"Redact";
    }

    void RedactTool::Stash()
    {
        if (m_current < m_state.size())
            m_state[m_current].regions = m_regions.Items();
    }

    void RedactTool::ImageReady(size_t index, int width, int height)
    {
        if (index >= m_state.size())
            return;
        if (m_regions.Bounds().w > 1 || m_regions.Size())
            Stash();
        m_current = index;
        m_state[index].width = width;
        m_state[index].height = height;
        m_regions.Clear();
        m_regions.Items() = m_state[index].regions;
        m_regions.SetBounds({ 0, 0, static_cast<double>(width), static_cast<double>(height) });
    }

    int RedactTool::Propose(size_t index, const std::vector<RectD>& boxes, const std::string& origin, const std::vector<std::wstring>& labels)
    {
        if (index >= m_state.size())
            return 0;
        const bool live = index == m_current && m_regions.Bounds().w > 1;
        auto& target = live ? m_regions.Items() : m_state[index].regions;
        int added = 0;
        for (size_t i = 0; i < boxes.size(); ++i)
        {
            const RectD& box = boxes[i];
            if (box.Empty() || std::any_of(target.begin(), target.end(), [&](const Region& r) { return Overlap(r.rect, box) > 0.5; }))
                continue;
            const std::wstring label = i < labels.size() ? labels[i] : L"";
            if (live)
                m_regions.Add(box, m_kind, origin, label);
            else
                target.push_back(Region{ 0, box, false, m_kind, origin, label });
            ++added;
        }
        return added;
    }

    size_t RedactTool::ProposalCount() const
    {
        size_t n = 0;
        for (size_t i = 0; i < m_state.size(); ++i)
        {
            const auto& list = i == m_current ? m_regions.Items() : m_state[i].regions;
            for (const auto& r : list)
                n += r.origin.empty() ? 0 : 1;
        }
        return n;
    }

    std::vector<std::vector<CoverSpec>> RedactTool::AllCovers() const
    {
        std::vector<std::vector<CoverSpec>> out(m_state.size());
        for (size_t i = 0; i < m_state.size(); ++i)
            for (const auto& r : i == m_current ? m_regions.Items() : m_state[i].regions)
            {
                const CoverKind kind = KindOf(r);
                out[i].push_back({ r.rect, kind, kind == CoverKind::Solid ? m_fill : 0u });
            }
        return out;
    }

    std::unique_ptr<Prepass> RedactTool::MakePrepass()
    {
        if (m_mode != Mode::Review)
            return nullptr;
        auto prepass = std::make_unique<Prepass>();
        prepass->message = m_files.size() == 1 ? std::wstring(L"Checking the photo for personal details")
                                               : std::format(L"Checking {} photos for personal details", m_files.size());
        prepass->run = [this](std::stop_token stop, const std::function<void(double)>& progress) {
            if (!vision::CanReadText())
                return false;   // no OCR language: the scrub alone
            std::map<PiiKind, size_t> counts;
            for (size_t i = 0; i < m_files.size() && !stop.stop_requested(); ++i)
            {
                Raster image;
                std::wstring error;
                if (SUCCEEDED(LoadRaster(m_files[i], image, error)))
                {
                    m_state[i].width = static_cast<int>(image.width);
                    m_state[i].height = static_cast<int>(image.height);
                    std::vector<RectD> boxes;
                    std::vector<std::wstring> labels;
                    for (const auto& hit : FindPii(vision::ReadText(image, &error)))
                    {
                        boxes.push_back(PiiCover(hit.box, image.width, image.height));
                        labels.emplace_back(PiiLabel(hit.kind));
                        ++counts[hit.kind];
                    }
                    Propose(i, boxes, "pii", labels);
                }
                if (progress)
                    progress(static_cast<double>(i + 1) / static_cast<double>(m_files.size()));
            }
            std::wstring found;
            size_t k = 0;
            for (const auto& [kind, n] : counts)
                found += (k++ ? (k == counts.size() ? L" and " : L", ") : L"") + Plural(kind, n);
            m_found = found.empty() ? std::wstring() : L"Found " + found + L".";
            return !counts.empty() && !stop.stop_requested();
        };
        return prepass;
    }

    void RedactTool::Find(const std::string& what)
    {
        if (!m_host || m_host->Busy())
            return;
        auto picture = m_host->Picture();
        if (!picture)
            return;
        const size_t index = m_current;
        m_host->SetStatus(what == "faces" ? L"Looking for faces…" : what == "text" ? L"Looking for text…" : L"Looking for personal details…");
        m_host->RunAsync([this, picture, index, what](std::stop_token) -> std::function<void()> {
            std::wstring error;
            std::vector<RectD> boxes;
            std::vector<std::wstring> labels;
            const double w = picture->width, h = picture->height;
            if (what == "faces")
            {
                for (const auto& face : vision::FindFaces(*picture, &error))
                {
                    boxes.push_back(FaceCover(face, w, h));
                    labels.emplace_back(L"Face");
                }
            }
            else
            {
                const auto lines = vision::ReadText(*picture, &error);
                if (what == "text")
                {
                    boxes = TextLineBoxes(lines, w, h);
                    labels.assign(boxes.size(), L"Text");
                }
                else
                    for (const auto& hit : FindPii(lines))
                    {
                        boxes.push_back(PiiCover(hit.box, w, h));
                        labels.emplace_back(PiiLabel(hit.kind));
                    }
            }
            return [this, index, what, boxes = std::move(boxes), labels = std::move(labels), error] {
                const int added = Propose(index, boxes, what, labels);
                const wchar_t* noun = what == "faces" ? L"face" : what == "text" ? L"line of text" : L"personal detail";
                const wchar_t* nouns = what == "faces" ? L"faces" : what == "text" ? L"lines of text" : L"personal details";
                if (!error.empty() && boxes.empty())
                    m_host->SetStatus(error);
                else if (boxes.empty())
                    m_host->SetStatus(std::format(L"No {} found", nouns));
                else
                    m_host->SetStatus(std::format(L"{} {} found · review them below", boxes.size(), boxes.size() == 1 ? noun : nouns) +
                                      (added < static_cast<int>(boxes.size()) ? L" (some were already covered)" : L""));
                if (added)
                    m_host->Changed();
                m_host->Redraw();
            };
        });
    }

    void RedactTool::Draw(IPainter& p) const
    {
        const auto& theme = p.Theme();
        const auto& items = m_regions.Items();
        for (const auto& r : items)
        {
            const CoverKind kind = KindOf(r);
            if (kind == CoverKind::Solid)
                p.FillRect(r.rect, Color::Rgb(m_fill));
            else
                p.DrawEffect(r.rect, kind == CoverKind::Blur ? Effect::Blur : Effect::Pixelate);
        }
        for (size_t i = 0; i < items.size(); ++i)
        {
            const auto& r = items[i];
            if (r.selected)
                p.StrokeRect(r.rect, theme.accent, 2.f);
            else if (!r.origin.empty())
                p.StrokeRect(r.rect, theme.accent, 1.5f, true);
            else
            {
                p.StrokeRect(r.rect, Color{ 0, 0, 0, 0.45f }, 2.5f);
                p.StrokeRect(r.rect, Color{ 1, 1, 1, 0.8f }, 1.f);
            }
            if (!r.origin.empty() && !r.label.empty())
                p.Tag({ r.rect.x, r.rect.y }, r.label, theme.accent, theme.onAccent);
            if (r.selected && m_regions.SelectedCount() == 1)
                for (const Grip g : kGrips)
                    p.Grip(GripAt(r.rect, g), m_hot.index == static_cast<int>(i) && m_hot.grip == g);
        }
    }

    Response RedactTool::Key(unsigned vk, bool shift, bool ctrl)
    {
        // 1, 2, 3: solid, blur, pixelate for the selection (and new covers).
        if (!ctrl && vk >= '1' && vk <= '3')
            return OnControl(ControlEvent{ "kind", static_cast<int>(vk - '1') });
        return RegionTool::Key(vk, shift, ctrl);
    }

    void RedactTool::BuildPanel(std::vector<Control>& out) const
    {
        const int selected = m_regions.SelectedCount();
        int kind = m_kind;
        if (const int first = m_regions.FirstSelected(); first >= 0)
            kind = m_regions.Items()[static_cast<size_t>(first)].style;

        if (m_mode == Mode::Review)
        {
            Control intro;
            intro.kind = ControlKind::Note;
            intro.text = (m_found.empty() ? std::wstring() : m_found + L" ") +
                         L"Covers are added unless you remove them. All metadata is removed either way.";
            out.push_back(intro);
        }

        Control section;
        section.kind = ControlKind::Section;
        section.label = selected ? (selected == 1 ? std::wstring(L"Selected cover") : std::format(L"{} selected covers", selected)) : L"New covers";
        out.push_back(section);
        Control choice;
        choice.id = "kind";
        choice.kind = ControlKind::Choice;
        choice.options = { L"Solid", L"Blur", L"Pixelate" };
        choice.selected = std::clamp(kind, 0, 2);
        out.push_back(choice);
        Control fill;
        fill.id = "fill";
        fill.kind = ControlKind::Swatches;
        fill.label = L"Colour";
        fill.swatches = { Color::Rgb(0x000000), Color::Rgb(0xFFFFFF), Color::Rgb(m_custom) };
        fill.custom = true;
        fill.selected = m_fill == 0x000000 ? 0 : m_fill == 0xFFFFFF ? 1 : 2;
        out.push_back(fill);
        Control note;
        note.kind = ControlKind::Note;
        note.text = L"Solid covers are the safest: blur and pixelation can sometimes be partly undone.";
        out.push_back(note);

        section.label = L"Find";
        out.push_back(section);
        const bool busy = m_host && m_host->Busy();
        const bool ocr = vision::CanReadText();
        Control button;
        button.kind = ControlKind::Button;
        button.id = "findPii";
        button.label = L"Personal details";
        button.icon = 0xE72E;   // Lock
        button.enabled = ocr && !busy;
        out.push_back(button);
        button.id = "findText";
        button.label = L"All text";
        button.icon = 0xE8D2;   // Font
        button.half = true;
        out.push_back(button);
        button.id = "findFaces";
        button.label = L"Faces";
        button.icon = 0xE77B;   // Contact
        button.enabled = vision::CanFindFaces() && !busy;
        out.push_back(button);
        if (!ocr)
        {
            Control none;
            none.kind = ControlKind::Note;
            none.text = L"Finding text needs a Windows language with text recognition (Settings > Time & language > Language).";
            out.push_back(none);
        }

        const size_t proposals = ProposalCount();
        if (proposals)
        {
            section.label = std::format(L"{} suggestion{}", proposals, proposals == 1 ? L"" : L"s");
            out.push_back(section);
            Control keep;
            keep.kind = ControlKind::Button;
            keep.id = "keepAll";
            keep.label = L"Keep all";
            keep.icon = 0xE73E;   // CheckMark
            keep.half = true;
            out.push_back(keep);
            keep.id = "removeAll";
            keep.label = L"Remove all";
            keep.icon = 0xE711;   // Cancel
            out.push_back(keep);
        }

        section.label = L"Covers";
        out.push_back(section);
        Control count;
        count.kind = ControlKind::Note;
        count.text = m_regions.Size() == 0 ? std::wstring(L"Drag on the picture to cover something.")
                                           : std::format(L"{} on this picture. Drag to add more; Del removes the selection.", m_regions.Size());
        out.push_back(count);
        Control remove;
        remove.kind = ControlKind::Button;
        remove.id = "deleteSelected";
        remove.label = L"Remove selected";
        remove.icon = 0xE74D;   // Delete
        remove.enabled = selected > 0;
        out.push_back(remove);
    }

    Response RedactTool::OnControl(const ControlEvent& e)
    {
        if (e.id == "kind" && e.index >= 0 && e.index <= 2)
        {
            m_kind = e.index;
            m_newStyle = e.index;
            for (auto& r : m_regions.Items())
                if (r.selected)
                    r.style = e.index;
            return kChanged;
        }
        if (e.id == "fill")
        {
            if (e.index == 2 && e.rgb)
                m_custom = e.rgb & 0xFFFFFF;
            m_fill = e.index == 0 ? 0x000000 : e.index == 1 ? 0xFFFFFF : m_custom;
            m_kind = 0;
            m_newStyle = 0;
            for (auto& r : m_regions.Items())
                if (r.selected)
                    r.style = 0;
            return kChanged;
        }
        if (e.id == "findText" || e.id == "findFaces" || e.id == "findPii")
        {
            Find(e.id == "findText" ? "text" : e.id == "findFaces" ? "faces" : "pii");
            return { true, false, true };
        }
        if (e.id == "keepAll" || e.id == "removeAll")
        {
            Stash();
            for (auto& file : m_state)
            {
                if (e.id == "removeAll")
                    std::erase_if(file.regions, [](const Region& r) { return !r.origin.empty(); });
                else
                    for (auto& r : file.regions)
                        r.origin.clear(), r.label.clear();
            }
            m_regions.Items() = m_state[m_current].regions;
            return kChanged;
        }
        if (e.id == "deleteSelected")
            return m_regions.DeleteSelected() ? kChanged : Response{};
        return {};
    }

    nlohmann::json RedactTool::State() const
    {
        auto files = nlohmann::json::array();
        for (size_t i = 0; i < m_state.size(); ++i)
        {
            RegionSet set;
            set.SetBounds({ 0, 0, 1e9, 1e9 });
            set.SetMinSize(0);
            set.Items() = i == m_current ? m_regions.Items() : m_state[i].regions;
            files.push_back(SaveRegions(set));
        }
        return { { "files", files }, { "fill", m_fill }, { "custom", m_custom }, { "kind", m_kind } };
    }

    void RedactTool::Restore(const nlohmann::json& state)
    {
        m_fill = state.value("fill", 0u);
        m_custom = state.value("custom", 0xE81123u);
        m_kind = state.value("kind", 0);
        m_newStyle = m_kind;
        const auto& files = state.contains("files") ? state["files"] : nlohmann::json::array();
        for (size_t i = 0; i < m_state.size() && i < files.size(); ++i)
        {
            RegionSet set;
            set.SetBounds({ 0, 0, 1e9, 1e9 });
            set.SetMinSize(0);
            LoadRegions(set, files[i]);
            m_state[i].regions = set.Items();
        }
        m_regions.Items() = m_state[m_current].regions;
    }

    bool RedactTool::CanApply(std::wstring* why) const
    {
        if (m_mode == Mode::Review)
            return true;
        for (const auto& file : AllCovers())
            if (!file.empty())
                return true;
        if (why)
            *why = L"Drag over what to hide, or use Find";
        return false;
    }

    nlohmann::json RedactTool::Params() const
    {
        return CoversToJson(m_files, AllCovers());
    }
}
