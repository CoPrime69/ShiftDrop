#pragma once
#include "Ffmpeg.h"
#include "OutputPath.h"
#include "PathUtil.h"
#include "RecipeStore.h"
#include "Utf.h"
#include "platform/Process.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <string>

namespace sd::test
{
    // A fresh folder under %TEMP% per test, removed afterwards.
    class TempDir
    {
    public:
        TempDir()
        {
            const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
            m_path = std::filesystem::temp_directory_path() /
                     std::format(L"sdtest-{}-{}", platform::CurrentProcessId(), info ? Widen(info->name()) : L"x");
            std::filesystem::remove_all(m_path);
            std::filesystem::create_directories(m_path);
        }
        ~TempDir()
        {
            std::error_code ec;
            std::filesystem::remove_all(m_path, ec);
        }
        TempDir(const TempDir&) = delete;
        TempDir& operator=(const TempDir&) = delete;

        std::wstring operator/(const std::wstring& name) const { return (m_path / name).wstring(); }
        const std::filesystem::path& path() const { return m_path; }

    private:
        std::filesystem::path m_path;
    };

    // Sets the output naming style for one test and puts the default back afterwards.
    struct NamingScope
    {
        explicit NamingScope(OutputNaming naming) { SetOutputNaming(naming); }
        ~NamingScope() { SetOutputNaming(OutputNaming::Parentheses); }
        NamingScope(const NamingScope&) = delete;
        NamingScope& operator=(const NamingScope&) = delete;
    };

    inline void WriteText(const std::wstring& path, const std::string& text)
    {
        std::ofstream(std::filesystem::path(path), std::ios::binary) << text;
    }

    inline std::string ReadAll(const std::wstring& path)
    {
        std::ifstream in(std::filesystem::path(path), std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), {});
    }

    // Built-in recipes plus the seeded example, loaded from a throwaway recipes folder.
    struct Recipes
    {
        explicit Recipes(const TempDir& dir) { store.Load(registry, dir / L"recipes"); }
        const Recipe& Get(std::string_view id) const
        {
            const auto* r = store.Find(id);
            EXPECT_NE(r, nullptr) << std::string(id);
            return *r;
        }
        ActionRegistry registry;
        RecipeStore store;
    };

    // The real ffmpeg when CI (or a developer) points SHIFTDROP_FFMPEG at ffmpeg.exe; the tests that
    // need it check Available() and are skipped otherwise. One engine for the whole run.
    inline std::shared_ptr<const FfmpegEngine> TestFfmpeg()
    {
        static const auto engine = std::make_shared<const FfmpegEngine>(FfmpegEngine::Locate());
        return engine;
    }

    inline size_t CountFiles(const TempDir& dir, const std::wstring& extension)
    {
        size_t n = 0;
        for (const auto& e : std::filesystem::directory_iterator(dir.path()))
            if (e.path().extension() == extension)
                ++n;
        return n;
    }
}

// The helpers that need the platform's image codecs or its sandbox, from
// tests/platform/<platform>/PlatformTestUtil.h (the tests' include path picks the folder).
#include "PlatformTestUtil.h"
