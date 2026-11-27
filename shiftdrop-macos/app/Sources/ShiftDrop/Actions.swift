import AppKit
import PDFKit

struct ActionOutput {
    var outputs: [URL] = []
    var message: String?    // a result with no file: "Text copied"
    var title: String?      // replaces the default "1 file converted to PDF"
}

/// PDF, document, subtitle, sheet, archive and OCR actions (the non-image, non-media half of the recipes).
enum Actions {
    /// Actions that turn all the dropped files into one output.
    static let batch: Set<String> = ["pdf.merge", "images.to_pdf", "archive.zip",
                                     "file.float", "file.copy", "file.copy_path", "file.reveal", "file.move", "file.terminal"]
    static let prefixes = ["pdf.", "docs.", "subs.", "sheets.", "archive.", "ocr.", "images.", "file."]

    static func handles(_ action: String) -> Bool { prefixes.contains { action.hasPrefix($0) } }
    static func isBatch(_ action: String) -> Bool { batch.contains(action) }

    // MARK: Staging: write somewhere hidden, then rename to the first free "name (suffix).ext"

    private static func staging(near source: URL) -> URL {
        OutputPath.folder(for: source).appendingPathComponent(".\(UUID().uuidString.prefix(8)).sdpart")
    }

    private static func commit(data: Data, source: URL, suffix: String, ext: String) throws -> URL {
        let s = staging(near: source)
        defer { try? FileManager.default.removeItem(at: s) }
        try data.write(to: s)
        return try OutputPath.commit(staging: s, source: source, suffix: suffix, ext: ext)
    }

    private static func commit(text: String, source: URL, suffix: String, ext: String) throws -> URL {
        try commit(data: Data(text.utf8), source: source, suffix: suffix, ext: ext)
    }

    /// `build` fills a staging file (or folder); it is then renamed into place.
    private static func commit(source: URL, suffix: String, ext: String, build: (URL) throws -> Void) throws -> URL {
        let s = staging(near: source)
        defer { try? FileManager.default.removeItem(at: s) }
        try build(s)
        return try OutputPath.commit(staging: s, source: source, suffix: suffix, ext: ext)
    }

    // MARK: Run

    static func run(_ recipe: Recipe, _ step: Step, _ files: [URL], progress: @escaping (Double) -> Void, isCancelled: @escaping () -> Bool) throws -> ActionOutput {
        guard let src = files.first else { throw PipelineError("No files") }
        let suffix = recipe.suffix, stem = FileKinds.stem(src)
        let format = step.string("format", "")
        func done(_ url: URL) -> ActionOutput { ActionOutput(outputs: [url]) }

        switch step.action {
        // PDF -> pictures, text, Markdown, DOCX, HTML, EPUB
        case "pdf.to_images":
            let doc = try Pdf.open(src)
            let fmt: ImageFormat = format == "png" ? .png : .jpg
            if doc.pageCount == 1, let page = doc.page(at: 0) {
                return done(try commit(data: try ImagePipeline.encode(try Pdf.render(page, dpi: 300), as: fmt, quality: 90), source: src, suffix: suffix, ext: fmt.ext))
            }
            return done(try commit(source: src, suffix: suffix, ext: "") { dir in
                try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
                _ = try Pdf.writePages(doc, format: fmt, stem: stem, into: dir, progress: progress, isCancelled: isCancelled)
            })
        case "pdf.to_text":
            let text = try Pdf.text(try Pdf.open(src), progress: progress, isCancelled: isCancelled)
            return done(try commit(text: text + "\n", source: src, suffix: suffix, ext: "txt"))
        case "pdf.to_markdown", "pdf.to_docx", "pdf.to_html", "pdf.to_epub":
            let doc = Documents.Doc(title: stem, markdown: try Pdf.markdown(try Pdf.open(src), progress: progress, isCancelled: isCancelled))
            return done(try writeDoc(doc, format: String(step.action.dropFirst("pdf.to_".count)).replacingOccurrences(of: "markdown", with: "md"), source: src, suffix: suffix))

        // pictures -> one PDF, PDF merge and compress
        case "images.to_pdf":
            let doc = try Pdf.fromImages(files, progress: progress, isCancelled: isCancelled)
            let url = try commit(data: try Pdf.data(doc), source: src, suffix: suffix, ext: "pdf")
            return ActionOutput(outputs: [url], title: files.count == 1 ? "Converted to PDF" : "\(files.count) pictures in one PDF")
        case "pdf.merge":
            guard files.count >= 2 else { throw PipelineError("Drop two or more PDFs to merge them") }
            let url = try commit(data: try Pdf.data(try Pdf.merge(files, isCancelled: isCancelled)), source: src, suffix: suffix, ext: "pdf")
            return ActionOutput(outputs: [url], title: "\(files.count) PDFs merged")
        case "pdf.compress":
            // A target (percent of the source) walks down the ladder until it fits; "as far as it goes" keeps the smallest.
            let original = (try? src.resourceValues(forKeys: [.fileSizeKey]).fileSize) ?? Int.max
            let percent = step.int("percent", 0)
            let target = percent > 0 ? original / 100 * percent : 0
            var best: Data?
            for (i, level) in (percent > 0 ? Pdf.ladder : [Pdf.ladder[0]]).enumerated() {
                let d = try Pdf.recompress(src, dpi: level.dpi, quality: level.quality, isCancelled: isCancelled)
                if best == nil || d.count < best!.count { best = d }
                progress(Double(i + 1) / Double(Pdf.ladder.count))
                if target > 0 && d.count <= target { break }
                if target == 0 { break }
            }
            guard let result = best, result.count < original else { throw PipelineError("Already as small as it goes") }
            return done(try commit(data: result, source: src, suffix: suffix, ext: "pdf"))

        // text from a picture or a PDF, to the clipboard
        case "ocr.copy_text":
            let text: String
            if FileKinds.kind(of: src) == .pdf { text = try Pdf.text(try Pdf.open(src), progress: progress, isCancelled: isCancelled) }
            else { text = try OCR.text(in: ImagePipeline.load(src)) }
            let trimmed = text.trimmingCharacters(in: .whitespacesAndNewlines)
            guard !trimmed.isEmpty else { throw PipelineError("No text found") }
            onMain { NSPasteboard.general.clearContents(); NSPasteboard.general.setString(trimmed, forType: .string) }
            return ActionOutput(message: "Text copied: \(trimmed.split { $0.isWhitespace }.count) words")

        // documents
        case "docs.convert":
            return done(try writeDoc(try Documents.read(src), format: format, source: src, suffix: suffix))

        // subtitles and sheets
        case "subs.convert":
            let cues = Subtitles.parse(try String(contentsOf: src, encoding: .utf8))
            guard !cues.isEmpty else { throw PipelineError("No subtitles found in \(src.lastPathComponent)") }
            let text = format == "vtt" ? Subtitles.vtt(cues) : (format == "txt" ? Subtitles.txt(cues) : Subtitles.srt(cues))
            return done(try commit(text: text, source: src, suffix: suffix, ext: format))
        case "sheets.convert":
            let from: Character = src.pathExtension.lowercased() == "tsv" ? "\t" : ","
            let to: Character = format == "tsv" ? "\t" : ","
            var enc = String.Encoding.utf8
            let rows = Sheets.parse(try String(contentsOf: src, usedEncoding: &enc), delimiter: from)
            return done(try commit(text: Sheets.write(rows, delimiter: to), source: src, suffix: suffix, ext: format))

        // archives and folders
        case "archive.extract":
            return done(try commit(source: src, suffix: suffix, ext: "") { dir in
                try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
                try Archives.extract(src, to: dir, isCancelled: isCancelled)
            })
        case "archive.repack":
            let fmt = archiveFormat(format)
            let tmp = FileManager.default.temporaryDirectory.appendingPathComponent("sd-unpack-\(UUID().uuidString)")
            try FileManager.default.createDirectory(at: tmp, withIntermediateDirectories: true)
            defer { try? FileManager.default.removeItem(at: tmp) }
            try Archives.extract(src, to: tmp, isCancelled: isCancelled)
            return done(try commit(source: src, suffix: suffix, ext: fmt.ext) { try Archives.createFromContents(of: tmp, format: fmt, output: $0, isCancelled: isCancelled) })
        case "archive.pack_folder":
            let fmt = archiveFormat(format)
            return done(try commit(source: src, suffix: suffix, ext: fmt.ext) { try Archives.create([src], format: fmt, output: $0, isCancelled: isCancelled) })
        case "archive.zip":   // one ZIP of everything dropped
            let named = files.count == 1 ? src : src.deletingLastPathComponent().appendingPathComponent("Archive")
            let url = try commit(source: named, suffix: suffix, ext: "zip") { try Archives.create(files, format: .zip, output: $0, isCancelled: isCancelled) }
            return ActionOutput(outputs: [url], title: "\(JobRunner.plural(files.count, "item")) zipped")
        case _ where step.action.hasPrefix("file."):
            let r = FileActions.run(step, recipe: recipe, files: files)
            return ActionOutput(outputs: r.outputs, message: r.title)
        default:
            throw PipelineError("Not available yet: \(step.action)")
        }
    }

    private static func archiveFormat(_ id: String) -> Archives.Format { id == "targz" ? .targz : (id == "7z" ? .sevenZip : .zip) }

    private static func writeDoc(_ doc: Documents.Doc, format: String, source: URL, suffix: String) throws -> URL {
        switch format {
        case "docx": return try commit(data: try Documents.docx(doc), source: source, suffix: suffix, ext: "docx")
        case "html": return try commit(data: Documents.html(doc), source: source, suffix: suffix, ext: "html")
        case "md": return try commit(data: Documents.md(doc), source: source, suffix: suffix, ext: "md")
        case "txt": return try commit(data: Documents.txt(doc), source: source, suffix: suffix, ext: "txt")
        case "pdf": return try commit(source: source, suffix: suffix, ext: "pdf") { try Documents.pdf(doc, to: $0) }
        case "epub": return try commit(source: source, suffix: suffix, ext: "epub") { try Documents.epub(doc, to: $0) }
        default: throw PipelineError("Unknown format \(format)")
        }
    }
}
