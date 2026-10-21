#pragma once
#include "canvas/Raster.h"

#include <string>
#include <vector>

// Fill and sign: the signatures a person keeps for reuse, and making a photo or scan of one ready to
// place. A signature is a PNG of the ink on a transparent background in
// %LOCALAPPDATA%\ShiftDrop\signatures; nothing is uploaded or shared, and removing one deletes its file.
namespace sd::photo
{
    class SignatureStore
    {
    public:
        static constexpr size_t kMax = 12;   // the oldest beyond this are removed when one is added

        explicit SignatureStore(std::wstring folder) : m_folder(std::move(folder)) {}
        // The app's store, under %LOCALAPPDATA%\ShiftDrop.
        static SignatureStore Default();

        const std::wstring& Folder() const { return m_folder; }
        // The saved signatures, newest first.
        std::vector<std::wstring> List() const;
        // Saves `ink` as a new signature; its path, or empty with `error` set. A `draft` is one the
        // person chose not to keep: it is not listed, and goes a day later (by then the file it was
        // placed on has been saved).
        std::wstring Add(const canvas::Raster& ink, std::wstring& error, bool draft = false) const;
        // Deletes a saved signature. Only files of this store's folder; false for any other path.
        bool Remove(const std::wstring& path) const;
        bool Holds(const std::wstring& path) const;

    private:
        std::wstring m_folder;
    };

    // A photo or scan of a signature on paper, made see-through: the paper (anything close to the
    // page's own lightness, so a greyish photo of white paper counts too) becomes transparent and the
    // ink keeps its colour, with soft edges.
    void PaperToTransparent(canvas::Raster& picture);

    // Crops to the ink (pixels that are not nearly transparent) plus `margin` pixels on each side.
    // False, leaving the picture as it was, when there is no ink at all.
    bool TrimToInk(canvas::Raster& picture, unsigned margin);
}
