import AppKit

// Colours from the Windows UI spec (src/ThemeTokens.cpp, Accent.cpp). Solid style.
func rgb(_ hex: UInt32, _ alpha: CGFloat = 1) -> NSColor {
    NSColor(srgbRed: CGFloat((hex >> 16) & 0xFF) / 255, green: CGFloat((hex >> 8) & 0xFF) / 255, blue: CGFloat(hex & 0xFF) / 255, alpha: alpha)
}

func mix(_ a: NSColor, _ b: NSColor, _ t: CGFloat) -> NSColor {
    let x = a.usingColorSpace(.sRGB)!, y = b.usingColorSpace(.sRGB)!
    return NSColor(srgbRed: x.redComponent + (y.redComponent - x.redComponent) * t, green: x.greenComponent + (y.greenComponent - x.greenComponent) * t,
                   blue: x.blueComponent + (y.blueComponent - x.blueComponent) * t, alpha: x.alphaComponent + (y.alphaComponent - x.alphaComponent) * t)
}

/// Accent colour, as on Windows (src/Accent.cpp): the system accent by default, or a preset with Tide first.
/// Pick a preset with `defaults write dev.shiftdrop.mac accent tide|blue|purple|pink|red|orange|green|graphite|system`.
struct AccentVariants { var fill, dark, light: UInt32 }

enum Accent {
    static let presets: [(id: String, v: AccentVariants)] = [
        ("tide", AccentVariants(fill: 0x12A58E, dark: 0x2DD4B4, light: 0x0B7A69)),   // ShiftDrop's teal, first
        ("blue", AccentVariants(fill: 0x3B82F6, dark: 0x60A5FA, light: 0x0067C0)),
        ("purple", AccentVariants(fill: 0x7C4DFF, dark: 0xB794F4, light: 0x7B3FC4)),
        ("pink", AccentVariants(fill: 0xE64B9A, dark: 0xF79AC8, light: 0xB4246F)),
        ("red", AccentVariants(fill: 0xE8504F, dark: 0xFF8F8F, light: 0xC42B2B)),
        ("orange", AccentVariants(fill: 0xF2802B, dark: 0xFB923C, light: 0xB85000)),
        ("green", AccentVariants(fill: 0x2FA84F, dark: 0x5CD67E, light: 0x1F7A3A)),
        ("graphite", AccentVariants(fill: 0x6B7280, dark: 0xB4B9C3, light: 0x4A4F5A)),
    ]

    /// The default ("system", or anything unknown) follows the macOS accent colour.
    static func current() -> AccentVariants {
        let id = UserDefaults.standard.string(forKey: "accent")?.lowercased() ?? "system"
        if let p = presets.first(where: { $0.id == id }) { return p.v }
        return derive(systemRGB())
    }

    private static func systemRGB() -> UInt32 {
        let c = NSColor.controlAccentColor.usingColorSpace(.sRGB) ?? NSColor(srgbRed: 0, green: 0.48, blue: 1, alpha: 1)
        func b(_ v: CGFloat) -> UInt32 { UInt32((max(0, min(1, v)) * 255).rounded()) }
        return b(c.redComponent) << 16 | b(c.greenComponent) << 8 | b(c.blueComponent)
    }

    // Same contrast rules as Accent.cpp: text on the dark tone is 0x15171E, on the light tone white, 4.5:1 minimum.
    private static let textOnDark: UInt32 = 0x15171E, textOnLight: UInt32 = 0xFFFFFF
    private static let darkSurface: UInt32 = 0x2E3138, lightSurface: UInt32 = 0xF3F4F6, minContrast = 4.5

    private static func luminance(_ c: UInt32) -> Double {
        func ch(_ s: UInt32) -> Double { let v = Double((c >> s) & 0xFF) / 255; return v <= 0.04045 ? v / 12.92 : pow((v + 0.055) / 1.055, 2.4) }
        return 0.2126 * ch(16) + 0.7152 * ch(8) + 0.0722 * ch(0)
    }
    private static func contrast(_ a: UInt32, _ b: UInt32) -> Double {
        let la = luminance(a), lb = luminance(b)
        return (max(la, lb) + 0.05) / (min(la, lb) + 0.05)
    }
    private static func mixRGB(_ a: UInt32, _ b: UInt32, _ t: Double) -> UInt32 {
        var out: UInt32 = 0
        for s in [UInt32(0), 8, 16] {
            let ca = Double((a >> s) & 0xFF), cb = Double((b >> s) & 0xFF)
            out |= UInt32((ca + (cb - ca) * t).rounded()) << s
        }
        return out
    }
    private static func toward(_ from: UInt32, _ to: UInt32, _ ok: (UInt32) -> Bool) -> UInt32 {
        for i in 0..<50 { let c = mixRGB(from, to, Double(i) / 50); if ok(c) { return c } }
        return to
    }

    static func derive(_ rgb: UInt32) -> AccentVariants {
        let fillText = contrast(rgb, textOnDark) >= contrast(rgb, textOnLight) ? textOnDark : textOnLight
        let fill = toward(rgb, fillText == textOnDark ? 0xFFFFFF : 0x000000) { contrast($0, fillText) >= minContrast }
        let dark = toward(rgb, 0xFFFFFF) { contrast($0, textOnDark) >= minContrast && contrast($0, darkSurface) >= minContrast }
        let light = toward(rgb, 0x000000) { contrast($0, textOnLight) >= minContrast && contrast($0, lightSurface) >= minContrast }
        return AccentVariants(fill: fill, dark: dark, light: light)
    }
}

struct Palette {
    var dark: Bool
    var accent, accentHi, textOnAccent, accentMark: NSColor
    var text, textDim, subtleText, shadow: NSColor
    var disc, discLine, tile, tileLine, centre, centreLine, dot: NSColor

    /// Built from the current accent (system accent by default; Tide is the first preset).
    static func make(dark: Bool) -> Palette {
        let v = Accent.current()
        let light = rgb(v.light), darkTone = rgb(v.dark)
        let accent = light   // the hovered tile and active section use the light tone with white text in both themes
        let accentHi = mix(light, .white, 0.20)
        if dark {
            return Palette(dark: true, accent: accent, accentHi: accentHi, textOnAccent: .white, accentMark: darkTone,
                           text: rgb(0xEEF0F3), textDim: rgb(0xEEF0F3, 0.38), subtleText: rgb(0xA9AEB8), shadow: rgb(0x000000, 0.5),
                           disc: rgb(0x1A1C22), discLine: rgb(0xFFFFFF, 0.08), tile: rgb(0x2A2D35), tileLine: rgb(0xFFFFFF, 0.06),
                           centre: rgb(0x141619), centreLine: rgb(0xFFFFFF, 0.08), dot: rgb(0xEEF0F3, 0.28))
        }
        return Palette(dark: false, accent: accent, accentHi: accentHi, textOnAccent: .white, accentMark: light,
                       text: rgb(0x1B1D23), textDim: rgb(0x1B1D23, 0.38), subtleText: rgb(0x5E6370), shadow: rgb(0x14161E, 0.18),
                       disc: rgb(0xFFFFFF), discLine: rgb(0x15171E, 0.10), tile: rgb(0xEEF0F3), tileLine: rgb(0xD5D8DE),
                       centre: rgb(0xFFFFFF), centreLine: rgb(0x15171E, 0.10), dot: rgb(0x1B1D23, 0.25))
    }
}
