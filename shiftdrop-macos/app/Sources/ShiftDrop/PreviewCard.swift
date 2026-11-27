import AppKit
import ImageIO
import AVFoundation
import PDFKit

// MARK: - Prediction Model

struct Prediction {
    var token: UInt64 = 0
    var isFinal: Bool = false
    var format: String = ""              // e.g. "WebP" or "PNG" or "JPEG, quality 92"
    var predictedBytes: Int64?
    var sourceBytes: Int64 = 0
    var sizeText: String = ""            // e.g. "about 1.2 MB instead of 3.4 MB"
    var outputName: String = ""          // e.g. "saved as IMG_2041 (converted).webp"
    var detail: String = ""              // e.g. "4032×3024 → 1920×1440 · removes metadata"
    var thumbnail: NSImage?
    var orientation: (rotation: Int, flipHorizontal: Bool) = (0, false)
}

// MARK: - Predictor Engine

final class Predictor {
    static let shared = Predictor()

    private let queue = DispatchQueue(label: "shiftdrop.predictor", qos: .userInitiated)
    private var cache: [String: Prediction] = [:]
    private let lock = NSLock()

    static func predicts(_ recipe: Recipe) -> Bool {
        guard let first = recipe.steps.first else { return false }
        if first.action == "file.duplicate" { return true }
        if first.action.hasPrefix("media.") { return true }
        if first.action.hasPrefix("image.") { return true }
        if first.action.hasPrefix("pdf.") && first.action != "ocr.copy_text" { return true }
        if first.action.hasPrefix("docs.") { return true }
        if first.action.hasPrefix("archive.") { return true }
        if first.action == "images.to_pdf" { return true }
        if recipe.ring == "reduce" { return true }
        return false
    }

    static func outputExtension(for recipe: Recipe, source: URL) -> String {
        guard let first = recipe.steps.first else { return source.pathExtension }
        let fmt = first.string("format", "")
        switch first.action {
        case "image.encode":
            return fmt == "same" ? source.pathExtension : fmt
        case "image.compress":
            let f = ImageFormat(path: source)
            return (f == .webp || f == .avif) ? source.pathExtension : "jpg"
        case "media.convert":
            return Media.format(fmt)?.ext ?? (fmt.isEmpty ? "mp4" : fmt)
        case "media.compress":
            return Media.kind(of: source) == .audio ? "m4a" : "mp4"
        case "pdf.to_images":
            return fmt == "png" ? "png" : "jpg"
        case "pdf.to_text": return "txt"
        case "pdf.to_markdown": return "md"
        case "pdf.to_docx": return "docx"
        case "pdf.to_epub": return "epub"
        case "pdf.to_html": return "html"
        case "pdf.merge", "pdf.compress", "images.to_pdf": return "pdf"
        case "docs.convert": return fmt.isEmpty ? "pdf" : fmt
        case "subs.convert": return fmt.isEmpty ? "srt" : fmt
        case "sheets.convert": return fmt.isEmpty ? "csv" : fmt
        case "archive.repack": return fmt.isEmpty ? "zip" : fmt
        case "archive.zip", "archive.pack_folder": return "zip"
        case "archive.extract": return ""
        case "file.duplicate": return source.pathExtension
        default:
            if recipe.ring == "reduce" { return source.pathExtension }
            return fmt.isEmpty ? source.pathExtension : fmt
        }
    }

    static func formatLabel(for recipe: Recipe, ext: String, source: URL) -> String {
        guard let first = recipe.steps.first else { return recipe.label }
        if first.action == "file.duplicate" { return "Duplicate" }
        if first.action == "image.compress" {
            let f = ImageFormat(path: source)
            return (f == .webp || f == .avif) ? "\(f!.rawValue.uppercased()), compressed" : "JPEG, compressed"
        }
        if first.action == "media.compress" {
            return Media.kind(of: source) == .audio ? "M4A, compressed" : "MP4, compressed"
        }
        if first.action == "media.convert" {
            let fmt = first.string("format", "mp4")
            return Media.format(fmt)?.label ?? fmt.uppercased()
        }
        if recipe.ring == "reduce" { return "Reduce size" }
        if !ext.isEmpty { return ext.uppercased() }
        return recipe.label
    }

    static func formatSize(_ bytes: Int64) -> String {
        ByteCountFormatter.string(fromByteCount: bytes, countStyle: .file)
    }

    func request(token: UInt64, files: [URL], slice: WheelSlice, recipe: Recipe, completion: @escaping (Prediction) -> Void) {
        guard let first = files.first else { return }
        let totalSourceBytes = files.reduce(Int64(0)) { $0 + Int64((try? $1.resourceValues(forKeys: [.fileSizeKey]).fileSize) ?? 0) }
        let firstSourceBytes = Int64((try? first.resourceValues(forKeys: [.fileSizeKey]).fileSize) ?? 0)
        let outExt = Self.outputExtension(for: recipe, source: first)
        let targetURL = OutputPath.reserve(source: first, suffix: recipe.suffix, ext: outExt)
        let outputFilename = targetURL.lastPathComponent
        let multiSuffix = files.count > 1 ? "  + \(files.count - 1) more" : ""
        let outputName = "saved as \(outputFilename)\(multiSuffix)"
        let fmtLabel = Self.formatLabel(for: recipe, ext: outExt, source: first)

        // Orientation
        var rotation = 0, flipH = false
        for step in recipe.steps {
            if step.action == "image.rotate" { rotation += step.int("degrees", 90) }
            else if step.action == "image.flip" { flipH.toggle() }
        }

        // Check cache
        let cacheKey = "\(first.path)|\(firstSourceBytes)|\(recipe.id)"
        lock.lock()
        if let cached = cache[cacheKey], cached.isFinal {
            lock.unlock()
            var res = cached
            res.token = token
            res.outputName = outputName
            completion(res)
            return
        }
        lock.unlock()

        // Quick preliminary prediction (thumbnail and quick size estimate)
        let quickThumb = Self.generateThumbnail(for: first, rotation: rotation, flipH: flipH)
        let quick = Prediction(
            token: token,
            isFinal: false,
            format: fmtLabel,
            predictedBytes: nil,
            sourceBytes: totalSourceBytes,
            sizeText: "estimating\u{2026}",
            outputName: outputName,
            detail: "",
            thumbnail: quickThumb,
            orientation: (rotation, flipH)
        )

        // Async calculation
        queue.async { [weak self] in
            guard let self = self else { return }
            var predBytes: Int64?
            var isCapped = false
            var isExact = false
            var detailText = ""

            let firstStep = recipe.steps.first ?? Step(action: "", params: [:])

            if firstStep.action == "file.duplicate" {
                predBytes = totalSourceBytes
                isExact = true
            } else if recipe.ring == "reduce" || firstStep.action == "media.half" {
                let percent = firstStep.int("percent", 50)
                predBytes = max(1, totalSourceBytes * Int64(percent) / 100)
            } else if firstStep.action == "image.scale" {
                let percent = firstStep.int("percent", 50)
                let scale = Double(percent) / 100.0
                predBytes = max(1, Int64(Double(totalSourceBytes) * scale * scale * 1.15))
                detailText = "\(percent)% width & height"
            } else if firstStep.int("maxKB", 0) > 0 {
                let maxKB = Int64(firstStep.int("maxKB", 0)) * 1024
                predBytes = maxKB
                isCapped = totalSourceBytes > maxKB
            } else if firstStep.int("targetMB", 0) > 0 {
                let maxMB = Int64(firstStep.int("targetMB", 0)) * 1024 * 1024
                predBytes = maxMB
                isCapped = totalSourceBytes > maxMB
            } else if ImagePipeline.isImage(first) {
                let (est, det) = self.estimateImage(url: first, recipe: recipe, sourceBytes: firstSourceBytes, outExt: outExt)
                if let est = est {
                    let ratio = totalSourceBytes > 0 && firstSourceBytes > 0 ? Double(totalSourceBytes) / Double(firstSourceBytes) : 1.0
                    predBytes = Int64(Double(est) * ratio)
                }
                detailText = det
            } else if Media.kind(of: first) != .none {
                let est = self.estimateMedia(url: first, recipe: recipe, sourceBytes: firstSourceBytes, outExt: outExt)
                if let est = est {
                    let ratio = totalSourceBytes > 0 && firstSourceBytes > 0 ? Double(totalSourceBytes) / Double(firstSourceBytes) : 1.0
                    predBytes = Int64(Double(est) * ratio)
                }
            } else if first.pathExtension.lowercased() == "pdf" {
                if firstStep.action == "pdf.compress" {
                    predBytes = max(1, totalSourceBytes * 55 / 100)
                } else if firstStep.action == "pdf.to_images" {
                    let pages = (try? Pdf.open(first).pageCount) ?? 1
                    let bytesPerPage: Int64 = outExt == "png" ? 550_000 : 220_000
                    predBytes = Int64(pages) * bytesPerPage
                    detailText = "\(pages) \(pages == 1 ? "page" : "pages") at 300 dpi"
                } else if firstStep.action == "pdf.merge" {
                    predBytes = max(1, totalSourceBytes * 98 / 100)
                    detailText = "\(files.count) PDFs merged in order"
                }
            } else if firstStep.action.hasPrefix("archive.") {
                predBytes = max(1, totalSourceBytes * 70 / 100)
            }

            let sizeText = Self.buildSizeText(predicted: predBytes, source: totalSourceBytes, isCapped: isCapped, isExact: isExact)

            if rotation != 0 {
                detailText = "Rotates \(rotation)°"
            } else if flipH {
                detailText = "Flips horizontally"
            }

            let finalPrediction = Prediction(
                token: token,
                isFinal: true,
                format: fmtLabel,
                predictedBytes: predBytes,
                sourceBytes: totalSourceBytes,
                sizeText: sizeText,
                outputName: outputName,
                detail: detailText,
                thumbnail: quickThumb,
                orientation: (rotation, flipH)
            )

            self.lock.lock()
            self.cache[cacheKey] = finalPrediction
            self.lock.unlock()

            DispatchQueue.main.async {
                completion(finalPrediction)
            }
        }

        // Deliver preliminary immediately
        completion(quick)
    }

    private func estimateImage(url: URL, recipe: Recipe, sourceBytes: Int64, outExt: String) -> (Int64?, String) {
        guard let src = CGImageSourceCreateWithURL(url as CFURL, nil) else { return (nil, "") }
        let props = CGImageSourceCopyPropertiesAtIndex(src, 0, nil) as? [CFString: Any]
        let origW = (props?[kCGImagePropertyPixelWidth] as? Int) ?? 0
        let origH = (props?[kCGImagePropertyPixelHeight] as? Int) ?? 0
        guard origW > 0, origH > 0 else { return (nil, "") }

        var targetW = origW
        var targetH = origH
        var detailText = ""

        for step in recipe.steps {
            if step.action == "image.resize" {
                let maxEdge = step.int("maxEdge", 0)
                if maxEdge > 0 && max(targetW, targetH) > maxEdge {
                    let scale = Double(maxEdge) / Double(max(targetW, targetH))
                    targetW = max(1, Int(Double(targetW) * scale + 0.5))
                    targetH = max(1, Int(Double(targetH) * scale + 0.5))
                    detailText = "\(origW)×\(origH) → \(targetW)×\(targetH)"
                }
            } else if step.action == "image.scale" {
                let percent = step.int("percent", 100)
                let scale = Double(percent) / 100.0
                targetW = max(1, Int(Double(targetW) * scale + 0.5))
                targetH = max(1, Int(Double(targetH) * scale + 0.5))
                detailText = "\(origW)×\(origH) → \(targetW)×\(targetH)"
            }
        }

        let targetFmt = ImageFormat(rawValue: outExt.lowercased()) ?? .jpg
        if targetFmt == .bmp {
            return (Int64(targetW * targetH * 3 + 54), detailText)
        }

        // Try hardware-accelerated 512px thumbnail encoding to measure exact image entropy
        let opts: [CFString: Any] = [
            kCGImageSourceCreateThumbnailFromImageAlways: true,
            kCGImageSourceCreateThumbnailWithTransform: true,
            kCGImageSourceThumbnailMaxPixelSize: 512
        ]
        if let thumb = CGImageSourceCreateThumbnailAtIndex(src, 0, opts as CFDictionary) {
            let q: Int
            if let first = recipe.steps.first, first.action == "image.compress" {
                q = first.int("quality", 75)
            } else if targetFmt == .webp {
                q = 85
            } else if targetFmt == .avif {
                q = 60
            } else if targetFmt == .heic {
                q = 80
            } else {
                q = 92
            }
            if let encoded = try? ImagePipeline.encode(thumb, as: targetFmt, quality: q) {
                let thumbPixels = Double(thumb.width * thumb.height)
                let targetPixels = Double(targetW * targetH)
                if thumbPixels > 0 {
                    let ratio = targetPixels / thumbPixels
                    let estimated = Double(encoded.count) * pow(ratio, 0.95)
                    return (Int64(estimated), detailText)
                }
            }
        }

        // Formula fallback
        let pixelRatio = Double(targetW * targetH) / Double(origW * origH)
        let formatFactor: Double
        switch targetFmt {
        case .webp: formatFactor = 0.55
        case .avif: formatFactor = 0.45
        case .heic: formatFactor = 0.50
        case .jpg: formatFactor = 0.70
        case .png: formatFactor = 1.60
        case .tiff: formatFactor = 2.40
        case .bmp: formatFactor = 3.00
        }
        return (Int64(Double(sourceBytes) * pixelRatio * formatFactor), detailText)
    }

    private func estimateMedia(url: URL, recipe: Recipe, sourceBytes: Int64, outExt: String) -> Int64? {
        let asset = AVURLAsset(url: url)
        let duration = CMTimeGetSeconds(asset.duration)
        guard duration > 0 && !duration.isNaN && !duration.isInfinite else {
            return Int64(Double(sourceBytes) * 0.7)
        }
        if let f = Media.format(outExt) {
            if f.fromAudio {
                let kbps: Double = (outExt == "mp3" || outExt == "m4a") ? 192 : 128
                return Int64(duration * (kbps * 1000 / 8))
            } else {
                let kbps: Double = 2200
                return Int64(duration * (kbps * 1000 / 8))
            }
        }
        return Int64(Double(sourceBytes) * 0.75)
    }

    static func buildSizeText(predicted: Int64?, source: Int64, isCapped: Bool, isExact: Bool) -> String {
        let srcText = formatSize(source)
        guard let pred = predicted else { return "estimating\u{2026}" }
        let predText = formatSize(pred)
        if isExact || pred == source {
            return "\(predText) (same size)"
        }
        if isCapped {
            return "under \(predText) instead of \(srcText)"
        }
        return "about \(predText) instead of \(srcText)"
    }

    static func generateThumbnail(for url: URL, rotation: Int = 0, flipH: Bool = false) -> NSImage? {
        let opts: [CFString: Any] = [
            kCGImageSourceCreateThumbnailFromImageAlways: true,
            kCGImageSourceCreateThumbnailWithTransform: true,
            kCGImageSourceThumbnailMaxPixelSize: 128
        ]
        var cgImage: CGImage?
        if let src = CGImageSourceCreateWithURL(url as CFURL, nil) {
            cgImage = CGImageSourceCreateThumbnailAtIndex(src, 0, opts as CFDictionary)
        }
        if cgImage == nil && (url.pathExtension.lowercased() == "pdf") {
            if let doc = try? Pdf.open(url), doc.pageCount > 0, let page = doc.page(at: 0) {
                cgImage = try? Pdf.render(page, dpi: 72)
            }
        }
        guard let base = cgImage else {
            return NSWorkspace.shared.icon(forFile: url.path)
        }

        let turn = ((rotation % 360) + 360) % 360
        if turn == 0 && !flipH {
            return NSImage(cgImage: base, size: NSSize(width: base.width, height: base.height))
        }

        let w = base.width, h = base.height
        let (nw, nh) = (turn == 90 || turn == 270) ? (h, w) : (w, h)
        let colorSpace = CGColorSpace(name: CGColorSpace.sRGB)!
        guard let ctx = CGContext(data: nil, width: nw, height: nh, bitsPerComponent: 8, bytesPerRow: 0,
                                  space: colorSpace, bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue) else {
            return NSImage(cgImage: base, size: NSSize(width: nw, height: nh))
        }
        switch turn {
        case 90:
            ctx.translateBy(x: 0, y: CGFloat(nh))
            ctx.rotate(by: -.pi / 2)
        case 180:
            ctx.translateBy(x: CGFloat(nw), y: CGFloat(nh))
            ctx.rotate(by: .pi)
        case 270:
            ctx.translateBy(x: CGFloat(nw), y: 0)
            ctx.rotate(by: .pi / 2)
        default: break
        }
        if flipH {
            ctx.translateBy(x: CGFloat(w), y: 0)
            ctx.scaleBy(x: -1, y: 1)
        }
        ctx.draw(base, in: CGRect(x: 0, y: 0, width: w, height: h))
        if let transformed = ctx.makeImage() {
            return NSImage(cgImage: transformed, size: NSSize(width: nw, height: nh))
        }
        return NSImage(cgImage: base, size: NSSize(width: nw, height: nh))
    }
}

// MARK: - Preview Card View

final class PreviewCardView: NSView {
    override var isFlipped: Bool { true }

    var prediction: Prediction? {
        didSet {
            updateHeight()
            needsDisplay = true
        }
    }

    var forceDark: Bool?
    private var palette: Palette { Palette.make(dark: effectiveAppearance.bestMatch(from: [.darkAqua, .aqua]) == .darkAqua) }
    private var pal: Palette { forceDark.map { Palette.make(dark: $0) } ?? palette }

    static let cardWidth: CGFloat = 330
    static let shadowPad: CGFloat = 16

    var cardHeight: CGFloat {
        (prediction?.detail.isEmpty ?? true) ? 52 : 68
    }

    var windowSize: NSSize {
        NSSize(width: Self.cardWidth + 2 * Self.shadowPad, height: cardHeight + 2 * Self.shadowPad)
    }

    private func updateHeight() {
        guard let window = self.window else { return }
        let targetSize = windowSize
        if window.frame.size != targetSize {
            var frame = window.frame
            let dh = targetSize.height - frame.height
            frame.size = targetSize
            frame.origin.y -= dh
            window.setFrame(frame, display: true)
        }
    }

    override func draw(_ dirtyRect: NSRect) {
        guard let g = NSGraphicsContext.current?.cgContext, let pred = prediction else { return }
        let p = pal
        let sPad = Self.shadowPad
        let cWidth = Self.cardWidth
        let cHeight = cardHeight
        let cardRect = CGRect(x: sPad, y: sPad, width: cWidth, height: cHeight)
        let path = CGPath(roundedRect: cardRect, cornerWidth: 8.5, cornerHeight: 8.5, transform: nil)

        // Shadow
        g.saveGState()
        g.setShadow(offset: CGSize(width: 0, height: 4), blur: 16, color: p.shadow.cgColor)
        g.addPath(path)
        g.setFillColor(rgb(p.dark ? 0x1F2127 : 0xFFFFFF).cgColor)
        g.fillPath()
        g.restoreGState()

        // Card outline
        g.addPath(path)
        g.setStrokeColor(rgb(p.dark ? 0xFFFFFF : 0x15171E, p.dark ? 0.10 : 0.08).cgColor)
        g.setLineWidth(1)
        g.strokePath()

        // Thumbnail (28x34 pt)
        let thumbW: CGFloat = 28, thumbH: CGFloat = 34
        let thumbRect = CGRect(x: cardRect.minX + 12, y: cardRect.midY - thumbH / 2, width: thumbW, height: thumbH)
        let thumbPath = CGPath(roundedRect: thumbRect, cornerWidth: 4, cornerHeight: 4, transform: nil)

        g.saveGState()
        g.addPath(thumbPath)
        g.clip()
        if let thumb = pred.thumbnail {
            let tw = thumb.size.width, th = thumb.size.height
            if tw > 0 && th > 0 {
                let fit = max(thumbW / tw, thumbH / th)
                let dw = tw * fit, dh = th * fit
                let dx = thumbRect.midX - dw / 2, dy = thumbRect.midY - dh / 2
                thumb.draw(in: CGRect(x: dx, y: dy, width: dw, height: dh))
            }
        } else {
            g.setFillColor(rgb(p.dark ? 0x2A2D35 : 0xEEF0F3).cgColor)
            g.fill(thumbRect)
        }
        g.restoreGState()

        g.addPath(thumbPath)
        g.setStrokeColor(rgb(p.dark ? 0xFFFFFF : 0x15171E, p.dark ? 0.08 : 0.06).cgColor)
        g.setLineWidth(0.8)
        g.strokePath()

        // Text area
        let textX = thumbRect.maxX + 10
        let textRight = cardRect.maxX - 12
        let availW = textRight - textX
        let hasDetail = !pred.detail.isEmpty

        let topY = cardRect.minY + (hasDetail ? 8 : 10)
        let line2Y = topY + 17
        let line3Y = line2Y + 16

        // Format label (Left)
        let formatAttrs: [NSAttributedString.Key: Any] = [
            .font: NSFont.systemFont(ofSize: 12.5, weight: .semibold),
            .foregroundColor: p.text
        ]
        let formatStr = NSAttributedString(string: pred.format, attributes: formatAttrs)
        formatStr.draw(at: CGPoint(x: textX, y: topY))
        let formatW = formatStr.size().width

        // Size prediction (Right)
        let maxAvailableForSize = availW - formatW - 8
        let sizeAttrs: [NSAttributedString.Key: Any] = [
            .font: NSFont.systemFont(ofSize: 11.5, weight: .regular),
            .foregroundColor: p.subtleText
        ]
        var sizeDisplay = pred.sizeText
        var sizeStr = NSAttributedString(string: sizeDisplay, attributes: sizeAttrs)
        if sizeStr.size().width > maxAvailableForSize {
            sizeDisplay = sizeDisplay.replacingOccurrences(of: " instead of ", with: " · was ")
            sizeStr = NSAttributedString(string: sizeDisplay, attributes: sizeAttrs)
        }
        if sizeStr.size().width > maxAvailableForSize {
            sizeDisplay = sizeDisplay.replacingOccurrences(of: "about ", with: "~")
            sizeStr = NSAttributedString(string: sizeDisplay, attributes: sizeAttrs)
        }
        let sizeW = sizeStr.size().width
        sizeStr.draw(at: CGPoint(x: textRight - sizeW, y: topY + 1))

        // Output filename (Line 2)
        let nameAttrs: [NSAttributedString.Key: Any] = [
            .font: NSFont.systemFont(ofSize: 11.5, weight: .regular),
            .foregroundColor: p.text
        ]
        let nameStyle = NSMutableParagraphStyle()
        nameStyle.lineBreakMode = .byTruncatingMiddle
        var nameFullAttrs = nameAttrs
        nameFullAttrs[.paragraphStyle] = nameStyle
        let nameStr = NSAttributedString(string: pred.outputName, attributes: nameFullAttrs)
        nameStr.draw(with: CGRect(x: textX, y: line2Y, width: availW, height: 16),
                     options: [.usesLineFragmentOrigin, .truncatesLastVisibleLine])

        // Detail (Line 3, if present)
        if hasDetail {
            let detailAttrs: [NSAttributedString.Key: Any] = [
                .font: NSFont.systemFont(ofSize: 10.5, weight: .regular),
                .foregroundColor: p.subtleText
            ]
            let detailStyle = NSMutableParagraphStyle()
            detailStyle.lineBreakMode = .byTruncatingTail
            var detailFullAttrs = detailAttrs
            detailFullAttrs[.paragraphStyle] = detailStyle
            let detailStr = NSAttributedString(string: pred.detail, attributes: detailFullAttrs)
            detailStr.draw(with: CGRect(x: textX, y: line3Y, width: availW, height: 15),
                           options: [.usesLineFragmentOrigin, .truncatesLastVisibleLine])
        }
    }
}

// MARK: - Preview Card Controller

final class PreviewCardController {
    let panel: NSPanel
    let view: PreviewCardView
    private(set) var visible = false

    private var hideTimer: Timer?
    private var token: UInt64 = 0
    private var lastWheelCenter = NSPoint.zero
    private var lastWheelRadius: CGFloat = 0
    private var lastScreen: NSScreen?

    init() {
        let initialH: CGFloat = 52
        view = PreviewCardView(frame: NSRect(origin: .zero, size: NSSize(width: PreviewCardView.cardWidth + 2 * PreviewCardView.shadowPad, height: initialH + 2 * PreviewCardView.shadowPad)))
        panel = NSPanel(contentRect: view.frame,
                        styleMask: [.borderless, .nonactivatingPanel],
                        backing: .buffered,
                        defer: false)
        panel.contentView = view
        panel.isOpaque = false
        panel.backgroundColor = .clear
        panel.hasShadow = false
        panel.level = .screenSaver
        panel.isFloatingPanel = true
        panel.hidesOnDeactivate = false
        panel.ignoresMouseEvents = true
        panel.collectionBehavior = [.canJoinAllSpaces, .fullScreenAuxiliary, .stationary]
    }

    func update(files: [URL], slice: WheelSlice?, recipe: Recipe?, wheelCenter: NSPoint, wheelRadius: CGFloat, screen: NSScreen?) {
        lastWheelCenter = wheelCenter
        lastWheelRadius = wheelRadius
        lastScreen = screen

        guard let slice = slice, let recipe = recipe, !files.isEmpty, Predictor.predicts(recipe) else {
            scheduleHide()
            return
        }

        hideTimer?.invalidate()
        hideTimer = nil
        token += 1
        let currentToken = token

        Predictor.shared.request(token: currentToken, files: files, slice: slice, recipe: recipe) { [weak self] prediction in
            DispatchQueue.main.async {
                guard let self = self, self.token == currentToken else { return }
                self.show(prediction: prediction)
            }
        }
    }

    private func show(prediction: Prediction) {
        view.prediction = prediction
        let size = view.windowSize
        let screen = lastScreen ?? NSScreen.main ?? NSScreen.screens[0]
        let origin = computeOrigin(size: size, screen: screen)
        panel.setFrame(NSRect(origin: origin, size: size), display: true)

        if !visible {
            panel.orderFrontRegardless()
            visible = true
        }
    }

    private func computeOrigin(size: NSSize, screen: NSScreen) -> NSPoint {
        let f = screen.visibleFrame
        let gap: CGFloat = 12
        let sPad = PreviewCardView.shadowPad
        let cWidth = PreviewCardView.cardWidth
        let cHeight = view.cardHeight

        let wheelBottom = lastWheelCenter.y - lastWheelRadius
        let wheelTop = lastWheelCenter.y + lastWheelRadius

        let roomBelow = (wheelBottom - gap - cHeight) >= (f.minY + 8)
        let cardY: CGFloat
        if roomBelow {
            cardY = wheelBottom - gap - cHeight
        } else {
            cardY = min(wheelTop + gap, f.maxY - cHeight - 8)
        }
        let rawX = lastWheelCenter.x - cWidth / 2
        let cardX = min(max(rawX, f.minX + 8), f.maxX - cWidth - 8)

        return NSPoint(x: cardX - sPad, y: cardY - sPad)
    }

    private func scheduleHide() {
        guard visible else { return }
        hideTimer?.invalidate()
        hideTimer = Timer.scheduledTimer(withTimeInterval: 0.12, repeats: false) { [weak self] _ in
            self?.hideNow()
        }
    }

    func hideNow() {
        hideTimer?.invalidate()
        hideTimer = nil
        guard visible else { return }
        visible = false
        panel.orderOut(nil)
        view.prediction = nil
    }
}
