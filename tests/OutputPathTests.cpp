#include "OutputPath.h"
#include "TestUtil.h"

using namespace sd;
using namespace sd::test;

TEST(OutputPath, NamesFollowThePattern)
{
    EXPECT_EQ(OutputName(L"photo", L"converted", 1, L".png"), L"photo (converted).png");
    EXPECT_EQ(OutputName(L"photo", L"converted", 2, L".png"), L"photo (converted 2).png");
    EXPECT_EQ(OutputName(L"my.photo", L"edited", 10, L".jpg"), L"my.photo (edited 10).jpg");
}

TEST(OutputPath, LowerExtension)
{
    EXPECT_EQ(LowerExtension(L"C:\\a\\Photo.JPEG"), L".jpeg");
    EXPECT_EQ(LowerExtension(L"C:\\a.b\\noext"), L"");
    EXPECT_EQ(LowerExtension(L"archive.tar.GZ"), L".gz");
}

TEST(OutputPath, EveryNamingStyle)
{
    EXPECT_EQ(OutputName(OutputNaming::Parentheses, L"photo", L"converted", 1, L".png"), L"photo (converted).png");
    EXPECT_EQ(OutputName(OutputNaming::Dash, L"photo", L"converted", 1, L".png"), L"photo-converted.png");
    EXPECT_EQ(OutputName(OutputNaming::Dash, L"photo", L"converted", 3, L".png"), L"photo-converted-3.png");
    EXPECT_EQ(OutputName(OutputNaming::Underscore, L"photo", L"converted", 1, L".png"), L"photo_converted.png");
    EXPECT_EQ(OutputName(OutputNaming::Underscore, L"photo", L"converted", 2, L".png"), L"photo_converted_2.png");
    EXPECT_EQ(OutputName(OutputNaming::Subfolder, L"photo", L"converted", 1, L".png"), L"photo.png");
    EXPECT_EQ(OutputName(OutputNaming::Subfolder, L"photo", L"converted", 2, L".png"), L"photo (2).png");
    // A suffix with a space takes the style's separator; a folder output has no extension.
    EXPECT_EQ(OutputName(OutputNaming::Dash, L"song", L"no metadata", 1, L".mp3"), L"song-no-metadata.mp3");
    EXPECT_EQ(OutputName(OutputNaming::Underscore, L"clip", L"frames", 1, L""), L"clip_frames");
    // No suffix (a zip of several items): the name alone, then numbered in the style's way.
    EXPECT_EQ(OutputName(OutputNaming::Parentheses, L"Archive", L"", 1, L".zip"), L"Archive.zip");
    EXPECT_EQ(OutputName(OutputNaming::Parentheses, L"Archive", L"", 2, L".zip"), L"Archive (2).zip");
    EXPECT_EQ(OutputName(OutputNaming::Dash, L"Archive", L"", 3, L".zip"), L"Archive-3.zip");
    EXPECT_EQ(OutputName(OutputNaming::Underscore, L"Archive", L"", 2, L".7z"), L"Archive_2.7z");
    EXPECT_EQ(OutputName(OutputNaming::Subfolder, L"Archive", L"", 2, L".zip"), L"Archive (2).zip");
    // The current style is the default one until Settings changes it.
    EXPECT_EQ(CurrentOutputNaming(), OutputNaming::Parentheses);
    EXPECT_EQ(OutputName(L"photo", L"edited", 1, L".jpg"), L"photo (edited).jpg");
}

