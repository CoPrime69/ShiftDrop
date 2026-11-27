import AppKit

// ShiftDrop: the Mac gesture + wheel prototype (milestone 2).
//   run:       open ShiftDrop.app   (see build_app.sh)
//   snapshot:  ShiftDrop --snapshot out.png [ring] [hotSlot] [dark|light] [files]

func writeSnapshot(path: String, ring: Int, hot: Int, dark: Bool, files: Int) {
    _ = NSApplication.shared
    let side = WheelMetrics.windowSide(for: 1.15)
    let view = WheelView(frame: NSRect(x: 0, y: 0, width: side, height: side))
    view.backdrop = dark ? rgb(0x2C3140) : rgb(0xC9CED8)
    view.scale = 1.15
    let fileURLs = (0..<files).map { URL(fileURLWithPath: "/tmp/f\($0).png") }
    let realRings = WheelContents.rings(for: fileURLs)
    if !realRings.isEmpty { view.rings = realRings }
    view.snapshotState(ring: ring, hotSlot: hot, dark: dark, filesCount: files)
    let scale: CGFloat = 2
    guard let rep = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: Int(side * scale), pixelsHigh: Int(side * scale), bitsPerSample: 8,
                                     samplesPerPixel: 4, hasAlpha: true, isPlanar: false, colorSpaceName: .deviceRGB, bytesPerRow: 0, bitsPerPixel: 0) else { return }
    rep.size = NSSize(width: side, height: side)
    view.cacheDisplay(in: view.bounds, to: rep)
    try? rep.representation(using: .png, properties: [:])?.write(to: URL(fileURLWithPath: path))
    print("wrote \(path)")
}

func writeIslandSnapshot(path: String, kind: String, dark: Bool) {
    _ = NSApplication.shared
    let size = NSSize(width: IslandMetrics.maxWidth + 2 * IslandMetrics.pad, height: IslandMetrics.maxHeight + 2 * IslandMetrics.pad)
    final class Back: NSView { var color = NSColor.gray; override func draw(_ r: NSRect) { color.setFill(); bounds.fill() } }
    let back = Back(frame: NSRect(origin: .zero, size: size)); back.color = dark ? rgb(0x2C3140) : rgb(0xC9CED8)
    back.appearance = NSAppearance(named: dark ? .darkAqua : .aqua)
    let view = IslandView(frame: back.bounds)
    back.addSubview(view)
    view.debugShow(kind)
    let scale: CGFloat = 2
    guard let rep = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: Int(size.width * scale), pixelsHigh: Int(size.height * scale), bitsPerSample: 8,
                                     samplesPerPixel: 4, hasAlpha: true, isPlanar: false, colorSpaceName: .deviceRGB, bytesPerRow: 0, bitsPerPixel: 0) else { return }
    rep.size = size
    back.cacheDisplay(in: back.bounds, to: rep)
    try? rep.representation(using: .png, properties: [:])?.write(to: URL(fileURLWithPath: path))
    print("wrote \(path)")
}

func writeCardSnapshot(path: String, recipeId: String, file: String, dark: Bool) {
    _ = NSApplication.shared
    let url = URL(fileURLWithPath: file)
    guard let recipe = WheelContents.recipes.first(where: { $0.id == recipeId }) else {
        print("recipe \(recipeId) not found")
        exit(1)
    }
    let card = PreviewCardView()
    card.forceDark = dark
    var predFinished: Prediction?
    Predictor.shared.request(token: 1, files: [url], slice: WheelSlice(id: recipe.id, label: recipe.label), recipe: recipe) { pred in
        if pred.isFinal { predFinished = pred }
    }
    let start = Date()
    while predFinished == nil && Date().timeIntervalSince(start) < 2.0 {
        RunLoop.current.run(mode: .default, before: Date(timeIntervalSinceNow: 0.05))
    }
    guard let prediction = predFinished else {
        print("prediction timed out")
        exit(1)
    }
    card.prediction = prediction
    let size = card.windowSize
    card.frame = NSRect(origin: .zero, size: size)
    let scale: CGFloat = 2
    guard let rep = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: Int(size.width * scale), pixelsHigh: Int(size.height * scale), bitsPerSample: 8,
                                     samplesPerPixel: 4, hasAlpha: true, isPlanar: false, colorSpaceName: .deviceRGB, bytesPerRow: 0, bitsPerPixel: 0) else { return }
    rep.size = size
    card.cacheDisplay(in: card.bounds, to: rep)
    try? rep.representation(using: .png, properties: [:])?.write(to: URL(fileURLWithPath: path))
    print("wrote \(path)")
}

func writeShelfSnapshot(path: String, dark: Bool, hoverBtn: String? = nil) {
    _ = NSApplication.shared
    let view = FloatShelfView(frame: NSRect(x: 0, y: 0, width: 280, height: 200))
    view.appearance = NSAppearance(named: dark ? .darkAqua : .aqua)
    view.updateLayout()
    if let btn = hoverBtn {
        view.hoveredRowIndex = 0
        view.hoveredButton = btn
    }
    let size = view.frame.size
    let scale: CGFloat = 2
    guard let rep = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: Int(size.width * scale), pixelsHigh: Int(size.height * scale), bitsPerSample: 8,
                                     samplesPerPixel: 4, hasAlpha: true, isPlanar: false, colorSpaceName: .deviceRGB, bytesPerRow: 0, bitsPerPixel: 0) else { return }
    rep.size = size
    view.cacheDisplay(in: view.bounds, to: rep)
    try? rep.representation(using: .png, properties: [:])?.write(to: URL(fileURLWithPath: path))
    print("wrote \(path)")
}

let args = CommandLine.arguments
if let i = args.firstIndex(of: "--snapshot-shelf"), i + 1 < args.count {
    let dark = (args.count > i + 2 ? args[i + 2] : "dark") != "light"
    let hover = (args.count > i + 3 && !args[i + 3].isEmpty) ? args[i + 3] : nil
    writeShelfSnapshot(path: args[i + 1], dark: dark, hoverBtn: hover)
    exit(0)
}
if let i = args.firstIndex(of: "--snapshot-card"), i + 2 < args.count {
    let outPath = args[i + 1]
    let recipeId = args[i + 2]
    let sampleFile = (args.count > i + 3 && !args[i + 3].hasPrefix("--") && args[i + 3] != "dark" && args[i + 3] != "light") ? args[i + 3] : "/tmp/sample_predict.png"
    let dark = !args.contains("light")
    writeCardSnapshot(path: outPath, recipeId: recipeId, file: sampleFile, dark: dark)
    exit(0)
}
if let i = args.firstIndex(of: "--snapshot-island"), i + 2 < args.count {
    writeIslandSnapshot(path: args[i + 1], kind: args[i + 2], dark: (args.count > i + 3 ? args[i + 3] : "dark") != "light")
    exit(0)
}
if let i = args.firstIndex(of: "--snapshot"), i + 1 < args.count {
    let a = Array(args[(i + 2)...])
    writeSnapshot(path: args[i + 1], ring: Int(a.first ?? "0") ?? 0, hot: a.count > 1 ? (Int(a[1]) ?? -2) : -2,
                  dark: (a.count > 2 ? a[2] : "dark") != "light", files: a.count > 3 ? (Int(a[3]) ?? 3) : 3)
    exit(0)
}

// ShiftDrop --rings <file>...   prints the wheel sections and slices offered for these files
if let i = args.firstIndex(of: "--rings"), i + 1 < args.count {
    let files = args[(i + 1)...].map { URL(fileURLWithPath: $0) }
    for ring in WheelContents.rings(for: files) {
        print("[\(ring.title)] " + ring.slices.map { ($0.enabled ? "" : "(") + $0.label + ($0.enabled ? "" : ")") }.joined(separator: " "))
    }
    exit(0)
}

// ShiftDrop --run <recipe-id> <file>...   runs a recipe headlessly (for tests), through the same runner as the app
if let i = args.firstIndex(of: "--run"), i + 2 < args.count {
    _ = NSApplication.shared
    let id = args[i + 1]
    guard let recipe = WheelContents.recipes.first(where: { $0.id == id }) else { print("no recipe \(id)"); exit(2) }
    let files = args[(i + 2)...].map { URL(fileURLWithPath: $0) }
    var finished: JobResult?
    JobRunner().run(recipe: recipe, files: files, progress: { _ in }, done: { finished = $0 })
    while finished == nil { RunLoop.current.run(mode: .default, before: Date(timeIntervalSinceNow: 0.05)) }
    let r = finished!
    print("\(r.outcome == .success ? "OK" : (r.outcome == .partial ? "PARTIAL" : "FAIL")) \(r.title)\(r.subtitle.isEmpty ? "" : " | " + r.subtitle)")
    for o in r.outputs { print("  -> \(o.lastPathComponent)") }
    exit(r.outcome == .failure ? 1 : 0)
}

final class WheelController {
    let panel: NSPanel
    let view: WheelView
    private(set) var visible = false
    var logLine: (String) -> Void = { print($0) }

    init() {
        let side = WheelMetrics.windowSide(for: 1.5)
        view = WheelView(frame: NSRect(x: 0, y: 0, width: side, height: side))
        panel = NSPanel(contentRect: view.frame, styleMask: [.borderless, .nonactivatingPanel], backing: .buffered, defer: false)
        panel.contentView = view
        panel.isOpaque = false
        panel.backgroundColor = .clear
        panel.hasShadow = false                       // the wheel draws its own shadow
        panel.level = .screenSaver
        panel.isFloatingPanel = true
        panel.hidesOnDeactivate = false
        panel.collectionBehavior = [.canJoinAllSpaces, .fullScreenAuxiliary, .stationary]
        view.onFinished = { [weak self] in self?.hide() }
    }

    func show(at p: NSPoint) {
        let side = WheelMetrics.windowSide(for: view.scale)
        var origin = NSPoint(x: p.x - side / 2, y: p.y - side / 2)
        if let screen = NSScreen.screens.first(where: { NSMouseInRect(p, $0.frame, false) }) ?? NSScreen.main {
            // Keep the tiles (not the shadow margin) inside the visible area.
            let room = (WheelMetrics.margin + 8) * view.scale
            let f = screen.visibleFrame
            origin.x = min(max(origin.x, f.minX - room), f.maxX - side + room)
            origin.y = min(max(origin.y, f.minY - room), f.maxY - side + room)
        }
        view.reset()
        panel.setFrame(NSRect(origin: origin, size: NSSize(width: side, height: side)), display: false)
        view.frame = NSRect(origin: .zero, size: NSSize(width: side, height: side))
        panel.orderFrontRegardless()
        visible = true
    }

    func hide() {
        guard visible else { return }
        visible = false
        panel.orderOut(nil)
        view.reset()
        onHidden?()
    }
    var onHidden: (() -> Void)?

    var centerPoint: NSPoint {
        NSPoint(x: panel.frame.midX, y: panel.frame.midY)
    }
    var outerRadius: CGFloat {
        (view.currentOuterRadius + 8) * view.scale
    }
}

final class AppDelegate: NSObject, NSApplicationDelegate, NSMenuDelegate {
    let controller = WheelController()
    let island = IslandController()
    let previewCard = PreviewCardController()
    let gesture = Gesture()
    let runner = JobRunner()
    let history = HistoryStore()
    var status: NSStatusItem!
    var stateItem: NSMenuItem!
    var diagItem: NSMenuItem!
    var undoItem: NSMenuItem!
    var soundItem: NSMenuItem!
    var shelfItem: NSMenuItem!

    private func currentScreen() -> NSScreen? {
        let p = NSEvent.mouseLocation
        return NSScreen.screens.first { NSMouseInRect(p, $0.frame, false) } ?? NSScreen.main
    }

    func applicationWillTerminate(_ n: Notification) { FfmpegEngine.shared.terminateAll() }

    func applicationDidFinishLaunching(_ n: Notification) {
        setvbuf(stdout, nil, _IONBF, 0)
        status = NSStatusBar.system.statusItem(withLength: NSStatusItem.squareLength)
        status.button?.image = NSImage(systemSymbolName: "circle.hexagongrid", accessibilityDescription: "ShiftDrop")
        let menu = NSMenu()
        menu.delegate = self
        stateItem = NSMenuItem(title: "Starting\u{2026}", action: nil, keyEquivalent: "")
        diagItem = NSMenuItem(title: "events 0", action: nil, keyEquivalent: "")
        undoItem = NSMenuItem(title: "Nothing to undo", action: #selector(undoLast), keyEquivalent: "z")
        undoItem.target = self
        menu.addItem(stateItem); menu.addItem(diagItem); menu.addItem(.separator()); menu.addItem(undoItem)
        soundItem = menu.addItem(withTitle: "Sounds", action: #selector(toggleSounds), keyEquivalent: "")
        soundItem.target = self
        soundItem.state = Sounds.enabled ? .on : .off

        shelfItem = menu.addItem(withTitle: "Float shelf", action: #selector(toggleFloatShelf), keyEquivalent: "")
        shelfItem.target = self
        FloatShelf.shared.onVisibilityChanged = { [weak self] shown in
            self?.shelfItem.state = shown ? .on : .off
        }

        // Wheel Size submenu
        let sizeMenu = NSMenu()
        let sizes: [(String, CGFloat)] = [
            ("Small (85%)", 0.85),
            ("Medium (100%)", 1.0),
            ("Large (115%)", 1.15),
            ("Extra Large (130%)", 1.30)
        ]
        let currentScale = UserDefaults.standard.object(forKey: "wheelScale") as? CGFloat ?? 1.15
        controller.view.scale = currentScale
        for (title, scaleVal) in sizes {
            let item = NSMenuItem(title: title, action: #selector(setWheelScale(_:)), keyEquivalent: "")
            item.representedObject = scaleVal
            item.target = self
            if abs(scaleVal - currentScale) < 0.01 { item.state = .on }
            sizeMenu.addItem(item)
        }
        let sizeItem = NSMenuItem(title: "Wheel Size", action: nil, keyEquivalent: "")
        sizeItem.submenu = sizeMenu
        menu.addItem(sizeItem)

        let folder = menu.addItem(withTitle: "Open recipes folder", action: #selector(openRecipes), keyEquivalent: ""); folder.target = self
        let perm = menu.addItem(withTitle: "Input Monitoring settings\u{2026}", action: #selector(openSettings), keyEquivalent: ""); perm.target = self
        menu.addItem(.separator())
        menu.addItem(withTitle: "Quit", action: #selector(NSApplication.terminate(_:)), keyEquivalent: "q")
        status.menu = menu

        // The wheel: content comes from the dragged files; the top panel follows it.
        controller.view.contentProvider = { files in WheelContents.rings(for: files) }
        controller.view.onHighlightChanged = { [weak self] files, slice, recipe in
            guard let self = self else { return }
            self.previewCard.update(files: files, slice: slice, recipe: recipe,
                                    wheelCenter: self.controller.centerPoint,
                                    wheelRadius: self.controller.outerRadius,
                                    screen: self.currentScreen())
        }
        controller.view.onShown = { [weak self] rings, index in self?.island.showSwitcher(rings: rings, index: index, screen: self?.currentScreen()) }
        controller.view.onRingChanged = { [weak self] i in self?.island.setRing(i) }
        controller.view.onDrop = { [weak self] files, slice, _ in
            self?.previewCard.hideNow()
            DispatchQueue.main.async { self?.run(slice: slice, files: files) }   // after the drag session has ended
        }
        controller.onHidden = { [weak self] in
            self?.gesture.markHidden()
            self?.island.hideSwitcher()
            self?.previewCard.hideNow()
        }
        gesture.onShow = { [weak self] p in self?.controller.show(at: p) }
        gesture.onMouseUp = { [weak self] in   // a drop is delivered after the button goes up: give it time
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.7) { self?.controller.hide() }
        }
        gesture.onScroll = { [weak self] step in
            guard let v = self?.controller.view else { return }
            v.setRing(v.ringIndex + step)
        }
        gesture.onSelectRing = { [weak self] ring in
            guard let v = self?.controller.view, ring < v.rings.count else { return }
            v.setRing(ring)
        }
        gesture.onCancel = { [weak self] in
            guard self?.controller.visible == true else { return }
            self?.controller.hide()
            self?.previewCard.hideNow()
        }
        gesture.isWheelShown = { [weak self] in self?.controller.visible ?? false }

        let listen = CGPreflightListenEventAccess()
        print("Input Monitoring: \(listen)")
        if !listen { CGRequestListenEventAccess() }
        if gesture.start() { stateItem.title = "Watching for file drags (hold Shift)" }
        else { stateItem.title = "Needs Input Monitoring permission"; print("event tap failed") }
        Timer.scheduledTimer(withTimeInterval: 0.5, repeats: true) { [weak self] _ in
            guard let self else { return }
            self.diagItem.title = "events \(self.gesture.eventCount), drags \(self.gesture.dragCount), file drag: \(self.gesture.fileDragActive)"
        }
    }

    // MARK: Running a slice

    private func run(slice: WheelSlice, files: [URL]) {
        guard let recipe = WheelContents.recipes.first(where: { $0.id == slice.id }), let first = recipe.steps.first else { return }
        let screen = currentScreen()
        if first.action.hasPrefix("file.") {
            let result = FileActions.run(first, recipe: recipe, files: files)
            finish(result, recipe: recipe, screen: screen)
            return
        }
        let n = files.count
        island.showProgress(title: "\(recipe.suffix == "converted" ? "Converting" : (recipe.suffix == "compressed" || recipe.suffix == "reduced" ? "Compressing" : "Working on")) \(JobRunner.plural(n, "file"))\u{2026}", total: n,
                            onCancel: { [weak self] in self?.runner.cancel() }, screen: screen)
        runner.run(recipe: recipe, files: files,
                         progress: { [weak self] done in self?.island.setProgress(done) },
                         done: { [weak self] result in self?.finish(result, recipe: recipe, screen: screen) })
    }

    private func finish(_ result: JobResult, recipe: Recipe, screen: NSScreen?) {
        if !result.outputs.isEmpty && recipe.steps.first?.action.hasPrefix("file.move") != true { history.record(title: "\(recipe.label): \(result.title)", outputs: result.outputs) }
        let outs = result.outputs
        var actions = IslandView.Actions()
        if !outs.isEmpty {
            actions.open = { outs.forEach { NSWorkspace.shared.open($0) } }
            actions.reveal = { NSWorkspace.shared.activateFileViewerSelecting(outs) }
            actions.copy = { NSPasteboard.general.clearContents(); NSPasteboard.general.writeObjects(outs as [NSURL]) }
            actions.float = { FloatShelf.shared.add(outs); FloatShelf.shared.show() }
        }
        island.showResult(result, actions: actions, screen: screen)
        if result.outcome == .failure { Sounds.error() }
        else if !outs.isEmpty { Sounds.success() }   // something was saved
        print("RESULT \(result.title) \(outs.map(\.path))")
    }

    // MARK: Menu

    func menuNeedsUpdate(_ menu: NSMenu) {
        if let e = history.lastUndoable { undoItem.title = "Undo: \(e.title)"; undoItem.isEnabled = true }
        else { undoItem.title = "Nothing to undo"; undoItem.isEnabled = false }
    }

    @objc func undoLast() {
        guard let e = history.lastUndoable else { return }
        let message = history.undo(e.id)
        island.showResult(JobResult(outcome: message.hasPrefix("Undone") ? .success : .failure, title: message, subtitle: "", outputs: []), actions: IslandView.Actions(), screen: currentScreen())
    }

    @objc func toggleSounds() {
        Sounds.enabled.toggle()
        soundItem.state = Sounds.enabled ? .on : .off
        if Sounds.enabled { Sounds.section() }   // a sample of what it sounds like
    }

    @objc func toggleFloatShelf() {
        FloatShelf.shared.toggle()
    }

    @objc func setWheelScale(_ sender: NSMenuItem) {
        guard let scaleVal = sender.representedObject as? CGFloat else { return }
        controller.view.scale = scaleVal
        UserDefaults.standard.set(scaleVal, forKey: "wheelScale")
        if let menu = sender.menu {
            for item in menu.items { item.state = (item == sender) ? .on : .off }
        }
    }

    @objc func openRecipes() {
        try? FileManager.default.createDirectory(at: RecipeStore.userFolder, withIntermediateDirectories: true)
        NSWorkspace.shared.open(RecipeStore.userFolder)
    }

    @objc func openSettings() {
        NSWorkspace.shared.open(URL(string: "x-apple.systempreferences:com.apple.preference.security?Privacy_ListenEvent")!)
    }
}

let app = NSApplication.shared
app.setActivationPolicy(.accessory)
let delegate = AppDelegate()
app.delegate = delegate
app.run()
