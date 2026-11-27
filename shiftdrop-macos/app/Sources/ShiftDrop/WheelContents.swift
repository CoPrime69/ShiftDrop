import Foundation

// Which slices each wheel shows for the dragged files (port of the idea in src/WheelContents.* and the
// per-kind filtering of the Windows wheel): a section shows only what fits, and sections with nothing
// to offer are left out, so a folder of text files gets just File actions.
enum WheelContents {
    static let sections: [(key: String, title: String, icon: String)] = [
        ("convert", "Convert", "arrow.triangle.2.circlepath"),
        ("tools", "Tools", "slider.horizontal.3"),
        ("reduce", "Reduce size", "arrow.down.right.and.arrow.up.left"),
        ("files", "File actions", "doc.on.doc"),
    ]

    static var recipes: [Recipe] {
        var list = RecipeStore.all
        for user in RecipeStore.loadUser() {
            if let i = list.firstIndex(where: { $0.id == user.id }) { list[i] = user } else { list.append(user) }
        }
        return list
    }

    private static func allImages(_ files: [URL]) -> Bool { !files.isEmpty && files.allSatisfy(ImagePipeline.isImage) }

    /// The kind of media every file is (video or audio), or .none when they are mixed or not media.
    private static func mediaKind(_ files: [URL]) -> MediaKind {
        let kinds = Set(files.map { Media.kind(of: $0) == .video ? 1 : (Media.kind(of: $0) == .audio ? 2 : 0) })
        return kinds == [1] ? .video : (kinds == [2] ? .audio : .none)
    }

    private static func applies(_ r: Recipe, to files: [URL]) -> Bool {
        guard let first = r.steps.first else { return false }
        let kind = FileKinds.common(files)
        let fmt = first.string("format", "")
        // Converting to what every file already is makes no sense.
        func isSame(_ aliases: [String]) -> Bool { files.allSatisfy { aliases.contains($0.pathExtension.lowercased()) } }
        switch first.action {
        case let a where a.hasPrefix("file."): return true
        case "archive.zip": return true
        case let a where a.hasPrefix("image."): return allImages(files)
        case "media.convert":
            let k = mediaKind(files)
            guard k != .none, let f = Media.format(fmt), Media.accepts(f, k) else { return false }
            return !isSame(f.aliases)
        case "media.compress": return mediaKind(files) != .none
        case "pdf.to_images", "pdf.to_text", "pdf.to_markdown", "pdf.to_docx", "pdf.to_html", "pdf.to_epub", "pdf.compress": return kind == .pdf
        case "pdf.merge": return kind == .pdf && files.count >= 2
        case "images.to_pdf": return kind == .image
        case "ocr.copy_text": return kind == .image || kind == .pdf
        case "docs.convert":
            let same: [String: [String]] = ["docx": ["docx"], "pdf": [], "md": ["md", "markdown"], "html": ["html", "htm"], "epub": [], "txt": ["txt"]]
            return kind == .document && !isSame(same[fmt] ?? [])
        case "subs.convert": return kind == .subtitles && !isSame([fmt])
        case "sheets.convert": return kind == .sheet && !isSame([fmt])
        case "archive.extract": return kind == .archive
        case "archive.repack":
            let same: [String: [String]] = ["zip": ["zip"], "targz": ["tgz"], "7z": ["7z"]]
            return kind == .archive && !isSame(same[fmt] ?? []) && !files.contains { $0.lastPathComponent.lowercased().hasSuffix(fmt == "targz" ? ".tar.gz" : ".\(fmt)") }
        case "archive.pack_folder": return kind == .folder
        default: return false
        }
    }

    static func rings(for files: [URL]) -> [WheelRing] {
        let formats = Set(files.compactMap { ImageFormat(path: $0)?.rawValue })
        let total = files.reduce(0) { $0 + ((try? $1.resourceValues(forKeys: [.fileSizeKey]).fileSize) ?? 0) }
        var out: [WheelRing] = []
        for s in sections {
            var slices: [WheelSlice] = []
            for r in recipes where r.ring == s.key && applies(r, to: files) {
                // Converting to the format every file already has makes no sense.
                if s.key == "convert", r.id.hasPrefix("to-"), formats.count == 1, formats.first == String(r.id.dropFirst(3)) { continue }
                var slice = WheelSlice(id: r.id, label: r.label, detail: r.detail, icon: r.icon)
                if r.steps.first?.action.hasPrefix("media.") == true, FfmpegEngine.shared.path == nil {
                    slice.enabled = false
                    slice.detail = "FFmpeg is missing from this build"   // dimmed, with the reason in the centre
                }
                if s.key == "reduce", let p = r.steps.first?.params["percent"] as? Int, total > 0 {
                    slice.headline = ByteCountFormatter.string(fromByteCount: Int64(total * p / 100), countStyle: .file)
                }
                slices.append(slice)
            }
            if !slices.isEmpty { out.append(WheelRing(key: s.key, title: s.title, icon: s.icon, slices: slices)) }
        }
        return out
    }
}
