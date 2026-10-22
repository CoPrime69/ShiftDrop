#pragma once
#include "canvas/Covers.h"
#include "canvas/RegionTool.h"
#include "redact/Pii.h"

#include <string>
#include <vector>

namespace sd::redact
{
    // A cover as the engine gets it, per file, in oriented picture pixels.
    struct CoverSpec
    {
        canvas::RectD rect;
        canvas::CoverKind kind = canvas::CoverKind::Solid;
        uint32_t rgb = 0;
    };

    // Params of redact.image and sharesafe.clean: {"files":[{"path":..., "covers":[{x,y,w,h,kind,rgb}]}]}.
    nlohmann::json CoversToJson(const std::vector<std::wstring>& files, const std::vector<std::vector<CoverSpec>>& covers);
    std::vector<CoverSpec> CoversFor(const nlohmann::json& params, const std::wstring& file);
    // Whole pixels, clipped to the picture.
    std::vector<canvas::Cover> ToPixelCovers(const std::vector<CoverSpec>& covers, int width, int height);

    // Proposed covers from OCR and face boxes: one per line of text, one per face (a little larger
    // than the box, so hair and chins are covered too), one per personal detail.
    std::vector<canvas::RectD> TextLineBoxes(const std::vector<OcrLine>& lines, double width, double height);
    canvas::RectD FaceCover(const canvas::RectD& face, double width, double height);
    canvas::RectD PiiCover(const canvas::RectD& box, double width, double height);

    // The Redact Photos editor: several movable cover regions per picture (solid in black, white or
    // any colour, strong blur, or pixelation), with "Find text", "Find faces" and "Find personal
    // info" proposing regions to review. In review mode (Share-safe) it first looks for personal
    // details and only opens when it finds some.
    class RedactTool final : public canvas::RegionTool
    {
    public:
        enum class Mode { Redact, Review };
        RedactTool(std::vector<std::wstring> files, Mode mode);

        std::wstring Title() const override;
        wchar_t Icon() const override { return m_mode == Mode::Review ? wchar_t(0xEA18) : wchar_t(0xED1A); }   // Shield; the wheel's Redact
        std::wstring ApplyLabel() const override;
        bool PerFile() const override { return true; }
        void ImageReady(size_t index, int width, int height) override;
        std::unique_ptr<canvas::Prepass> MakePrepass() override;

        void Draw(canvas::IPainter& painter) const override;
        canvas::Response Key(unsigned vk, bool shift, bool ctrl) override;
        void BuildPanel(std::vector<canvas::Control>& out) const override;
        canvas::Response OnControl(const canvas::ControlEvent& e) override;

        nlohmann::json State() const override;
        void Restore(const nlohmann::json& state) override;
        bool CanApply(std::wstring* why) const override;
        nlohmann::json Params() const override;

        // Adds proposals to file `index` (skipping ones that repeat an existing region). For tests and
        // the finders.
        int Propose(size_t index, const std::vector<canvas::RectD>& boxes, const std::string& origin, const std::vector<std::wstring>& labels);
        size_t ProposalCount() const;
        std::vector<std::vector<CoverSpec>> AllCovers() const;

    private:
        struct FileState
        {
            std::vector<canvas::Region> regions;
            int width = 0, height = 0;
        };
        void Stash();   // m_regions -> m_state[m_current]
        void Find(const std::string& what);
        uint32_t Fill() const { return m_fill; }
        canvas::CoverKind KindOf(const canvas::Region& r) const { return static_cast<canvas::CoverKind>(std::clamp(r.style, 0, 2)); }

        std::vector<std::wstring> m_files;
        Mode m_mode;
        std::vector<FileState> m_state;
        size_t m_current = 0;
        uint32_t m_fill = 0x000000;
        uint32_t m_custom = 0xE81123;   // the third swatch until a colour is picked
        int m_kind = 0;                 // for new covers
        std::wstring m_found;           // the review's summary ("2 email addresses, 1 phone number")
    };
}
