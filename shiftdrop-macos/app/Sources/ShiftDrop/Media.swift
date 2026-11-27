import Foundation

// Audio and video: which files are media, what each output format is, and the ffmpeg arguments that produce it.
// A port of src/Media.{h,cpp}. The one real difference is the H.264 encoder: Apple Silicon has hardware H.264
// (VideoToolbox) in place of NVENC/QSV/AMF/Media Foundation.

enum MediaKind { case none, video, audio }

struct MediaFormat {
    let id: String, label: String, ext: String
    let aliases: [String]        // source extensions that already are this format
    let detail: String
    let fromVideo: Bool, fromAudio: Bool
    let remux: Bool              // copy the streams (MKV takes almost anything)
}

struct MediaInfo {
    var durationUs: Int64 = -1
    var videoCodec = "", audioCodec = ""
    var width = 0, height = 0
    var fps = 0.0
}

struct MediaPlan {
    var copyAll = false, copyVideo = false, copyAudio = false
    var hevc = false
    var hwDecode = false
    var videoBps: Int64 = 0      // > 0: encode the video at this average bitrate instead of a quality
}

struct SizeFit { var videoBps: Int64 = 0, audioBps: Int64 = 0, height = 0 }

enum Media {
    static let videoExtensions: Set<String> = ["mp4", "m4v", "mov", "qt", "mkv", "webm", "avi", "wmv", "asf", "flv", "mpg", "mpeg", "mpe", "vob", "ts", "mts", "m2ts", "3gp", "3g2", "ogv", "mxf", "divx"]
    static let audioExtensions: Set<String> = ["mp3", "m4a", "m4b", "aac", "wav", "wave", "flac", "ogg", "oga", "opus", "aif", "aiff", "aifc", "wma", "alac", "ac3", "amr", "ape", "wv", "mka", "caf"]

    static func kind(of url: URL) -> MediaKind {
        let e = url.pathExtension.lowercased()
        return videoExtensions.contains(e) ? .video : (audioExtensions.contains(e) ? .audio : .none)
    }

    /// Wheel order: video formats, then audio. MP3 is offered for both (audio from a video).
    static let formats: [MediaFormat] = [
        .init(id: "mp4", label: "MP4", ext: "mp4", aliases: ["mp4", "m4v"], detail: "H.264 video, AAC audio", fromVideo: true, fromAudio: false, remux: false),
        .init(id: "mov", label: "MOV", ext: "mov", aliases: ["mov", "qt"], detail: "QuickTime, H.264 and AAC", fromVideo: true, fromAudio: false, remux: false),
        .init(id: "mkv", label: "MKV", ext: "mkv", aliases: ["mkv"], detail: "Matroska, stream copy", fromVideo: true, fromAudio: false, remux: true),
        .init(id: "webm", label: "WebM", ext: "webm", aliases: ["webm"], detail: "VP9 video, Opus audio", fromVideo: true, fromAudio: false, remux: false),
        .init(id: "avi", label: "AVI", ext: "avi", aliases: ["avi", "divx"], detail: "MPEG-4 video, MP3 audio", fromVideo: true, fromAudio: false, remux: false),
        .init(id: "wmv", label: "WMV", ext: "wmv", aliases: ["wmv", "asf"], detail: "Windows Media Video", fromVideo: true, fromAudio: false, remux: false),
        .init(id: "gif", label: "GIF", ext: "gif", aliases: ["gif"], detail: "Animated GIF, 12 fps", fromVideo: true, fromAudio: false, remux: false),
        .init(id: "mp3", label: "MP3", ext: "mp3", aliases: ["mp3"], detail: "MP3, VBR about 190 kbps", fromVideo: true, fromAudio: true, remux: false),
        .init(id: "m4a", label: "M4A", ext: "m4a", aliases: ["m4a", "m4b"], detail: "AAC, 192 kbps", fromVideo: false, fromAudio: true, remux: false),
        .init(id: "wav", label: "WAV", ext: "wav", aliases: ["wav", "wave"], detail: "WAV, 16-bit PCM", fromVideo: false, fromAudio: true, remux: false),
        .init(id: "flac", label: "FLAC", ext: "flac", aliases: ["flac"], detail: "FLAC, lossless", fromVideo: false, fromAudio: true, remux: false),
        .init(id: "ogg", label: "OGG", ext: "ogg", aliases: ["ogg", "oga"], detail: "Ogg Vorbis, quality 5", fromVideo: false, fromAudio: true, remux: false),
        .init(id: "opus", label: "Opus", ext: "opus", aliases: ["opus"], detail: "Opus, 128 kbps", fromVideo: false, fromAudio: true, remux: false),
        .init(id: "aiff", label: "AIFF", ext: "aiff", aliases: ["aif", "aiff", "aifc"], detail: "AIFF, 16-bit PCM", fromVideo: false, fromAudio: true, remux: false),
        .init(id: "wma", label: "WMA", ext: "wma", aliases: ["wma"], detail: "WMA, 192 kbps", fromVideo: false, fromAudio: true, remux: false),
    ]

    static func format(_ id: String) -> MediaFormat? { formats.first { $0.id == id } }
    static func accepts(_ f: MediaFormat, _ kind: MediaKind) -> Bool { (kind == .video && f.fromVideo) || (kind == .audio && f.fromAudio) }

    // MARK: Plans

    private static func oneOf(_ codec: String, _ list: [String]) -> Bool { !codec.isEmpty && list.contains(codec) }

    /// Video codecs each container takes as they are, picked so the result plays where people expect that format to play.
    private static func containerTakesVideo(_ id: String, _ c: String) -> Bool {
        switch id {
        case "mp4": return oneOf(c, ["h264", "hevc"])
        case "mov": return oneOf(c, ["h264", "hevc", "prores"])
        case "webm": return oneOf(c, ["vp8", "vp9", "av1"])
        case "avi": return oneOf(c, ["mpeg4"])
        case "wmv": return oneOf(c, ["wmv1", "wmv2", "wmv3", "vc1"])
        default: return false
        }
    }

    private static func containerTakesAudio(_ id: String, _ c: String) -> Bool {
        switch id {
        case "mp4": return oneOf(c, ["aac", "mp3"])
        case "mov": return oneOf(c, ["aac", "mp3", "alac", "pcm_s16le", "pcm_s16be", "pcm_s24le"])
        case "webm": return oneOf(c, ["opus", "vorbis"])
        case "avi": return oneOf(c, ["mp3", "ac3"])
        case "wmv", "wma": return oneOf(c, ["wmav2"])
        case "mp3": return oneOf(c, ["mp3"])
        case "m4a": return oneOf(c, ["aac", "alac"])
        case "wav": return oneOf(c, ["pcm_s16le"])
        case "flac": return oneOf(c, ["flac"])
        case "ogg": return oneOf(c, ["vorbis"])
        case "opus": return oneOf(c, ["opus"])
        case "aiff": return oneOf(c, ["pcm_s16be"])
        default: return false
        }
    }

    static func planConversion(_ f: MediaFormat, _ info: MediaInfo) -> MediaPlan {
        var plan = MediaPlan()
        if f.remux { plan.copyAll = true; return plan }
        if f.id == "gif" { return plan }
        let audioOnly = !f.fromVideo
        plan.copyVideo = !audioOnly && containerTakesVideo(f.id, info.videoCodec)
        plan.hevc = info.videoCodec == "hevc"
        plan.copyAudio = containerTakesAudio(f.id, info.audioCodec)
        // Decoding on the GPU too saves the CPU a lot on 4K phone video; ffmpeg falls back by itself.
        plan.hwDecode = !audioOnly && !plan.copyVideo && !info.videoCodec.isEmpty && (f.id == "mp4" || f.id == "mov")
        return plan
    }

    /// The fallback when copying failed (a stream the container refused): re-encode everything.
    static func transcodePlan(_ p: MediaPlan) -> MediaPlan { MediaPlan(hwDecode: p.hwDecode, videoBps: p.videoBps) }

    /// A video Convert never comes out bigger than its source: when quality overshoots, run again at `share` of the
    /// source's own bitrate, less room for the sound. 0 when the length is unknown.
    static func capVideoBps(sourceBytes: Int64, durationUs: Int64, share: Double, hasAudio: Bool) -> Int64 {
        guard sourceBytes > 0, durationUs > 0 else { return 0 }
        let budget = Double(sourceBytes) * 8 * 1_000_000 / Double(durationUs) * share
        let audio = hasAudio ? min(192_000, budget / 4) : 0
        return max(32_000, Int64(budget - audio))
    }

    // MARK: ffmpeg arguments

    private static let evenSize = "crop=trunc(iw/2)*2:trunc(ih/2)*2"   // 4:2:0 needs even sides

    /// H.264 on the Apple Silicon media engine, at an average bitrate (size follows the setting, not the content).
    private static func h264(bps: Int64) -> [String] {
        ["-c:v", "h264_videotoolbox", "-b:v", String(bps), "-maxrate", String(bps * 2), "-pix_fmt", "yuv420p"]
    }

    static func inputArgs(_ plan: MediaPlan) -> [String] { plan.hwDecode ? ["-hwaccel", "videotoolbox"] : [] }

    static func outputArgs(_ f: MediaFormat, _ plan: MediaPlan) -> [String] {
        let id = f.id
        var a: [String] = []
        if plan.copyAll { return ["-map", "0", "-c", "copy", "-f", "matroska"] }
        if id == "gif" {
            // One pass with a palette made for this clip; square pixels first.
            return ["-map", "0:v:0", "-an", "-vf",
                    "fps=12,scale='min(480,iw*sar)':-2:flags=lanczos,setsar=1,split[a][b];[a]palettegen=stats_mode=diff[p];[b][p]paletteuse=dither=bayer:bayer_scale=4",
                    "-loop", "0", "-f", "gif"]
        }
        if f.fromVideo && id != "mp3" {
            a += ["-map", "0:v:0?", "-map", "0:a:0?"]   // "?" makes each optional (a silent clip)
            if plan.copyVideo {
                a += ["-c:v", "copy"]
                if plan.hevc && (id == "mp4" || id == "mov") { a += ["-tag:v", "hvc1"] }   // Apple players need the hvc1 tag
            } else if id == "mp4" || id == "mov" || id == "mkv" {
                a += ["-vf", evenSize] + h264(bps: plan.videoBps > 0 ? plan.videoBps : 8_000_000)
            } else if plan.videoBps > 0 {
                let b = String(id == "wmv" ? min(plan.videoBps, 6_000_000) : plan.videoBps)
                if id == "webm" { a += ["-c:v", "libvpx-vp9", "-crf", "32", "-b:v", b, "-row-mt", "1", "-deadline", "realtime", "-cpu-used", "6"] }
                else if id == "avi" { a += ["-vf", evenSize, "-c:v", "mpeg4", "-b:v", b, "-tag:v", "XVID"] }
                else if id == "wmv" { a += ["-vf", evenSize, "-c:v", "wmv2", "-b:v", b] }
            } else if id == "webm" {
                a += ["-c:v", "libvpx-vp9", "-crf", "32", "-b:v", "0", "-row-mt", "1", "-deadline", "realtime", "-cpu-used", "6"]
            } else if id == "avi" {
                a += ["-vf", evenSize, "-c:v", "mpeg4", "-q:v", "3", "-tag:v", "XVID"]
            } else if id == "wmv" {
                a += ["-vf", evenSize, "-c:v", "wmv2", "-b:v", "6M"]
            }
        } else {
            a += ["-map", "0:a:0", "-vn"]   // audio formats: the first audio stream, cover art dropped
        }

        if plan.copyAudio { a += ["-c:a", "copy"] }
        else {
            switch id {
            case "mp4", "mov", "mkv": a += ["-c:a", "aac", "-b:a", "192k"]
            case "webm", "opus": a += ["-c:a", "libopus", "-b:a", "128k"]
            case "avi": a += ["-c:a", "libmp3lame", "-q:a", "4"]
            case "wmv", "wma": a += ["-c:a", "wmav2", "-b:a", "192k"]
            case "mp3": a += ["-c:a", "libmp3lame", "-q:a", "2"]
            case "m4a": a += ["-c:a", "aac", "-b:a", "192k"]
            case "wav": a += ["-c:a", "pcm_s16le"]
            case "flac": a += ["-c:a", "flac"]
            case "ogg":
                // libvorbis when the build has it (best quality); else ffmpeg's own (experimental) Vorbis encoder.
                a += FfmpegEngine.shared.hasEncoder("libvorbis") ? ["-c:a", "libvorbis", "-q:a", "5"] : ["-c:a", "vorbis", "-strict", "-2", "-ac", "2", "-b:a", "160k"]
            case "aiff": a += ["-c:a", "pcm_s16be"]
            default: break
            }
        }
        // The muxer is named because the staging file's extension says nothing.
        switch id {
        case "mp4", "mov": a += ["-movflags", "+faststart", "-f", id]
        case "m4a": a += ["-movflags", "+faststart", "-f", "ipod"]
        case "mkv": a += ["-f", "matroska"]
        case "wmv", "wma": a += ["-f", "asf"]
        default: a += ["-f", id]
        }
        return a
    }

    /// The whole command line after "ffmpeg": local files only, progress as key=value lines on stdout.
    static func ffmpegArgs(input: String, inputArgs: [String], outputArgs: [String], output: String) -> [String] {
        ["-hide_banner", "-nostdin", "-protocol_whitelist", "file"] + inputArgs + ["-i", input] + outputArgs + ["-progress", "pipe:1", "-nostats", "-y", output]
    }

    // MARK: Compress (media.compress)

    /// False when the target is too small for the input's length to give a watchable result.
    static func planSizeFit(targetBytes: Int64, info: MediaInfo) -> SizeFit? {
        guard targetBytes > 0, info.durationUs > 0 else { return nil }
        let seconds = Double(info.durationUs) / 1e6
        let total = Double(targetBytes) * 8 * 0.96 / seconds   // 4% for the container and the encoder's drift
        var fit = SizeFit()
        if info.videoCodec.isEmpty {
            if total < 24_000 { return nil }
            fit.audioBps = Int64(min(total, 256_000))
            return fit
        }
        fit.audioBps = info.audioCodec.isEmpty ? 0 : (total >= 1_500_000 ? 128_000 : (total >= 500_000 ? 96_000 : 64_000))
        let video = total - Double(fit.audioBps)
        if video < 100_000 { return nil }
        fit.videoBps = Int64(video)
        // The tallest standard height whose pixel count the bitrate can carry at ~0.08 bits per pixel per frame.
        let fps = info.fps > 0 ? min(info.fps, 60) : 30
        let pixels = video / (fps * 0.08)
        let aspect = info.width > 0 && info.height > 0 ? Double(info.width) / Double(info.height) : 16.0 / 9.0
        let maxHeight = (pixels / aspect).squareRoot()
        for h in [2160, 1440, 1080, 720, 540, 480, 360, 240] where Double(h) <= maxHeight || h == 240 {
            fit.height = (info.height > 0 && h >= info.height) ? 0 : h
            break
        }
        return fit
    }

    /// H.264 + AAC MP4 for video, AAC M4A for audio. `fit` nil = the quality preset; `maxHeight` 0 = any.
    static func compressArgs(info: MediaInfo, fit: SizeFit?, maxHeight: Int) -> [String] {
        if info.videoCodec.isEmpty {
            let bps = fit?.audioBps ?? 96_000
            return ["-map", "0:a:0", "-vn", "-c:a", "aac", "-b:a", String(bps), "-movflags", "+faststart", "-f", "ipod"]
        }
        var a = ["-map", "0:v:0", "-map", "0:a:0?"]
        var height = fit?.height ?? 0
        if maxHeight > 0 && info.height > maxHeight && (height == 0 || height > maxHeight) { height = maxHeight }
        a += height > 0 ? ["-vf", "scale=-2:\(height):flags=bicubic"] : ["-vf", evenSize]
        if let fit { a += h264(bps: fit.videoBps) }
        else {
            // Preset: sized from the output's pixels (about 0.06 bits per pixel per frame).
            let h = height > 0 ? height : (info.height > 0 ? info.height : 1080)
            let w = info.height > 0 ? info.width * h / info.height : h * 16 / 9
            let fps = info.fps > 0 ? min(info.fps, 60) : 30
            a += h264(bps: Int64(Double(w) * Double(h) * fps * 0.06))
        }
        let audio = fit?.audioBps ?? 128_000
        if audio > 0 { a += ["-c:a", "aac", "-b:a", String(audio)] }
        a += ["-movflags", "+faststart", "-f", "mp4"]
        return a
    }

    // MARK: Parsing ffmpeg's log

    static func parseInfo(_ lines: [String]) -> MediaInfo {
        var info = MediaInfo()
        for line in lines {
            if info.durationUs < 0, let d = parseDuration(line) { info.durationUs = d }
            guard line.contains("Stream #0:") else { continue }
            if line.contains("Video: "), info.videoCodec.isEmpty, !line.contains("(attached pic)") {   // cover art is not a video stream
                info.videoCodec = codec(after: "Video: ", in: line)
                if let m = line.range(of: #"(?<![0-9A-Za-z])(\d{2,5})x(\d{2,5})(?![0-9A-Za-z])"#, options: .regularExpression) {
                    let parts = line[m].split(separator: "x")
                    info.width = Int(parts[0]) ?? 0; info.height = Int(parts[1]) ?? 0
                }
                if let m = line.range(of: #"([0-9.]+) fps"#, options: .regularExpression) {
                    info.fps = Double(line[m].replacingOccurrences(of: " fps", with: "")) ?? 0
                }
            } else if line.contains("Audio: "), info.audioCodec.isEmpty {
                info.audioCodec = codec(after: "Audio: ", in: line)
            }
        }
        return info
    }

    private static func codec(after key: String, in line: String) -> String {
        guard let r = line.range(of: key) else { return "" }
        return String(line[r.upperBound...].prefix { $0 != " " && $0 != "," })
    }

    /// "  Duration: 00:01:23.45, start: ..." -> microseconds; nil for "N/A".
    static func parseDuration(_ line: String) -> Int64? {
        guard let r = line.range(of: "Duration: ") else { return nil }
        let parts = line[r.upperBound...].prefix { $0 != "," }.split(separator: ":")
        guard parts.count == 3, let h = Double(parts[0]), let m = Double(parts[1]), let s = Double(parts[2]) else { return nil }
        return Int64((h * 3600 + m * 60 + s) * 1e6)
    }

    static func parseProgress(_ line: String) -> Int64? {
        guard line.hasPrefix("out_time_us="), let v = Int64(line.dropFirst("out_time_us=".count)), v >= 0 else { return nil }
        return v
    }
}
