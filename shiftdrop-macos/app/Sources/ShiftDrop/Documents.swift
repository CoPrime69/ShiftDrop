import AppKit
import Foundation

// Document conversion (docs-to-docx / pdf / md / html / epub / txt). Reading and writing go through AppKit's
// NSAttributedString (DOCX, DOC, RTF, RTFD, ODT, HTML, TXT) plus a small Markdown converter of our own. AppKit's HTML
// importer and the print system must run on the main thread, so those steps hop there.

func onMain<T>(_ body: () throws -> T) rethrows -> T {
    Thread.isMainThread ? try body() : try DispatchQueue.main.sync(execute: body)
}

enum Markdown {
    // MARK: Markdown -> HTML

    private static func escape(_ s: String) -> String {
        s.replacingOccurrences(of: "&", with: "&amp;").replacingOccurrences(of: "<", with: "&lt;").replacingOccurrences(of: ">", with: "&gt;")
    }

    private static func regex(_ s: String, _ pattern: String, _ template: String) -> String {
        guard let re = try? NSRegularExpression(pattern: pattern, options: []) else { return s }
        return re.stringByReplacingMatches(in: s, range: NSRange(s.startIndex..., in: s), withTemplate: template)
    }

    static func inline(_ raw: String) -> String {
        // Code spans first, so nothing inside them is formatted.
        var codes: [String] = []
        var s = raw
        if let re = try? NSRegularExpression(pattern: "`([^`]+)`") {
            let ns = s as NSString
            var out = "", last = 0
            for m in re.matches(in: s, range: NSRange(location: 0, length: ns.length)) {
                out += ns.substring(with: NSRange(location: last, length: m.range.location - last))
                codes.append("<code>\(escape(ns.substring(with: m.range(at: 1))))</code>")
                out += "\u{1}\(codes.count - 1)\u{2}"
                last = m.range.location + m.range.length
            }
            s = out + ns.substring(from: last)
        }
        s = escape(s)
        s = regex(s, #"!\[([^\]]*)\]\(([^)\s]+)[^)]*\)"#, "<img alt=\"$1\" src=\"$2\"/>")
        s = regex(s, #"\[([^\]]+)\]\(([^)\s]+)[^)]*\)"#, "<a href=\"$2\">$1</a>")
        s = regex(s, #"\*\*(.+?)\*\*"#, "<strong>$1</strong>")
        s = regex(s, #"__(.+?)__"#, "<strong>$1</strong>")
        s = regex(s, #"(?<![*\w])\*(?!\s)(.+?)(?<!\s)\*(?![*\w])"#, "<em>$1</em>")
        s = regex(s, #"(?<![_\w])_(?!\s)(.+?)(?<!\s)_(?![_\w])"#, "<em>$1</em>")
        for (i, c) in codes.enumerated() { s = s.replacingOccurrences(of: "\u{1}\(i)\u{2}", with: c) }
        return s
    }

    static func body(_ md: String) -> String {
        let lines = md.replacingOccurrences(of: "\r\n", with: "\n").components(separatedBy: "\n")
        var out: [String] = []
        var i = 0
        func matches(_ line: String, _ p: String) -> Bool { line.range(of: p, options: .regularExpression) != nil }
        while i < lines.count {
            let line = lines[i]
            let trimmed = line.trimmingCharacters(in: .whitespaces)
            if trimmed.isEmpty { i += 1; continue }
            if trimmed.hasPrefix("```") {   // fenced code
                var code: [String] = []; i += 1
                while i < lines.count && !lines[i].trimmingCharacters(in: .whitespaces).hasPrefix("```") { code.append(lines[i]); i += 1 }
                i += 1
                out.append("<pre><code>\(escape(code.joined(separator: "\n")))</code></pre>")
            } else if matches(trimmed, #"^#{1,6}\s"#) {
                let level = trimmed.prefix { $0 == "#" }.count
                out.append("<h\(level)>\(inline(String(trimmed.dropFirst(level)).trimmingCharacters(in: .whitespaces)))</h\(level)>"); i += 1
            } else if matches(trimmed, #"^(-{3,}|\*{3,}|_{3,})$"#) {
                out.append("<hr/>"); i += 1
            } else if matches(trimmed, #"^[-*+]\s"#) || matches(trimmed, #"^\d+[.)]\s"#) {
                let ordered = matches(trimmed, #"^\d+[.)]\s"#)
                var items: [String] = []
                while i < lines.count {
                    let t = lines[i].trimmingCharacters(in: .whitespaces)
                    if ordered ? matches(t, #"^\d+[.)]\s"#) : matches(t, #"^[-*+]\s"#) {
                        items.append("<li>\(inline(regex(t, ordered ? #"^\d+[.)]\s+"# : #"^[-*+]\s+"#, "")))</li>"); i += 1
                    } else { break }
                }
                out.append("<\(ordered ? "ol" : "ul")>\n\(items.joined(separator: "\n"))\n</\(ordered ? "ol" : "ul")>")
            } else if trimmed.hasPrefix(">") {
                var q: [String] = []
                while i < lines.count, lines[i].trimmingCharacters(in: .whitespaces).hasPrefix(">") {
                    q.append(regex(lines[i].trimmingCharacters(in: .whitespaces), #"^>\s?"#, "")); i += 1
                }
                out.append("<blockquote>\(body(q.joined(separator: "\n")))</blockquote>")
            } else if trimmed.hasPrefix("|") && i + 1 < lines.count && matches(lines[i + 1], #"^\s*\|?\s*:?-{2,}"#) {
                func cells(_ l: String) -> [String] {
                    var t = l.trimmingCharacters(in: .whitespaces)
                    if t.hasPrefix("|") { t.removeFirst() }; if t.hasSuffix("|") { t.removeLast() }
                    return t.components(separatedBy: "|").map { $0.trimmingCharacters(in: .whitespaces) }
                }
                var rows = "<tr>" + cells(trimmed).map { "<th>\(inline($0))</th>" }.joined() + "</tr>\n"
                i += 2
                while i < lines.count, lines[i].trimmingCharacters(in: .whitespaces).hasPrefix("|") {
                    rows += "<tr>" + cells(lines[i]).map { "<td>\(inline($0))</td>" }.joined() + "</tr>\n"; i += 1
                }
                out.append("<table>\n\(rows)</table>")
            } else {
                var para: [String] = []
                while i < lines.count {
                    let t = lines[i].trimmingCharacters(in: .whitespaces)
                    if t.isEmpty || t.hasPrefix("```") || matches(t, #"^#{1,6}\s"#) || matches(t, #"^[-*+]\s"#) || matches(t, #"^\d+[.)]\s"#) || t.hasPrefix(">") { break }
                    para.append(t); i += 1
                }
                out.append("<p>\(inline(para.joined(separator: " ")))</p>")
            }
        }
        return out.joined(separator: "\n")
    }

    static func xhtml(_ md: String, title: String) -> String {
        """
        <?xml version="1.0" encoding="UTF-8"?>
        <!DOCTYPE html>
        <html xmlns="http://www.w3.org/1999/xhtml" lang="en"><head><meta charset="utf-8"/><title>\(escape(title))</title>
        <style>body{font-family:-apple-system,Helvetica,Arial,sans-serif;line-height:1.5;max-width:42em;margin:2em auto;padding:0 1em}pre{background:#f4f4f6;padding:.8em;overflow:auto}code{font-family:Menlo,monospace}blockquote{border-left:3px solid #ccc;margin-left:0;padding-left:1em;color:#555}table{border-collapse:collapse}td,th{border:1px solid #ccc;padding:.3em .6em}img{max-width:100%}</style>
        </head><body>
        \(body(md))
        </body></html>
        """
    }

    // MARK: Attributed text -> Markdown

    static func from(_ text: NSAttributedString) -> String {
        let full = text.string as NSString
        // The body size is the most common font size, by characters.
        var weight: [Int: Int] = [:]
        text.enumerateAttribute(.font, in: NSRange(location: 0, length: full.length)) { value, range, _ in
            if let f = value as? NSFont { weight[Int((f.pointSize * 2).rounded()), default: 0] += range.length }
        }
        let body = Double(weight.max { $0.value < $1.value }?.key ?? 24) / 2
        var out: [String] = []
        full.enumerateSubstrings(in: NSRange(location: 0, length: full.length), options: .byParagraphs) { _, range, _, _ in
            let para = text.attributedSubstring(from: range)
            let plain = para.string.trimmingCharacters(in: .whitespacesAndNewlines)
            if plain.isEmpty { if out.last != "" { out.append("") }; return }
            var size = 0.0, boldChars = 0, total = 0
            para.enumerateAttribute(.font, in: NSRange(location: 0, length: para.length)) { v, r, _ in
                if let f = v as? NSFont {
                    size = max(size, Double(f.pointSize)); total += r.length
                    if f.fontDescriptor.symbolicTraits.contains(.bold) { boldChars += r.length }
                }
            }
            let ratio = body > 0 ? size / body : 1
            var inline = ""
            para.enumerateAttributes(in: NSRange(location: 0, length: para.length)) { attrs, r, _ in
                var piece = (para.string as NSString).substring(with: r).replacingOccurrences(of: "\u{FFFC}", with: "").replacingOccurrences(of: "\t", with: " ")
                let lead = piece.prefix { $0 == " " }, trail = piece.reversed().prefix { $0 == " " }
                piece = piece.trimmingCharacters(in: .whitespaces)
                guard !piece.isEmpty else { inline += lead + trail; return }
                if let f = attrs[.font] as? NSFont, ratio < 1.15 {
                    let traits = f.fontDescriptor.symbolicTraits
                    if traits.contains(.bold) && traits.contains(.italic) { piece = "***\(piece)***" }
                    else if traits.contains(.bold) && boldChars < total { piece = "**\(piece)**" }
                    else if traits.contains(.italic) { piece = "*\(piece)*" }
                }
                if let link = attrs[.link] { piece = "[\(piece)](\(link))" }
                inline += lead + piece + trail
            }
            inline = inline.trimmingCharacters(in: .whitespaces)
            // A list item: the list marker is text in the paragraph ("•\t…" or "1.\t…") or a text list in its style.
            if let style = para.attribute(.paragraphStyle, at: 0, effectiveRange: nil) as? NSParagraphStyle, let list = style.textLists.first {
                let ordered = list.markerFormat.rawValue.contains("decimal") || list.markerFormat.rawValue.contains("alpha") || list.markerFormat.rawValue.contains("roman")
                let stripped = inline.replacingOccurrences(of: #"^[\s•◦▪‣·\-–*]+|^\d+[.)]\s*"#, with: "", options: .regularExpression)
                out.append((ordered ? "1. " : "- ") + stripped)
                return
            }
            if let m = inline.range(of: #"^(•|◦|▪|‣|·|–|-)\s+"#, options: .regularExpression) { out.append("- " + inline[m.upperBound...]); return }
            // "1<tab>text": a numbered item whose marker lost its dot.
            if let m = para.string.range(of: #"^\s*(\d{1,2})[.)]?\t"#, options: .regularExpression), let num = para.string[m].filter(\.isNumber) as String?, !num.isEmpty {
                out.append("\(num). " + inline.replacingOccurrences(of: #"^\d{1,2}[.)]?\s+"#, with: "", options: .regularExpression)); return
            }
            let heading = ratio >= 1.6 ? 1 : (ratio >= 1.35 ? 2 : (ratio >= 1.15 ? 3 : (total > 0 && boldChars == total && plain.count < 80 && !plain.hasSuffix(".") ? 4 : 0)))
            if heading > 0 { out.append(String(repeating: "#", count: heading) + " " + plain); out.append(""); return }
            out.append(inline)
            out.append("")
        }
        // Collapse runs of blank lines.
        var result: [String] = []
        for l in out { if l.isEmpty && result.last?.isEmpty == true { continue }; result.append(l) }
        return result.joined(separator: "\n").trimmingCharacters(in: .whitespacesAndNewlines) + "\n"
    }
}

enum Documents {
    /// A document in the forms the writers need; each is made on demand.
    final class Doc {
        let title: String
        private var _attributed: NSAttributedString?
        private var _markdown: String?
        init(title: String, attributed: NSAttributedString? = nil, markdown: String? = nil) { self.title = title; _attributed = attributed; _markdown = markdown }

        var markdown: String {
            if let m = _markdown { return m }
            let m = onMain { Markdown.from(_attributed ?? NSAttributedString()) }
            _markdown = m
            return m
        }
        var attributed: NSAttributedString {
            if let a = _attributed { return a }
            let a = Documents.attributed(fromHTML: Markdown.xhtml(_markdown ?? "", title: title))
            _attributed = a
            return a
        }
    }

    static func attributed(fromHTML html: String) -> NSAttributedString {
        onMain {
            let data = Data(html.utf8)
            return (try? NSAttributedString(data: data, options: [.documentType: NSAttributedString.DocumentType.html, .characterEncoding: String.Encoding.utf8.rawValue],
                                            documentAttributes: nil)) ?? NSAttributedString(string: "")
        }
    }

    static func read(_ url: URL) throws -> Doc {
        let title = FileKinds.stem(url)
        switch url.pathExtension.lowercased() {
        case "md", "markdown":
            return Doc(title: title, markdown: try String(contentsOf: url, encoding: .utf8))
        case "txt":
            var enc = String.Encoding.utf8
            let text = try String(contentsOf: url, usedEncoding: &enc)
            // Plain text keeps its line structure: paragraphs are blank-line separated.
            let md = text.replacingOccurrences(of: "\r\n", with: "\n").components(separatedBy: "\n\n")
                .map { $0.components(separatedBy: "\n").map { $0.trimmingCharacters(in: .whitespaces) }.joined(separator: " ") }.joined(separator: "\n\n")
            return Doc(title: title, markdown: md)
        default:
            return try onMain {
                guard let a = try? NSAttributedString(url: url, options: [:], documentAttributes: nil) else { throw PipelineError("Can\u{2019}t read \(url.lastPathComponent)") }
                return Doc(title: title, attributed: a)
            }
        }
    }

    // MARK: Writers (each returns the bytes or writes a file)

    static func docx(_ doc: Doc) throws -> Data {
        try onMain {
            let a = doc.attributed
            return try a.data(from: NSRange(location: 0, length: a.length), documentAttributes: [.documentType: NSAttributedString.DocumentType.officeOpenXML])
        }
    }

    static func html(_ doc: Doc) -> Data { Data(Markdown.xhtml(doc.markdown, title: doc.title).utf8) }

    static func txt(_ doc: Doc) -> Data {
        let text = onMain { doc.attributed.string }
        return Data(text.replacingOccurrences(of: "\u{FFFC}", with: "").utf8)
    }

    static func md(_ doc: Doc) -> Data { Data(doc.markdown.utf8) }

    /// US Letter in the Americas, A4 elsewhere.
    static func pdf(_ doc: Doc, to url: URL) throws {
        try onMain {
            let metric = Locale.current.measurementSystem != .us
            let paper = metric ? NSSize(width: 595.28, height: 841.89) : NSSize(width: 612, height: 792), margin: CGFloat = 54
            let info = NSPrintInfo()
            info.paperSize = paper
            info.topMargin = margin; info.bottomMargin = margin; info.leftMargin = margin; info.rightMargin = margin
            info.jobDisposition = .save
            info.dictionary()[NSPrintInfo.AttributeKey.jobSavingURL] = url
            let width = paper.width - 2 * margin
            let view = NSTextView(frame: NSRect(x: 0, y: 0, width: width, height: 100))
            view.textContainer?.containerSize = NSSize(width: width, height: .greatestFiniteMagnitude)
            view.textContainer?.widthTracksTextView = true
            view.isVerticallyResizable = true
            view.textStorage?.setAttributedString(doc.attributed)
            view.layoutManager?.ensureLayout(for: view.textContainer!)
            let used = view.layoutManager?.usedRect(for: view.textContainer!) ?? .zero
            view.setFrameSize(NSSize(width: width, height: max(used.height + 8, 100)))
            let op = NSPrintOperation(view: view, printInfo: info)
            op.showsPrintPanel = false; op.showsProgressPanel = false
            guard op.run(), FileManager.default.fileExists(atPath: url.path) else { throw PipelineError("Couldn\u{2019}t make the PDF") }
        }
    }

    /// EPUB 3: mimetype (stored, first), container, package, navigation and one content page.
    static func epub(_ doc: Doc, to url: URL) throws {
        let fm = FileManager.default
        let dir = fm.temporaryDirectory.appendingPathComponent("sd-epub-\(UUID().uuidString)")
        try fm.createDirectory(at: dir.appendingPathComponent("META-INF"), withIntermediateDirectories: true)
        try fm.createDirectory(at: dir.appendingPathComponent("OEBPS"), withIntermediateDirectories: true)
        defer { try? fm.removeItem(at: dir) }
        func esc(_ s: String) -> String { s.replacingOccurrences(of: "&", with: "&amp;").replacingOccurrences(of: "<", with: "&lt;") }
        try "application/epub+zip".write(to: dir.appendingPathComponent("mimetype"), atomically: true, encoding: .ascii)
        try """
        <?xml version="1.0"?><container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container"><rootfiles><rootfile full-path="OEBPS/content.opf" media-type="application/oebps-package+xml"/></rootfiles></container>
        """.write(to: dir.appendingPathComponent("META-INF/container.xml"), atomically: true, encoding: .utf8)
        let id = UUID().uuidString
        try """
        <?xml version="1.0" encoding="UTF-8"?><package xmlns="http://www.idpf.org/2007/opf" version="3.0" unique-identifier="id"><metadata xmlns:dc="http://purl.org/dc/elements/1.1/"><dc:identifier id="id">urn:uuid:\(id)</dc:identifier><dc:title>\(esc(doc.title))</dc:title><dc:language>en</dc:language><meta property="dcterms:modified">\(ISO8601DateFormatter().string(from: Date()).prefix(19))Z</meta></metadata><manifest><item id="nav" href="nav.xhtml" media-type="application/xhtml+xml" properties="nav"/><item id="content" href="content.xhtml" media-type="application/xhtml+xml"/></manifest><spine><itemref idref="content"/></spine></package>
        """.write(to: dir.appendingPathComponent("OEBPS/content.opf"), atomically: true, encoding: .utf8)
        try """
        <?xml version="1.0" encoding="UTF-8"?><html xmlns="http://www.w3.org/1999/xhtml" xmlns:epub="http://www.idpf.org/2007/ops"><head><title>\(esc(doc.title))</title></head><body><nav epub:type="toc"><ol><li><a href="content.xhtml">\(esc(doc.title))</a></li></ol></nav></body></html>
        """.write(to: dir.appendingPathComponent("OEBPS/nav.xhtml"), atomically: true, encoding: .utf8)
        try Markdown.xhtml(doc.markdown, title: doc.title).replacingOccurrences(of: "<!DOCTYPE html>\n", with: "")
            .write(to: dir.appendingPathComponent("OEBPS/content.xhtml"), atomically: true, encoding: .utf8)
        try? fm.removeItem(at: url)
        try runZip(["-X0", url.path, "mimetype"], in: dir)
        try runZip(["-Xr9D", url.path, "META-INF", "OEBPS"], in: dir)
    }

    private static func runZip(_ args: [String], in dir: URL) throws {
        let p = Process()
        p.executableURL = URL(fileURLWithPath: "/usr/bin/zip")
        p.arguments = args; p.currentDirectoryURL = dir
        p.standardOutput = FileHandle.nullDevice; p.standardError = FileHandle.nullDevice
        try p.run(); p.waitUntilExit()
        guard p.terminationStatus == 0 else { throw PipelineError("zip failed") }
    }
}
