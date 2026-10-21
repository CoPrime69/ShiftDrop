#include "Features.h"
#include "Recipe.h"
#include "TestUtil.h"

using namespace sd;
using namespace sd::test;

namespace
{
    std::string ErrorOf(std::string_view json)
    {
        ActionRegistry registry;
        Recipe r;
        std::string error;
        EXPECT_FALSE(ParseRecipe(json, registry, r, error)) << json;
        return error;
    }
}

TEST(Recipe, BuiltInsAndExampleLoadWithoutErrors)
{
    TempDir dir;
    Recipes recipes(dir);
    EXPECT_TRUE(recipes.store.Errors().empty());
    // The example is seeded into an empty recipes folder and loads like any user recipe.
    EXPECT_TRUE(std::filesystem::exists(dir / L"recipes\\web-ready.json"));
    const auto& web = recipes.Get("web-ready");
    EXPECT_FALSE(web.builtIn);
    EXPECT_EQ(web.steps.size(), 3u);

    size_t convert = 0, tools = 0, reduce = 0, files = 0;
    for (const auto& r : recipes.store.All())
        ++(r.ring == "convert" ? convert : r.ring == "tools" ? tools : r.ring == "reduce" ? reduce : files);
    EXPECT_EQ(convert, 52u);  // PNG, JPG, WebP, AVIF, HEIC, TIFF, BMP + Web + 15 video/audio formats + 5 PDF + 6 documents, 5 spreadsheets, 3 subtitles, 3 office bridge + 7 archive
    EXPECT_EQ(tools, 39u);    // 6 quick edits + 2 image and 4 media compress presets + Inspect + 7 PDF/OCR + 7 video and 5 audio tools + Crop, Redact, Share-safe + Edit, Annotate, Background, Collage
    EXPECT_EQ(reduce, 24u);   // 90% to 20% smaller, for pictures, video and audio, and PDFs
    EXPECT_EQ(files, 10u);    // Copy, Move to folder, Copy path, Terminal, Duplicate, Float, Share, Remove metadata, Zip, Rename
    EXPECT_TRUE(recipes.Get("move").IsFileAction());
}

TEST(Recipe, SliceLabelsReadAsWholeWords)
{
    TempDir dir;
    Recipes recipes(dir);
    // The two rotations say which way in words: arrows alone were hard to tell apart.
    EXPECT_EQ(recipes.Get("rotate-right").label, L"Rotate right");
    EXPECT_EQ(recipes.Get("rotate-left").label, L"Rotate left");
    EXPECT_EQ(recipes.Get("move").label, L"Move to folder");
    EXPECT_EQ(recipes.Get("rename").label, L"Rename");
    // Slices that open an editor have no trailing "...", which read as a cut-off label.
    const std::pair<const char*, const wchar_t*> editors[] = {
        { "video-trim", L"Trim" },   { "video-crop", L"Crop" },         { "video-speed", L"Speed" },   { "video-join", L"Join" },
        { "video-frames", L"Frames" }, { "video-split", L"Split" },     { "video-redact", L"Redact" }, { "audio-trim", L"Trim" },
        { "audio-channels", L"Channel" }, { "audio-bleep", L"Bleep" }, { "audio-visualizer", L"Visuals" },
    };
    for (const auto& [id, label] : editors)
        EXPECT_EQ(recipes.Get(id).label, label) << id;
    for (const auto& r : recipes.store.All())
    {
        EXPECT_FALSE(r.label.ends_with(L"\u2026") || r.label.ends_with(L"...")) << r.id;
        EXPECT_EQ(r.label.find_first_of(L"\u21bb\u21ba"), std::wstring::npos) << r.id;
    }
    // Which slices show their label alone, in bold: format names and media presets.
    EXPECT_TRUE(recipes.Get("to-png").NameOnlySlice());
    EXPECT_TRUE(recipes.Get("media-under-10mb").NameOnlySlice());
    EXPECT_FALSE(recipes.Get("rotate-right").NameOnlySlice());
    EXPECT_FALSE(recipes.Get("image-under-1mb").NameOnlySlice());
    EXPECT_FALSE(recipes.Get("video-trim").NameOnlySlice());
}

TEST(Recipe, DefaultsAreFilledIn)
{
    ActionRegistry registry;
    Recipe r;
    std::string error;
    ASSERT_TRUE(ParseRecipe(R"({ "id": "x", "label": "X", "steps": [ { "action": "image.resize" }, { "action": "image.encode" } ] })",
                            registry, r, error)) << error;
    EXPECT_EQ(r.steps[0].params["maxEdge"], 2048);
    EXPECT_EQ(r.steps[1].params["format"], "png");
    EXPECT_EQ(r.steps[1].params["quality"], 90);
    EXPECT_EQ(r.ring, "convert");
    EXPECT_EQ(r.suffix, L"converted");
}

TEST(Recipe, CommentsAreAllowed)
{
    ActionRegistry registry;
    Recipe r;
    std::string error;
    EXPECT_TRUE(ParseRecipe("// note\n{ \"id\": \"x\", \"label\": \"X\", /* inline */ \"steps\": [ { \"action\": \"image.encode\" } ] }",
                            registry, r, error)) << error;
}

TEST(Recipe, ErrorsNameTheField)
{
    EXPECT_NE(ErrorOf(R"({ "id": "x", "label": "X", "steps": [ { "action": "image.encode", "params": { "quality": 140 } } ] })")
                  .find("steps[0].params.quality"), std::string::npos);
    EXPECT_NE(ErrorOf(R"({ "id": "x", "label": "X", "steps": [ { "action": "image.blur" } ] })").find("unknown action 'image.blur'"),
              std::string::npos);
    EXPECT_NE(ErrorOf(R"({ "id": "x", "label": "X", "colour": "red", "steps": [ { "action": "image.encode" } ] })").find("colour"),
              std::string::npos);
    EXPECT_NE(ErrorOf(R"({ "id": "x", "label": "X", "steps": [ { "action": "image.encode", "params": { "format": "gif" } } ] })")
                  .find("format"), std::string::npos);
    EXPECT_NE(ErrorOf(R"({ "id": "a b", "label": "X", "steps": [ { "action": "image.encode" } ] })").find("id"), std::string::npos);
    EXPECT_NE(ErrorOf(R"({ "id": "x", "label": "X", "ring": "edit", "steps": [ { "action": "image.encode" } ] })").find("ring"),
              std::string::npos);
    EXPECT_NE(ErrorOf(R"({ "id": "x", "label": "X", "suffix": "a/b", "steps": [ { "action": "image.encode" } ] })").find("suffix"),
              std::string::npos);
}

TEST(Recipe, StepOrderIsEnforced)
{
    EXPECT_NE(ErrorOf(R"({ "id": "x", "label": "X", "steps": [ { "action": "image.encode" }, { "action": "image.resize" } ] })")
                  .find("must be the last step"), std::string::npos);
    EXPECT_NE(ErrorOf(R"({ "id": "x", "label": "X", "steps": [ { "action": "image.resize" } ] })").find("last step must be image.encode"),
              std::string::npos);
    EXPECT_NE(ErrorOf(R"({ "id": "x", "label": "X", "steps": [ { "action": "file.copy" }, { "action": "image.encode" } ] })")
                  .find("only step"), std::string::npos);
}

TEST(Recipe, SameFormatResolvesPerSource)
{
    TempDir dir;
    Recipes recipes(dir);
    const auto& rotate = recipes.Get("rotate-right");
    EXPECT_STREQ(rotate.OutputFormatFor(L".jpeg")->id, "jpg");
    EXPECT_STREQ(rotate.OutputFormatFor(L".webp")->id, "webp");
    EXPECT_STREQ(rotate.OutputFormatFor(L".gif")->id, "png");   // no GIF encoder: PNG keeps every pixel
}

TEST(Recipe, UserRecipeReplacesBuiltIn)
{
    TempDir dir;
    std::filesystem::create_directories(dir.path() / L"recipes");
    WriteText(dir / L"recipes\\png.json", R"({ "id": "to-png", "label": "PNG!", "steps": [ { "action": "image.encode" } ] })");
    WriteText(dir / L"recipes\\broken.json", R"({ "id": "broken", "steps": [] })");
    Recipes recipes(dir);
    EXPECT_EQ(recipes.Get("to-png").label, L"PNG!");
    ASSERT_EQ(recipes.store.Errors().size(), 1u);
    EXPECT_NE(recipes.store.Errors()[0].find(L"broken.json"), std::wstring::npos);
}

TEST(Features, SlicesAndSectionsTurnOff)
{
    EXPECT_EQ(SplitIds(" to-tiff, to-bmp,,to-tiff "), (std::set<std::string>{ "to-bmp", "to-tiff" }));
    EXPECT_EQ(JoinIds({ "b", "a" }), "a,b");
    EXPECT_TRUE(SplitIds("").empty());

    Features f;
    Recipe tiff;
    tiff.id = "to-tiff";
    tiff.ring = "convert";
    Recipe rotate;
    rotate.id = "rotate-right";
    rotate.ring = "tools";
    EXPECT_TRUE(f.RecipeOn(tiff));
    f.off.insert("to-tiff");
    EXPECT_FALSE(f.RecipeOn(tiff));
    EXPECT_TRUE(f.RecipeOn(rotate));
    f.sections[static_cast<size_t>(Ring::Tools)] = false;
    EXPECT_FALSE(f.RecipeOn(rotate));
    EXPECT_EQ(f.SectionsOn(), 3);
    EXPECT_EQ(RingFromId("reduce"), Ring::Reduce);
    EXPECT_EQ(RingFromId("nonsense"), Ring::Convert);
}
