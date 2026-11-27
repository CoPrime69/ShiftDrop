import AppKit
import PDFKit
import Quartz

/// PDF work through PDFKit and Core Graphics: pictures to PDF, merge, compress, pages to images, text and Markdown.
enum Pdf {
    static func open(_ url: URL) throws -> PDFDocument {
        guard let doc = PDFDocument(url: url) else { throw PipelineError("Can\u{2019}t read \(url.lastPathComponent)") }
        if doc.isLocked && !doc.unlock(withPassword: "") { throw PipelineError("\(url.lastPathComponent) is password protected") }
        guard doc.pageCount > 0 else { throw PipelineError("\(url.lastPathComponent) has no pages") }
        return doc
    }

    /// One page as a picture at `dpi` (300 for Convert), white behind it, never above 9000 px on a side.
    static func render(_ page: PDFPage, dpi: CGFloat) throws -> CGImage {
        let box = page.bounds(for: .mediaBox)
        let rotated = page.rotation % 180 != 0
        var scale = dpi / 72
        let side = max(box.width, box.height) * scale
        if side > 9000 { scale *= 9000 / side }
        let w = Int((rotated ? box.height : box.width) * scale), h = Int((rotated ? box.width : box.height) * scale)
        guard w > 0, h > 0, let ctx = CGContext(data: nil, width: w, height: h, bitsPerComponent: 8, bytesPerRow: 0, space: CGColorSpace(name: CGColorSpace.sRGB)!,
                                                bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue) else { throw PipelineError("Out of memory") }
        ctx.setFillColor(CGColor(red: 1, green: 1, blue: 1, alpha: 1))
        ctx.fill(CGRect(x: 0, y: 0, width: w, height: h))
        ctx.scaleBy(x: scale, y: scale)
        // PDFPage.draw uses the page's own (rotated) coordinate space.
        page.draw(with: .mediaBox, to: ctx)
        guard let img = ctx.makeImage() else { throw PipelineError("Couldn\u{2019}t draw a page") }
        return img
    }

    // MARK: Pages to pictures

    static func writePages(_ doc: PDFDocument, format: ImageFormat, stem: String, into dir: URL, progress: (Double) -> Void, isCancelled: () -> Bool) throws -> [URL] {
        var out: [URL] = []
        let digits = String(doc.pageCount).count
        for i in 0..<doc.pageCount {
            if isCancelled() { throw CancelledError() }
            guard let page = doc.page(at: i) else { continue }
            let data = try ImagePipeline.encode(try render(page, dpi: 300), as: format, quality: 90)
            let name = doc.pageCount == 1 ? "\(stem).\(format.ext)" : "\(stem) page \(String(format: "%0\(digits)d", i + 1)).\(format.ext)"
            let url = dir.appendingPathComponent(name)
            try data.write(to: url)
            out.append(url)
            progress(Double(i + 1) / Double(doc.pageCount))
        }
        return out
    }

    // MARK: Text

    /// The text of every page; a page with no text layer (a scan) is read with OCR.
    static func text(_ doc: PDFDocument, progress: (Double) -> Void, isCancelled: () -> Bool) throws -> String {
        var pages: [String] = []
        for i in 0..<doc.pageCount {
            if isCancelled() { throw CancelledError() }
            guard let page = doc.page(at: i) else { continue }
            var t = (page.string ?? "").trimmingCharacters(in: .whitespacesAndNewlines)
            if t.count < 3 { t = (try? OCR.text(in: render(page, dpi: 200))) ?? "" }
            pages.append(t)
            progress(Double(i + 1) / Double(doc.pageCount))
        }
        return pages.joined(separator: "\n\n")
    }

    /// Markdown with headings and lists kept (from the fonts of the text layer); scanned pages come out as plain paragraphs.
    static func markdown(_ doc: PDFDocument, progress: (Double) -> Void, isCancelled: () -> Bool) throws -> String {
        var parts: [String] = []
        for i in 0..<doc.pageCount {
            if isCancelled() { throw CancelledError() }
            guard let page = doc.page(at: i) else { continue }
            if let attr = page.attributedString, attr.string.trimmingCharacters(in: .whitespacesAndNewlines).count >= 3 {
                parts.append(onMain { Markdown.from(reflow(attr)) }.replacingOccurrences(of: #"[ \t]{2,}"#, with: " ", options: .regularExpression))
            } else if let t = try? OCR.text(in: render(page, dpi: 200)) {
                parts.append(t.replacingOccurrences(of: "\n", with: " ") + "\n")
            }
            progress(Double(i + 1) / Double(doc.pageCount))
        }
        return parts.joined(separator: "\n")
    }

    /// PDF text comes line by line; join the lines of one paragraph (same size, no sentence end, a full-width line).
    static func reflow(_ attr: NSAttributedString) -> NSAttributedString {
        let ns = attr.string as NSString
        var lines: [(range: NSRange, size: CGFloat)] = []
        ns.enumerateSubstrings(in: NSRange(location: 0, length: ns.length), options: .byLines) { _, r, _, _ in
            var size: CGFloat = 0
            attr.enumerateAttribute(.font, in: r) { v, _, _ in if let f = v as? NSFont { size = max(size, f.pointSize) } }
            lines.append((r, size))
        }
        let longest = lines.map { $0.range.length }.max() ?? 0
        let out = NSMutableAttributedString()
        for (i, line) in lines.enumerated() {
            let piece = attr.attributedSubstring(from: line.range)
            out.append(piece)
            guard i + 1 < lines.count else { break }
            let next = lines[i + 1], text = piece.string.trimmingCharacters(in: .whitespaces)
            let sameSize = abs(next.size - line.size) < 0.6
            let nextStartsList = ns.substring(with: next.range).range(of: #"^\s*((•|◦|▪|‣|·|–|-)\s|\d{1,2}[.)]?[\s\t])"#, options: .regularExpression) != nil
            // Lines inside a paragraph run (nearly) the full width; a short line ends its paragraph, a list item or a heading.
            let full = Double(line.range.length) > 0.7 * Double(longest)
            if sameSize && full && !nextStartsList {
                out.append(NSAttributedString(string: text.hasSuffix("-") ? "" : " "))
            } else {
                out.append(NSAttributedString(string: "\n"))
            }
        }
        return out
    }

    // MARK: Build, merge, compress

    static func fromImages(_ files: [URL], progress: (Double) -> Void, isCancelled: () -> Bool) throws -> PDFDocument {
        let doc = PDFDocument()
        for (i, f) in files.enumerated() {
            if isCancelled() { throw CancelledError() }
            let cg = try ImagePipeline.load(f)   // EXIF orientation applied
            let image = NSImage(cgImage: cg, size: NSSize(width: cg.width, height: cg.height))
            guard let page = PDFPage(image: image) else { throw PipelineError("Couldn\u{2019}t add \(f.lastPathComponent)") }
            doc.insert(page, at: doc.pageCount)
            progress(Double(i + 1) / Double(files.count))
        }
        return doc
    }

    static func merge(_ files: [URL], isCancelled: () -> Bool) throws -> PDFDocument {
        let out = PDFDocument()
        for f in files {
            if isCancelled() { throw CancelledError() }
            let doc = try open(f)
            for i in 0..<doc.pageCount { if let p = doc.page(at: i) { out.insert(p, at: out.pageCount) } }
        }
        return out
    }

    static func data(_ doc: PDFDocument) throws -> Data {
        let tmp = FileManager.default.temporaryDirectory.appendingPathComponent("sd-\(UUID().uuidString).pdf")
        defer { try? FileManager.default.removeItem(at: tmp) }
        guard doc.write(to: tmp), let d = try? Data(contentsOf: tmp) else { throw PipelineError("Couldn\u{2019}t save the PDF") }
        return d
    }

    // MARK: Compress

    /// (picture resolution in dpi, JPEG quality): from a light squeeze to the hardest one. Text stays text; only pictures change.
    static let ladder: [(dpi: Int, quality: Double)] = [(150, 0.7), (120, 0.6), (96, 0.5), (72, 0.4), (60, 0.3)]

    /// Redraws every page through a Quartz filter that downsamples and re-encodes the pictures (what Preview's
    /// "Reduce File Size" does). Links, bookmarks and form fields are not carried over: the pages are redrawn, not edited.
    static func recompress(_ url: URL, dpi: Int, quality: Double, isCancelled: () -> Bool) throws -> Data {
        guard let src = CGPDFDocument(url as CFURL), src.numberOfPages > 0 else { throw PipelineError("Can\u{2019}t read \(url.lastPathComponent)") }
        if src.isEncrypted && !src.unlockWithPassword("") { throw PipelineError("\(url.lastPathComponent) is password protected") }
        let fm = FileManager.default
        let filterURL = fm.temporaryDirectory.appendingPathComponent("sd-\(UUID().uuidString).qfilter")
        let plist: [String: Any] = [
            "Domains": ["Applications": true, "Printing": true],
            "FilterType": 1, "Name": "ShiftDrop",
            "FilterData": ["ColorSettings": ["ImageSettings": [
                "Compression Quality": quality, "ImageCompression": "ImageJPEGCompress",
                "ImageScaleSettings": ["ImageResolution": dpi, "ImageScaleInterpolate": true, "ImageSizeMax": 2400, "ImageSizeMin": 0],
            ]]],
        ]
        try PropertyListSerialization.data(fromPropertyList: plist, format: .binary, options: 0).write(to: filterURL)
        defer { try? fm.removeItem(at: filterURL) }
        guard let filter = QuartzFilter(url: filterURL) else { throw PipelineError("The compression filter isn\u{2019}t available") }

        let data = NSMutableData()
        guard let consumer = CGDataConsumer(data: data) else { throw PipelineError("Out of memory") }
        func outBox(_ page: CGPDFPage) -> CGRect {
            let b = page.getBoxRect(.cropBox)
            return page.rotationAngle % 180 != 0 ? CGRect(x: 0, y: 0, width: b.height, height: b.width) : CGRect(x: 0, y: 0, width: b.width, height: b.height)
        }
        var first = outBox(src.page(at: 1)!)
        guard let ctx = CGContext(consumer: consumer, mediaBox: &first, nil) else { throw PipelineError("Couldn\u{2019}t start the PDF") }
        filter.apply(to: ctx)
        for i in 1...src.numberOfPages {
            if isCancelled() { throw CancelledError() }
            guard let page = src.page(at: i) else { continue }
            var box = outBox(page)
            ctx.beginPDFPage([kCGPDFContextMediaBox as String: Data(bytes: &box, count: MemoryLayout<CGRect>.size)] as CFDictionary)
            ctx.concatenate(page.getDrawingTransform(.cropBox, rect: box, rotate: 0, preserveAspectRatio: true))
            ctx.drawPDFPage(page)
            ctx.endPDFPage()
        }
        ctx.closePDF()
        return data as Data
    }
}
