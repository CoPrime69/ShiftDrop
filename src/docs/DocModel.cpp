#include "docs/DocModel.h"

#include <format>

namespace sd::docs
{
    std::string PlainText(const std::vector<Span>& spans)
    {
        std::string out;
        for (const auto& s : spans)
            out += s.lineBreak ? std::string("\n") : s.text;
        return out;
    }

    void NormalizeSpans(std::vector<Span>& spans)
    {
        std::vector<Span> out;
        out.reserve(spans.size());
        for (auto& s : spans)
        {
            if (!s.lineBreak && s.image < 0 && s.text.empty())
                continue;
            if (!out.empty() && !s.lineBreak && s.image < 0 && !out.back().lineBreak && out.back().image < 0 && out.back().SameStyle(s))
                out.back().text += s.text;
            else
                out.push_back(std::move(s));
        }
        spans = std::move(out);
    }

    std::string TitleOf(const Document& doc, const std::string& fallback)
    {
        if (!doc.title.empty())
            return doc.title;
        for (const auto& b : doc.blocks)
            if (b.kind == BlockKind::Heading)
            {
                auto t = PlainText(b.spans);
                for (auto& c : t)
                    if (c == '\n')
                        c = ' ';
                if (!t.empty())
                    return t;
            }
        return fallback;
    }

    std::string SniffImage(const std::string& d, std::string* extension)
    {
        const auto set = [&](const char* mime, const char* ext) {
            if (extension)
                *extension = ext;
            return std::string(mime);
        };
        if (d.size() >= 8 && d.compare(0, 8, "\x89PNG\r\n\x1A\n") == 0)
            return set("image/png", ".png");
        if (d.size() >= 3 && d.compare(0, 3, "\xFF\xD8\xFF") == 0)
            return set("image/jpeg", ".jpg");
        if (d.size() >= 6 && (d.compare(0, 6, "GIF87a") == 0 || d.compare(0, 6, "GIF89a") == 0))
            return set("image/gif", ".gif");
        if (d.size() >= 2 && d.compare(0, 2, "BM") == 0)
            return set("image/bmp", ".bmp");
        if (d.size() >= 12 && d.compare(0, 4, "RIFF") == 0 && d.compare(8, 4, "WEBP") == 0)
            return set("image/webp", ".webp");
        const auto head = d.substr(0, 512);
        if (head.find("<svg") != std::string::npos)
            return set("image/svg+xml", ".svg");
        if (extension)
            extension->clear();
        return {};
    }

    bool ImageDimensions(const std::string& d, int& width, int& height)
    {
        const auto u8 = [&](size_t i) { return static_cast<unsigned>(static_cast<unsigned char>(d[i])); };
        if (d.size() >= 24 && d.compare(0, 8, "\x89PNG\r\n\x1A\n") == 0)
        {
            width = static_cast<int>(u8(16) << 24 | u8(17) << 16 | u8(18) << 8 | u8(19));
            height = static_cast<int>(u8(20) << 24 | u8(21) << 16 | u8(22) << 8 | u8(23));
            return width > 0 && height > 0;
        }
        if (d.size() >= 10 && (d.compare(0, 6, "GIF89a") == 0 || d.compare(0, 6, "GIF87a") == 0))
        {
            width = static_cast<int>(u8(6) | u8(7) << 8);
            height = static_cast<int>(u8(8) | u8(9) << 8);
            return width > 0 && height > 0;
        }
        if (d.size() >= 4 && u8(0) == 0xFF && u8(1) == 0xD8)
        {
            // Walk the JPEG segments to the first start-of-frame marker.
            size_t i = 2;
            while (i + 9 < d.size())
            {
                if (u8(i) != 0xFF)
                    return false;
                const unsigned marker = u8(i + 1);
                if (marker == 0xD8 || marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7))
                {
                    i += 2;
                    continue;
                }
                const size_t len = u8(i + 2) << 8 | u8(i + 3);
                if ((marker >= 0xC0 && marker <= 0xCF) && marker != 0xC4 && marker != 0xC8 && marker != 0xCC)
                {
                    height = static_cast<int>(u8(i + 5) << 8 | u8(i + 6));
                    width = static_cast<int>(u8(i + 7) << 8 | u8(i + 8));
                    return width > 0 && height > 0;
                }
                i += 2 + len;
            }
        }
        return false;
    }

    int AddImage(Document& doc, std::string data)
    {
        Image img;
        std::string ext;
        img.mime = SniffImage(data, &ext);
        if (img.mime.empty())
        {
            img.mime = "application/octet-stream";
            ext = ".bin";
        }
        img.name = std::format("image{}{}", doc.images.size() + 1, ext);
        ImageDimensions(data, img.width, img.height);
        img.data = std::move(data);
        doc.images.push_back(std::move(img));
        return static_cast<int>(doc.images.size() - 1);
    }
}
