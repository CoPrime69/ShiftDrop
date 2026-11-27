import Foundation

enum FileKind { case image, video, audio, pdf, document, sheet, subtitles, archive, folder, other }

enum FileKinds {
    static let documentExts: Set<String> = ["docx", "doc", "rtf", "rtfd", "html", "htm", "txt", "md", "markdown", "odt", "webarchive"]
    static let sheetExts: Set<String> = ["csv", "tsv"]
    static let subtitleExts: Set<String> = ["srt", "vtt"]
    static let archiveExts: Set<String> = ["zip", "tar", "tgz", "gz", "7z", "rar", "bz2", "tbz2", "xz", "txz"]

    static func kind(of url: URL) -> FileKind {
        let e = url.pathExtension.lowercased()
        if ImagePipeline.isImage(url) { return .image }
        switch Media.kind(of: url) { case .video: return .video; case .audio: return .audio; case .none: break }
        if e == "pdf" { return .pdf }
        if documentExts.contains(e) { return .document }   // rtfd is a folder, so check before the folder test
        if sheetExts.contains(e) { return .sheet }
        if subtitleExts.contains(e) { return .subtitles }
        if archiveExts.contains(e) { return .archive }
        if (try? url.resourceValues(forKeys: [.isDirectoryKey]).isDirectory) == true { return .folder }
        return .other
    }

    /// The one kind every file has, or nil when they are mixed.
    static func common(_ files: [URL]) -> FileKind? {
        guard let first = files.first.map(kind(of:)) else { return nil }
        return files.allSatisfy { kind(of: $0) == first } ? first : nil
    }

    /// "photo.tar.gz" -> "photo", "notes.md" -> "notes".
    static func stem(_ url: URL) -> String {
        let name = url.lastPathComponent
        // A folder has no extension ("My.Project"), except document packages like .rtfd.
        if (try? url.resourceValues(forKeys: [.isDirectoryKey]).isDirectory) == true, !documentExts.contains(url.pathExtension.lowercased()) { return name }
        for double in [".tar.gz", ".tar.bz2", ".tar.xz"] where name.lowercased().hasSuffix(double) { return String(name.dropLast(double.count)) }
        return url.deletingPathExtension().lastPathComponent
    }
}
