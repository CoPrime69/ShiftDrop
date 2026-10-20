#pragma once
#include <nlohmann/json.hpp>

#include <string>
#include <vector>

// The action model (ADR 0004). An *action* is one step an engine can perform; a *recipe* (Recipe.h)
// is an ordered list of steps with parameters, and the wheel only ever shows recipes. A plain
// "convert to PNG" slice is a one-step recipe.
namespace sd
{
    enum class ParamType
    {
        Int,
        Number,
        Bool,
        Enum,
    };

    struct ParamSpec
    {
        std::string key;
        ParamType type = ParamType::Int;
        double min = 0;                       // Int / Number
        double max = 0;
        std::vector<std::string> choices;     // Enum
        nlohmann::json defaultValue;          // used when the recipe omits the key
    };

    enum class StepKind
    {
        ImageTransform,   // pixels in -> pixels out, in memory (fusable: no intermediate file)
        ImageMetadata,    // changes what metadata the output carries
        ImageEncode,      // pixels -> bytes; must be the last step of an image recipe
        MediaEncode,      // audio/video file -> new file through ffmpeg (Media.h); a recipe's only step
        FileAction,       // acts on the file itself (clipboard, terminal, duplicate); a recipe's only step
    };

    struct ActionDescriptor
    {
        std::string id;                 // "image.resize"
        std::wstring label;             // "Resize"
        StepKind kind = StepKind::ImageTransform;
        std::vector<ParamSpec> params;
    };

    // Registry of every action the app knows. Engines register here; recipes are validated against it.
    class ActionRegistry
    {
    public:
        ActionRegistry();   // registers the built-in image actions
        const ActionDescriptor* Find(std::string_view id) const;
        const std::vector<ActionDescriptor>& All() const { return m_actions; }

    private:
        std::vector<ActionDescriptor> m_actions;
    };

    // Output formats of image.encode, in the order the wheel lists them.
    struct ImageFormatInfo
    {
        const char* id;          // "png"
        const wchar_t* label;    // "PNG"
        const wchar_t* extension;
        const wchar_t* aliases;  // source extensions that already are this format, e.g. ".jpg.jpeg.jpe.jfif"
        bool keepsAlpha;
        bool lossy;
    };
    const std::vector<ImageFormatInfo>& ImageFormats();
    const ImageFormatInfo* FindImageFormat(std::string_view id);
    bool IsAlreadyFormat(const ImageFormatInfo& format, const std::wstring& lowerExtension);
    // The format a file already is, or nullptr (HEIC, GIF, ... are decoded but not re-encoded as such).
    const ImageFormatInfo* FormatOfExtension(const std::wstring& lowerExtension);
}
