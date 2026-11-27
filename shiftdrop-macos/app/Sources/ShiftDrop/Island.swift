import AppKit

// The panel hanging from the top edge of the screen (src/Island.*, PanelLayout.h, mac/UI_SPEC.md section 5):
// a switcher while the wheel is open, then progress, then the result. Click-through except while it shows a result.

enum IslandMetrics {
    static let maxWidth: CGFloat = 720, pad: CGFloat = 24, topGap: CGFloat = 10, maxHeight: CGFloat = 72
    static let inset: CGFloat = 8
    static let switcherHeight: CGFloat = 44, progressHeight: CGFloat = 48, resultHeight: CGFloat = 44, resultTwoLines: CGFloat = 58
    static let tabFont: CGFloat = 13, hintFont: CGFloat = 11.5, icon: CGFloat = 18
    static let tabPad: CGFloat = 12, iconGap: CGFloat = 7, hintGap: CGFloat = 14, hintEnd: CGFloat = 10
    static let mark: CGFloat = 17, lead: CGFloat = 9, markGap: CGFloat = 9, tail: CGFloat = 4
    static let button: CGFloat = 26, buttonGap: CGFloat = 6, buttonEnd: CGFloat = 2.7
    static let barHeight: CGFloat = 2.3, barBottom: CGFloat = 3
    static let successSeconds = 2.0, carrySeconds = 5.0, failureSeconds = 12.0
}

final class IslandController {
    let panel: NSPanel
    let view: IslandView

    init() {
        let size = NSSize(width: IslandMetrics.maxWidth + 2 * IslandMetrics.pad, height: IslandMetrics.maxHeight + 2 * IslandMetrics.pad)
        view = IslandView(frame: NSRect(origin: .zero, size: size))
        panel = NSPanel(contentRect: NSRect(origin: .zero, size: size), styleMask: [.borderless, .nonactivatingPanel], backing: .buffered, defer: false)
        panel.contentView = view
        panel.isOpaque = false
        panel.backgroundColor = .clear
        panel.hasShadow = false
        panel.level = .statusBar
        panel.isFloatingPanel = true
        panel.hidesOnDeactivate = false
        panel.ignoresMouseEvents = true
        panel.collectionBehavior = [.canJoinAllSpaces, .fullScreenAuxiliary, .stationary]
        view.onWantsMouse = { [weak panel] wants in panel?.ignoresMouseEvents = !wants }
        view.onHidden = { [weak panel] in panel?.orderOut(nil) }
    }

    private func place(on screen: NSScreen?) {
        let s = screen ?? NSScreen.main ?? NSScreen.screens[0]
        let f = s.visibleFrame, size = panel.frame.size
        panel.setFrameOrigin(NSPoint(x: f.midX - size.width / 2, y: f.maxY - size.height + IslandMetrics.pad - IslandMetrics.topGap))
        panel.orderFrontRegardless()
    }

    func showSwitcher(rings: [WheelRing], index: Int, screen: NSScreen?) {
        place(on: screen)
        view.showSwitcher(rings: rings, index: index)
    }
    func setRing(_ index: Int) { view.setRing(index) }
    func hideSwitcher() { view.hideSwitcher() }

    func showProgress(title: String, total: Int, onCancel: (() -> Void)?, screen: NSScreen?) {
        place(on: screen)
        view.showProgress(title: title, total: total, onCancel: onCancel)
    }
    func setProgress(_ done: Double) { view.setProgress(done) }
    func showResult(_ r: JobResult, actions: IslandView.Actions, screen: NSScreen?) {
        place(on: screen)
        view.showResult(r, actions: actions)
    }
}

final class IslandView: NSView {
    struct Actions {
        var open: (() -> Void)?
        var reveal: (() -> Void)?
        var copy: (() -> Void)?
        var float: (() -> Void)?
        var any: Bool { open != nil || reveal != nil || copy != nil || float != nil }
    }
    enum State { case hidden, switcher, progress, result }
    enum Button: CaseIterable { case open, reveal, copy, float, dismiss }

    override var isFlipped: Bool { true }
    var onWantsMouse: ((Bool) -> Void)?
    var onHidden: (() -> Void)?

    private var state = State.hidden
    private var rings: [WheelRing] = []
    private var ringIndex = 0
    private var title = "", subtitle = ""
    private var total = 0
    private var done = 0.0
    private var outcome = JobResult.Outcome.success
    private var actions = Actions()
    private var onCancel: (() -> Void)?
    private var cancelled = false
    private var buttons: [Button] = []
    private var buttonRects: [Button: CGRect] = [:]
    private var pillRect = CGRect.zero
    private var hoverButton: Button?
    private var hoverPill = false
    private var resultSince = Date()

    // animation
    private var timer: Timer?
    private var last = CACurrentMediaTime()
    private var presence: CGFloat = 0, presenceT: CGFloat = 0
    private var retracting = false
    private var w: CGFloat = 0, h: CGFloat = 0
    private var contentT: CGFloat = 1
    private var tabPos: CGFloat = 0
    private var bar: CGFloat = 0
    private var spin: CGFloat = 0

    private var pal: Palette { Palette.make(dark: effectiveAppearance.bestMatch(from: [.darkAqua, .aqua]) == .darkAqua) }
    private var reduceMotion: Bool { NSWorkspace.shared.accessibilityDisplayShouldReduceMotion }

    override init(frame: NSRect) {
        super.init(frame: frame)
        addTrackingArea(NSTrackingArea(rect: .zero, options: [.mouseMoved, .mouseEnteredAndExited, .activeAlways, .inVisibleRect], owner: self))
    }
    required init?(coder: NSCoder) { fatalError() }

    // MARK: State changes

    private func present(_ s: State) {
        let wasHidden = state == .hidden || retracting
        state = s
        retracting = false
        contentT = 0
        if wasHidden { presenceT = 0; presence = 0; w = targetSize().width; h = targetSize().height }
        startTimer()
    }

    func showSwitcher(rings: [WheelRing], index: Int) {
        guard state != .progress && state != .result else { return }   // a running job or a result keeps the panel
        self.rings = rings; ringIndex = index; tabPos = CGFloat(index)
        onWantsMouse?(false)
        present(.switcher)
    }

    func setRing(_ i: Int) { ringIndex = i }

    func hideSwitcher() { if state == .switcher { retract() } }

    func showProgress(title: String, total: Int, onCancel: (() -> Void)?) {
        self.title = title; self.total = max(total, 1); done = 0; bar = 0; cancelled = false; self.onCancel = onCancel
        onWantsMouse?(onCancel != nil)
        present(.progress)
    }

    func setProgress(_ d: Double) { done = d }

    func showResult(_ r: JobResult, actions: Actions) {
        title = r.title; subtitle = r.subtitle; outcome = r.outcome; self.actions = actions
        buttons = []
        if actions.open != nil { buttons.append(.open) }
        if actions.reveal != nil { buttons.append(.reveal) }
        if actions.copy != nil { buttons.append(.copy) }
        if actions.float != nil { buttons.append(.float) }
        if !buttons.isEmpty { buttons.append(.dismiss) }
        resultSince = Date(); hoverPill = false; hoverButton = nil
        onWantsMouse?(true)
        present(.result)
    }

    private func retract() {
        guard state != .hidden else { return }
        retracting = true
        onWantsMouse?(false)
        startTimer()
    }

    // MARK: Animation

    private func startTimer() {
        guard timer == nil else { return }
        last = CACurrentMediaTime()
        timer = Timer.scheduledTimer(withTimeInterval: 1.0 / 60, repeats: true) { [weak self] _ in self?.tick() }
        RunLoop.main.add(timer!, forMode: .common)
    }

    private func approach(_ v: CGFloat, _ target: CGFloat, _ dt: CGFloat, tau: CGFloat) -> CGFloat { v + (target - v) * (1 - exp(-dt / tau)) }

    private func tick() {
        let now = CACurrentMediaTime(), dt = min(0.05, CGFloat(now - last))
        last = now
        let motion = !reduceMotion
        if retracting {
            presenceT = max(0, presenceT - dt / 0.22)
            presence = motion ? easeOutCubic(presenceT) : 0
            if presenceT <= 0 { stopAndHide(); return }
        } else {
            presenceT = min(1, presenceT + dt / 0.52)
            presence = motion ? spring(presenceT) : 1
        }
        let t = targetSize()
        w = motion ? approach(w, t.width, dt, tau: 0.06) : t.width
        h = motion ? approach(h, t.height, dt, tau: 0.06) : t.height
        contentT = min(1, contentT + dt / 0.18)
        tabPos = motion ? approach(tabPos, CGFloat(ringIndex), dt, tau: 0.055) : CGFloat(ringIndex)
        let frac = state == .progress ? CGFloat(done) / CGFloat(total) : 0
        bar = motion ? approach(bar, frac, dt, tau: 0.12) : frac
        spin = (spin + dt * 360 * 1.1).truncatingRemainder(dividingBy: 360)

        if state == .result {   // a success tucks away; a failure waits longer; both stay while the mouse is over them
            let limit = outcome == .failure || outcome == .partial ? IslandMetrics.failureSeconds : (actions.any ? IslandMetrics.carrySeconds : IslandMetrics.successSeconds)
            if !hoverPill && Date().timeIntervalSince(resultSince) > limit { retract() }
        }
        needsDisplay = true
    }

    private func stopAndHide() {
        timer?.invalidate(); timer = nil
        state = .hidden; retracting = false
        onHidden?()
        needsDisplay = true
    }

    // MARK: Layout

    private func textWidth(_ s: String, _ size: CGFloat, _ weight: NSFont.Weight = .regular) -> CGFloat {
        NSAttributedString(string: s, attributes: [.font: NSFont.systemFont(ofSize: size, weight: weight)]).size().width.rounded(.up)
    }

    private var hint: String { "Tab, Scroll or 1-\(max(rings.count, 1))" }

    private func tabWidths() -> [CGFloat] {
        rings.map { IslandMetrics.tabPad * 2 + IslandMetrics.icon + IslandMetrics.iconGap + textWidth($0.title, IslandMetrics.tabFont, .semibold) }
    }

    private func hasSecondLine() -> Bool { !subtitle.isEmpty }

    private func targetSize() -> CGSize {
        let m = IslandMetrics.self
        switch state {
        case .switcher, .hidden:
            let tabs = tabWidths().reduce(0, +)
            let width = m.inset * 2 + tabs + (rings.count > 1 ? m.hintGap + textWidth(hint, m.hintFont) + m.hintEnd : 0)
            return CGSize(width: min(width, m.maxWidth), height: m.switcherHeight)
        case .progress:
            let width = m.inset + m.lead + m.mark + m.markGap + textWidth(title, 13, .semibold) + m.tail + (onCancel != nil ? m.button + m.buttonEnd : 0) + m.inset + 16
            return CGSize(width: min(max(width, 220), m.maxWidth), height: m.progressHeight)
        case .result:
            let textW = max(textWidth(title, 13, .semibold), textWidth(subtitle, m.hintFont))
            let btn = buttons.isEmpty ? 0 : CGFloat(buttons.count) * m.button + CGFloat(buttons.count - 1) * m.buttonGap + m.buttonEnd + 10
            let width = m.inset + m.lead + m.mark + m.markGap + textW + m.tail + btn + m.inset + 6
            return CGSize(width: min(max(width, 200), m.maxWidth), height: hasSecondLine() ? m.resultTwoLines : m.resultHeight)
        }
    }

    // MARK: Drawing

    override func draw(_ dirtyRect: NSRect) {
        guard state != .hidden, let g = NSGraphicsContext.current?.cgContext else { return }
        let p = pal, m = IslandMetrics.self
        let top = m.pad + (presence - 1) * (h + m.pad + m.topGap)
        pillRect = CGRect(x: bounds.midX - w / 2, y: top, width: w, height: h)
        let radius = min(h / 2, 24)
        let path = CGPath(roundedRect: pillRect, cornerWidth: radius, cornerHeight: radius, transform: nil)

        g.saveGState()
        g.setShadow(offset: CGSize(width: 0, height: -5), blur: 18, color: p.shadow.cgColor)
        g.addPath(path); g.setFillColor(rgb(p.dark ? 0x1F2127 : 0xFFFFFF).cgColor); g.fillPath()
        g.restoreGState()
        g.addPath(path)
        g.setStrokeColor((outcome == .failure && state == .result ? rgb(p.dark ? 0xFF7A7A : 0xC02B2B, 0.9) : rgb(p.dark ? 0xFFFFFF : 0x15171E, p.dark ? 0.10 : 0.08)).cgColor)
        g.setLineWidth(outcome == .failure && state == .result ? 1.5 : 1); g.strokePath()

        g.saveGState()
        g.addPath(path); g.clip()
        g.setAlpha(contentT)
        switch state {
        case .switcher: drawSwitcher(g, p)
        case .progress: drawProgress(g, p)
        case .result: drawResult(g, p)
        case .hidden: break
        }
        g.restoreGState()
    }

    private func text(_ s: String, size: CGFloat, weight: NSFont.Weight = .regular, color: NSColor, x: CGFloat, midY: CGFloat, maxWidth: CGFloat? = nil) {
        let style = NSMutableParagraphStyle(); style.lineBreakMode = .byTruncatingTail
        let str = NSAttributedString(string: s, attributes: [.font: NSFont.systemFont(ofSize: size, weight: weight), .foregroundColor: color, .paragraphStyle: style])
        let sz = str.size()
        str.draw(in: CGRect(x: x, y: midY - sz.height / 2, width: maxWidth ?? sz.width + 2, height: sz.height))
    }

    private func symbol(_ name: String, size: CGFloat, color: NSColor, in box: CGRect) {
        guard let base = NSImage(systemSymbolName: name, accessibilityDescription: nil)?
            .withSymbolConfiguration(NSImage.SymbolConfiguration(pointSize: size, weight: .semibold)) else { return }
        let tinted = NSImage(size: base.size, flipped: false) { r in base.draw(in: r); color.set(); r.fill(using: .sourceAtop); return true }
        let fit = min(1, min(box.width / base.size.width, box.height / base.size.height))
        let sz = CGSize(width: base.size.width * fit, height: base.size.height * fit)
        tinted.draw(in: CGRect(x: box.midX - sz.width / 2, y: box.midY - sz.height / 2, width: sz.width, height: sz.height), from: .zero, operation: .sourceOver, fraction: 1, respectFlipped: true, hints: nil)
    }

    private func drawSwitcher(_ g: CGContext, _ p: Palette) {
        let m = IslandMetrics.self
        let widths = tabWidths()
        var x = pillRect.minX + m.inset
        let hi = CGRect(x: 0, y: pillRect.minY + 8, width: 0, height: pillRect.height - 16)
        // sliding highlight: interpolate between the tabs' rects
        let lo = Int(floor(tabPos)), fr = tabPos - floor(tabPos)
        func tabX(_ i: Int) -> CGFloat { pillRect.minX + m.inset + widths.prefix(max(0, min(i, widths.count))).reduce(0, +) }
        if !widths.isEmpty {
            let a = max(0, min(lo, widths.count - 1)), b = max(0, min(lo + 1, widths.count - 1))
            let rx = tabX(a) + (tabX(b) - tabX(a)) * fr, rw = widths[a] + (widths[b] - widths[a]) * fr
            let r = CGRect(x: rx, y: hi.minY, width: rw, height: hi.height)
            g.addPath(CGPath(roundedRect: r, cornerWidth: r.height / 2, cornerHeight: r.height / 2, transform: nil))
            g.setFillColor(p.accent.cgColor); g.fillPath()
        }
        for (i, ring) in rings.enumerated() {
            let on = abs(CGFloat(i) - tabPos) < 0.5
            let color = on ? p.textOnAccent : (p.dark ? rgb(0x9DA2AC) : rgb(0x5E6370))
            symbol(ring.icon, size: m.icon - 3, color: color, in: CGRect(x: x + m.tabPad, y: pillRect.midY - m.icon / 2, width: m.icon, height: m.icon))
            text(ring.title, size: m.tabFont, weight: .semibold, color: color, x: x + m.tabPad + m.icon + m.iconGap, midY: pillRect.midY)
            x += widths[i]
        }
        if rings.count > 1 {
            text(hint, size: m.hintFont, color: p.dark ? rgb(0x9DA2AC) : rgb(0x5E6370), x: x + m.hintGap, midY: pillRect.midY)
        }
    }

    private func drawProgress(_ g: CGContext, _ p: Palette) {
        let m = IslandMetrics.self
        let dim = p.dark ? rgb(0x9DA2AC) : rgb(0x5E6370), tx = p.dark ? rgb(0xEEF0F3) : rgb(0x1B1D23)
        let cy = pillRect.midY - 1
        let mx = pillRect.minX + m.inset + m.lead, c = CGPoint(x: mx + m.mark / 2, y: cy)
        // spinner: a track ring and an accent arc
        g.setLineWidth(2); g.setLineCap(.round)
        g.setStrokeColor(rgb(p.dark ? 0x353A44 : 0xE3E5EA).cgColor)
        g.strokeEllipse(in: CGRect(x: c.x - m.mark / 2 + 1, y: c.y - m.mark / 2 + 1, width: m.mark - 2, height: m.mark - 2))
        g.setStrokeColor(p.accentMark.cgColor)
        let a0 = (spin - 90) * .pi / 180
        g.addArc(center: c, radius: m.mark / 2 - 1, startAngle: a0, endAngle: a0 + .pi * 0.9, clockwise: false)
        g.strokePath()
        let shown = cancelled ? "Cancelling\u{2026}" : title
        let right = pillRect.maxX - m.inset - (onCancel != nil ? m.button + m.buttonEnd : 0) - m.tail
        let tx0 = mx + m.mark + m.markGap
        text(shown, size: 13, weight: .semibold, color: tx, x: tx0, midY: cy, maxWidth: max(0, right - tx0))
        if let cancel = onCancel, !cancelled {
            _ = cancel
            let r = CGRect(x: pillRect.maxX - m.inset - m.buttonEnd - m.button, y: cy - m.button / 2, width: m.button, height: m.button)
            buttonRects[.dismiss] = r
            if hoverButton == .dismiss { g.setFillColor(rgb(p.dark ? 0xFFFFFF : 0x15171E, p.dark ? 0.07 : 0.05).cgColor); g.fillEllipse(in: r) }
            symbol("xmark", size: 11, color: dim, in: r)
        }
        // progress bar along the bottom edge
        let bw = (pillRect.width - 2 * m.inset - 8) * bar
        let br = CGRect(x: pillRect.minX + m.inset + 4, y: pillRect.maxY - m.barBottom - m.barHeight, width: bw, height: m.barHeight)
        g.setFillColor(p.accentMark.cgColor)
        g.addPath(CGPath(roundedRect: br, cornerWidth: m.barHeight / 2, cornerHeight: m.barHeight / 2, transform: nil)); g.fillPath()
    }

    private func drawResult(_ g: CGContext, _ p: Palette) {
        let m = IslandMetrics.self
        let dim = p.dark ? rgb(0x9DA2AC) : rgb(0x5E6370), tx = p.dark ? rgb(0xEEF0F3) : rgb(0x1B1D23)
        let cy = pillRect.midY
        let mx = pillRect.minX + m.inset + m.lead
        let (name, color): (String, NSColor)
        switch outcome {
        case .success: (name, color) = ("checkmark.circle.fill", p.accentMark)
        case .partial: (name, color) = ("exclamationmark.triangle.fill", rgb(p.dark ? 0xF5C04A : 0x8A5A00))
        case .failure: (name, color) = ("exclamationmark.circle.fill", rgb(p.dark ? 0xFF7A7A : 0xC02B2B))
        }
        symbol(name, size: m.mark + 3, color: color, in: CGRect(x: mx, y: cy - m.mark / 2 - 1, width: m.mark + 2, height: m.mark + 2))
        let tx0 = mx + m.mark + m.markGap
        let btnW = buttons.isEmpty ? 0 : CGFloat(buttons.count) * m.button + CGFloat(buttons.count - 1) * m.buttonGap + m.buttonEnd + 6
        let right = pillRect.maxX - m.inset - btnW - m.tail
        let line2 = hoverPill && hoverButton == nil ? (actions.reveal != nil ? "Click to show in folder" : "Click to dismiss") : subtitle
        if line2.isEmpty {
            text(title, size: 13, weight: .semibold, color: tx, x: tx0, midY: cy, maxWidth: max(0, right - tx0))
        } else {
            text(title, size: 13, weight: .semibold, color: tx, x: tx0, midY: cy - 9, maxWidth: max(0, right - tx0))
            text(line2, size: m.hintFont, color: dim, x: tx0, midY: cy + 9, maxWidth: max(0, right - tx0))
        }
        buttonRects = [:]
        var bx = pillRect.maxX - m.inset - m.buttonEnd - m.button
        for b in buttons.reversed() {
            let r = CGRect(x: bx, y: cy - m.button / 2, width: m.button, height: m.button)
            buttonRects[b] = r
            if hoverButton == b { g.setFillColor(rgb(p.dark ? 0xFFFFFF : 0x15171E, p.dark ? 0.10 : 0.07).cgColor); g.fillEllipse(in: r) }
            let icon: String
            switch b {
            case .open: icon = "arrow.up.forward.app"
            case .reveal: icon = "folder"
            case .copy: icon = "doc.on.doc"
            case .float: icon = "tray.and.arrow.down"
            case .dismiss: icon = "xmark"
            }
            symbol(icon, size: 12, color: hoverButton == b ? tx : dim, in: r.insetBy(dx: 6, dy: 6))
            bx -= m.button + m.buttonGap
        }
    }

    /// Snapshot helper: shows `kind` fully settled, without timers.
    func debugShow(_ kind: String) {
        switch kind {
        case "switcher":
            rings = WheelContents.rings(for: [URL(fileURLWithPath: "/tmp/a.jpg")]); ringIndex = 1; tabPos = 1; state = .switcher
        case "progress":
            title = "Converting 4 files\u{2026}"; total = 4; done = 2.5; bar = 0.62; onCancel = {}; state = .progress
        case "result":
            title = "1 file converted to WebP"; subtitle = "IMG_2041 (converted).webp"; outcome = .success
            buttons = [.open, .reveal, .copy, .dismiss]; actions = Actions(open: {}, reveal: {}, copy: {}); state = .result
        case "failure":
            title = "Couldn\u{2019}t do it"; subtitle = "notes.txt isn\u{2019}t a picture"; outcome = .failure; buttons = []; state = .result
        default: break
        }
        presence = 1; contentT = 1
        let t = targetSize(); w = t.width; h = t.height
    }

    // MARK: Mouse (only while a result or a cancellable job is shown)

    override func acceptsFirstMouse(for event: NSEvent?) -> Bool { true }

    private func button(at point: CGPoint) -> Button? { buttonRects.first { $0.value.contains(point) }?.key }

    override func mouseMoved(with event: NSEvent) {
        let pt = convert(event.locationInWindow, from: nil)
        hoverPill = pillRect.contains(pt)
        hoverButton = button(at: pt)
        needsDisplay = true
    }
    override func mouseExited(with event: NSEvent) { hoverPill = false; hoverButton = nil }

    override func mouseUp(with event: NSEvent) {
        let pt = convert(event.locationInWindow, from: nil)
        guard pillRect.contains(pt) else { return }
        let b = button(at: pt)
        if state == .progress {
            if b == .dismiss, !cancelled { cancelled = true; onCancel?() }
            return
        }
        guard state == .result else { return }
        switch b {
        case .open: actions.open?(); retract()
        case .reveal: actions.reveal?(); retract()
        case .copy: actions.copy?(); title = "Copied"; subtitle = ""
        case .float: actions.float?(); title = "Added to Float shelf"; subtitle = ""
        case .dismiss: retract()
        case nil: actions.reveal?(); retract()   // a click on the result shows the files in Finder
        }
    }
}
