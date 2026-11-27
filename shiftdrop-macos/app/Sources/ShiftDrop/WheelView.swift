import AppKit

struct WheelSlice {
    var id: String = ""
    var label: String
    var detail: String = ""
    var enabled = true
    var icon: String?        // SF Symbol name; nil = text only (format names)
    var headline: String?    // replaces the centre's big text while hovered (Reduce size: the new size)
}

struct WheelRing {
    var key: String = ""
    var title: String
    var icon: String = "circle"
    var slices: [WheelSlice]
}

/// Sample content, just to exercise the layout: 8 slices (one ring), 12 (two rings), 20 (paging).
func sampleRings() -> [WheelRing] {
    func s(_ names: [String]) -> [WheelSlice] { names.map { WheelSlice(id: $0, label: $0, detail: "\($0), best quality") } }
    return [
        WheelRing(title: "Convert", slices: s(["PNG", "JPG", "WebP", "AVIF", "HEIC", "TIFF", "BMP", "PDF"])),
        WheelRing(title: "Tools", slices: s(["Rotate", "Flip", "1920 px", "1080 px", "50%", "Compress", "Crop", "Inspect", "Edit", "Annotate", "Redact", "Share-safe"])),
        WheelRing(title: "Reduce size", slices: s(["90%", "80%", "70%", "60%", "50%", "40%", "30%", "20%"])),
        WheelRing(title: "File actions", slices: s((1...20).map { "Action \($0)" })),
    ]
}

/// The radial menu. Flipped so the maths matches the Windows (y down) code.
final class WheelView: NSView {
    enum Mode { case probe, wheel }

    override var isFlipped: Bool { true }

    var mode = Mode.probe
    var rings = sampleRings() {
        didSet {
            if ringIndex >= rings.count { ringIndex = 0 }
            relayout()
        }
    }
    var ringIndex = 0
    var page = 0
    var files: [URL] = []
    var hot = WheelMetrics.kCentre {
        didSet {
            if hot != oldValue {
                if mode == .wheel, hot >= 0, isEnabled(hot), !suppressSound { Sounds.hover() }
                notifyHighlight()
            }
        }
    }
    var suppressSound = false   // snapshots and resets stay silent
    var hotT: [CGFloat] = []
    private var fittedLabels: [FittedLabel] = []
    var openT: CGFloat = 0
    var scale: CGFloat = 1.15
    var backdrop: NSColor?          // snapshot mode only
    var contentProvider: (([URL]) -> [WheelRing])?
    var onShown: (([WheelRing], Int) -> Void)?
    var onRingChanged: ((Int) -> Void)?
    var onDrop: (([URL], WheelSlice, Int) -> Void)?
    var onFinished: (() -> Void)?
    var onSwitch: ((Int) -> Void)?
    var onHighlightChanged: (([URL], WheelSlice?, Recipe?) -> Void)?
    var currentOuterRadius: CGFloat { layout.outerEdge }

    func notifyHighlight() {
        guard mode == .wheel else {
            onHighlightChanged?(files, nil, nil)
            return
        }
        if hot >= 0, isEnabled(hot) {
            let ci = layout.contentIndex(hot)
            if ci >= 0, ci < content.count {
                let slice = content[ci]
                let recipe = WheelContents.recipes.first(where: { $0.id == slice.id }) ?? RecipeStore.find(slice.id)
                onHighlightChanged?(files, slice, recipe)
                return
            }
        }
        onHighlightChanged?(files, nil, nil)
    }

    private var timer: Timer?
    private var lastTick = CACurrentMediaTime()
    private var layout = WheelLayout()
    private var palette: Palette { Palette.make(dark: effectiveAppearance.bestMatch(from: [.darkAqua, .aqua]) == .darkAqua) }
    var forceDark: Bool?

    private var pal: Palette { forceDark.map { Palette.make(dark: $0) } ?? palette }
    private var centre: CGPoint { CGPoint(x: bounds.midX, y: bounds.midY) }
    private var currentRing: WheelRing? {
        guard !rings.isEmpty else { return nil }
        let idx = max(0, min(ringIndex, rings.count - 1))
        return rings[idx]
    }
    private var content: [WheelSlice] { currentRing?.slices ?? [] }

    override init(frame: NSRect) {
        super.init(frame: frame)
        registerForDraggedTypes([.fileURL])
        relayout()
    }
    required init?(coder: NSCoder) { fatalError() }

    // MARK: State

    func relayout() {
        if ringIndex >= rings.count { ringIndex = max(0, rings.count - 1) }
        layout = WheelLayout.arrange(total: content.count, page: page)
        hotT = Array(repeating: 0, count: layout.slots.count)
        fittedLabels = WheelLabelFitter.fitAll(layout: layout, slices: content)
    }

    func setRing(_ index: Int) {
        guard !rings.isEmpty else { return }
        let previous = ringIndex
        ringIndex = ((index % rings.count) + rings.count) % rings.count
        if mode == .wheel && ringIndex != previous && !suppressSound { Sounds.section() }
        page = 0
        hot = WheelMetrics.kCentre
        relayout()
        needsDisplay = true
        onRingChanged?(ringIndex)
        notifyHighlight()
    }

    func reset() {
        timer?.invalidate(); timer = nil
        mode = .probe; files = []; openT = 0; hot = WheelMetrics.kCentre
        ringIndex = 0
        page = 0
        relayout()
        needsDisplay = true
        notifyHighlight()
    }

    private func startTimer() {
        timer?.invalidate()
        lastTick = CACurrentMediaTime()
        timer = Timer.scheduledTimer(withTimeInterval: 1.0 / 60, repeats: true) { [weak self] _ in self?.tick() }
        RunLoop.main.add(timer!, forMode: .common)
    }

    private func tick() {
        let now = CACurrentMediaTime(), dt = CGFloat(now - lastTick)
        lastTick = now
        let reduce = NSWorkspace.shared.accessibilityDisplayShouldReduceMotion
        openT = reduce ? 1 : min(1, openT + dt / 0.42)
        for i in hotT.indices {
            let target: CGFloat = (i == hot && isEnabled(i)) ? 1 : 0
            hotT[i] = reduce ? target : hotT[i] + (target - hotT[i]) * (1 - exp(-dt / 0.042))   // tau 42 ms
        }
        needsDisplay = true
    }

    private func isEnabled(_ slot: Int) -> Bool {
        if layout.isMore(slot) { return true }
        let i = layout.contentIndex(slot)
        return i >= 0 && i < content.count && content[i].enabled
    }

    private func hitTest(windowPoint p: NSPoint) -> Int {
        let q = convert(p, from: nil)
        let dx = (q.x - centre.x) / scale, dy = (q.y - centre.y) / scale
        let deg = atan2(dx, -dy) * 180 / .pi
        return layout.hit(distance: hypot(dx, dy), degrees: deg, reach: WheelMetrics.margin)
    }

    func setHot(_ slot: Int) { hot = slot }

    // MARK: Drag destination

    private func readURLs(_ info: NSDraggingInfo) -> [URL] {
        (info.draggingPasteboard.readObjects(forClasses: [NSURL.self], options: [.urlReadingFileURLsOnly: true]) as? [URL]) ?? []
    }

    override func draggingEntered(_ sender: NSDraggingInfo) -> NSDragOperation {
        let urls = readURLs(sender)
        guard !urls.isEmpty else { return [] }   // not a file drag: never flash a menu
        if mode == .probe, let provider = contentProvider {
            let r = provider(urls)
            guard !r.isEmpty else { return [] }
            ringIndex = 0
            page = 0
            rings = r
        }
        files = urls
        if mode == .probe {
            mode = .wheel
            onShown?(rings, ringIndex)
            openT = 0
            startTimer()
        }
        return draggingUpdated(sender)
    }

    override func draggingUpdated(_ sender: NSDraggingInfo) -> NSDragOperation {
        hot = hitTest(windowPoint: sender.draggingLocation)
        needsDisplay = true
        return (hot >= 0 && isEnabled(hot)) ? .copy : []
    }

    override func draggingExited(_ sender: NSDraggingInfo?) { hot = WheelMetrics.kOutside }

    override func prepareForDragOperation(_ sender: NSDraggingInfo) -> Bool { hot >= 0 && isEnabled(hot) }

    override func performDragOperation(_ sender: NSDraggingInfo) -> Bool {
        guard hot >= 0, isEnabled(hot) else { return false }
        if layout.isMore(hot) {   // "More" turns the page
            page += 1; relayout(); hot = WheelMetrics.kCentre; return false
        }
        let i = layout.contentIndex(hot)
        guard i >= 0, i < content.count else { return false }
        onDrop?(files, content[i], ringIndex)
        return true
    }

    override func concludeDragOperation(_ sender: NSDraggingInfo?) { onFinished?() }
    override func draggingEnded(_ sender: NSDraggingInfo) { if mode == .wheel { onFinished?() } }

    // MARK: Drawing

    private func ellipse(_ c: CGPoint, _ r: CGFloat) -> CGRect { CGRect(x: c.x - r, y: c.y - r, width: 2 * r, height: 2 * r) }

    override func draw(_ dirtyRect: NSRect) {
        guard let g = NSGraphicsContext.current?.cgContext else { return }
        if let b = backdrop { b.setFill(); bounds.fill() }
        if mode == .probe {
            // Almost invisible but not transparent: a pixel with alpha 0 is click-through, so it would never get the drop.
            g.setFillColor(NSColor(white: 1, alpha: 0.01).cgColor)
            g.fillEllipse(in: ellipse(centre, (layout.outerEdge + WheelMetrics.margin) * scale))
            return
        }
        let p = pal
        let open = spring(openT)
        let s = scale * (0.72 + 0.28 * open)
        let alpha = min(1, openT * 0.42 / 0.12)
        g.saveGState()
        g.setAlpha(alpha)
        g.translateBy(x: centre.x, y: centre.y); g.scaleBy(x: s / scale, y: s / scale); g.translateBy(x: -centre.x, y: -centre.y)

        // plate with shadow
        let plate = (layout.outerEdge + 8) * scale
        g.saveGState()
        g.setShadow(offset: CGSize(width: 0, height: -6), blur: 22, color: p.shadow.cgColor)
        g.setFillColor(p.disc.cgColor)
        g.fillEllipse(in: ellipse(centre, plate))
        g.restoreGState()
        g.setStrokeColor(p.discLine.cgColor); g.setLineWidth(1)
        g.strokeEllipse(in: ellipse(centre, plate - 0.5))

        drawSlices(g, p)
        drawCentre(g, p)
        g.restoreGState()
    }

    private func drawSlices(_ g: CGContext, _ p: Palette) {
        let builder = TileBuilder(centre: centre, scale: scale)
        let order = layout.slots.indices.sorted { hotT[$0] < hotT[$1] }   // hovered tile last
        for i in order {
            let slot = layout.slots[i]
            let t = isEnabled(i) ? hotT[i] : 0, e = easeOutCubic(t)
            let band = layout.band(i)
            let innerOfTwo = layout.rings == 2 && slot.ring == 0
            let mid = layout.midDeg(i)
            let push = (innerOfTwo ? WheelMetrics.innerPush : WheelMetrics.push) * scale * e
            let rad = mid * .pi / 180
            g.saveGState()
            g.translateBy(x: push * sin(rad), y: -push * cos(rad))
            let inner = (band.inner * scale).rounded(), outer = (band.outer * scale).rounded() + (innerOfTwo ? WheelMetrics.innerLift : WheelMetrics.lift) * scale * e
            if let tile = builder.tile(count: slot.count, index: slot.index, inner: inner, outer: outer) {
                g.addPath(tile); g.setFillColor(p.tile.cgColor); g.fillPath(using: .evenOdd)
                if t > 0.001 {
                    g.saveGState()
                    g.addPath(tile); g.clip(using: .evenOdd)
                    let colors = [p.accentHi.cgColor, p.accent.cgColor] as CFArray
                    if let grad = CGGradient(colorsSpace: CGColorSpace(name: CGColorSpace.sRGB), colors: colors, locations: [0, 1]) {
                        g.setAlpha(t)
                        g.drawLinearGradient(grad, start: builder.polar(outer, mid), end: builder.polar(inner, mid), options: [.drawsBeforeStartLocation, .drawsAfterEndLocation])
                    }
                    g.restoreGState()
                }
                g.addPath(tile); g.setStrokeColor(p.tileLine.withAlphaComponent(p.tileLine.alphaComponent * (1 - t)).cgColor)
                g.setLineWidth(1); g.strokePath()
            }
            // label fitted to tile (WheelLabelFitter)
            let ci = layout.contentIndex(i)
            let enabled = isEnabled(i)
            let colour = enabled ? mix(p.text, p.textOnAccent, t) : p.textDim
            if i < fittedLabels.count {
                let fit = fittedLabels[i]
                let radius = (fit.radius + 2 * e) * scale
                let at = builder.polar(radius, mid)
                if fit.hasIcon, ci >= 0, let icon = content[ci].icon {
                    drawSymbol(icon, size: 20 * scale, color: colour, at: CGPoint(x: at.x, y: at.y + fit.iconDy * scale))
                }
                for k in 0..<fit.lines.count {
                    drawText(fit.lines[k], size: fit.fontSize * scale, weight: .semibold, color: colour, at: CGPoint(x: at.x, y: at.y + fit.lineDy[k] * scale))
                }
            }
            g.restoreGState()
        }
    }

    private func drawCentre(_ g: CGContext, _ p: Palette) {
        let r = WheelMetrics.centre * scale
        g.setFillColor(p.centre.cgColor); g.fillEllipse(in: ellipse(centre, r))
        g.setStrokeColor(p.centreLine.cgColor); g.setLineWidth(1); g.strokeEllipse(in: ellipse(centre, r - 0.5))

        let hovered = (hot >= 0 && hot < hotT.count) ? hot : -1
        let detailT = hovered >= 0 ? hotT[hovered] : 0
        let n = files.count
        let main = n == 0 ? "Drop" : (n == 1 ? "1 file" : "\(n) files")
        var headline: String?
        if hovered >= 0 {
            let ci = layout.contentIndex(hovered)
            headline = (ci >= 0 && ci < content.count) ? content[ci].headline : nil
        }
        drawCentreMain(main, color: p.text, alpha: headline == nil ? 1 : 1 - detailT)
        if let headline, detailT > 0 { drawCentreMain(headline, color: p.text, alpha: detailT) }
        let sub = currentRing?.title ?? ""
        var detail = ""
        if hovered >= 0 {
            let ci = layout.contentIndex(hovered)
            detail = (ci >= 0 && ci < content.count) ? content[ci].detail : ""
        }
        drawCentreDetail(sub, color: p.subtleText, alpha: 1 - detailT)
        if detailT > 0 && !detail.isEmpty {
            drawCentreDetail(detail, color: p.subtleText, alpha: detailT)
        }
        // one dot per wheel, the current one in the accent
        let gap = 9 * scale, y = centre.y + r - 9 * scale
        let x0 = centre.x - gap * CGFloat(rings.count - 1) / 2
        for i in rings.indices {
            let on = i == ringIndex, dr = (on ? 2.6 : 2.1) * scale
            g.setFillColor((on ? p.accentMark : p.dot).cgColor)
            g.fillEllipse(in: ellipse(CGPoint(x: x0 + gap * CGFloat(i), y: y), dr))
        }
    }

    private func drawCentreMain(_ text: String, color: NSColor, alpha: CGFloat) {
        guard alpha > 0.001, !text.isEmpty else { return }
        let maxW = 84 * scale
        let point = CGPoint(x: centre.x, y: centre.y - 16 * scale)
        let baseSize = WheelMetrics.centreMain * scale
        let attrs: [NSAttributedString.Key: Any] = [
            .font: NSFont.systemFont(ofSize: baseSize, weight: .semibold),
            .foregroundColor: color.withAlphaComponent(color.alphaComponent * alpha)
        ]
        let str = NSAttributedString(string: text, attributes: attrs)
        let sz = str.size()
        if sz.width <= maxW {
            str.draw(at: CGPoint(x: point.x - sz.width / 2, y: point.y - sz.height / 2))
        } else {
            let scaledSize = max(12 * scale, baseSize * (maxW / sz.width))
            let scaledAttrs: [NSAttributedString.Key: Any] = [
                .font: NSFont.systemFont(ofSize: scaledSize, weight: .semibold),
                .foregroundColor: color.withAlphaComponent(color.alphaComponent * alpha)
            ]
            let scaledStr = NSAttributedString(string: text, attributes: scaledAttrs)
            let ssz = scaledStr.size()
            scaledStr.draw(at: CGPoint(x: point.x - ssz.width / 2, y: point.y - ssz.height / 2))
        }
    }

    private func drawCentreDetail(_ text: String, color: NSColor, alpha: CGFloat) {
        guard alpha > 0.001, !text.isEmpty else { return }
        let maxW = 84 * scale
        let maxH = 32 * scale
        let baseSize = WheelMetrics.centreDetail * scale
        let targetCenterY = centre.y + 17 * scale

        let sizes: [CGFloat] = [baseSize, baseSize * 0.92, baseSize * 0.84, baseSize * 0.78]
        let style = NSMutableParagraphStyle()
        style.alignment = .center
        style.lineBreakMode = .byWordWrapping

        var chosenFont = NSFont.systemFont(ofSize: sizes[0], weight: .regular)
        var textRect = CGRect.zero

        for sz in sizes {
            let font = NSFont.systemFont(ofSize: sz, weight: .regular)
            let attrs: [NSAttributedString.Key: Any] = [.font: font, .paragraphStyle: style]
            let str = NSAttributedString(string: text, attributes: attrs)
            let b = str.boundingRect(with: CGSize(width: maxW, height: 1000),
                                     options: [.usesLineFragmentOrigin, .usesFontLeading])
            if b.height <= maxH + 0.5 {
                chosenFont = font
                textRect = b
                break
            }
        }
        if textRect.isEmpty {
            chosenFont = NSFont.systemFont(ofSize: sizes.last!, weight: .regular)
            style.lineBreakMode = .byTruncatingTail
            let attrs: [NSAttributedString.Key: Any] = [.font: chosenFont, .paragraphStyle: style]
            let str = NSAttributedString(string: text, attributes: attrs)
            textRect = str.boundingRect(with: CGSize(width: maxW, height: maxH),
                                        options: [.usesLineFragmentOrigin, .usesFontLeading])
        }

        let drawRect = CGRect(x: centre.x - maxW / 2,
                              y: targetCenterY - textRect.height / 2,
                              width: maxW,
                              height: textRect.height)

        let attrs: [NSAttributedString.Key: Any] = [
            .font: chosenFont,
            .foregroundColor: color.withAlphaComponent(color.alphaComponent * alpha),
            .paragraphStyle: style
        ]
        let str = NSAttributedString(string: text, attributes: attrs)
        str.draw(with: drawRect, options: [.usesLineFragmentOrigin, .usesFontLeading, .truncatesLastVisibleLine])
    }

    private var symbolCache: [String: NSImage] = [:]

    private func drawSymbol(_ name: String, size: CGFloat, color: NSColor, at point: CGPoint) {
        let c = color.usingColorSpace(.sRGB) ?? color
        let key = "\(name)|\(Int(size))|\(Int(c.redComponent * 255))\(Int(c.greenComponent * 255))\(Int(c.blueComponent * 255))\(Int(c.alphaComponent * 255))"
        if symbolCache.count > 600 { symbolCache.removeAll() }
        var img = symbolCache[key]
        if img == nil, let base = NSImage(systemSymbolName: name, accessibilityDescription: nil)?
            .withSymbolConfiguration(NSImage.SymbolConfiguration(pointSize: size * 0.85, weight: .medium)) {
            img = NSImage(size: base.size, flipped: false) { r in
                base.draw(in: r)
                color.set()
                r.fill(using: .sourceAtop)
                return true
            }
            symbolCache[key] = img
        }
        guard let img else { return }
        let box = img.size
        let fit = min(1, size / max(box.width, box.height))
        let w = box.width * fit, h = box.height * fit
        img.draw(in: CGRect(x: point.x - w / 2, y: point.y - h / 2, width: w, height: h), from: .zero, operation: .sourceOver, fraction: 1, respectFlipped: true, hints: nil)
    }

    private func drawText(_ text: String, size: CGFloat, weight: NSFont.Weight, color: NSColor, at point: CGPoint) {
        let attrs: [NSAttributedString.Key: Any] = [.font: NSFont.systemFont(ofSize: size, weight: weight), .foregroundColor: color]
        let str = NSAttributedString(string: text, attributes: attrs)
        let sz = str.size()
        str.draw(at: CGPoint(x: point.x - sz.width / 2, y: point.y - sz.height / 2))
    }

    // MARK: Snapshot helper

    func snapshotState(ring: Int, hotSlot: Int, dark: Bool, filesCount: Int) {
        forceDark = dark
        suppressSound = true
        setRing(ring)
        mode = .wheel
        openT = 1
        hot = hotSlot
        files = (0..<filesCount).map { URL(fileURLWithPath: "/tmp/f\($0).jpg") }
        for i in hotT.indices { hotT[i] = (i == hot) ? 1 : 0 }
    }
}
