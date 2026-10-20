#pragma once
#include "canvas/RegionTool.h"
#include "crop/CropMath.h"

namespace sd::crop
{
    // The Crop editor: one crop box with grips, aspect presets, exact pixel size fields, a
    // rule-of-thirds grid, quarter turns and a straighten slider. The box shapes the first picture;
    // the rest of a batch get the same crop, centred (or in the same place).
    class CropTool final : public canvas::RegionTool
    {
    public:
        explicit CropTool(size_t fileCount);

        std::wstring Title() const override { return L"Crop"; }
        std::wstring ApplyLabel() const override { return m_files > 1 ? L"Crop all" : L"Crop"; }
        void ImageReady(size_t index, int width, int height) override;
        canvas::ViewRotation Rotation() const override { return { m_params.quarterTurns, m_params.straighten }; }

        void Draw(canvas::IPainter& painter) const override;
        canvas::Response Key(unsigned vk, bool shift, bool ctrl) override;
        void BuildPanel(std::vector<canvas::Control>& out) const override;
        canvas::Response OnControl(const canvas::ControlEvent& e) override;

        nlohmann::json State() const override;
        void Restore(const nlohmann::json& state) override;
        bool CanApply(std::wstring* why) const override;
        nlohmann::json Params() const override;

        // For tests and the panel.
        RectD Box() const;
        const CropParams& Current() const { return m_params; }

    protected:
        void RegionsChanged() override;

    private:
        double Ratio() const;
        SizeD Rotated() const;
        void ApplyBounds(bool resetBox);
        void SetBox(const RectD& box);
        void Rotate(int quarterTurns);

        size_t m_files;
        int m_imageW = 0, m_imageH = 0;   // oriented, before the crop's quarter turns
        CropParams m_params;
        bool m_thirds = true;
        SizeD m_lastSize;
    };
}
