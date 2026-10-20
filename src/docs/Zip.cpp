#include "docs/Zip.h"
#include "docs/Bytes.h"

// miniz without its zlib-style aliases (compress, Byte...), which would clash with zlib, used by QPDF.
#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES
#include <miniz.h>

#include <cstring>

namespace sd::docs
{
    struct ZipReader::Impl
    {
        std::string bytes;
        mz_zip_archive zip{};
        bool open = false;

        ~Impl()
        {
            if (open)
                mz_zip_reader_end(&zip);
        }
    };

    ZipReader::ZipReader() : m(std::make_unique<Impl>()) {}
    ZipReader::~ZipReader() = default;

    bool ZipReader::Open(std::string bytes)
    {
        if (m->open)
            mz_zip_reader_end(&m->zip);
        m->open = false;
        m->bytes = std::move(bytes);
        std::memset(&m->zip, 0, sizeof(m->zip));
        m->open = mz_zip_reader_init_mem(&m->zip, m->bytes.data(), m->bytes.size(), 0) != 0;
        return m->open;
    }

    std::vector<std::string> ZipReader::Names() const
    {
        std::vector<std::string> names;
        if (!m->open)
            return names;
        const mz_uint n = mz_zip_reader_get_num_files(&m->zip);
        for (mz_uint i = 0; i < n; ++i)
        {
            mz_zip_archive_file_stat st{};
            if (mz_zip_reader_file_stat(&m->zip, i, &st))
                names.emplace_back(st.m_filename);
        }
        return names;
    }

    bool ZipReader::Has(std::string_view name) const
    {
        if (!m->open)
            return false;
        const std::string n(name);
        return mz_zip_reader_locate_file(&m->zip, n.c_str(), nullptr, 0) >= 0;   // flags 0: any case
    }

    std::optional<std::string> ZipReader::Read(std::string_view name) const
    {
        if (!m->open)
            return std::nullopt;
        std::string n(name);
        if (!n.empty() && n.front() == '/')
            n.erase(0, 1);
        int index = mz_zip_reader_locate_file(&m->zip, n.c_str(), nullptr, MZ_ZIP_FLAG_CASE_SENSITIVE);
        if (index < 0)
            index = mz_zip_reader_locate_file(&m->zip, n.c_str(), nullptr, 0);
        if (index < 0)
            return std::nullopt;
        mz_zip_archive_file_stat st{};
        if (!mz_zip_reader_file_stat(&m->zip, static_cast<mz_uint>(index), &st) || st.m_uncomp_size > (1ull << 31))
            return std::nullopt;   // over 2 GB uncompressed: a zip bomb, not a document part
        std::string out(static_cast<size_t>(st.m_uncomp_size), '\0');
        if (!out.empty() && !mz_zip_reader_extract_to_mem(&m->zip, static_cast<mz_uint>(index), out.data(), out.size(), 0))
            return std::nullopt;
        return out;
    }

    struct ZipWriter::Impl
    {
        mz_zip_archive zip{};
        bool open = false;

        ~Impl()
        {
            if (open)
                mz_zip_writer_end(&zip);
        }
    };

    ZipWriter::ZipWriter() : m(std::make_unique<Impl>())
    {
        m->open = mz_zip_writer_init_heap(&m->zip, 0, 64 * 1024) != 0;
    }
    ZipWriter::~ZipWriter() = default;

    bool ZipWriter::Add(std::string_view name, std::string_view data, bool store)
    {
        if (!m->open)
            return false;
        const std::string n(name);
        return mz_zip_writer_add_mem(&m->zip, n.c_str(), data.data(), data.size(),
                                     static_cast<mz_uint>(store ? MZ_NO_COMPRESSION : MZ_DEFAULT_LEVEL)) != 0;
    }

    bool ZipWriter::Finish(std::string& out)
    {
        if (!m->open)
            return false;
        void* buffer = nullptr;
        size_t size = 0;
        const bool ok = mz_zip_writer_finalize_heap_archive(&m->zip, &buffer, &size) != 0;
        if (ok)
            out.assign(static_cast<const char*>(buffer), size);
        mz_free(buffer);   // finalize hands the buffer over to us
        mz_zip_writer_end(&m->zip);
        m->open = false;
        return ok;
    }
}
