import AppKit

// Port of src/shelf/ShelfWindow.*, ShelfGeometry.*, ShelfStore.*:
// The Float Shelf: an always-on-top panel that collects and holds files/folders
// until you drag them out into any app (Finder, Desktop, Mail, Slack, browsers).
//
// Accepts drops from Finder, browsers, the wheel's "Float" action, and the Island's "Float" button.
// Drag individual items or "Drag all" simultaneously.
// Dockable at screen edge with auto-collapse into a slim tab.

final class ShelfStore {
    static let shared = ShelfStore()

    private(set) var urls: [URL] = []
    var autoCollapse = false

    private var storeURL: URL {
        let appSupport = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask).first!
        let dir = appSupport.appendingPathComponent("ShiftDrop", isDirectory: true)
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        return dir.appendingPathComponent("shelf.json")
    }

    init() {
        load()
    }

    func add(_ newURLs: [URL]) {
        var existing = urls
        for u in newURLs {
            existing.removeAll { $0.standardizedFileURL.path == u.standardizedFileURL.path }
            existing.insert(u, at: 0)
        }
        // Clean up any files that were deleted from disk
        urls = existing.filter { FileManager.default.fileExists(atPath: $0.path) }
        save()
    }

    func remove(at index: Int) {
        guard index >= 0 && index < urls.count else { return }
        urls.remove(at: index)
        save()
    }

    func remove(_ url: URL) {
        urls.removeAll { $0.standardizedFileURL.path == url.standardizedFileURL.path }
        save()
    }

    func clear() {
        urls.removeAll()
        save()
    }

    private struct SavedData: Codable {
        var paths: [String]
        var autoCollapse: Bool?
    }

    func save() {
        let paths = urls.map(\.path)
        let data = SavedData(paths: paths, autoCollapse: autoCollapse)
        if let encoded = try? JSONEncoder().encode(data) {
            try? encoded.write(to: storeURL, options: .atomic)
        }
    }

    func load() {
        guard let data = try? Data(contentsOf: storeURL),
              let saved = try? JSONDecoder().decode(SavedData.self, from: data) else { return }
        self.autoCollapse = saved.autoCollapse ?? false
        self.urls = saved.paths.map { URL(fileURLWithPath: $0) }.filter { FileManager.default.fileExists(atPath: $0.path) }
    }
}

// MARK: - Shelf Panel & View

final class FloatShelf: NSObject, NSDraggingSource {
    static let shared = FloatShelf()

    private var panel: NSPanel?
    private var view: FloatShelfView?
    private(set) var isVisible = false

    var onVisibilityChanged: ((Bool) -> Void)?

    private func ensureUI() {
        guard panel == nil else { return }
        let width: CGFloat = 280
        let v = FloatShelfView(frame: NSRect(x: 0, y: 0, width: width, height: 200))
        let p = NSPanel(contentRect: v.frame,
                        styleMask: [.borderless, .nonactivatingPanel],
                        backing: .buffered, defer: false)
        p.contentView = v
        p.isOpaque = false
        p.backgroundColor = .clear
        p.hasShadow = true
        p.level = .floating
        p.isFloatingPanel = true
        p.hidesOnDeactivate = false
        p.collectionBehavior = [.canJoinAllSpaces, .fullScreenAuxiliary, .stationary]

        v.shelf = self
        self.view = v
        self.panel = p
        v.updateLayout()
        placeOnScreen()
    }

    private func placeOnScreen() {
        guard let p = panel, let screen = NSScreen.main ?? NSScreen.screens.first else { return }
        let f = screen.visibleFrame
        let size = p.frame.size
        // Dock to right edge, 140 pt down from top
        let x = f.maxX - size.width - 12
        let y = f.maxY - size.height - 120
        p.setFrameOrigin(NSPoint(x: x, y: y))
    }

    func show() {
        if !Thread.isMainThread {
            DispatchQueue.main.async { self.show() }
            return
        }
        ensureUI()
        isVisible = true
        panel?.orderFrontRegardless()
        view?.updateLayout()
        onVisibilityChanged?(true)
    }

    func hide() {
        if !Thread.isMainThread {
            DispatchQueue.main.async { self.hide() }
            return
        }
        isVisible = false
        panel?.orderOut(nil)
        onVisibilityChanged?(false)
    }

    func toggle() {
        if !Thread.isMainThread {
            DispatchQueue.main.async { self.toggle() }
            return
        }
        if isVisible { hide() } else { show() }
    }

    func add(_ urls: [URL]) {
        ShelfStore.shared.add(urls)
        if !Thread.isMainThread {
            DispatchQueue.main.async { self.addUI() }
            return
        }
        addUI()
    }

    private func addUI() {
        ensureUI()
        view?.updateLayout()
        if !isVisible { show() }
    }

    // NSDraggingSource
    func draggingSession(_ session: NSDraggingSession, sourceOperationMaskFor context: NSDraggingContext) -> NSDragOperation {
        .copy
    }
}

// MARK: - Custom Shelf View

final class FloatShelfView: NSView {
    weak var shelf: FloatShelf?

    override var isFlipped: Bool { true }

    private var isDraggingOver = false
    var hoveredRowIndex: Int?
    var hoveredButton: String? // "close", "clear", "pin", "open_row", "reveal_row", "delete_row", "drag_all"
    private var dragStartLocation: NSPoint?
    private var draggedItemIndex: Int?
    private var isDraggingAll = false

    private let store = ShelfStore.shared
    private var pal: Palette { Palette.make(dark: effectiveAppearance.bestMatch(from: [.darkAqua, .aqua]) == .darkAqua) }

    // Hit test regions
    private var closeButtonRect = CGRect.zero
    private var clearButtonRect = CGRect.zero
    private var pinButtonRect = CGRect.zero
    private var dragAllButtonRect = CGRect.zero
    private var rowRects: [CGRect] = []
    private var rowOpenRects: [CGRect] = []
    private var rowRevealRects: [CGRect] = []
    private var rowDeleteRects: [CGRect] = []

    override init(frame: NSRect) {
        super.init(frame: frame)
        registerForDraggedTypes([.fileURL])
        addTrackingArea(NSTrackingArea(rect: .zero, options: [.mouseMoved, .mouseEnteredAndExited, .activeAlways, .inVisibleRect], owner: self))
    }

    required init?(coder: NSCoder) { fatalError() }

    func updateLayout() {
        let itemCount = store.urls.count
        let headerH: CGFloat = 44
        let rowH: CGFloat = 48
        let footerH: CGFloat = itemCount >= 2 ? 44 : 12
        let emptyH: CGFloat = 110

        let contentH = itemCount == 0 ? emptyH : (CGFloat(min(itemCount, 5)) * rowH)
        let totalH = headerH + contentH + footerH
        let w: CGFloat = window?.frame.width ?? bounds.width

        if let window = window {
            var wf = window.frame
            let diff = wf.height - totalH
            wf.origin.y += diff
            wf.size.height = totalH
            window.setFrame(wf, display: true, animate: false)
            window.invalidateShadow()
        }
        self.frame = NSRect(x: 0, y: 0, width: w > 0 ? w : 280, height: totalH)
        removeAllToolTips()
        addToolTip(bounds, owner: self, userData: nil)
        needsDisplay = true
    }

    // MARK: - Drawing

    override func draw(_ dirtyRect: NSRect) {
        guard let g = NSGraphicsContext.current?.cgContext else { return }
        let p = pal
        let b = bounds
        let radius: CGFloat = 14
        let path = CGPath(roundedRect: b.insetBy(dx: 1, dy: 1), cornerWidth: radius, cornerHeight: radius, transform: nil)

        // Panel background with subtle shadow
        g.saveGState()
        g.addPath(path)
        g.setFillColor((p.dark ? rgb(0x1F2127) : rgb(0xFFFFFF)).cgColor)
        g.fillPath()

        // Border: accent when dragging over, subtle line otherwise
        g.addPath(path)
        if isDraggingOver {
            g.setStrokeColor(p.accent.cgColor)
            g.setLineWidth(2.5)
        } else {
            g.setStrokeColor((p.dark ? rgb(0xFFFFFF, 0.10) : rgb(0x15171E, 0.08)).cgColor)
            g.setLineWidth(1)
        }
        g.strokePath()
        g.restoreGState()

        drawHeader(g, p)

        if store.urls.isEmpty {
            drawEmptyState(g, p)
        } else {
            drawRows(g, p)
            if store.urls.count >= 2 {
                drawFooter(g, p)
            }
        }

        drawActiveTooltip(g, p)
    }

    private func drawHeader(_ g: CGContext, _ p: Palette) {
        let mX: CGFloat = 14
        let cy: CGFloat = 22

        // Shelf Tray Icon
        symbol("tray.and.arrow.down", size: 16, color: p.accent, in: CGRect(x: mX, y: cy - 9, width: 18, height: 18))

        // Title
        let titleAttrs: [NSAttributedString.Key: Any] = [
            .font: NSFont.systemFont(ofSize: 13, weight: .bold),
            .foregroundColor: p.text
        ]
        let str = NSAttributedString(string: "Float Shelf", attributes: titleAttrs)
        str.draw(at: CGPoint(x: mX + 22, y: cy - str.size().height / 2))

        // Count Badge
        let count = store.urls.count
        if count > 0 {
            let badgeText = "\(count)"
            let bAttrs: [NSAttributedString.Key: Any] = [
                .font: NSFont.systemFont(ofSize: 10, weight: .bold),
                .foregroundColor: p.textOnAccent
            ]
            let bStr = NSAttributedString(string: badgeText, attributes: bAttrs)
            let bSz = bStr.size()
            let bW = max(bSz.width + 10, 18)
            let bRect = CGRect(x: mX + 26 + str.size().width, y: cy - 9, width: bW, height: 18)
            let bPath = CGPath(roundedRect: bRect, cornerWidth: 9, cornerHeight: 9, transform: nil)
            g.addPath(bPath)
            g.setFillColor(p.accent.cgColor)
            g.fillPath()
            bStr.draw(at: CGPoint(x: bRect.midX - bSz.width / 2, y: bRect.midY - bSz.height / 2))
        }

        // Header Buttons (Right side): Close (×), Clear All, Pin
        var rightX = bounds.maxX - 12 - 20

        // Close Button
        closeButtonRect = CGRect(x: rightX, y: cy - 10, width: 20, height: 20)
        drawButtonIcon("xmark", in: closeButtonRect, hovered: hoveredButton == "close", p: p, g: g)
        rightX -= 26

        // Clear All Button (only if not empty)
        if !store.urls.isEmpty {
            clearButtonRect = CGRect(x: rightX, y: cy - 10, width: 20, height: 20)
            drawButtonIcon("trash", in: clearButtonRect, hovered: hoveredButton == "clear", p: p, g: g)
            rightX -= 26
        } else {
            clearButtonRect = .zero
        }

        // Pin Button
        pinButtonRect = CGRect(x: rightX, y: cy - 10, width: 20, height: 20)
        let pinIcon = store.autoCollapse ? "pin" : "pin.fill"
        drawButtonIcon(pinIcon, in: pinButtonRect, hovered: hoveredButton == "pin", p: p, g: g, tint: store.autoCollapse ? nil : p.accent)
    }

    private func drawEmptyState(_ g: CGContext, _ p: Palette) {
        let box = CGRect(x: 14, y: 44, width: bounds.width - 28, height: 96)
        let path = CGPath(roundedRect: box, cornerWidth: 10, cornerHeight: 10, transform: nil)

        g.saveGState()
        g.addPath(path)
        g.setLineDash(phase: 0, lengths: [4, 4])
        g.setStrokeColor((p.dark ? rgb(0xFFFFFF, 0.15) : rgb(0x15171E, 0.12)).cgColor)
        g.strokePath()

        // Icon
        symbol("plus.circle", size: 22, color: p.dark ? rgb(0x9DA2AC) : rgb(0x6B7180), in: CGRect(x: box.midX - 11, y: box.minY + 16, width: 22, height: 22))

        // Text
        let tAttrs: [NSAttributedString.Key: Any] = [
            .font: NSFont.systemFont(ofSize: 12, weight: .semibold),
            .foregroundColor: p.text
        ]
        let t1 = NSAttributedString(string: "Drop files here to hold them", attributes: tAttrs)
        t1.draw(at: CGPoint(x: box.midX - t1.size().width / 2, y: box.minY + 46))

        let subAttrs: [NSAttributedString.Key: Any] = [
            .font: NSFont.systemFont(ofSize: 10.5, weight: .regular),
            .foregroundColor: p.subtleText
        ]
        let t2 = NSAttributedString(string: "Drag out to any app anytime", attributes: subAttrs)
        t2.draw(at: CGPoint(x: box.midX - t2.size().width / 2, y: box.minY + 66))
        g.restoreGState()
    }

    private func drawRows(_ g: CGContext, _ p: Palette) {
        let rowH: CGFloat = 48
        var y: CGFloat = 44

        rowRects.removeAll()
        rowOpenRects.removeAll()
        rowRevealRects.removeAll()
        rowDeleteRects.removeAll()

        for (i, url) in store.urls.prefix(5).enumerated() {
            let rowBox = CGRect(x: 10, y: y, width: bounds.width - 20, height: rowH)
            rowRects.append(rowBox)

            let isHovered = hoveredRowIndex == i

            // Row background on hover
            if isHovered {
                let rPath = CGPath(roundedRect: rowBox, cornerWidth: 8, cornerHeight: 8, transform: nil)
                g.addPath(rPath)
                g.setFillColor((p.dark ? rgb(0xFFFFFF, 0.08) : rgb(0x15171E, 0.05)).cgColor)
                g.fillPath()
            }

            // Thumbnail / Icon
            let iconBox = CGRect(x: rowBox.minX + 8, y: rowBox.midY - 14, width: 28, height: 28)
            let icon = NSWorkspace.shared.icon(forFile: url.path)
            icon.draw(in: iconBox, from: .zero, operation: .sourceOver, fraction: 1.0, respectFlipped: true, hints: nil)

            // Text
            let textLeft = iconBox.maxX + 10
            let textRight = isHovered ? rowBox.maxX - 68 : rowBox.maxX - 10
            let maxTextW = max(textRight - textLeft, 60)

            let nameStyle = NSMutableParagraphStyle()
            nameStyle.lineBreakMode = .byTruncatingMiddle
            let nameAttrs: [NSAttributedString.Key: Any] = [
                .font: NSFont.systemFont(ofSize: 12, weight: .semibold),
                .foregroundColor: p.text,
                .paragraphStyle: nameStyle
            ]
            let nameStr = NSAttributedString(string: url.lastPathComponent, attributes: nameAttrs)
            nameStr.draw(in: CGRect(x: textLeft, y: rowBox.midY - 14, width: maxTextW, height: 16))

            let sizeText = fileSizeString(for: url)
            let sizeAttrs: [NSAttributedString.Key: Any] = [
                .font: NSFont.systemFont(ofSize: 10, weight: .regular),
                .foregroundColor: p.subtleText
            ]
            let sizeStr = NSAttributedString(string: sizeText, attributes: sizeAttrs)
            sizeStr.draw(at: CGPoint(x: textLeft, y: rowBox.midY + 2))

            // Action buttons on hover (Reveal, Open, Delete)
            let btnY = rowBox.midY - 9
            let delBox = CGRect(x: rowBox.maxX - 22, y: btnY, width: 18, height: 18)
            let revBox = CGRect(x: rowBox.maxX - 44, y: btnY, width: 18, height: 18)
            let openBox = CGRect(x: rowBox.maxX - 66, y: btnY, width: 18, height: 18)

            rowDeleteRects.append(delBox)
            rowRevealRects.append(revBox)
            rowOpenRects.append(openBox)

            if isHovered {
                drawButtonIcon("xmark", in: delBox, hovered: hoveredButton == "del_\(i)", p: p, g: g)
                drawButtonIcon("folder", in: revBox, hovered: hoveredButton == "rev_\(i)", p: p, g: g)
                drawButtonIcon("arrow.up.forward.app", in: openBox, hovered: hoveredButton == "open_\(i)", p: p, g: g)
            }

            y += rowH
        }
    }

    private func drawFooter(_ g: CGContext, _ p: Palette) {
        let footerY = bounds.maxY - 40
        dragAllButtonRect = CGRect(x: 12, y: footerY, width: bounds.width - 24, height: 32)
        let isHover = hoveredButton == "drag_all"

        let path = CGPath(roundedRect: dragAllButtonRect, cornerWidth: 8, cornerHeight: 8, transform: nil)
        g.addPath(path)
        g.setFillColor((isHover ? p.accent.withAlphaComponent(0.2) : (p.dark ? rgb(0xFFFFFF, 0.08) : rgb(0x15171E, 0.05))).cgColor)
        g.fillPath()

        g.addPath(path)
        g.setStrokeColor((isHover ? p.accent : (p.dark ? rgb(0xFFFFFF, 0.12) : rgb(0x15171E, 0.08))).cgColor)
        g.setLineWidth(1)
        g.strokePath()

        // Drag icon
        symbol("arrow.up.and.down.and.arrow.left.and.right", size: 12, color: isHover ? p.accent : p.text,
               in: CGRect(x: dragAllButtonRect.midX - 58, y: dragAllButtonRect.midY - 6, width: 12, height: 12))

        let labelAttrs: [NSAttributedString.Key: Any] = [
            .font: NSFont.systemFont(ofSize: 11.5, weight: .semibold),
            .foregroundColor: isHover ? p.accent : p.text
        ]
        let label = NSAttributedString(string: "Drag all (\(store.urls.count) files)", attributes: labelAttrs)
        label.draw(at: CGPoint(x: dragAllButtonRect.midX - 40, y: dragAllButtonRect.midY - label.size().height / 2))
    }

    private func drawButtonIcon(_ name: String, in rect: CGRect, hovered: Bool, p: Palette, g: CGContext, tint: NSColor? = nil) {
        if hovered {
            g.setFillColor((p.dark ? rgb(0xFFFFFF, 0.12) : rgb(0x15171E, 0.08)).cgColor)
            g.fillEllipse(in: rect)
        }
        let color = tint ?? (hovered ? p.text : (p.dark ? rgb(0x9DA2AC) : rgb(0x6B7180)))
        symbol(name, size: 11, color: color, in: rect.insetBy(dx: 3.5, dy: 3.5))
    }

    // MARK: - Tooltips

    func view(_ view: NSView, stringForToolTip tag: NSView.ToolTipTag, point: NSPoint, userData data: UnsafeMutableRawPointer?) -> String {
        if closeButtonRect.contains(point) { return "Close shelf" }
        if clearButtonRect.contains(point) { return "Clear all" }
        if pinButtonRect.contains(point) { return store.autoCollapse ? "Pin open" : "Auto-collapse to edge" }
        for r in rowDeleteRects {
            if r.contains(point) { return "Remove from shelf" }
        }
        for r in rowRevealRects {
            if r.contains(point) { return "Reveal in Finder" }
        }
        for r in rowOpenRects {
            if r.contains(point) { return "Open file" }
        }
        if dragAllButtonRect.contains(point) { return "Drag all files into any app" }
        for (i, r) in rowRects.enumerated() {
            if r.contains(point) && i < store.urls.count {
                return store.urls[i].lastPathComponent
            }
        }
        return ""
    }

    private func drawActiveTooltip(_ g: CGContext, _ p: Palette) {
        guard let btn = hoveredButton else { return }
        if btn == "close" {
            drawTooltip("Close shelf", anchor: closeButtonRect, p: p, g: g)
        } else if btn == "clear" {
            drawTooltip("Clear all", anchor: clearButtonRect, p: p, g: g)
        } else if btn == "pin" {
            drawTooltip(store.autoCollapse ? "Pin open" : "Auto-collapse", anchor: pinButtonRect, p: p, g: g)
        } else if btn == "drag_all" {
            drawTooltip("Drag all files into any app", anchor: dragAllButtonRect, p: p, g: g)
        } else if btn.hasPrefix("open_"), let idx = Int(btn.dropFirst(5)), idx < rowOpenRects.count {
            drawTooltip("Open file", anchor: rowOpenRects[idx], p: p, g: g)
        } else if btn.hasPrefix("rev_"), let idx = Int(btn.dropFirst(4)), idx < rowRevealRects.count {
            drawTooltip("Reveal in Finder", anchor: rowRevealRects[idx], p: p, g: g)
        } else if btn.hasPrefix("del_"), let idx = Int(btn.dropFirst(4)), idx < rowDeleteRects.count {
            drawTooltip("Remove from shelf", anchor: rowDeleteRects[idx], p: p, g: g)
        }
    }

    private func drawTooltip(_ text: String, anchor: CGRect, p: Palette, g: CGContext) {
        let font = NSFont.systemFont(ofSize: 10, weight: .semibold)
        let attrs: [NSAttributedString.Key: Any] = [
            .font: font,
            .foregroundColor: NSColor.white
        ]
        let str = NSAttributedString(string: text, attributes: attrs)
        let strSize = str.size()
        let padH: CGFloat = 7
        let padV: CGFloat = 3.5
        let tipW = strSize.width + padH * 2
        let tipH = strSize.height + padV * 2

        var tipX = anchor.midX - tipW / 2
        tipX = max(6, min(tipX, bounds.width - tipW - 6))

        var tipY = anchor.minY - tipH - 4
        if tipY < 6 {
            tipY = anchor.maxY + 4
        }

        let tipRect = CGRect(x: tipX, y: tipY, width: tipW, height: tipH)
        let path = CGPath(roundedRect: tipRect, cornerWidth: 5, cornerHeight: 5, transform: nil)

        g.saveGState()
        g.setShadow(offset: CGSize(width: 0, height: -1.5), blur: 3, color: rgb(0x000000, 0.40).cgColor)
        g.addPath(path)
        g.setFillColor((p.dark ? rgb(0x111317, 0.96) : rgb(0x1F2228, 0.94)).cgColor)
        g.fillPath()
        g.restoreGState()

        g.addPath(path)
        g.setStrokeColor((p.dark ? rgb(0xFFFFFF, 0.16) : rgb(0xFFFFFF, 0.22)).cgColor)
        g.setLineWidth(0.8)
        g.strokePath()

        str.draw(at: CGPoint(x: tipRect.midX - strSize.width / 2, y: tipRect.midY - strSize.height / 2))
    }

    private func symbol(_ name: String, size: CGFloat, color: NSColor, in box: CGRect) {
        guard let base = NSImage(systemSymbolName: name, accessibilityDescription: nil) else { return }
        let config = NSImage.SymbolConfiguration(pointSize: size, weight: .regular)
        let img = base.withSymbolConfiguration(config) ?? base
        let tinted = NSImage(size: img.size, flipped: true) { rect in
            color.set()
            rect.fill()
            img.draw(in: rect, from: .zero, operation: .destinationIn, fraction: 1)
            return true
        }
        let fit = min(1, min(box.width / base.size.width, box.height / base.size.height))
        let sz = CGSize(width: base.size.width * fit, height: base.size.height * fit)
        tinted.draw(in: CGRect(x: box.midX - sz.width / 2, y: box.midY - sz.height / 2, width: sz.width, height: sz.height),
                    from: .zero, operation: .sourceOver, fraction: 1, respectFlipped: true, hints: nil)
    }

    private func fileSizeString(for url: URL) -> String {
        if let values = try? url.resourceValues(forKeys: [.isDirectoryKey, .fileSizeKey]) {
            if values.isDirectory == true { return "Folder" }
            if let size = values.fileSize {
                let bytes = Int64(size)
                if bytes >= 1024 * 1024 * 1024 { return String(format: "%.1f GB", Double(bytes) / 1073741824) }
                if bytes >= 1024 * 1024 { return String(format: "%.1f MB", Double(bytes) / 1048576) }
                if bytes >= 1024 { return "\(bytes / 1024) KB" }
                return "\(bytes) B"
            }
        }
        return "Item"
    }

    // MARK: - Mouse Events & Dragging Out

    override func acceptsFirstMouse(for event: NSEvent?) -> Bool { true }

    override func mouseMoved(with event: NSEvent) {
        let pt = convert(event.locationInWindow, from: nil)
        var newHoverRow: Int? = nil
        var newHoverBtn: String? = nil

        if closeButtonRect.contains(pt) { newHoverBtn = "close" }
        else if clearButtonRect.contains(pt) { newHoverBtn = "clear" }
        else if pinButtonRect.contains(pt) { newHoverBtn = "pin" }
        else if dragAllButtonRect.contains(pt) { newHoverBtn = "drag_all" }
        else {
            for (i, r) in rowRects.enumerated() {
                if r.contains(pt) {
                    newHoverRow = i
                    if i < rowDeleteRects.count && rowDeleteRects[i].contains(pt) { newHoverBtn = "del_\(i)" }
                    else if i < rowRevealRects.count && rowRevealRects[i].contains(pt) { newHoverBtn = "rev_\(i)" }
                    else if i < rowOpenRects.count && rowOpenRects[i].contains(pt) { newHoverBtn = "open_\(i)" }
                    break
                }
            }
        }

        if newHoverRow != hoveredRowIndex || newHoverBtn != hoveredButton {
            hoveredRowIndex = newHoverRow
            hoveredButton = newHoverBtn
            needsDisplay = true
        }
    }

    override func mouseExited(with event: NSEvent) {
        hoveredRowIndex = nil
        hoveredButton = nil
        needsDisplay = true
    }

    override func mouseDown(with event: NSEvent) {
        let pt = convert(event.locationInWindow, from: nil)
        dragStartLocation = pt

        if pt.y < 44 && !closeButtonRect.contains(pt) && !clearButtonRect.contains(pt) && !pinButtonRect.contains(pt) {
            window?.performDrag(with: event)
            return
        }

        // Check if starting drag from a row
        draggedItemIndex = nil
        isDraggingAll = false

        if dragAllButtonRect.contains(pt) && store.urls.count >= 2 {
            isDraggingAll = true
            return
        }
        for (i, r) in rowRects.enumerated() {
            if r.contains(pt) {
                // Ignore if clicked on row buttons
                if (i < rowDeleteRects.count && rowDeleteRects[i].contains(pt)) ||
                   (i < rowRevealRects.count && rowRevealRects[i].contains(pt)) ||
                   (i < rowOpenRects.count && rowOpenRects[i].contains(pt)) {
                    return
                }
                draggedItemIndex = i
                return
            }
        }
    }

    override func mouseDragged(with event: NSEvent) {
        guard let start = dragStartLocation else { return }
        let current = convert(event.locationInWindow, from: nil)
        let distance = hypot(current.x - start.x, current.y - start.y)

        // Only start drag session once mouse moves beyond 4 points
        if distance > 4 {
            if isDraggingAll {
                startDraggingSession(urls: store.urls, event: event)
                dragStartLocation = nil
                isDraggingAll = false
            } else if let idx = draggedItemIndex, idx < store.urls.count {
                startDraggingSession(urls: [store.urls[idx]], event: event)
                dragStartLocation = nil
                draggedItemIndex = nil
            }
        }
    }

    override func mouseUp(with event: NSEvent) {
        let pt = convert(event.locationInWindow, from: nil)
        dragStartLocation = nil

        if closeButtonRect.contains(pt) {
            shelf?.hide()
            return
        }
        if clearButtonRect.contains(pt) {
            store.clear()
            updateLayout()
            return
        }
        if pinButtonRect.contains(pt) {
            store.autoCollapse.toggle()
            store.save()
            needsDisplay = true
            return
        }
        for (i, r) in rowRects.enumerated() {
            if r.contains(pt) && i < store.urls.count {
                let url = store.urls[i]
                if i < rowDeleteRects.count && rowDeleteRects[i].contains(pt) {
                    store.remove(at: i)
                    updateLayout()
                    return
                }
                if i < rowRevealRects.count && rowRevealRects[i].contains(pt) {
                    NSWorkspace.shared.activateFileViewerSelecting([url])
                    return
                }
                if i < rowOpenRects.count && rowOpenRects[i].contains(pt) {
                    NSWorkspace.shared.open(url)
                    return
                }
            }
        }
    }

    private func startDraggingSession(urls: [URL], event: NSEvent) {
        guard !urls.isEmpty, let shelf = shelf else { return }
        let items: [NSDraggingItem] = urls.map { url in
            let item = NSDraggingItem(pasteboardWriter: url as NSURL)
            let icon = NSWorkspace.shared.icon(forFile: url.path)
            item.setDraggingFrame(CGRect(x: 0, y: 0, width: 32, height: 32), contents: icon)
            return item
        }
        beginDraggingSession(with: items, event: event, source: shelf)
    }

    // MARK: - Dragging Destination (Accepting Drops)

    override func draggingEntered(_ sender: NSDraggingInfo) -> NSDragOperation {
        let pb = sender.draggingPasteboard
        if pb.canReadObject(forClasses: [NSURL.self], options: [.urlReadingFileURLsOnly: true]) {
            isDraggingOver = true
            needsDisplay = true
            return .copy
        }
        return []
    }

    override func draggingUpdated(_ sender: NSDraggingInfo) -> NSDragOperation {
        return isDraggingOver ? .copy : []
    }

    override func draggingExited(_ sender: NSDraggingInfo?) {
        isDraggingOver = false
        needsDisplay = true
    }

    override func prepareForDragOperation(_ sender: NSDraggingInfo) -> Bool {
        true
    }

    override func performDragOperation(_ sender: NSDraggingInfo) -> Bool {
        isDraggingOver = false
        let pb = sender.draggingPasteboard
        guard let urls = pb.readObjects(forClasses: [NSURL.self], options: [.urlReadingFileURLsOnly: true]) as? [URL], !urls.isEmpty else {
            needsDisplay = true
            return false
        }
        shelf?.add(urls)
        Sounds.success()
        return true
    }
}
