#include "Ask.h"
#include "PipelineRunner.h"
#include "TestUtil.h"
#include "Utf.h"
#include "archive/ArchiveFormats.h"
#include "archive/ArchiveSource.h"
#include "archive/ArchiveWriter.h"
#include "archive/Entries.h"
#include "archive/Sinks.h"
#include "sandbox/Sandbox.h"

#include <archive.h>
#include <archive_entry.h>
#include <zlib.h>

#include <atomic>
#include <map>
#include <random>
#include <thread>

using namespace sd;
using namespace sd::archive;
using namespace sd::test;

namespace
{
    // Archives are read by the sandboxed worker; in the tests the worker is this exe (TestMain.cpp
    // hands "--sd-worker" runs to sandbox::WorkerMain).
    void UseTestWorker()
    {
        wchar_t self[MAX_PATH]{};
        GetModuleFileNameW(nullptr, self, MAX_PATH);
        sandbox::SetWorkerPath(self);
    }

    struct Crafted
    {
        std::string name;
        std::string data;
        int type = AE_IFREG;        // AE_IFREG, AE_IFDIR, AE_IFLNK
        std::string link;           // symlink target, or hard link target (with AE_IFREG)
    };

    // Writes an archive with exactly the names given, the way a malicious tool would: libarchive's
    // writer does not clean names, only extraction should. A ZIP with a `password` is encrypted with
    // ZipCrypto (the password's UTF-8 bytes, as 7-Zip writes them).
    void WriteCrafted(const std::wstring& path, const char* format, const std::vector<Crafted>& entries, bool store = false,
                      const char* password = nullptr)
    {
        struct archive* a = archive_write_new();
        if (std::string_view(format) == "zip")
        {
            archive_write_set_format_zip(a);
            if (store)
                archive_write_set_format_option(a, "zip", "compression", "store");
            if (password)
            {
                ASSERT_EQ(archive_write_set_format_option(a, "zip", "encryption", "traditional"), ARCHIVE_OK) << archive_error_string(a);
                ASSERT_EQ(archive_write_set_passphrase(a, password), ARCHIVE_OK) << archive_error_string(a);
            }
        }
        else
            archive_write_set_format_pax_restricted(a);
        ASSERT_EQ(archive_write_open_filename_w(a, path.c_str()), ARCHIVE_OK);
        for (const auto& c : entries)
        {
            struct archive_entry* e = archive_entry_new();
            archive_entry_copy_pathname(e, c.name.c_str());
            archive_entry_set_filetype(e, static_cast<unsigned>(c.type));
            archive_entry_set_perm(e, 0644);
            archive_entry_set_mtime(e, 1589718896, 0);   // 2020-05-17 12:34:56 UTC
            if (c.type == AE_IFLNK)
                archive_entry_copy_symlink(e, c.link.c_str());
            else if (!c.link.empty())
                archive_entry_copy_hardlink(e, c.link.c_str());
            archive_entry_set_size(e, c.type == AE_IFREG && c.link.empty() ? static_cast<la_int64_t>(c.data.size()) : 0);
            ASSERT_GE(archive_write_header(a, e), ARCHIVE_WARN) << archive_error_string(a);
            if (c.type == AE_IFREG && c.link.empty() && !c.data.empty())
                archive_write_data(a, c.data.data(), c.data.size());
            archive_entry_free(e);
        }
        archive_write_close(a);
        archive_write_free(a);
    }

    // A ZIP written by hand: stored entries with raw name bytes and the flags we choose (no UTF-8 flag
    // for an old DOS-era archive, bit 0 for an encrypted one).
    std::string HandMadeZip(const std::vector<std::tuple<std::string, std::string, uint16_t>>& entries)
    {
        std::string out, central;
        const auto put16 = [](std::string& s, uint16_t v) { s += static_cast<char>(v & 0xFF); s += static_cast<char>(v >> 8); };
        const auto put32 = [&](std::string& s, uint32_t v) { put16(s, static_cast<uint16_t>(v & 0xFFFF)); put16(s, static_cast<uint16_t>(v >> 16)); };
        for (const auto& [name, data, flags] : entries)
        {
            const auto crc = static_cast<uint32_t>(crc32(0, reinterpret_cast<const Bytef*>(data.data()), static_cast<uInt>(data.size())));
            const auto offset = static_cast<uint32_t>(out.size());
            put32(out, 0x04034b50); put16(out, 20); put16(out, flags); put16(out, 0); put16(out, 0); put16(out, 0x21);
            put32(out, crc); put32(out, static_cast<uint32_t>(data.size())); put32(out, static_cast<uint32_t>(data.size()));
            put16(out, static_cast<uint16_t>(name.size())); put16(out, 0);
            out += name;
            out += data;
            put32(central, 0x02014b50); put16(central, 20); put16(central, 20); put16(central, flags); put16(central, 0); put16(central, 0);
            put16(central, 0x21); put32(central, crc); put32(central, static_cast<uint32_t>(data.size())); put32(central, static_cast<uint32_t>(data.size()));
            put16(central, static_cast<uint16_t>(name.size())); put16(central, 0); put16(central, 0); put16(central, 0); put16(central, 0);
            put32(central, 0); put32(central, offset);
            central += name;
        }
        const auto at = static_cast<uint32_t>(out.size());
        out += central;
        put32(out, 0x06054b50); put16(out, 0); put16(out, 0); put16(out, static_cast<uint16_t>(entries.size()));
        put16(out, static_cast<uint16_t>(entries.size())); put32(out, static_cast<uint32_t>(central.size())); put32(out, at); put16(out, 0);
        return out;
    }

    // Does ZipCrypto's one-byte check let `password` through for the first entry of `zip`? A wrong
    // password passes it once in 256 (the data then fails to decrypt), so the tests pick one that
    // the check rejects. The cipher is the one in the PKWARE APPNOTE (section 6.1).
    bool PassesZipCryptoCheck(const std::string& zip, std::string_view password)
    {
        const auto u16 = [&](size_t at) { return static_cast<uint16_t>(static_cast<uint8_t>(zip[at]) | static_cast<uint8_t>(zip[at + 1]) << 8); };
        const uint8_t check = static_cast<uint8_t>(zip[(u16(6) & 8) ? 11 : 17]);   // time or CRC, high byte
        const size_t header = 30 + size_t{ u16(26) } + u16(28);
        uint32_t keys[3] = { 305419896u, 591751049u, 878082192u };
        const auto crc = [](uint32_t c, uint8_t b) { return static_cast<uint32_t>(crc32(c ^ 0xffffffffu, &b, 1) ^ 0xffffffffu); };
        const auto update = [&](uint8_t c) {
            keys[0] = crc(keys[0], c);
            keys[1] = (keys[1] + (keys[0] & 0xff)) * 134775813u + 1;
            keys[2] = crc(keys[2], static_cast<uint8_t>(keys[1] >> 24));
        };
        for (const char c : password)
            update(static_cast<uint8_t>(c));
        uint8_t last = 0;
        for (size_t i = 0; i < 12; ++i)
        {
            const uint32_t t = keys[2] | 2;
            last = static_cast<uint8_t>(zip[header + i]) ^ static_cast<uint8_t>((t * (t ^ 1)) >> 8);
            update(last);
        }
        return last == check;
    }

    // Answers the questions of src/Ask.h for one test.
    struct Answers
    {
        Answers(ask::ChooseFn choose, ask::PasswordFn password) { ask::SetAnswers(std::move(choose), std::move(password)); }
        ~Answers() { ask::SetAnswers({}, {}); }
    };

    // CreateFileW rather than std::ofstream, which some C++ libraries open with a narrow name.
    void WriteBytes(const std::wstring& path, const void* data, size_t size)
    {
        wil::unique_hfile f(CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
        ASSERT_TRUE(f) << "cannot create " << Narrow(path);
        DWORD written = 0;
        ASSERT_TRUE(WriteFile(f.get(), data, static_cast<DWORD>(size), &written, nullptr));
    }
    void WriteFileW(const std::wstring& path, const std::string& text) { WriteBytes(path, text.data(), text.size()); }
    std::string ReadFileW(const std::wstring& path)
    {
        wil::unique_hfile f(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr));
        std::string out;
        char buffer[65536];
        DWORD read = 0;
        while (f && ReadFile(f.get(), buffer, sizeof(buffer), &read, nullptr) && read > 0)
            out.append(buffer, read);
        return out;
    }

    // Relative path -> contents of every file under `root` ("sub/a.txt"), and "dir/" for folders.
    std::map<std::wstring, std::string> Tree(const std::filesystem::path& root)
    {
        std::map<std::wstring, std::string> out;
        for (const auto& e : std::filesystem::recursive_directory_iterator(root))
        {
            auto rel = std::filesystem::relative(e.path(), root).generic_wstring();
            if (e.is_directory())
                out[rel + L"/"] = "";
            else
                out[rel] = ReadFileW(e.path().wstring());
        }
        return out;
    }

    int64_t MTime(const std::wstring& path)
    {
        WIN32_FILE_ATTRIBUTE_DATA info{};
        GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info);
        return static_cast<int64_t>((static_cast<uint64_t>(info.ftLastWriteTime.dwHighDateTime) << 32) | info.ftLastWriteTime.dwLowDateTime);
    }

    void SetMTime(const std::wstring& path, int64_t ticks)
    {
        wil::unique_hfile f(CreateFileW(path.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                        FILE_FLAG_BACKUP_SEMANTICS, nullptr));
        FILETIME ft{ static_cast<DWORD>(ticks & 0xFFFFFFFF), static_cast<DWORD>(static_cast<uint64_t>(ticks) >> 32) };
        SetFileTime(f.get(), nullptr, nullptr, &ft);
    }


    // RAR can't be written (no free encoder exists), so these come from libarchive's own test suite
    // (BSD-2-Clause, Copyright (c) 2003-2011 Tim Kientzle and contributors): a RAR 4 archive with a
    // file, a symlink, a folder and an empty folder; a stored RAR 5; an encrypted RAR 4.
    // libarchive/test/test_read_format_rar.rar.uu
    const unsigned char kRar4[] = {
        0x52, 0x61, 0x72, 0x21, 0x1a, 0x07, 0x00, 0xcf, 0x90, 0x73, 0x00, 0x00, 0x0d, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x84, 0x52, 0x74, 0x20, 0x90, 0x32, 0x00, 0x14, 0x00, 0x00, 0x00, 0x14, 0x00, 0x00, 0x00, 0x03, 0x42, 0xa2, 0xc8, 0xbe,
        0xb7, 0x76, 0xda, 0x3e, 0x14, 0x30, 0x08, 0x00, 0xa4, 0x81, 0x00, 0x00, 0x74, 0x65, 0x73, 0x74, 0x2e, 0x74, 0x78, 0x74,
        0x80, 0x08, 0xb7, 0x76, 0xda, 0x3e, 0xb7, 0x76, 0xda, 0x3e, 0x74, 0x65, 0x73, 0x74, 0x20, 0x74, 0x65, 0x78, 0x74, 0x20,
        0x64, 0x6f, 0x63, 0x75, 0x6d, 0x65, 0x6e, 0x74, 0x0d, 0x0a, 0x9d, 0x2f, 0x74, 0x20, 0x90, 0x32, 0x00, 0x08, 0x00, 0x00,
        0x00, 0x08, 0x00, 0x00, 0x00, 0x03, 0x7b, 0x44, 0xc9, 0xb6, 0xd1, 0x4c, 0xd8, 0x3e, 0x14, 0x30, 0x08, 0x00, 0xff, 0xa1,
        0x00, 0x00, 0x74, 0x65, 0x73, 0x74, 0x6c, 0x69, 0x6e, 0x6b, 0xc0, 0x08, 0xd1, 0x4c, 0xd8, 0x3e, 0x50, 0x5f, 0xda, 0x3e,
        0x74, 0x65, 0x73, 0x74, 0x2e, 0x74, 0x78, 0x74, 0xcd, 0xe0, 0x74, 0x20, 0x90, 0x3a, 0x00, 0x14, 0x00, 0x00, 0x00, 0x14,
        0x00, 0x00, 0x00, 0x03, 0x42, 0xa2, 0xc8, 0xbe, 0x63, 0x77, 0xda, 0x3e, 0x14, 0x30, 0x10, 0x00, 0xa4, 0x81, 0x00, 0x00,
        0x74, 0x65, 0x73, 0x74, 0x64, 0x69, 0x72, 0x5c, 0x74, 0x65, 0x73, 0x74, 0x2e, 0x74, 0x78, 0x74, 0xc0, 0xcc, 0x63, 0x77,
        0xda, 0x3e, 0x63, 0x77, 0xda, 0x3e, 0x74, 0x65, 0x73, 0x74, 0x20, 0x74, 0x65, 0x78, 0x74, 0x20, 0x64, 0x6f, 0x63, 0x75,
        0x6d, 0x65, 0x6e, 0x74, 0x0d, 0x0a, 0xa1, 0xc8, 0x74, 0xe0, 0x90, 0x31, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x03, 0x00, 0x00, 0x00, 0x00, 0x63, 0x77, 0xda, 0x3e, 0x14, 0x30, 0x07, 0x00, 0xed, 0x41, 0x00, 0x00, 0x74, 0x65,
        0x73, 0x74, 0x64, 0x69, 0x72, 0xc0, 0xcc, 0x63, 0x77, 0xda, 0x3e, 0x64, 0x77, 0xda, 0x3e, 0xe6, 0xe7, 0x74, 0xe0, 0x90,
        0x36, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00, 0x9d, 0xab, 0xd5, 0x3e, 0x14,
        0x30, 0x0c, 0x00, 0xed, 0x41, 0x00, 0x00, 0x74, 0x65, 0x73, 0x74, 0x65, 0x6d, 0x70, 0x74, 0x79, 0x64, 0x69, 0x72, 0x80,
        0xcc, 0x9d, 0xab, 0xd5, 0x3e, 0xc5, 0x5d, 0xda, 0x3e, 0xc4, 0x3d, 0x7b, 0x00, 0x40, 0x07, 0x00,
    };

    // libarchive/test/test_read_format_rar5_stored.rar.uu
    const unsigned char kRar5Stored[] = {
        0x52, 0x61, 0x72, 0x21, 0x1a, 0x07, 0x01, 0x00, 0x33, 0x92, 0xb5, 0xe5, 0x0a, 0x01, 0x05, 0x06, 0x00, 0x05, 0x01, 0x01,
        0x80, 0x80, 0x00, 0x38, 0x30, 0x06, 0x63, 0x2c, 0x02, 0x03, 0x0b, 0x9d, 0x00, 0x04, 0x9d, 0x00, 0xa4, 0x83, 0x02, 0xb4,
        0x43, 0xa0, 0x95, 0x80, 0x00, 0x01, 0x0e, 0x68, 0x65, 0x6c, 0x6c, 0x6f, 0x77, 0x6f, 0x72, 0x6c, 0x64, 0x2e, 0x74, 0x78,
        0x74, 0x0a, 0x03, 0x13, 0x7e, 0x0e, 0xab, 0x5b, 0x56, 0xe9, 0x0e, 0x1a, 0x68, 0x65, 0x6c, 0x6c, 0x6f, 0x20, 0x6c, 0x69,
        0x62, 0x61, 0x72, 0x63, 0x68, 0x69, 0x76, 0x65, 0x20, 0x74, 0x65, 0x73, 0x74, 0x20, 0x73, 0x75, 0x69, 0x74, 0x65, 0x21,
        0x0a, 0x1d, 0x77, 0x56, 0x51, 0x03, 0x05, 0x04, 0x00,
    };

    // libarchive/test/test_read_format_rar_encryption_data.rar.uu
    const unsigned char kRarEncrypted[] = {
        0x52, 0x61, 0x72, 0x21, 0x1a, 0x07, 0x00, 0xcf, 0x90, 0x73, 0x00, 0x00, 0x0d, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x5d, 0xfc, 0x74, 0x24, 0x84, 0x2f, 0x00, 0x20, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x03, 0x5c, 0x86, 0xad, 0xa6,
        0xa5, 0x4c, 0x31, 0x43, 0x1d, 0x33, 0x07, 0x00, 0xb4, 0x81, 0x00, 0x00, 0x66, 0x6f, 0x6f, 0x2e, 0x74, 0x78, 0x74, 0x4f,
        0x03, 0xbc, 0xd9, 0xd6, 0x16, 0x4c, 0xdf, 0x45, 0x40, 0x1a, 0x92, 0x92, 0x88, 0xdf, 0x15, 0x2b, 0x8e, 0x9e, 0xcf, 0x70,
        0xb9, 0x40, 0xe8, 0xc9, 0x47, 0xe6, 0xce, 0xd6, 0xbf, 0x44, 0xe2, 0xb8, 0x35, 0xa8, 0x9c, 0xe8, 0x65, 0x37, 0x01, 0x07,
        0x45, 0x74, 0x24, 0x84, 0x2f, 0x00, 0x20, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x03, 0xbe, 0xe2, 0x90, 0xcd, 0xa6,
        0x4c, 0x31, 0x43, 0x1d, 0x33, 0x07, 0x00, 0xb4, 0x81, 0x00, 0x00, 0x62, 0x61, 0x72, 0x2e, 0x74, 0x78, 0x74, 0x4f, 0x03,
        0xbc, 0xd9, 0xd6, 0x16, 0x4c, 0xdf, 0x70, 0x74, 0x7a, 0x48, 0x89, 0x88, 0x0f, 0x66, 0x5b, 0x7d, 0xe5, 0x06, 0xb9, 0x72,
        0x1e, 0xa8, 0x25, 0x55, 0xf8, 0xca, 0x37, 0x01, 0xa3, 0xed, 0xf4, 0xf1, 0x14, 0xf3, 0x23, 0x64, 0xd1, 0xa1, 0xc4, 0x3d,
        0x7b, 0x00, 0x40, 0x07, 0x00,
    };

    const std::wstring kUnicode = L"\u00dcn\u00efc\u00f6d\u00e9 \u65e5\u672c\u8a9e \u0444\u0430\u0439\u043b.txt";   // Ünïcödé 日本語 файл.txt
    const int64_t kStamp = TicksFromUnix(1589718896, 0);

    // A folder with nesting, an empty folder, a Unicode name, binary data and a known time.
    std::wstring MakeTree(const TempDir& dir)
    {
        const auto root = dir / L"Photos.2024";
        std::filesystem::create_directories(root + L"\\sub\\deeper");
        std::filesystem::create_directories(root + L"\\empty");
        WriteText(root + L"\\a.txt", "alpha");
        WriteFileW(root + L"\\sub\\" + kUnicode, "unicode name");
        std::string binary(300'000, '\0');
        std::mt19937 rng(7);
        for (auto& c : binary)
            c = static_cast<char>(rng());
        WriteText(root + L"\\sub\\deeper\\noise.bin", binary);
        SetMTime(root + L"\\a.txt", kStamp);
        return root;
    }

    class ArchiveTest : public ::testing::Test
    {
    protected:
        ArchiveTest() : recipes(dir), runner(std::make_shared<const ImageCodecs>(), nullptr) { UseTestWorker(); }
        ItemResult Run(const char* id, const std::wstring& source, std::stop_token stop = {})
        {
            return runner.Run(recipes.Get(id), 0, source, stop);
        }
        ItemResult Batch(const char* id, const std::vector<std::wstring>& sources, std::stop_token stop = {})
        {
            return runner.RunBatch(recipes.Get(id), 0, sources, stop);
        }
        TempDir dir;
        Recipes recipes;
        PipelineRunner runner;
    };
}

// ---- Names and formats ---------------------------------------------------------------------------

TEST(Archive, KindsAndStems)
{
    EXPECT_EQ(KindOfPath(L"C:\\x\\a.ZIP"), Kind::Zip);
    EXPECT_EQ(KindOfPath(L"a.tar.gz"), Kind::TarGz);
    EXPECT_EQ(KindOfPath(L"a.tgz"), Kind::TarGz);
    EXPECT_EQ(KindOfPath(L"a.tar"), Kind::Tar);
    EXPECT_EQ(KindOfPath(L"a.7z"), Kind::SevenZip);
    EXPECT_EQ(KindOfPath(L"a.rar"), Kind::Rar);
    EXPECT_EQ(KindOfPath(L"notes.txt.gz"), Kind::Gz);
    EXPECT_EQ(KindOfPath(L"a.tar.xz"), Kind::TarXz);
    EXPECT_EQ(KindOfPath(L"photo.jpg"), Kind::None);
    EXPECT_EQ(KindOfPath(L".zip"), Kind::None);
    EXPECT_EQ(ArchiveStem(L"C:\\x\\photos.tar.gz"), L"photos");
    EXPECT_EQ(ArchiveStem(L"notes.txt.gz"), L"notes.txt");
    EXPECT_EQ(ArchiveStem(L"my.files.zip"), L"my.files");
    EXPECT_TRUE(IsSameFormat(Kind::TarGz, WriteFormat::TarGz));
    EXPECT_FALSE(IsSameFormat(Kind::Tar, WriteFormat::TarGz));
}

TEST(Archive, SafePartsRefusesEscapes)
{
    std::vector<std::wstring> parts;
    std::wstring why;
    for (const wchar_t* bad : { L"../evil.txt", L"a/../../evil.txt", L"..\\evil.txt", L"/etc/passwd", L"\\Windows\\x", L"C:/Windows/x.dll",
                                L"c:evil", L"\\\\server\\share\\x", L"a/.../b", L"a/. ./b", L"a/.. /b" })
        EXPECT_FALSE(SafeParts(bad, parts, &why)) << Narrow(bad);
    ASSERT_TRUE(SafeParts(L"./docs//a.txt", parts, &why));
    EXPECT_EQ(JoinParts(parts, L'/'), L"docs/a.txt");
    ASSERT_TRUE(SafeParts(L"con.txt", parts, &why));
    EXPECT_EQ(parts.at(0), L"_con.txt");
    ASSERT_TRUE(SafeParts(L"LPT1", parts, &why));
    EXPECT_EQ(parts.at(0), L"_LPT1");
    ASSERT_TRUE(SafeParts(L"ab:stream|x?.txt", parts, &why));   // no alternate data streams, no wildcards
    EXPECT_EQ(parts.at(0), L"ab_stream_x_.txt");
    ASSERT_TRUE(SafeParts(L"trailing. /x", parts, &why));
    EXPECT_EQ(JoinParts(parts, L'/'), L"trailing/x");
    ASSERT_TRUE(SafeParts(L"./", parts, &why));
    EXPECT_TRUE(parts.empty());
    ASSERT_TRUE(SafeParts(kUnicode, parts, &why));
    EXPECT_EQ(parts.at(0), kUnicode);
}

TEST(Archive, DecodeNameFallsBackToTheOemCodePage)
{
    EXPECT_EQ(DecodeName("\xc3\xbc.txt", 437), L"\u00fc.txt");   // valid UTF-8 wins
    EXPECT_EQ(DecodeName("M\x81ller.txt", 437), L"M\u00fcller.txt");   // CP437 0x81 is ü
    int64_t s = 0;
    long ns = 0;
    UnixFromTicks(TicksFromUnix(1589718896, 123456700), s, ns);
    EXPECT_EQ(s, 1589718896);
    EXPECT_EQ(ns, 123456700);
}

// ---- Round trips ---------------------------------------------------------------------------------

TEST_F(ArchiveTest, RoundTripsEveryWritableFormat)
{
    const auto root = MakeTree(dir);
    const auto expected = Tree(root);
    for (const char* id : { "archive-folder-zip", "archive-folder-targz", "archive-folder-7z" })
    {
        const auto packed = Batch(id, { root });
        ASSERT_TRUE(packed.ok) << id << ": " << Narrow(packed.error);
        const auto& recipe = recipes.Get(id);
        const auto* format = FindWriteFormat(recipe.steps[0].params["format"].get<std::string>());
        EXPECT_EQ(std::filesystem::path(packed.output).filename(), L"Photos.2024 (" + recipe.suffix + L")" + format->extension) << id;

        const auto unpacked = Run("archive-extract", packed.output);
        ASSERT_TRUE(unpacked.ok) << id << ": " << Narrow(unpacked.error);
        // The archive holds "Photos.2024/...": the single top folder is not repeated inside the output.
        EXPECT_EQ(Tree(unpacked.output), expected) << id;
        EXPECT_NEAR(static_cast<double>(MTime(unpacked.output + L"\\a.txt")), static_cast<double>(kStamp), 2.0e7) << id << ": time kept";
        EXPECT_EQ(unpacked.message, L"Extracted 3 files") << id;
        std::filesystem::remove_all(unpacked.output);
    }
    EXPECT_EQ(CountFiles(dir, L".sdpart"), 0u);
}

TEST(Archive, PhotosVideoAndArchivesAreCompressedAlready)
{
    for (const wchar_t* name : { L"IMG_0001.JPG", L"Photos/clip.mp4", L"song.flac", L"backup.7z", L"Report.docx" })
        EXPECT_TRUE(IsCompressedAlready(name)) << Narrow(name);
    for (const wchar_t* name : { L"notes.txt", L"main.cpp", L"scan.bmp", L"photo.jpg/readme", L"jpg", L"" })
        EXPECT_FALSE(IsCompressedAlready(name)) << Narrow(name);
}

TEST_F(ArchiveTest, ZipStoresWhatIsCompressedAlready)
{
    const auto root = dir / L"Trip";
    std::filesystem::create_directories(root);
    std::string noise(200'000, '\0');
    std::mt19937 rng(3);
    for (auto& c : noise)
        c = static_cast<char>(rng());
    WriteText(root + L"\\photo.jpg", noise);
    WriteText(root + L"\\notes.txt", std::string(50'000, 'a'));
    const auto expected = Tree(root);
    const auto packed = Batch("archive-folder-zip", { root });
    ASSERT_TRUE(packed.ok) << Narrow(packed.error);

    // Each local file header: "PK\3\4", the method at +8 (0 stored, 8 deflate), the name at +30.
    const auto bytes = ReadAll(packed.output);
    std::map<std::string, int> methods;
    for (size_t at = bytes.find("PK\x03\x04"); at != std::string::npos && at + 30 <= bytes.size(); at = bytes.find("PK\x03\x04", at + 4))
    {
        const auto u16 = [&](size_t o) { return static_cast<uint8_t>(bytes[at + o]) | (static_cast<uint8_t>(bytes[at + o + 1]) << 8); };
        methods[bytes.substr(at + 30, u16(26))] = u16(8);
    }
    EXPECT_EQ(methods["Trip/photo.jpg"], 0);
    EXPECT_EQ(methods["Trip/notes.txt"], 8);

    const auto unpacked = Run("archive-extract", packed.output);
    ASSERT_TRUE(unpacked.ok) << Narrow(unpacked.error);
    EXPECT_EQ(Tree(unpacked.output), expected);
}

TEST_F(ArchiveTest, ZipNamesAreUtf8WithTheFlagSet)
{
    const auto file = dir / kUnicode;
    WriteFileW(file, "x");
    const auto packed = Batch("archive-zip", { file });
    ASSERT_TRUE(packed.ok) << Narrow(packed.error);
    EXPECT_EQ(std::filesystem::path(packed.output).filename(), std::filesystem::path(file).stem().wstring() + L" (zipped).zip");
    const auto bytes = ReadAll(packed.output);
    ASSERT_GT(bytes.size(), 30u);
    const uint16_t flags = static_cast<uint8_t>(bytes[6]) | (static_cast<uint8_t>(bytes[7]) << 8);
    EXPECT_TRUE(flags & 0x0800) << "general purpose bit 11 (UTF-8 names)";
    EXPECT_NE(bytes.find(Narrow(kUnicode)), std::string::npos) << "the name is stored as UTF-8";
}

TEST_F(ArchiveTest, SeveralItemsMakeArchiveZip)
{
    const auto folder = dir / L"Holiday";
    std::filesystem::create_directories(folder + L"\\inner");
    WriteText(folder + L"\\a.txt", "a");
    WriteText(folder + L"\\b.txt", "b");
    WriteText(folder + L"\\inner\\c.txt", "c");
    const auto packed = Batch("archive-zip", { folder + L"\\a.txt", folder + L"\\b.txt", folder + L"\\inner" });
    ASSERT_TRUE(packed.ok) << Narrow(packed.error);
    EXPECT_EQ(packed.output, folder + L"\\Archive.zip");
    const auto again = Batch("archive-zip", { folder + L"\\a.txt", folder + L"\\b.txt" });
    EXPECT_EQ(again.output, folder + L"\\Archive (2).zip") << "never overwrites";

    const auto unpacked = Run("archive-extract", packed.output);
    ASSERT_TRUE(unpacked.ok) << Narrow(unpacked.error);
    const std::map<std::wstring, std::string> expected{ { L"a.txt", "a" }, { L"b.txt", "b" }, { L"inner/", "" }, { L"inner/c.txt", "c" } };
    EXPECT_EQ(Tree(unpacked.output), expected);

    // Items from two folders: "Archive" too, next to the first; the other formats as well.
    const auto other = dir / L"Work";
    std::filesystem::create_directories(other);
    WriteText(other + L"\\d.txt", "d");
    const auto mixed = Batch("archive-folder-7z", { folder + L"\\inner", other });
    ASSERT_TRUE(mixed.ok) << Narrow(mixed.error);
    EXPECT_EQ(mixed.output, folder + L"\\Archive.7z");

    // One folder keeps its name, one file its own.
    const auto one = Batch("archive-zip", { other });
    ASSERT_TRUE(one.ok) << Narrow(one.error);
    EXPECT_EQ(one.output, dir / L"Work (zipped).zip");
    const auto file = Batch("archive-zip", { folder + L"\\a.txt" });
    ASSERT_TRUE(file.ok) << Narrow(file.error);
    EXPECT_EQ(file.output, folder + L"\\a (zipped).zip");
}

TEST_F(ArchiveTest, ConvertsBetweenFormats)
{
    const auto root = MakeTree(dir);
    const auto expected = Tree(root);
    const auto zip = Batch("archive-folder-zip", { root });
    ASSERT_TRUE(zip.ok) << Narrow(zip.error);
    EXPECT_TRUE(runner.IsNoOp(recipes.Get("archive-to-zip"), zip.output));

    const auto tgz = Run("archive-to-targz", zip.output);
    ASSERT_TRUE(tgz.ok) << Narrow(tgz.error);
    EXPECT_EQ(std::filesystem::path(tgz.output).filename(), L"Photos.2024 (zipped) (converted).tar.gz");
    const auto sevenZip = Run("archive-to-7z", tgz.output);
    ASSERT_TRUE(sevenZip.ok) << Narrow(sevenZip.error);
    const auto back = Run("archive-to-zip", sevenZip.output);
    ASSERT_TRUE(back.ok) << Narrow(back.error);
    const auto unpacked = Run("archive-extract", back.output);
    ASSERT_TRUE(unpacked.ok) << Narrow(unpacked.error);
    EXPECT_EQ(Tree(unpacked.output), expected);
}

TEST_F(ArchiveTest, SingleGzFileExtractsAndConverts)
{
    std::string text(5000, 'z');
    const auto gz = dir / L"notes.txt.gz";
    {
        gzFile f = gzopen_w(gz.c_str(), "wb");
        ASSERT_NE(f, nullptr);
        gzwrite(f, text.data(), static_cast<unsigned>(text.size()));
        gzclose(f);
    }
    const auto out = Run("archive-extract", gz);
    ASSERT_TRUE(out.ok) << Narrow(out.error);
    EXPECT_EQ(out.output, dir / L"notes (extracted).txt");
    EXPECT_EQ(ReadAll(out.output), text);

    const auto zip = Run("archive-to-zip", gz);
    ASSERT_TRUE(zip.ok) << Narrow(zip.error);
    const auto unpacked = Run("archive-extract", zip.output);
    ASSERT_TRUE(unpacked.ok) << Narrow(unpacked.error);
    EXPECT_EQ(ReadAll(unpacked.output + L"\\notes.txt"), text);
}

TEST_F(ArchiveTest, ReadsRarAndRar5)
{
    const auto rar = dir / L"old.rar";
    WriteBytes(rar, kRar4, sizeof(kRar4));
    const auto out = Run("archive-extract", rar);
    ASSERT_TRUE(out.ok) << Narrow(out.error);
    const std::map<std::wstring, std::string> expected{ { L"test.txt", "test text document\r\n" }, { L"testdir/", "" },
                                                        { L"testdir/test.txt", "test text document\r\n" }, { L"testemptydir/", "" } };
    EXPECT_EQ(Tree(out.output), expected) << "the symlink inside (testlink) is skipped";
    EXPECT_EQ(out.message, L"Extracted 2 files, 1 link skipped");

    const auto rar5 = dir / L"new.rar";
    WriteBytes(rar5, kRar5Stored, sizeof(kRar5Stored));
    const auto zip = Run("archive-to-zip", rar5);
    ASSERT_TRUE(zip.ok) << Narrow(zip.error);
    const auto unpacked = Run("archive-extract", zip.output);
    ASSERT_TRUE(unpacked.ok) << Narrow(unpacked.error);
    EXPECT_EQ(ReadAll(unpacked.output + L"\\helloworld.txt"), "hello libarchive test suite!\n");

    const auto locked = dir / L"locked.rar";
    WriteBytes(locked, kRarEncrypted, sizeof(kRarEncrypted));
    const auto refused = Run("archive-extract", locked);
    EXPECT_FALSE(refused.ok);
    EXPECT_NE(refused.error.find(L"password-protected"), std::wstring::npos) << Narrow(refused.error);
}

// ---- Untrusted archives --------------------------------------------------------------------------

TEST_F(ArchiveTest, TraversalAndAbsoluteNamesAreRefused)
{
    for (const char* name : { "../evil.txt", "ok/../../evil.txt", "/evil.txt", "C:/evil.txt", "..\\evil.txt" })
    {
        const auto tar = dir / L"bad.tar";
        std::filesystem::remove(tar);
        WriteCrafted(tar, "tar", { { "fine.txt", "fine" }, { name, "gotcha" } });
        const auto r = Run("archive-extract", tar);
        EXPECT_FALSE(r.ok) << name;
        EXPECT_TRUE(r.error.starts_with(L"Blocked:")) << name << ": " << Narrow(r.error);
        EXPECT_FALSE(std::filesystem::exists(dir / L"evil.txt")) << name;
        EXPECT_FALSE(std::filesystem::exists(dir / L"bad (extracted)")) << name << ": nothing half-extracted";
        EXPECT_EQ(CountFiles(dir, L".sdpart"), 0u) << name;
    }
    const auto zip = dir / L"bad.zip";
    WriteCrafted(zip, "zip", { { "../../evil.txt", "gotcha" } });
    const auto r = Run("archive-extract", zip);
    EXPECT_FALSE(r.ok);
    EXPECT_FALSE(std::filesystem::exists(dir.path().parent_path() / L"evil.txt"));
}

TEST_F(ArchiveTest, LinksAreSkippedAndHardLinksInsideAreCopied)
{
    const auto tar = dir / L"links.tar";
    WriteCrafted(tar, "tar", { { "real.txt", "real" },
                               { "abs-link", "", AE_IFLNK, "C:/Windows/win.ini" },
                               { "up-link", "", AE_IFLNK, "../../outside" },
                               { "hard.txt", "", AE_IFREG, "real.txt" },
                               { "hard-out.txt", "", AE_IFREG, "../outside.txt" } });
    const auto r = Run("archive-extract", tar);
    ASSERT_TRUE(r.ok) << Narrow(r.error);
    const std::map<std::wstring, std::string> expected{ { L"hard.txt", "real" }, { L"real.txt", "real" } };
    EXPECT_EQ(Tree(r.output), expected);
    EXPECT_EQ(r.message, L"Extracted 2 files, 3 links skipped");
}

TEST_F(ArchiveTest, NamesThatDifferOnlyInCaseAreKeptApart)
{
    const auto tar = dir / L"case.tar";
    WriteCrafted(tar, "tar", { { "README", "upper" }, { "readme", "lower" }, { "con.txt", "device" }, { "ab:c.txt", "stream" } });
    const auto r = Run("archive-extract", tar);
    ASSERT_TRUE(r.ok) << Narrow(r.error);
    const std::map<std::wstring, std::string> expected{ { L"README", "upper" }, { L"readme (2)", "lower" }, { L"_con.txt", "device" }, { L"ab_c.txt", "stream" } };
    EXPECT_EQ(Tree(r.output), expected);
}

TEST_F(ArchiveTest, OemNamesWithoutTheUtf8Flag)
{
    const auto zip = dir / L"dos.zip";
    WriteText(zip, HandMadeZip({ { "M\x81ller.txt", "dos", 0 }, { "\xc3\xbc-utf8.txt", "flagged", 0x0800 } }));
    const auto r = Run("archive-extract", zip);
    ASSERT_TRUE(r.ok) << Narrow(r.error);
    // CP437 on an English Windows; whatever this PC's OEM code page says otherwise.
    const auto oem = DecodeName("M\x81ller.txt", GetOEMCP());
    const std::map<std::wstring, std::string> expected{ { oem, "dos" }, { L"\u00fc-utf8.txt", "flagged" } };
    EXPECT_EQ(Tree(r.output), expected);
}

TEST_F(ArchiveTest, PasswordProtectedZipAsksForThePassword)
{
    const auto zip = dir / L"secret.zip";
    const std::string big(300000, 'b');
    WriteCrafted(zip, "zip", { { "docs/a.txt", "alpha" }, { "b.txt", big } }, false, "s3cret");
    std::wstring wrong;
    for (const auto* candidate : { L"wrong", L"wrong 2", L"wrong 3" })
        if (!PassesZipCryptoCheck(ReadAll(zip), Narrow(candidate)))
        {
            wrong = candidate;
            break;
        }
    ASSERT_FALSE(wrong.empty());

    // A wrong password, then the right one: asked twice (the second time told it was wrong), and not
    // again for the second entry.
    std::vector<ask::PasswordRequest> asked;
    std::vector<std::wstring> typed{ wrong, L"s3cret" };
    Answers answers(nullptr, [&](const ask::PasswordRequest& r, Secret& password) {
        asked.push_back(r);
        return password.Assign(typed.at(std::min(asked.size(), typed.size()) - 1));
    });
    const auto r = Run("archive-extract", zip);
    ASSERT_TRUE(r.ok) << Narrow(r.error);
    const std::map<std::wstring, std::string> expected{ { L"b.txt", big }, { L"docs/", "" }, { L"docs/a.txt", "alpha" } };
    EXPECT_EQ(Tree(r.output), expected);
    ASSERT_EQ(asked.size(), 2u);
    EXPECT_FALSE(asked[0].wrong);
    EXPECT_TRUE(asked[1].wrong);
    EXPECT_EQ(asked[0].file, L"secret.zip");
    EXPECT_EQ(asked[0].title, L"Extract");

    // Convert asks the same way, and the result opens without a password.
    asked.clear();
    typed = { L"s3cret" };
    const auto converted = Run("archive-to-7z", zip);
    ASSERT_TRUE(converted.ok) << Narrow(converted.error);
    EXPECT_EQ(asked.size(), 1u);
    asked.clear();
    const auto back = Run("archive-extract", converted.output);
    ASSERT_TRUE(back.ok) << Narrow(back.error);
    EXPECT_EQ(Tree(back.output), expected);
    EXPECT_TRUE(asked.empty());
}

TEST_F(ArchiveTest, NonAsciiPasswordsWork)
{
    // Accents: the archive holds the UTF-8 bytes, the box gives UTF-16; each common encoding is tried.
    const auto zip = dir / L"umlaut.zip";
    WriteCrafted(zip, "zip", { { "x.txt", "x" } }, false, "p\xc3\xa4ssw\xc3\xb6rd");
    Answers answers(nullptr, [](const ask::PasswordRequest&, Secret& password) { return password.Assign(L"p\u00e4ssw\u00f6rd"); });
    const auto r = Run("archive-extract", zip);
    ASSERT_TRUE(r.ok) << Narrow(r.error);
    EXPECT_EQ(ReadAll(r.output + L"\\x.txt"), "x");
}

TEST_F(ArchiveTest, CancellingThePasswordBoxStopsCleanly)
{
    const auto zip = dir / L"secret.zip";
    WriteCrafted(zip, "zip", { { "a.txt", "alpha" }, { "b.txt", "beta" } }, false, "s3cret");
    int asked = 0;
    Answers answers(nullptr, [&](const ask::PasswordRequest&, Secret&) {
        ++asked;
        return false;
    });
    for (const char* id : { "archive-extract", "archive-to-7z" })
    {
        const auto r = Run(id, zip);
        EXPECT_FALSE(r.ok) << id;
        EXPECT_TRUE(r.skipped) << id << ": cancelled by the user, not a failure";
        EXPECT_NE(r.error.find(L"no password given"), std::wstring::npos) << id << ": " << Narrow(r.error);
        EXPECT_EQ(CountFiles(dir, L".sdpart"), 0u) << id;
    }
    EXPECT_EQ(asked, 2);
    EXPECT_FALSE(std::filesystem::exists(dir / L"secret (extracted)"));
    EXPECT_FALSE(std::filesystem::exists(dir / L"secret (converted).7z"));

    // With no one to ask (no UI, no answers), the same.
    ask::SetAnswers({}, {});
    const auto r = Run("archive-extract", zip);
    EXPECT_TRUE(r.skipped);
    EXPECT_NE(r.error.find(L"no password given"), std::wstring::npos) << Narrow(r.error);
}

TEST_F(ArchiveTest, PasswordsThatCantHelpAreNotAskedFor)
{
    int asked = 0;
    Answers answers(nullptr, [&](const ask::PasswordRequest&, Secret& password) {
        ++asked;
        return password.Assign(L"anything");
    });

    // An encrypted RAR: libarchive can't decrypt RAR, so no question, just the reason.
    const auto rar = dir / L"locked.rar";
    WriteBytes(rar, kRarEncrypted, sizeof(kRarEncrypted));
    const auto r = Run("archive-extract", rar);
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.error.find(L"password-protected RAR"), std::wstring::npos) << Narrow(r.error);

    // A WinZip AES entry (method 99 with its 0x9901 extra field): asked for only where libarchive
    // has a crypto library to decrypt it.
    std::string aes;
    const auto put16 = [](std::string& s, uint16_t v) { s += static_cast<char>(v & 0xFF); s += static_cast<char>(v >> 8); };
    const auto put32 = [&](std::string& s, uint32_t v) { put16(s, static_cast<uint16_t>(v & 0xFFFF)); put16(s, static_cast<uint16_t>(v >> 16)); };
    const std::string name = "a.txt", extra = std::string("\x01\x99\x07\x00\x02\x00" "AE\x03\x00\x00", 11);
    const std::string data(16 + 2 + 5 + 10, '\x5a');   // salt, check, 5 bytes, authentication code
    std::string central;
    put32(aes, 0x04034b50); put16(aes, 51); put16(aes, 1); put16(aes, 99); put16(aes, 0); put16(aes, 0x21);
    put32(aes, 0); put32(aes, static_cast<uint32_t>(data.size())); put32(aes, 5); put16(aes, static_cast<uint16_t>(name.size()));
    put16(aes, static_cast<uint16_t>(extra.size()));
    aes += name + extra + data;
    put32(central, 0x02014b50); put16(central, 51); put16(central, 51); put16(central, 1); put16(central, 99); put16(central, 0);
    put16(central, 0x21); put32(central, 0); put32(central, static_cast<uint32_t>(data.size())); put32(central, 5);
    put16(central, static_cast<uint16_t>(name.size())); put16(central, static_cast<uint16_t>(extra.size())); put16(central, 0);
    put16(central, 0); put16(central, 0); put32(central, 0); put32(central, 0);
    central += name + extra;
    const auto at = static_cast<uint32_t>(aes.size());
    aes += central;
    put32(aes, 0x06054b50); put16(aes, 0); put16(aes, 0); put16(aes, 1); put16(aes, 1); put32(aes, static_cast<uint32_t>(central.size()));
    put32(aes, at); put16(aes, 0);
    const auto zip = dir / L"aes.zip";
    WriteText(zip, aes);

    const bool canDecrypt = [] {
        struct archive* w = archive_write_new();
        archive_write_set_format_zip(w);
        const bool ok = archive_write_set_format_option(w, "zip", "encryption", "aes256") == ARCHIVE_OK;
        archive_write_free(w);
        return ok;
    }();
    asked = 0;
    const auto z = Run("archive-extract", zip);
    EXPECT_FALSE(z.ok) << "the data is not really encrypted";
    EXPECT_EQ(CountFiles(dir, L".sdpart"), 0u);
    if (canDecrypt)
        EXPECT_EQ(asked, 1);
    else
    {
        EXPECT_EQ(asked, 0);
        EXPECT_NE(z.error.find(L"AES"), std::wstring::npos) << Narrow(z.error);
    }
}

TEST_F(ArchiveTest, DamagedArchiveFailsCleanly)
{
    const auto zip = dir / L"broken.zip";
    WriteText(zip, "PK\x03\x04 this is not really a zip file at all");
    const auto r = Run("archive-extract", zip);
    EXPECT_FALSE(r.ok);
    EXPECT_FALSE(r.error.empty());
    EXPECT_EQ(CountFiles(dir, L".sdpart"), 0u);
}

TEST_F(ArchiveTest, BombsHitTheLimits)
{
    // 64 MB of zeros packs into a few hundred KB.
    const auto zip = dir / L"bomb.zip";
    WriteCrafted(zip, "zip", { { "zeros-1.bin", std::string(32 << 20, '\0') }, { "zeros-2.bin", std::string(32 << 20, '\0') } });
    const auto inputSize = std::filesystem::file_size(zip);
    ASSERT_LT(inputSize, 1u << 20);

    const auto extractWith = [&](const Limits& limits) {
        const auto out = dir / L"out";
        std::filesystem::remove_all(out);
        std::filesystem::create_directories(out);
        GuardSink* guardPtr = nullptr;
        ExtractSink sink(out, [&](uint64_t n) { return guardPtr->Charge(n); });
        GuardSink guard(sink, limits, inputSize);
        guardPtr = &guard;
        auto r = ReadEntries(zip, Kind::Zip, limits, guard, {}, {});
        sink.End();
        return r;
    };
    Limits total;
    total.maxTotalBytes = 16 << 20;
    EXPECT_EQ(extractWith(total).status, Status::Limit);
    Limits ratio;
    ratio.ratioAfterBytes = 1 << 20;
    ratio.maxRatio = 50;
    EXPECT_EQ(extractWith(ratio).status, Status::Limit);
    Limits entries;
    entries.maxEntries = 1;
    EXPECT_EQ(extractWith(entries).status, Status::Limit);
    EXPECT_TRUE(extractWith(Limits{}).ok()) << "the defaults are generous";
}

TEST_F(ArchiveTest, CancelStopsPromptlyAndLeavesNothing)
{
    // 96 MB of incompressible data, stored, so extraction takes a moment.
    std::string noise(96 << 20, '\0');
    std::mt19937 rng(3);
    for (size_t i = 0; i < noise.size(); i += 4)
    {
        const auto v = rng();
        std::memcpy(noise.data() + i, &v, 4);
    }
    const auto zip = dir / L"big.zip";
    WriteCrafted(zip, "zip", { { "noise.bin", noise } }, true);
    noise.clear();

    for (const char* id : { "archive-extract", "archive-to-7z" })
    {
        std::stop_source stop;
        std::atomic<bool> started{ false };
        std::jthread canceller([&](std::stop_token own) {
            while (!started && !own.stop_requested())
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            stop.request_stop();
        });
        const auto t0 = std::chrono::steady_clock::now();
        const auto r = runner.Run(recipes.Get(id), 0, zip, stop.get_token(), [&](double) { started = true; });
        const auto took = std::chrono::steady_clock::now() - t0;
        EXPECT_FALSE(r.ok) << id;
        EXPECT_TRUE(r.skipped) << id << ": " << Narrow(r.error);
        EXPECT_LT(took, std::chrono::seconds(20)) << id;
        EXPECT_EQ(CountFiles(dir, L".sdpart"), 0u) << id;
        EXPECT_FALSE(std::filesystem::exists(dir / L"big (extracted)")) << id;
        EXPECT_FALSE(std::filesystem::exists(dir / L"big (converted).7z")) << id;
    }

    // Packing stops too.
    std::stop_source stop;
    std::atomic<bool> started{ false };
    std::jthread canceller([&](std::stop_token own) {
        while (!started && !own.stop_requested())
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        stop.request_stop();
    });
    const auto r = runner.RunBatch(recipes.Get("archive-folder-7z"), 0, { zip }, stop.get_token(), [&](double) { started = true; });
    EXPECT_TRUE(r.skipped) << Narrow(r.error);
    EXPECT_EQ(CountFiles(dir, L".sdpart"), 0u);
    EXPECT_EQ(CountFiles(dir, L".7z"), 0u);
}

TEST_F(ArchiveTest, SlicesApplyToTheRightFiles)
{
    const auto folder = dir / L"Folder";
    std::filesystem::create_directories(folder);
    const auto zip = dir / L"a.zip";
    const auto photo = dir / L"photo.jpg";
    WriteText(photo, "x");
    WriteCrafted(zip, "zip", { { "x.txt", "x" } });
    EXPECT_TRUE(runner.Applies(recipes.Get("archive-zip"), photo));      // Alt wheel: anything
    EXPECT_TRUE(runner.Applies(recipes.Get("archive-zip"), folder));
    EXPECT_TRUE(runner.Applies(recipes.Get("archive-folder-7z"), folder));   // Shift wheel: folders
    EXPECT_FALSE(runner.Applies(recipes.Get("archive-folder-7z"), photo));
    EXPECT_TRUE(runner.Applies(recipes.Get("archive-extract"), zip));
    EXPECT_FALSE(runner.Applies(recipes.Get("archive-extract"), photo));
    EXPECT_FALSE(runner.Applies(recipes.Get("archive-extract"), folder));
    EXPECT_TRUE(runner.EngineFor(recipes.Get("archive-zip"))->IsBatch(recipes.Get("archive-zip")));
    EXPECT_FALSE(runner.EngineFor(recipes.Get("archive-extract"))->IsBatch(recipes.Get("archive-extract")));
    EXPECT_TRUE(runner.IsNoOp(recipes.Get("archive-to-zip"), zip));
    EXPECT_FALSE(runner.IsNoOp(recipes.Get("archive-to-7z"), zip));
}

