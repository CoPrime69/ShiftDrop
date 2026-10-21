#pragma once
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Zip containers in memory (DOCX, XLSX, ODS and EPUB are all zips), on miniz.
namespace sd::docs
{
    class ZipReader
    {
    public:
        ZipReader();
        ~ZipReader();
        ZipReader(const ZipReader&) = delete;
        ZipReader& operator=(const ZipReader&) = delete;

        // Takes the archive's bytes (kept alive by the reader). False if it is not a zip.
        bool Open(std::string bytes);
        std::vector<std::string> Names() const;
        bool Has(std::string_view name) const;
        // The entry's uncompressed bytes. Names are matched exactly first, then case-insensitively
        // (some writers differ in case from their own relationship targets).
        std::optional<std::string> Read(std::string_view name) const;

    private:
        struct Impl;
        std::unique_ptr<Impl> m;
    };

    class ZipWriter
    {
    public:
        ZipWriter();
        ~ZipWriter();
        ZipWriter(const ZipWriter&) = delete;
        ZipWriter& operator=(const ZipWriter&) = delete;

        // `store` keeps the entry uncompressed (EPUB's and ODF's "mimetype" must be stored, and first).
        bool Add(std::string_view name, std::string_view data, bool store = false);
        // Finishes the archive into `out`. The writer cannot be used afterwards.
        bool Finish(std::string& out);

    private:
        struct Impl;
        std::unique_ptr<Impl> m;
    };
}
