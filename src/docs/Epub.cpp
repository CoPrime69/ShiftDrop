#include "docs/Bytes.h"
#include "docs/Formats.h"
#include "docs/Zip.h"
#include "Utf.h"

#include <windows.h>

#include <format>

// EPUB 3 writer: a zip whose first entry is an uncompressed "mimetype", then META-INF/container.xml
// pointing at the package document (content.opf), which lists every file and the reading order.
// An EPUB 2 toc.ncx is included too, so older readers still get a table of contents.
namespace sd::docs
{
    namespace
    {
        struct NavEntry
        {
            std::string href;   // "text/chapter1.xhtml#h3"
            std::string text;
            int level = 1;
        };

        bool EpubCoreImage(const std::string& mime)
        {
            // Core media types of EPUB 3.3; anything else (BMP) would make the book invalid.
            return mime == "image/png" || mime == "image/jpeg" || mime == "image/gif" || mime == "image/svg+xml" || mime == "image/webp";
        }

        std::string UserLanguage()
        {
            wchar_t name[LOCALE_NAME_MAX_LENGTH]{};
            if (GetUserDefaultLocaleName(name, LOCALE_NAME_MAX_LENGTH) > 0)
                return Narrow(name);
            return "en";
        }
    }

    bool WriteEpub(const Document& source, const std::string& fallbackTitle, std::string& out)
    {
        // Pictures EPUB readers may not show become their alt text.
        Document doc = source;
        const auto stripImages = [&](std::vector<Span>& spans) {
            for (auto& s : spans)
                if (s.image >= 0 && (static_cast<size_t>(s.image) >= doc.images.size() || !EpubCoreImage(doc.images[static_cast<size_t>(s.image)].mime)))
                {
                    s.image = -1;
                    if (!s.text.empty())
                        s.text = "[" + s.text + "]";
                }
        };
        for (auto& b : doc.blocks)
        {
            stripImages(b.spans);
            for (auto& row : b.rows)
                for (auto& c : row)
                    stripImages(c.spans);
        }

        const auto title = TitleOf(doc, fallbackTitle);
        const auto lang = doc.language.empty() ? UserLanguage() : doc.language;
        const auto uid = NewUuid();

        // Chapters split at level-1 headings when there are at least two of them.
        std::vector<size_t> starts{ 0 };
        size_t h1 = 0;
        for (const auto& b : doc.blocks)
            h1 += b.kind == BlockKind::Heading && b.level == 1;
        if (h1 >= 2)
        {
            for (size_t i = 1; i < doc.blocks.size(); ++i)
                if (doc.blocks[i].kind == BlockKind::Heading && doc.blocks[i].level == 1)
                    starts.push_back(i);
        }
        starts.push_back(doc.blocks.size());

        HtmlOptions o;
        o.xhtml = true;
        o.imageDir = "../images/";
        o.cssHref = "../style.css";
        o.language = lang;

        std::vector<std::string> chapters;
        std::vector<NavEntry> nav;
        int headingId = 1;
        for (size_t c = 0; c + 1 < starts.size(); ++c)
        {
            if (starts[c] == starts[c + 1])
                continue;   // nothing before the first heading
            const auto file = std::format("text/chapter{}.xhtml", chapters.size() + 1);
            const int firstId = headingId;
            for (size_t i = starts[c]; i < starts[c + 1]; ++i)
            {
                const auto& b = doc.blocks[i];
                if (b.kind != BlockKind::Heading)
                    continue;
                if (b.level <= 2)
                {
                    auto text = PlainText(b.spans);
                    for (auto& ch : text)
                        if (ch == '\n')
                            ch = ' ';
                    nav.push_back({ std::format("{}#h{}", file, headingId), text.empty() ? std::string("Section") : text, b.level });
                }
                ++headingId;
            }
            // Chapter title: its first heading, else the book's.
            std::string chapterTitle = title;
            for (size_t i = starts[c]; i < starts[c + 1]; ++i)
                if (doc.blocks[i].kind == BlockKind::Heading)
                {
                    chapterTitle = PlainText(doc.blocks[i].spans);
                    break;
                }
            std::string page = "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<!DOCTYPE html>\n<html xmlns=\"http://www.w3.org/1999/xhtml\" xmlns:epub=\"http://www.idpf.org/2007/ops\" lang=\"" +
                               XmlEscape(lang) + "\" xml:lang=\"" + XmlEscape(lang) + "\">\n<head>\n<meta charset=\"utf-8\"/>\n<title>" + XmlEscape(chapterTitle) +
                               "</title>\n<link rel=\"stylesheet\" type=\"text/css\" href=\"../style.css\"/>\n</head>\n<body>\n" +
                               WriteHtmlBlocks(doc, starts[c], starts[c + 1], o, firstId) + "</body>\n</html>\n";
            chapters.push_back(std::move(page));
        }
        if (chapters.empty())
            chapters.push_back("<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<!DOCTYPE html>\n<html xmlns=\"http://www.w3.org/1999/xhtml\"><head><meta charset=\"utf-8\"/><title>" +
                               XmlEscape(title) + "</title></head><body><p></p></body></html>\n");
        if (nav.empty())
            nav.push_back({ "text/chapter1.xhtml", title, 1 });

        // nav.xhtml: level-2 entries nest inside the level-1 entry before them.
        std::string navList = "<ol>\n";
        bool inner = false, liOpen = false;
        for (const auto& e : nav)
        {
            const auto link = "<a href=\"" + XmlEscape(e.href) + "\">" + XmlEscape(e.text) + "</a>";
            if (e.level >= 2 && liOpen)
            {
                if (!inner)
                {
                    navList += "<ol>\n";
                    inner = true;
                }
                navList += "<li>" + link + "</li>\n";
                continue;
            }
            if (inner)
            {
                navList += "</ol>\n";
                inner = false;
            }
            if (liOpen)
                navList += "</li>\n";
            navList += "<li>" + link;
            liOpen = true;
        }
        if (inner)
            navList += "</ol>\n";
        if (liOpen)
            navList += "</li>\n";
        navList += "</ol>\n";
        const std::string navDoc = "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<!DOCTYPE html>\n<html xmlns=\"http://www.w3.org/1999/xhtml\" xmlns:epub=\"http://www.idpf.org/2007/ops\" lang=\"" +
                                   XmlEscape(lang) + "\" xml:lang=\"" + XmlEscape(lang) + "\">\n<head>\n<meta charset=\"utf-8\"/>\n<title>" + XmlEscape(title) +
                                   "</title>\n</head>\n<body>\n<nav epub:type=\"toc\" id=\"toc\">\n<h1>Contents</h1>\n" + navList + "</nav>\n</body>\n</html>\n";

        std::string ncx = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<ncx xmlns=\"http://www.daisy.org/z3986/2005/ncx/\" version=\"2005-1\">\n<head><meta name=\"dtb:uid\" content=\"" +
                          XmlEscape(uid) + "\"/><meta name=\"dtb:depth\" content=\"1\"/></head>\n<docTitle><text>" + XmlEscape(title) + "</text></docTitle>\n<navMap>\n";
        for (size_t i = 0; i < nav.size(); ++i)
            ncx += std::format("<navPoint id=\"np{0}\" playOrder=\"{0}\"><navLabel><text>{1}</text></navLabel><content src=\"{2}\"/></navPoint>\n", i + 1,
                               XmlEscape(nav[i].text), XmlEscape(nav[i].href));
        ncx += "</navMap>\n</ncx>\n";

        std::string manifest = "<item id=\"nav\" href=\"nav.xhtml\" media-type=\"application/xhtml+xml\" properties=\"nav\"/>\n"
                               "<item id=\"ncx\" href=\"toc.ncx\" media-type=\"application/x-dtbncx+xml\"/>\n"
                               "<item id=\"css\" href=\"style.css\" media-type=\"text/css\"/>\n";
        std::string spine;
        for (size_t i = 0; i < chapters.size(); ++i)
        {
            manifest += std::format("<item id=\"c{0}\" href=\"text/chapter{0}.xhtml\" media-type=\"application/xhtml+xml\"/>\n", i + 1);
            spine += std::format("<itemref idref=\"c{}\"/>\n", i + 1);
        }
        for (size_t i = 0; i < doc.images.size(); ++i)
            if (EpubCoreImage(doc.images[i].mime))
                manifest += std::format("<item id=\"img{}\" href=\"images/{}\" media-type=\"{}\"/>\n", i + 1, XmlEscape(doc.images[i].name), doc.images[i].mime);
        const std::string opf = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<package xmlns=\"http://www.idpf.org/2007/opf\" version=\"3.0\" unique-identifier=\"bookid\" xml:lang=\"" +
                                XmlEscape(lang) + "\">\n<metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\">\n<dc:identifier id=\"bookid\">" + XmlEscape(uid) +
                                "</dc:identifier>\n<dc:title>" + XmlEscape(title) + "</dc:title>\n<dc:language>" + XmlEscape(lang) +
                                "</dc:language>\n<meta property=\"dcterms:modified\">" + UtcNowIso() + "</meta>\n</metadata>\n<manifest>\n" + manifest +
                                "</manifest>\n<spine toc=\"ncx\">\n" + spine + "</spine>\n</package>\n";
        const char* container = R"(<?xml version="1.0" encoding="UTF-8"?>
<container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container">
<rootfiles><rootfile full-path="OEBPS/content.opf" media-type="application/oebps-package+xml"/></rootfiles>
</container>
)";

        ZipWriter zip;
        // "mimetype" must be the first entry, stored (not compressed), with no extra field.
        bool ok = zip.Add("mimetype", "application/epub+zip", true) && zip.Add("META-INF/container.xml", container) &&
                  zip.Add("OEBPS/content.opf", opf) && zip.Add("OEBPS/nav.xhtml", navDoc) && zip.Add("OEBPS/toc.ncx", ncx) &&
                  zip.Add("OEBPS/style.css", DefaultCss());
        for (size_t i = 0; i < chapters.size(); ++i)
            ok = ok && zip.Add(std::format("OEBPS/text/chapter{}.xhtml", i + 1), chapters[i]);
        for (const auto& img : doc.images)
            if (EpubCoreImage(img.mime))
                ok = ok && zip.Add("OEBPS/images/" + img.name, img.data, true);
        return ok && zip.Finish(out);
    }
}
