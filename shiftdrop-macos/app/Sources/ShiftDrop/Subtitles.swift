import Foundation

/// SRT and WebVTT (port of src/subtitles/).
enum Subtitles {
    struct Cue { var start: Double; var end: Double; var text: String }   // seconds

    static func parse(_ text: String) -> [Cue] {
        let blocks = text.replacingOccurrences(of: "\r\n", with: "\n").replacingOccurrences(of: "\u{FEFF}", with: "").components(separatedBy: "\n\n")
        var cues: [Cue] = []
        for block in blocks {
            let lines = block.components(separatedBy: "\n").filter { !$0.trimmingCharacters(in: .whitespaces).isEmpty }
            guard let i = lines.firstIndex(where: { $0.contains("-->") }) else { continue }
            let parts = lines[i].components(separatedBy: "-->")
            guard parts.count == 2, let s = seconds(parts[0]), let e = seconds(String(parts[1].trimmingCharacters(in: .whitespaces).split(separator: " ").first ?? "")) else { continue }
            cues.append(Cue(start: s, end: e, text: lines[(i + 1)...].joined(separator: "\n")))
        }
        return cues
    }

    /// "00:01:02,500", "01:02.500" or "1:02:03.4" -> seconds.
    static func seconds(_ raw: String) -> Double? {
        let parts = raw.trimmingCharacters(in: .whitespaces).replacingOccurrences(of: ",", with: ".").split(separator: ":").map(String.init)
        guard (2...3).contains(parts.count), let last = Double(parts[parts.count - 1]) else { return nil }
        let mins = Double(parts[parts.count - 2]) ?? 0, hours = parts.count == 3 ? (Double(parts[0]) ?? 0) : 0
        return hours * 3600 + mins * 60 + last
    }

    private static func stamp(_ t: Double, comma: Bool) -> String {
        let ms = Int((t * 1000).rounded()), h = ms / 3_600_000, m = ms / 60_000 % 60, s = ms / 1000 % 60, f = ms % 1000
        return String(format: "%02d:%02d:%02d%@%03d", h, m, s, comma ? "," : ".", f)
    }

    static func srt(_ cues: [Cue]) -> String {
        cues.enumerated().map { "\($0.offset + 1)\n\(stamp($0.element.start, comma: true)) --> \(stamp($0.element.end, comma: true))\n\($0.element.text)\n" }.joined(separator: "\n")
    }

    static func vtt(_ cues: [Cue]) -> String {
        "WEBVTT\n\n" + cues.map { "\(stamp($0.start, comma: false)) --> \(stamp($0.end, comma: false))\n\($0.text)\n" }.joined(separator: "\n")
    }

    /// Just the words, one line per subtitle, formatting tags removed.
    static func txt(_ cues: [Cue]) -> String {
        cues.map { $0.text.replacingOccurrences(of: #"<[^>]+>|\{\\[^}]*\}"#, with: "", options: .regularExpression).replacingOccurrences(of: "\n", with: " ") }.joined(separator: "\n") + "\n"
    }
}

/// CSV and TSV (the parts of src/sheets/ that need no spreadsheet engine).
enum Sheets {
    static func parse(_ text: String, delimiter: Character) -> [[String]] {
        var rows: [[String]] = [], row: [String] = [], field = "", quoted = false
        // Swift counts "\r\n" as one Character, so line ends are made "\n" first (else CRLF files, Excel's, never split into rows).
        let chars = Array(text.replacingOccurrences(of: "\u{FEFF}", with: "").replacingOccurrences(of: "\r\n", with: "\n"))
        var i = 0
        while i < chars.count {
            let c = chars[i]
            if quoted {
                if c == "\"" { if i + 1 < chars.count && chars[i + 1] == "\"" { field.append("\""); i += 1 } else { quoted = false } }
                else { field.append(c) }
            } else if c == "\"" && field.isEmpty { quoted = true }
            else if c == delimiter { row.append(field); field = "" }
            else if c == "\n" || c == "\r" {
                if c == "\r" && i + 1 < chars.count && chars[i + 1] == "\n" { i += 1 }
                row.append(field); field = ""; rows.append(row); row = []
            } else { field.append(c) }
            i += 1
        }
        if !field.isEmpty || !row.isEmpty { row.append(field); rows.append(row) }
        return rows
    }

    static func write(_ rows: [[String]], delimiter: Character) -> String {
        rows.map { row in
            row.map { f in
                f.contains(delimiter) || f.contains("\"") || f.contains("\n") || f.contains("\r") ? "\"" + f.replacingOccurrences(of: "\"", with: "\"\"") + "\"" : f
            }.joined(separator: String(delimiter))
        }.joined(separator: "\n") + "\n"
    }
}
