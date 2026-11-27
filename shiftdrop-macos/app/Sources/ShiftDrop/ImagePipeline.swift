import Foundation
import ImageIO
import CoreGraphics
import UniformTypeIdentifiers
import CWebP

struct PipelineError: Error, LocalizedError {
    let message: String
    init(_ m: String) { message = m }
    var errorDescription: String? { message }
}

enum ImageFormat: String {
    case png, jpg, webp, avif, heic, tiff, bmp

    var ext: String { rawValue }
    var uti: String {
        switch self {
        case .png: return "public.png"
        case .jpg: return "public.jpeg"
        case .webp: return "org.webmproject.webp"
        case .avif: return "public.avif"
        case .heic: return "public.heic"
        case .tiff: return "public.tiff"
        case .bmp: return "com.microsoft.bmp"
        }
    }
    var hasAlpha: Bool { self != .jpg && self != .bmp }
    var lossy: Bool { self == .jpg || self == .webp || self == .avif || self == .heic }

    /// The format of an existing file, from its extension.
    init?(path: URL) {
        switch path.pathExtension.lowercased() {
        case "png": self = .png
        case "jpg", "jpeg": self = .jpg
        case "webp": self = .webp
        case "avif": self = .avif
        case "heic", "heif": self = .heic
        case "tif", "tiff": self = .tiff
        case "bmp": self = .bmp
        default: return nil
        }
    }

    /// "same": keep the source format where it can be written, else PNG.
    static func same(as url: URL) -> ImageFormat { ImageFormat(path: url) ?? .png }
}

enum ImagePipeline {
    static let imageExtensions: Set<String> = ["png", "jpg", "jpeg", "webp", "heic", "heif", "avif", "tif", "tiff", "bmp", "gif"]

    static func isImage(_ url: URL) -> Bool { imageExtensions.contains(url.pathExtension.lowercased()) }

    // MARK: Decode (EXIF orientation applied, full resolution)

    static func load(_ url: URL) throws -> CGImage {
        guard let src = CGImageSourceCreateWithURL(url as CFURL, nil), CGImageSourceGetCount(src) > 0 else {
            throw PipelineError("Can\u{2019}t read \(url.lastPathComponent)")
        }
        let props = CGImageSourceCopyPropertiesAtIndex(src, 0, nil) as? [CFString: Any]
        let w = (props?[kCGImagePropertyPixelWidth] as? Int) ?? 0, h = (props?[kCGImagePropertyPixelHeight] as? Int) ?? 0
        let opts: [CFString: Any] = [kCGImageSourceCreateThumbnailFromImageAlways: true, kCGImageSourceCreateThumbnailWithTransform: true,
                                     kCGImageSourceThumbnailMaxPixelSize: max(w, h, 1), kCGImageSourceShouldCacheImmediately: true]
        if let img = CGImageSourceCreateThumbnailAtIndex(src, 0, opts as CFDictionary) { return img }
        if let img = CGImageSourceCreateImageAtIndex(src, 0, nil) { return img }
        throw PipelineError("Can\u{2019}t decode \(url.lastPathComponent)")
    }

    // MARK: Pixel operations

    private static func context(_ w: Int, _ h: Int, opaque: Bool = false) throws -> CGContext {
        let info = opaque ? CGImageAlphaInfo.noneSkipLast.rawValue : CGImageAlphaInfo.premultipliedLast.rawValue
        guard w > 0, h > 0, let ctx = CGContext(data: nil, width: w, height: h, bitsPerComponent: 8, bytesPerRow: 0,
                                                space: CGColorSpace(name: CGColorSpace.sRGB)!, bitmapInfo: info) else { throw PipelineError("Out of memory") }
        ctx.interpolationQuality = .high
        return ctx
    }

    static func resize(_ img: CGImage, to size: (Int, Int)) throws -> CGImage {
        let ctx = try context(size.0, size.1)
        ctx.draw(img, in: CGRect(x: 0, y: 0, width: size.0, height: size.1))
        guard let out = ctx.makeImage() else { throw PipelineError("Resize failed") }
        return out
    }

    static func rotate(_ img: CGImage, degrees: Int) throws -> CGImage {
        let w = img.width, h = img.height
        let turn = ((degrees % 360) + 360) % 360
        let (nw, nh) = (turn == 90 || turn == 270) ? (h, w) : (w, h)
        let ctx = try context(nw, nh)
        switch turn {
        case 90: ctx.translateBy(x: 0, y: CGFloat(nh)); ctx.rotate(by: -.pi / 2)
        case 180: ctx.translateBy(x: CGFloat(nw), y: CGFloat(nh)); ctx.rotate(by: .pi)
        case 270: ctx.translateBy(x: CGFloat(nw), y: 0); ctx.rotate(by: .pi / 2)
        default: break
        }
        ctx.draw(img, in: CGRect(x: 0, y: 0, width: w, height: h))
        guard let out = ctx.makeImage() else { throw PipelineError("Rotate failed") }
        return out
    }

    static func flip(_ img: CGImage, horizontal: Bool) throws -> CGImage {
        let ctx = try context(img.width, img.height)
        if horizontal { ctx.translateBy(x: CGFloat(img.width), y: 0); ctx.scaleBy(x: -1, y: 1) }
        else { ctx.translateBy(x: 0, y: CGFloat(img.height)); ctx.scaleBy(x: 1, y: -1) }
        ctx.draw(img, in: CGRect(x: 0, y: 0, width: img.width, height: img.height))
        guard let out = ctx.makeImage() else { throw PipelineError("Flip failed") }
        return out
    }

    /// Transparent pixels become white for formats without alpha (JPG, BMP).
    static func flattened(_ img: CGImage) throws -> CGImage {
        let ctx = try context(img.width, img.height, opaque: true)
        ctx.setFillColor(CGColor(red: 1, green: 1, blue: 1, alpha: 1))
        ctx.fill(CGRect(x: 0, y: 0, width: img.width, height: img.height))
        ctx.draw(img, in: CGRect(x: 0, y: 0, width: img.width, height: img.height))
        guard let out = ctx.makeImage() else { throw PipelineError("Flatten failed") }
        return out
    }

    // MARK: Encode

    static func encode(_ image: CGImage, as format: ImageFormat, quality: Int) throws -> Data {
        let img = format.hasAlpha ? image : try flattened(image)
        if format == .webp { return try encodeWebP(img, quality: quality) }
        let data = NSMutableData()
        guard let dest = CGImageDestinationCreateWithData(data, format.uti as CFString, 1, nil) else {
            throw PipelineError("\(format.ext.uppercased()) can\u{2019}t be saved on this Mac")
        }
        var props: [CFString: Any] = [:]
        if format.lossy { props[kCGImageDestinationLossyCompressionQuality] = Double(min(max(quality, 1), 100)) / 100 }
        CGImageDestinationAddImage(dest, img, props as CFDictionary)
        guard CGImageDestinationFinalize(dest), data.length > 0 else { throw PipelineError("Couldn\u{2019}t save as \(format.ext.uppercased())") }
        return data as Data
    }

    static func encodeWebP(_ img: CGImage, quality: Int) throws -> Data {
        let w = img.width, h = img.height
        guard w <= 16383, h <= 16383 else { throw PipelineError("Too large for WebP (limit 16383 px)") }
        let ctx = try context(w, h)
        ctx.draw(img, in: CGRect(x: 0, y: 0, width: w, height: h))
        guard let base = ctx.data else { throw PipelineError("Out of memory") }
        let stride = ctx.bytesPerRow
        let px = base.assumingMemoryBound(to: UInt8.self)
        // CoreGraphics gives premultiplied alpha; libwebp wants straight alpha.
        for y in 0..<h {
            let row = px + y * stride
            for x in 0..<w {
                let p = row + x * 4, a = Int(p[3])
                if a > 0 && a < 255 { p[0] = UInt8(min(255, Int(p[0]) * 255 / a)); p[1] = UInt8(min(255, Int(p[1]) * 255 / a)); p[2] = UInt8(min(255, Int(p[2]) * 255 / a)) }
            }
        }
        var out: UnsafeMutablePointer<UInt8>?
        let size = WebPEncodeRGBA(px, Int32(w), Int32(h), Int32(stride), Float(min(max(quality, 1), 100)), &out)
        guard size > 0, let out else { throw PipelineError("WebP encoding failed") }
        defer { WebPFree(out) }
        return Data(bytes: out, count: size)
    }

    // MARK: Compress (image.compress)

    /// WebP and AVIF stay what they are; everything else becomes a JPEG.
    private static func compressTarget(for source: URL) -> ImageFormat {
        let f = ImageFormat(path: source)
        return (f == .webp || f == .avif) ? f! : .jpg
    }

    static func compress(_ image: CGImage, source: URL, step: Step) throws -> (Data, ImageFormat) {
        let format = compressTarget(for: source)
        let sourceSize = (try? source.resourceValues(forKeys: [.fileSizeKey]).fileSize) ?? 0
        let maxKB = step.int("maxKB", 0), percent = step.int("percent", 0)
        var limit = 0
        if maxKB > 0 { limit = maxKB * 1024 }
        else if percent > 0 && sourceSize > 0 { limit = sourceSize * percent / 100 }
        if limit == 0 {
            let data = try encode(image, as: format, quality: step.int("quality", 75))
            if sourceSize > 0 && data.count >= sourceSize { throw PipelineError("Already small: the result wouldn\u{2019}t be smaller") }
            return (data, format)
        }
        // The highest quality that fits; scale the picture down only when even quality 40 is too big.
        var current = image
        for _ in 0..<14 {
            var lo = 40, hi = 95
            if let best = try bestFit(current, format: format, lo: &lo, hi: &hi, limit: limit) { return (best, format) }
            let w = max(16, Int(Double(current.width) * 0.85)), h = max(16, Int(Double(current.height) * 0.85))
            if w == current.width && h == current.height { break }
            current = try resize(current, to: (w, h))
        }
        throw PipelineError("Couldn\u{2019}t get it under \(limit / 1024) KB")
    }

    private static func bestFit(_ img: CGImage, format: ImageFormat, lo: inout Int, hi: inout Int, limit: Int) throws -> Data? {
        var best: Data?
        while lo <= hi {
            let mid = (lo + hi) / 2
            let data = try encode(img, as: format, quality: mid)
            if data.count <= limit { best = data; lo = mid + 1 } else { hi = mid - 1 }
        }
        return best
    }

    // MARK: Recipe

    /// Runs an image recipe on one file; returns the bytes and the extension to save them with.
    static func run(_ recipe: Recipe, input: URL) throws -> (Data, String) {
        var image = try load(input)
        for step in recipe.steps {
            switch step.action {
            case "image.resize":
                let edge = step.int("maxEdge", 2048), longest = max(image.width, image.height)
                if longest > edge {
                    let f = Double(edge) / Double(longest)
                    image = try resize(image, to: (max(1, Int(Double(image.width) * f)), max(1, Int(Double(image.height) * f))))
                }
            case "image.scale":
                let f = Double(step.int("percent", 50)) / 100
                image = try resize(image, to: (max(1, Int(Double(image.width) * f)), max(1, Int(Double(image.height) * f))))
            case "image.rotate": image = try rotate(image, degrees: Int(step.string("degrees", "90")) ?? 90)
            case "image.flip": image = try flip(image, horizontal: step.string("axis", "horizontal") == "horizontal")
            case "image.strip_metadata": break   // outputs never carry the source's metadata
            case "image.encode":
                let name = step.string("format", "png")
                let format = name == "same" ? ImageFormat.same(as: input) : (ImageFormat(rawValue: name) ?? .png)
                return (try encode(image, as: format, quality: step.int("quality", 90)), format.ext)
            case "image.compress":
                let (data, format) = try compress(image, source: input, step: step)
                return (data, format.ext)
            default: throw PipelineError("Unknown step \(step.action)")
            }
        }
        throw PipelineError("Recipe \(recipe.id) has no encode step")
    }
}
