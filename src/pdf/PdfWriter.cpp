#include "pdf/PdfWriter.h"

#include <zlib.h>

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <format>

namespace sd::pdf
{
    std::string Num(double value)
    {
        if (!std::isfinite(value))
            return "0";
        // Round to 4 decimals first so -0.00001 does not print as "-0".
        double rounded = std::round(value * 10000.0) / 10000.0;
        if (rounded == 0.0)
            rounded = 0.0;
        std::string s = std::format("{:.4f}", rounded);
        while (!s.empty() && s.back() == '0')
            s.pop_back();
        if (!s.empty() && s.back() == '.')
            s.pop_back();
        return s == "-0" ? "0" : s;
    }

    std::string LiteralString(std::string_view bytes)
    {
        std::string out = "(";
        for (const unsigned char c : bytes)
        {
            if (c == '(' || c == ')' || c == '\\')
            {
                out += '\\';
                out += static_cast<char>(c);
            }
            else if (c < 32 || c > 126)
                out += std::format("\\{:03o}", c);
            else
                out += static_cast<char>(c);
        }
        return out + ")";
    }

    std::string Deflate(std::string_view data)
    {
        uLongf size = compressBound(static_cast<uLong>(data.size()));
        std::string out(size, '\0');
        if (compress2(reinterpret_cast<Bytef*>(out.data()), &size, reinterpret_cast<const Bytef*>(data.data()),
                      static_cast<uLong>(data.size()), Z_DEFAULT_COMPRESSION) != Z_OK)
            return {};
        out.resize(size);
        return out;
    }

    PdfWriter::PdfWriter(const std::wstring& path)
    {
        m_out.open(std::filesystem::path(path), std::ios::binary | std::ios::trunc);
        m_ok = m_out.good();
        // The second line's high bytes tell transfer tools the file is binary (PDF 1.7, 7.5.2).
        Put("%PDF-1.7\n%\xE2\xE3\xCF\xD3\n");
    }

    void PdfWriter::Put(std::string_view s)
    {
        if (!m_ok)
            return;
        m_out.write(s.data(), static_cast<std::streamsize>(s.size()));
        m_pos += s.size();
        m_ok = m_out.good();
    }

    int PdfWriter::NewObject()
    {
        m_offsets.push_back(0);
        return static_cast<int>(m_offsets.size());
    }

    void PdfWriter::Object(int id, std::string_view body)
    {
        if (id < 1 || static_cast<size_t>(id) > m_offsets.size())
        {
            m_ok = false;
            return;
        }
        m_offsets[static_cast<size_t>(id) - 1] = m_pos;
        Put(std::format("{} 0 obj\n", id));
        Put(body);
        Put("\nendobj\n");
    }

    void PdfWriter::Stream(int id, std::string_view dict, std::string_view data)
    {
        if (id < 1 || static_cast<size_t>(id) > m_offsets.size())
        {
            m_ok = false;
            return;
        }
        m_offsets[static_cast<size_t>(id) - 1] = m_pos;
        Put(std::format("{} 0 obj\n<< {} /Length {} >>\nstream\n", id, dict, data.size()));
        Put(data);
        Put("\nendstream\nendobj\n");
    }

    void PdfWriter::FlateStream(int id, std::string_view dict, std::string_view data)
    {
        const std::string packed = Deflate(data);
        if (packed.empty() && !data.empty())
        {
            m_ok = false;
            return;
        }
        Stream(id, std::string(dict) + " /Filter /FlateDecode", packed);
    }

    bool PdfWriter::Finish(int root, int info)
    {
        const uint64_t xref = m_pos;
        Put(std::format("xref\n0 {}\n0000000000 65535 f \n", m_offsets.size() + 1));
        for (const uint64_t offset : m_offsets)
        {
            if (offset == 0)   // reserved and never written: a broken file, refuse it
                m_ok = false;
            Put(std::format("{:010} 00000 n \n", offset));
        }
        Put(std::format("trailer\n<< /Size {} /Root {} 0 R", m_offsets.size() + 1, root));
        if (info > 0)
            Put(std::format(" /Info {} 0 R", info));
        Put(std::format(" >>\nstartxref\n{}\n%%EOF\n", xref));
        m_out.flush();
        m_ok = m_ok && m_out.good();
        m_out.close();
        return m_ok;
    }
}
