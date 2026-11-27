import Foundation

struct CancelledError: Error {}

/// ffmpeg run as a child process (port of src/Ffmpeg.*): local files only, below-normal priority, progress read
/// from `-progress pipe:1`. Every running process is remembered so quitting never leaves one behind.
final class FfmpegEngine {
    static let shared = FfmpegEngine()
    let path: String?

    struct Result { var ok = false; var cancelled = false; var error = "" }

    private let lock = NSLock()
    private var running: [Process] = []

    private init() { path = Self.locate() }

    /// The app bundle's Resources/engines/ffmpeg (ShiftDrop's own LGPL build, see scripts/build-ffmpeg-macos.sh),
    /// or, in debug builds only, $SHIFTDROP_FFMPEG. Never a Homebrew or system ffmpeg: those may be GPL builds.
    static func locate() -> String? {
        var candidates: [String] = []
        if let res = Bundle.main.resourcePath { candidates.append(res + "/engines/ffmpeg") }
        #if DEBUG
        if let env = ProcessInfo.processInfo.environment["SHIFTDROP_FFMPEG"] { candidates.append(env) }   // debug builds only
        #endif
        return candidates.first { FileManager.default.isExecutableFile(atPath: $0) }
    }

    func terminateAll() {
        lock.lock(); let all = running; lock.unlock()
        all.forEach { if $0.isRunning { $0.terminate() } }
    }

    /// Runs ffmpeg and blocks until it exits or `isCancelled` says stop (then the process is terminated).
    /// `progress` gets 0...1 when `durationUs` is known. `log` receives ffmpeg's log lines (the first few hundred).
    func run(_ args: [String], durationUs: Int64 = 0, progress: ((Double) -> Void)? = nil, isCancelled: () -> Bool = { false },
             log: UnsafeMutablePointer<[String]>? = nil) -> Result {
        guard let path else { return Result(error: "FFmpeg isn\u{2019}t installed") }
        let p = Process()
        p.executableURL = URL(fileURLWithPath: path)
        p.arguments = args
        p.qualityOfService = .utility
        let out = Pipe(), err = Pipe()
        p.standardOutput = out; p.standardError = err; p.standardInput = FileHandle.nullDevice
        var lines: [String] = []
        let linesLock = NSLock()
        let group = DispatchGroup()

        func readLines(_ handle: FileHandle, _ each: @escaping (String) -> Void) {
            group.enter()
            DispatchQueue.global(qos: .utility).async {
                var buffer = Data()
                while true {
                    let chunk = handle.availableData
                    if chunk.isEmpty { break }
                    buffer.append(chunk)
                    while let i = buffer.firstIndex(where: { $0 == 10 || $0 == 13 }) {   // \n or \r (ffmpeg stats use \r)
                        let line = String(decoding: buffer[buffer.startIndex..<i], as: UTF8.self)
                        buffer.removeSubrange(buffer.startIndex...i)
                        if !line.isEmpty { each(line) }
                    }
                }
                group.leave()
            }
        }
        readLines(err.fileHandleForReading) { line in linesLock.lock(); if lines.count < 400 { lines.append(line) }; linesLock.unlock() }
        readLines(out.fileHandleForReading) { line in
            if durationUs > 0, let us = Media.parseProgress(line) { progress?(min(1, Double(us) / Double(durationUs))) }
        }
        do { try p.run() } catch { return Result(error: "Couldn\u{2019}t start FFmpeg: \(error.localizedDescription)") }
        lock.lock(); running.append(p); lock.unlock()
        var cancelled = false
        while p.isRunning {
            if !cancelled && isCancelled() { cancelled = true; p.terminate() }
            Thread.sleep(forTimeInterval: 0.05)
        }
        p.waitUntilExit()
        group.wait()
        lock.lock(); running.removeAll { $0 === p }; lock.unlock()
        linesLock.lock(); let captured = lines; linesLock.unlock()
        log?.pointee = captured
        if cancelled { return Result(cancelled: true, error: "cancelled") }
        if p.terminationStatus == 0 { return Result(ok: true) }
        let message = captured.last { $0.range(of: #"rror|nvalid|No such|not supported|Unknown|Unrecognized|Permission"#, options: .regularExpression) != nil } ?? captured.last ?? "ffmpeg failed"
        return Result(error: message)
    }

    private var encoderCache: Set<String>?

    /// Whether this ffmpeg build has the encoder (asked once: `ffmpeg -encoders`).
    func hasEncoder(_ name: String) -> Bool {
        lock.lock(); defer { lock.unlock() }
        if encoderCache == nil {
            var lines: [String] = []
            let sema = DispatchSemaphore(value: 0)
            // `run` takes the lock only briefly, so release it while asking.
            lock.unlock()
            DispatchQueue.global().async { _ = self.run(["-hide_banner", "-encoders"], log: &lines); sema.signal() }
            sema.wait()
            lock.lock()
            encoderCache = Set(lines.compactMap { $0.split(separator: " ", omittingEmptySubsequences: true).dropFirst().first.map(String.init) })
        }
        return encoderCache?.contains(name) ?? false
    }

    /// Streams, size and duration of an input: one quick run that writes nothing (exit 1 is expected).
    func probe(_ input: URL) -> MediaInfo {
        var lines: [String] = []
        _ = run(["-hide_banner", "-nostdin", "-protocol_whitelist", "file", "-i", input.path], log: &lines)
        return Media.parseInfo(lines)
    }
}

// MARK: - media.convert and media.compress

enum MediaJob {
    private static func size(_ url: URL) -> Int64 { Int64((try? url.resourceValues(forKeys: [.fileSizeKey]).fileSize) ?? 0) }

    /// Runs a media recipe on one file: ffmpeg writes into a hidden staging file next to the source, then it is
    /// renamed to the first free "name (suffix).ext" (a rename never replaces an existing file).
    static func run(_ recipe: Recipe, source: URL, progress: @escaping (Double) -> Void, isCancelled: @escaping () -> Bool) throws -> URL {
        let engine = FfmpegEngine.shared
        guard engine.path != nil else { throw PipelineError("FFmpeg is missing from the app (run scripts/build-engines.sh, then build_app.sh)") }
        guard let step = recipe.steps.first else { throw PipelineError("Empty recipe") }
        let info = engine.probe(source)
        if isCancelled() { throw CancelledError() }
        let dir = OutputPath.folder(for: source)
        let staging = dir.appendingPathComponent(".\(source.deletingPathExtension().lastPathComponent).\(UUID().uuidString.prefix(8)).sdpart")
        defer { try? FileManager.default.removeItem(at: staging) }

        var result = FfmpegEngine.Result()
        var ext = "mp4"
        let sourceBytes = size(source)
        let video = !info.videoCodec.isEmpty
        let run = { (args: [String]) in engine.run(args, durationUs: info.durationUs, progress: progress, isCancelled: isCancelled) }

        if step.action == "media.convert" {
            guard let f = Media.format(step.string("format", "mp4")) else { throw PipelineError("Unknown format") }
            ext = f.ext
            var plan = Media.planConversion(f, info)
            let reencodesVideo = video && f.fromVideo && f.id != "gif" && f.id != "mp3" && !plan.copyAll && !plan.copyVideo
            var capShare = 0.0
            // WebM, AVI and WMV aim at a quality and from an H.264 source usually land above it: hold them to the
            // source's bitrate from the first run, so a second encode is the exception.
            if reencodesVideo && ["webm", "avi", "wmv"].contains(f.id) {
                plan.videoBps = Media.capVideoBps(sourceBytes: sourceBytes, durationUs: info.durationUs, share: 0.9, hasAudio: !info.audioCodec.isEmpty)
                if plan.videoBps > 0 { capShare = 0.9 }
            }
            var used = plan
            result = run(Media.ffmpegArgs(input: source.path, inputArgs: Media.inputArgs(plan), outputArgs: Media.outputArgs(f, plan), output: staging.path))
            if !result.ok && !result.cancelled && (plan.copyAll || plan.copyVideo || plan.copyAudio) {
                // Copying a stream failed (one the container can't hold after all): encode everything.
                used = Media.transcodePlan(plan)
                result = run(Media.ffmpegArgs(input: source.path, inputArgs: Media.inputArgs(used), outputArgs: Media.outputArgs(f, used), output: staging.path))
            }
            // A re-encoded video never comes out bigger than its source: run again at the source's own bitrate, then lower.
            if result.ok && video && f.fromVideo && f.id != "gif" && f.id != "mp3" && !used.copyAll && !used.copyVideo {
                for share in [0.9, 0.6] {
                    if !result.ok || sourceBytes <= 0 || size(staging) <= sourceBytes { break }
                    if capShare > 0 && share >= capShare { continue }
                    used.videoBps = Media.capVideoBps(sourceBytes: sourceBytes, durationUs: info.durationUs, share: share, hasAudio: !info.audioCodec.isEmpty)
                    if used.videoBps <= 0 { break }
                    result = run(Media.ffmpegArgs(input: source.path, inputArgs: Media.inputArgs(used), outputArgs: Media.outputArgs(f, used), output: staging.path))
                }
                if result.ok && sourceBytes > 0 && size(staging) > sourceBytes {
                    result.ok = false
                    result.error = "as \(f.label) it would be bigger than the original; Reduce size makes it smaller"
                }
            }
        } else if step.action == "media.compress" {
            ext = video ? "mp4" : "m4a"
            result = try compress(step, source: source, info: info, sourceBytes: sourceBytes, staging: staging, run: run)
        } else {
            throw PipelineError("Unknown step \(step.action)")
        }
        if result.cancelled || isCancelled() { throw CancelledError() }
        guard result.ok else { throw PipelineError(result.error) }
        return try OutputPath.commit(staging: staging, source: source, suffix: recipe.suffix, ext: ext)
    }

    /// A size target (targetMB, or percent of the source) is met by bitrate; if the encoder overshoots it runs again
    /// lower, and a result above the target is never kept. The preset (no target) must at least come out smaller.
    private static func compress(_ step: Step, source: URL, info: MediaInfo, sourceBytes: Int64, staging: URL,
                                 run: ([String]) -> FfmpegEngine.Result) throws -> FfmpegEngine.Result {
        let targetMB = (step.params["targetMB"] as? Double) ?? Double(step.int("targetMB", 0))
        let percent = step.int("percent", 0), maxHeight = step.int("maxHeight", 0)
        let target: Int64 = targetMB > 0 ? Int64(targetMB * 1024 * 1024) : (percent > 0 ? sourceBytes * Int64(percent) / 100 : 0)
        var result = FfmpegEngine.Result()
        let args = { (fit: SizeFit?) in Media.ffmpegArgs(input: source.path, inputArgs: [], outputArgs: Media.compressArgs(info: info, fit: fit, maxHeight: maxHeight), output: staging.path) }

        if target <= 0 {
            result = run(args(nil))
            if result.ok && sourceBytes > 0 && size(staging) >= sourceBytes { result.ok = false; result.error = "already compressed: the result was not smaller" }
            return result
        }
        if sourceBytes > 0 && sourceBytes <= target && targetMB > 0 { result.error = "already under \(Int(targetMB)) MB"; return result }
        guard var fit = Media.planSizeFit(targetBytes: target, info: info) else {
            result.error = info.durationUs <= 0 ? "cannot tell how long this file is" : "the target is too small for a file this long"
            return result
        }
        for _ in 0..<3 {
            result = run(args(fit))
            if !result.ok { return result }
            let got = size(staging)
            if got <= target { return result }
            let k = Double(target) / Double(got) * 0.95   // over: scale every bitrate by how much it missed, with a margin
            fit.videoBps = Int64(Double(fit.videoBps) * k)
            if info.videoCodec.isEmpty { fit.audioBps = Int64(Double(fit.audioBps) * k) }
        }
        result.ok = false; result.error = "could not get under the target size"
        return result
    }
}
