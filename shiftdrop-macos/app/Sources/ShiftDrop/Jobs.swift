import AppKit

// Output naming ("photo (converted).png", then "(converted 2)"; an existing file is never replaced),
// the job runner, and History with Undo. Ports of OutputPath, JobQueue/PipelineRunner and HistoryStore.

enum OutputPath {
    /// Folder for outputs next to `source`; the Desktop when that folder can't be written to.
    static func folder(for source: URL) -> URL {
        let dir = source.deletingLastPathComponent()
        if FileManager.default.isWritableFile(atPath: dir.path) { return dir }
        return FileManager.default.urls(for: .desktopDirectory, in: .userDomainMask)[0]
    }

    static func candidate(dir: URL, stem: String, suffix: String, ext: String, n: Int) -> URL {
        let tag = n <= 1 ? suffix : "\(suffix) \(n)"
        let name = "\(stem) (\(tag))" + (ext.isEmpty ? "" : ".\(ext)")
        return dir.appendingPathComponent(name)
    }

    /// Writes `data` to the first free name. `.withoutOverwriting` makes the write itself refuse an existing file,
    /// so two jobs can never pick the same name.
    static func write(_ data: Data, source: URL, suffix: String, ext: String) throws -> URL {
        let dir = folder(for: source), stem = FileKinds.stem(source)
        for n in 1...999 {
            let url = candidate(dir: dir, stem: stem, suffix: suffix, ext: ext, n: n)
            do { try data.write(to: url, options: .withoutOverwriting); return url }
            catch let e as CocoaError where e.code == .fileWriteFileExists { continue }
        }
        throw PipelineError("No free file name for \(stem)")
    }

    /// Renames a finished staging file to the first free name. moveItem refuses an existing destination, so nothing is replaced.
    static func commit(staging: URL, source: URL, suffix: String, ext: String) throws -> URL {
        let dir = folder(for: source), stem = FileKinds.stem(source)
        for n in 1...999 {
            let url = candidate(dir: dir, stem: stem, suffix: suffix, ext: ext, n: n)
            do { try FileManager.default.moveItem(at: staging, to: url); return url }
            catch let e as CocoaError where e.code == .fileWriteFileExists { continue }
        }
        throw PipelineError("No free file name for \(stem)")
    }

    static func reserve(source: URL, suffix: String, ext: String) -> URL {
        let dir = folder(for: source), stem = FileKinds.stem(source)
        for n in 1...999 {
            let url = candidate(dir: dir, stem: stem, suffix: suffix, ext: ext, n: n)
            if !FileManager.default.fileExists(atPath: url.path) { return url }
        }
        return candidate(dir: dir, stem: stem, suffix: suffix, ext: ext, n: 1000)
    }
}

struct JobResult {
    enum Outcome { case success, partial, failure }
    var outcome: Outcome
    var title: String
    var subtitle: String
    var outputs: [URL]
}

final class JobRunner {
    private let queue = DispatchQueue(label: "shiftdrop.jobs", qos: .userInitiated)
    private var cancelled = false

    func cancel() { cancelled = true }

    static func plural(_ n: Int, _ word: String) -> String { n == 1 ? "1 \(word)" : "\(n) \(word)s" }

    /// Recipes run on a background queue. Most take one file at a time; batch actions (merge, one ZIP, pictures to one
    /// PDF) take them all. `progress` gets "groups done" (fractional while one is being worked on) on the main queue.
    func run(recipe: Recipe, files: [URL], progress: @escaping (Double) -> Void, done: @escaping (JobResult) -> Void) {
        cancelled = false
        queue.async { [self] in
            var outputs: [URL] = [], failures: [String] = [], messages: [String] = [], titles: [String] = []
            let step = recipe.steps.first ?? Step(action: "", params: [:])
            let groups: [[URL]] = Actions.isBatch(step.action) ? [files] : files.map { [$0] }
            for (i, group) in groups.enumerated() {
                if cancelled { break }
                let report: (Double) -> Void = { f in DispatchQueue.main.async { progress(Double(i) + f) } }
                do {
                    let file = group[0]
                    if step.action.hasPrefix("media.") {
                        guard Media.kind(of: file) != .none else { throw PipelineError("\(file.lastPathComponent) isn\u{2019}t a video or audio file") }
                        outputs.append(try MediaJob.run(recipe, source: file, progress: report, isCancelled: { [self] in cancelled }))
                    } else if step.action.hasPrefix("image.") {
                        guard ImagePipeline.isImage(file) else { throw PipelineError("\(file.lastPathComponent) isn\u{2019}t a picture") }
                        let (data, ext) = try ImagePipeline.run(recipe, input: file)
                        outputs.append(try OutputPath.write(data, source: file, suffix: recipe.suffix, ext: ext))
                    } else {
                        let r = try Actions.run(recipe, step, group, progress: report, isCancelled: { [self] in cancelled })
                        outputs += r.outputs
                        if let m = r.message { messages.append(m) }
                        if let t = r.title { titles.append(t) }
                    }
                } catch is CancelledError {
                    cancelled = true
                } catch {
                    failures.append("\(group[0].lastPathComponent): \(error.localizedDescription)")
                }
                DispatchQueue.main.async { progress(Double(i + 1)) }
            }
            var result = Self.summarize(recipe: recipe, total: groups.count, outputs: outputs, failures: failures, cancelled: cancelled)
            if failures.isEmpty && !cancelled {
                if outputs.isEmpty, let m = messages.first { result = JobResult(outcome: .success, title: m, subtitle: "", outputs: []) }
                else if titles.count == 1 && groups.count == 1 { result.title = titles[0] }
            }
            DispatchQueue.main.async { done(result) }
        }
    }

    static func summarize(recipe: Recipe, total: Int, outputs: [URL], failures: [String], cancelled: Bool) -> JobResult {
        if cancelled { return JobResult(outcome: .partial, title: "Cancelled", subtitle: outputs.isEmpty ? "" : plural(outputs.count, "file") + " done first", outputs: outputs) }
        let verb: String
        switch recipe.suffix {
        case "converted": verb = "converted to \(recipe.label)"
        case "edited": verb = "edited"
        case "compressed": verb = "compressed"
        case "reduced": verb = "reduced"
        case "merged": verb = "merged"
        case "extracted": verb = "extracted"
        case "zipped": verb = "zipped"
        case "archived": verb = "packed"
        case "searchable": verb = "made searchable"
        default: verb = "done"
        }
        if failures.isEmpty { return JobResult(outcome: .success, title: "\(plural(outputs.count, "file")) \(verb)", subtitle: outputs.count == 1 ? outputs[0].lastPathComponent : "", outputs: outputs) }
        if outputs.isEmpty { return JobResult(outcome: .failure, title: total == 1 ? "Couldn\u{2019}t do it" : "None of \(total) files worked", subtitle: failures[0], outputs: []) }
        return JobResult(outcome: .partial, title: "\(outputs.count) of \(total) \(verb)", subtitle: failures[0], outputs: outputs)
    }
}

// MARK: - File actions (file.*)

enum FileActions {
    static func run(_ step: Step, recipe: Recipe, files: [URL]) -> JobResult {
        switch step.action {
        case "file.float":
            FloatShelf.shared.add(files)
            FloatShelf.shared.show()
            return JobResult(outcome: .success, title: "Added to Float shelf", subtitle: JobRunner.plural(files.count, "item"), outputs: files)
        case "file.copy":
            NSPasteboard.general.clearContents()
            NSPasteboard.general.writeObjects(files as [NSURL])
            return JobResult(outcome: .success, title: "Copied \(JobRunner.plural(files.count, "file"))", subtitle: "Paste in Finder", outputs: [])
        case "file.copy_path":
            NSPasteboard.general.clearContents()
            NSPasteboard.general.setString(files.map(\.path).joined(separator: "\n"), forType: .string)
            return JobResult(outcome: .success, title: files.count == 1 ? "Path copied" : "\(files.count) paths copied", subtitle: "", outputs: [])
        case "file.terminal":
            let dir = files[0].hasDirectoryPath ? files[0] : files[0].deletingLastPathComponent()
            let p = Process(); p.executableURL = URL(fileURLWithPath: "/usr/bin/open"); p.arguments = ["-a", "Terminal", dir.path]
            try? p.run()
            return JobResult(outcome: .success, title: "Terminal opened", subtitle: dir.lastPathComponent, outputs: [])
        case "file.reveal":
            NSWorkspace.shared.activateFileViewerSelecting(files)
            return JobResult(outcome: .success, title: "Shown in Finder", subtitle: "", outputs: [])
        case "file.duplicate":
            var outputs: [URL] = [], failed: [String] = []
            for f in files {
                let dest = OutputPath.reserve(source: f, suffix: recipe.suffix, ext: f.pathExtension)
                do { try FileManager.default.copyItem(at: f, to: dest); outputs.append(dest) } catch { failed.append(error.localizedDescription) }
            }
            return JobRunner.summarize(recipe: Recipe(id: recipe.id, label: recipe.label, detail: "", ring: "", suffix: "duplicated", icon: nil, steps: []),
                                       total: files.count, outputs: outputs, failures: failed, cancelled: false)
        case "file.move":
            let panel = NSOpenPanel()
            panel.canChooseDirectories = true; panel.canChooseFiles = false; panel.canCreateDirectories = true; panel.prompt = "Move here"
            NSApp.activate(ignoringOtherApps: true)
            guard panel.runModal() == .OK, let dir = panel.url else { return JobResult(outcome: .partial, title: "Cancelled", subtitle: "", outputs: []) }
            var moved: [URL] = [], failed: [String] = []
            for f in files {
                let dest = dir.appendingPathComponent(f.lastPathComponent)
                if FileManager.default.fileExists(atPath: dest.path) { failed.append("\(f.lastPathComponent) already exists there"); continue }
                do { try FileManager.default.moveItem(at: f, to: dest); moved.append(dest) } catch { failed.append(error.localizedDescription) }
            }
            return JobRunner.summarize(recipe: Recipe(id: recipe.id, label: recipe.label, detail: "", ring: "", suffix: "moved", icon: nil, steps: []),
                                       total: files.count, outputs: moved, failures: failed, cancelled: false)
        default:
            return JobResult(outcome: .failure, title: "Not available yet", subtitle: step.action, outputs: [])
        }
    }
}

// MARK: - History and Undo

struct HistoryEntry: Codable {
    struct Output: Codable { var path: String; var size: Int; var modified: Double }
    var id: String
    var date: Date
    var title: String
    var outputs: [Output]
    var undone: Bool
}

final class HistoryStore {
    private(set) var entries: [HistoryEntry] = []
    private let url: URL = {
        let dir = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0].appendingPathComponent("ShiftDrop")
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        return dir.appendingPathComponent("history.json")
    }()

    init() {
        if let data = try? Data(contentsOf: url) {
            let d = JSONDecoder(); d.dateDecodingStrategy = .iso8601
            entries = (try? d.decode([HistoryEntry].self, from: data)) ?? []
        }
    }

    private func save() {
        let e = JSONEncoder(); e.dateEncodingStrategy = .iso8601
        if let data = try? e.encode(Array(entries.suffix(200))) { try? data.write(to: url, options: .atomic) }
    }

    func record(title: String, outputs: [URL]) {
        guard !outputs.isEmpty else { return }
        let outs = outputs.map { u -> HistoryEntry.Output in
            let v = try? u.resourceValues(forKeys: [.fileSizeKey, .contentModificationDateKey])
            return .init(path: u.path, size: v?.fileSize ?? 0, modified: v?.contentModificationDate?.timeIntervalSince1970 ?? 0)
        }
        entries.append(HistoryEntry(id: UUID().uuidString, date: Date(), title: title, outputs: outs, undone: false))
        save()
    }

    var lastUndoable: HistoryEntry? { entries.last { !$0.undone } }

    /// Sends the outputs to the Trash. Refuses when one was edited since, so Undo never throws away your work.
    func undo(_ id: String) -> String {
        guard let i = entries.firstIndex(where: { $0.id == id }) else { return "Nothing to undo" }
        var urls: [URL] = []
        for o in entries[i].outputs {
            let u = URL(fileURLWithPath: o.path)
            guard let v = try? u.resourceValues(forKeys: [.fileSizeKey, .contentModificationDateKey]) else { continue }   // already gone
            if v.fileSize != o.size || abs((v.contentModificationDate?.timeIntervalSince1970 ?? 0) - o.modified) > 1 {
                return "Not undone: \(u.lastPathComponent) was changed since"
            }
            urls.append(u)
        }
        for u in urls { try? FileManager.default.trashItem(at: u, resultingItemURL: nil) }
        entries[i].undone = true
        save()
        return "Undone: \(entries[i].title)"
    }
}
