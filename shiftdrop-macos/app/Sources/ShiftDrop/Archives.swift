import Foundation

/// Archives through bsdtar (libarchive, built into macOS): reads ZIP, TAR, GZ, BZ2, XZ, 7z and RAR; writes ZIP, TAR.GZ and 7z.
/// It never extracts outside the target folder (absolute paths and ".." are dropped unless -P is given, which we don't).
enum Archives {
    enum Format {
        case zip, targz, sevenZip
        var ext: String { switch self { case .zip: return "zip"; case .targz: return "tar.gz"; case .sevenZip: return "7z" } }
        var flags: [String] { switch self { case .zip: return ["--format", "zip"]; case .targz: return ["-z"]; case .sevenZip: return ["--format", "7zip"] } }
    }

    @discardableResult
    private static func tar(_ args: [String], isCancelled: () -> Bool = { false }) throws -> String {
        let p = Process()
        p.executableURL = URL(fileURLWithPath: "/usr/bin/tar")
        p.arguments = args
        let err = Pipe()
        p.standardError = err; p.standardOutput = FileHandle.nullDevice
        try p.run()
        var data = Data()
        let reader = DispatchQueue.global()
        let sema = DispatchSemaphore(value: 0)
        reader.async { data = err.fileHandleForReading.readDataToEndOfFile(); sema.signal() }
        while p.isRunning { if isCancelled() { p.terminate() }; Thread.sleep(forTimeInterval: 0.05) }
        sema.wait()
        if isCancelled() { throw CancelledError() }
        let text = String(decoding: data, as: UTF8.self).trimmingCharacters(in: .whitespacesAndNewlines)
        guard p.terminationStatus == 0 else { throw PipelineError(text.isEmpty ? "tar failed" : String(text.split(separator: "\n").last ?? "tar failed")) }
        return text
    }

    /// Unpacks `archive` into the existing folder `dir`.
    static func extract(_ archive: URL, to dir: URL, isCancelled: () -> Bool = { false }) throws {
        do {
            try tar(["-xf", archive.path, "-C", dir.path], isCancelled: isCancelled)
        } catch let error as PipelineError {
            // A bare .gz or .bz2 holds one compressed file, not a tar: unpack it under the archive's own name.
            let ext = archive.pathExtension.lowercased()
            guard ["gz", "bz2"].contains(ext) else { throw error }
            let tool = ext == "gz" ? "/usr/bin/gzip" : "/usr/bin/bzip2"
            let out = dir.appendingPathComponent(archive.deletingPathExtension().lastPathComponent)
            FileManager.default.createFile(atPath: out.path, contents: nil)
            let p = Process()
            p.executableURL = URL(fileURLWithPath: tool)
            p.arguments = ["-dc", archive.path]
            p.standardOutput = try FileHandle(forWritingTo: out)
            p.standardError = FileHandle.nullDevice
            try p.run()
            while p.isRunning { if isCancelled() { p.terminate() }; Thread.sleep(forTimeInterval: 0.05) }
            if isCancelled() { throw CancelledError() }
            guard p.terminationStatus == 0 else { throw PipelineError("\(archive.lastPathComponent) isn\u{2019}t a valid \(ext) file") }
        }
    }

    /// Packs `items` (files and folders, at the top level of the archive) into `output`.
    static func create(_ items: [URL], format: Format, output: URL, isCancelled: () -> Bool = { false }) throws {
        var args = ["--exclude", ".DS_Store", "--no-mac-metadata"] + format.flags + ["-cf", output.path]
        for item in items { args += ["-C", item.deletingLastPathComponent().path, item.lastPathComponent] }
        try tar(args, isCancelled: isCancelled)
    }

    /// Packs the contents of a folder (so the archive has no wrapping folder).
    static func createFromContents(of dir: URL, format: Format, output: URL, isCancelled: () -> Bool = { false }) throws {
        let items = (try? FileManager.default.contentsOfDirectory(at: dir, includingPropertiesForKeys: nil)) ?? []
        guard !items.isEmpty else { throw PipelineError("The archive is empty") }
        try create(items, format: format, output: output, isCancelled: isCancelled)
    }
}
