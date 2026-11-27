import AppKit

// Port of src/keyboard/WheelLabels.{h,cpp} and UI_SPEC.md section 3 / Brand Guidelines:
// Wheel labels: 13 Semibold, centred, one colour, never trimmed with "…"
// Labels go on one line, else on two broken at a space (never between a number and its unit:
// "10 MB" or "1920 px" stays together), the most even split first.
// The text block stays at least kLabelPadDip (1.5 pt) inside the tile's arcs and straight sides.

struct LabelSpec {
    var text: String
    var hasIcon: Bool
}

struct LabelRow {
    var w: CGFloat
    var h: CGFloat
}

struct FittedLabel {
    var lines: [String] = []
    var widths: [CGFloat] = []
    var hasIcon: Bool = false
    var radius: CGFloat = 0
    var iconDy: CGFloat = 0
    var lineDy: [CGFloat] = []
    var fontSize: CGFloat = WheelMetrics.labelSize
    var fits: Bool = true

    var text: String { lines.joined(separator: " ") }
}

enum WheelLabelFitter {
    static let kLabelDip: CGFloat = WheelMetrics.labelSize // 13 pt Semibold
    static let kIconDip: CGFloat = 20.0                   // Icon size
    static let kIconGapDip: CGFloat = 3.0                 // Gap between icon and label
    static let kLineHeight: CGFloat = 1.05                // Line height multiplier
    static let kLabelPadDip: CGFloat = 1.5                // Padding inside tile edges
    static let kTileGapDip: CGFloat = WheelMetrics.gap    // 4 pt gap between tiles

    private static let units: Set<String> = ["B", "KB", "MB", "GB", "TB", "px", "%", "fps", "x", "dpi"]

    private static func words(of text: String) -> [String] {
        text.split(separator: " ", omittingEmptySubsequences: true).map(String.init)
    }

    private static func breakAllowed(words: [String], before: Int) -> Bool {
        guard before > 0, before < words.count else { return false }
        let prev = words[before - 1]
        if let last = prev.last, last.isNumber {
            if units.contains(words[before]) { return false }
        }
        return true
    }

    private static func measureWidth(_ text: String, fontSize: CGFloat) -> CGFloat {
        let font = NSFont.systemFont(ofSize: fontSize, weight: .semibold)
        let str = NSAttributedString(string: text, attributes: [.font: font])
        return str.size().width + 1.0 // 1 pt slack for overhang
    }

    static func rowsOf(_ f: FittedLabel, fontSize: CGFloat) -> [LabelRow] {
        var rows: [LabelRow] = []
        if f.hasIcon {
            rows.append(LabelRow(w: kIconDip, h: kIconDip))
            rows.append(LabelRow(w: 0, h: kIconGapDip))
        }
        let lh = fontSize * kLineHeight
        for w in f.widths {
            rows.append(LabelRow(w: w, h: lh))
        }
        return rows
    }

    static func place(lines: [String], hasIcon: Bool, radius: CGFloat, fontSize: CGFloat) -> FittedLabel {
        var f = FittedLabel()
        f.lines = lines
        f.hasIcon = hasIcon
        f.radius = radius
        f.fontSize = fontSize
        for l in lines {
            f.widths.append(measureWidth(l, fontSize: fontSize))
        }
        let rows = rowsOf(f, fontSize: fontSize)
        let height = rows.reduce(0) { $0 + $1.h }
        var y = -height / 2.0
        if hasIcon {
            f.iconDy = y + kIconDip / 2.0
            y += kIconDip + kIconGapDip
        }
        let lh = fontSize * kLineHeight
        for _ in lines {
            f.lineDy.append(y + lh / 2.0)
            y += lh
        }
        return f
    }

    static func rowsFit(layout: WheelLayout, slot: Int, rows: [LabelRow], radius: CGFloat, pad: CGFloat = kLabelPadDip) -> Bool {
        guard slot >= 0, slot < layout.slots.count else { return false }
        let s = layout.slots[slot]
        let band = layout.band(slot)
        let midDeg = layout.midDeg(slot)
        let spanDeg = layout.spanDeg(slot)
        let kDeg = CGFloat.pi / 180.0
        let mid = midDeg * kDeg
        let half = spanDeg / 2.0 * kDeg

        // Coordinate system: 0° is 12 o'clock, clockwise, y is down (flipped coordinates)
        let px = radius * sin(mid)
        let py = -radius * cos(mid)

        var height: CGFloat = 0
        for r in rows { height += r.h }

        let side = kTileGapDip / 2.0 + pad
        let n0x = cos(mid - half), n0y = sin(mid - half)
        let n1x = cos(mid + half), n1y = sin(mid + half)
        let outer = band.outer - pad
        let inner = band.inner + pad

        var top = py - height / 2.0
        for r in rows {
            let x0 = px - r.w / 2.0
            let x1 = px + r.w / 2.0
            let y0 = top
            let y1 = top + r.h
            top += r.h
            if r.w <= 0 { continue }

            for x in [x0, x1] {
                for y in [y0, y1] {
                    if x * x + y * y > outer * outer {
                        return false
                    }
                    if s.count > 1 && (x * n0x + y * n0y < side || x * n1x + y * n1y > -side) {
                        return false
                    }
                }
            }
            // Inner arc bulges toward origin: check closest point on the row rect
            let dx: CGFloat = (x0 > 0) ? x0 : ((x1 < 0) ? -x1 : 0)
            let dy: CGFloat = (y0 > 0) ? y0 : ((y1 < 0) ? -y1 : 0)
            if dx * dx + dy * dy < inner * inner {
                return false
            }
        }
        return true
    }

    private static func tryFit(layout: WheelLayout, slot: Int, lines: [String], hasIcon: Bool, fontSize: CGFloat) -> FittedLabel? {
        var f = place(lines: lines, hasIcon: hasIcon, radius: 0, fontSize: fontSize)
        let rows = rowsOf(f, fontSize: fontSize)
        let band = layout.band(slot)
        let mid = (band.inner + band.outer) / 2.0
        let reach = (band.outer - band.inner) / 2.0

        // Test the middle of the tile first, then step outward and inward
        for off in stride(from: 0.0, through: reach, by: 1.0) {
            for r in [mid + off, mid - off] {
                if rowsFit(layout: layout, slot: slot, rows: rows, radius: r) {
                    f.radius = r
                    f.fits = true
                    return f
                }
                if off == 0 { break }
            }
        }
        return nil
    }

    static func fit(layout: WheelLayout, slot: Int, spec: LabelSpec) -> FittedLabel {
        let text = spec.text
        if text.isEmpty { return FittedLabel(lines: [], widths: [], hasIcon: false, radius: (layout.band(slot).inner + layout.band(slot).outer) / 2, iconDy: 0, lineDy: [], fontSize: kLabelDip, fits: true) }

        let allWords = words(of: text)
        var ways: [[String]] = [[text]]

        if allWords.count > 1 {
            var splits: [(String, String)] = []
            for i in 1..<allWords.count {
                if breakAllowed(words: allWords, before: i) {
                    let first = allWords[0..<i].joined(separator: " ")
                    let second = allWords[i..<allWords.count].joined(separator: " ")
                    splits.append((first, second))
                }
            }
            // Most even split first: minimizes the wider line
            splits.sort { a, b in
                let maxA = max(measureWidth(a.0, fontSize: kLabelDip), measureWidth(a.1, fontSize: kLabelDip))
                let maxB = max(measureWidth(b.0, fontSize: kLabelDip), measureWidth(b.1, fontSize: kLabelDip))
                return maxA < maxB
            }
            for (x, y) in splits {
                ways.append([x, y])
            }
        }

        // 1. Try standard 13 pt Semibold with icon (or without if spec has no icon)
        for lines in ways {
            if let fitted = tryFit(layout: layout, slot: slot, lines: lines, hasIcon: spec.hasIcon, fontSize: kLabelDip) {
                return fitted
            }
        }

        // 2. If it doesn't fit at 13 pt, try smaller sizes down to 10.5 pt
        for size: CGFloat in [12.0, 11.5, 11.0, 10.5] {
            for lines in ways {
                if let fitted = tryFit(layout: layout, slot: slot, lines: lines, hasIcon: spec.hasIcon, fontSize: size) {
                    return fitted
                }
            }
        }

        // 3. If an icon was requested but made the label unable to fit, try without icon
        if spec.hasIcon {
            for size: CGFloat in [kLabelDip, 12.0, 11.5, 11.0, 10.5] {
                for lines in ways {
                    if let fitted = tryFit(layout: layout, slot: slot, lines: lines, hasIcon: false, fontSize: size) {
                        return fitted
                    }
                }
            }
        }

        // 4. Fallback: place the best 2-line split (or 1 line) at mid radius
        let fallbackLines = ways.count > 1 ? ways[1] : ways[0]
        let band = layout.band(slot)
        var fallback = place(lines: fallbackLines, hasIcon: spec.hasIcon, radius: (band.inner + band.outer) / 2.0, fontSize: 10.5)
        fallback.fits = false
        return fallback
    }

    static func fitAll(layout: WheelLayout, slices: [WheelSlice]) -> [FittedLabel] {
        var results: [FittedLabel] = []
        for i in 0..<layout.slots.count {
            if layout.isMore(i) {
                results.append(fit(layout: layout, slot: i, spec: LabelSpec(text: "•••", hasIcon: false)))
            } else {
                let ci = layout.contentIndex(i)
                if ci >= 0 && ci < slices.count {
                    let slice = slices[ci]
                    results.append(fit(layout: layout, slot: i, spec: LabelSpec(text: slice.label, hasIcon: slice.icon != nil)))
                } else {
                    results.append(FittedLabel())
                }
            }
        }
        return results
    }
}
