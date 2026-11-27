import Foundation
import CoreGraphics

// Port of src/keyboard/WheelLayout.{h,cpp}. Angles in degrees, 0 = 12 o'clock, clockwise. Radii in points.
enum WheelMetrics {
    static let ringCapacity = 8
    static let twoRingCapacity = 16
    static let pageSize = twoRingCapacity - 1
    static let centre: CGFloat = 52
    static let innerIn: CGFloat = 58, innerOut: CGFloat = 128
    static let singleRingOut: CGFloat = 144
    static let outerIn: CGFloat = 134, outerOut: CGFloat = 204
    static let gap: CGFloat = 4, corner: CGFloat = 6
    static let lift: CGFloat = 7, push: CGFloat = 3, innerLift: CGFloat = 2, innerPush: CGFloat = 1
    static let margin: CGFloat = 36
    static let labelSize: CGFloat = 13, centreMain: CGFloat = 20, centreDetail: CGFloat = 12
    static let kOutside = -1, kCentre = -2

    static func windowSide(for scale: CGFloat) -> CGFloat {
        (2 * (outerOut + margin) * scale).rounded(.up)
    }

    /// Window side for the default scale.
    static var windowSide: CGFloat { windowSide(for: 1.15) }
}

struct Slot { var ring: Int; var index: Int; var count: Int }

struct WheelLayout {
    var slots: [Slot] = []
    var first = 0
    var total = 0
    var more = false
    var page = 0, pages = 1
    var rings = 1

    func isMore(_ slot: Int) -> Bool { more && slot == slots.count - 1 }
    func contentIndex(_ slot: Int) -> Int { (slot < 0 || slot >= slots.count || isMore(slot)) ? -1 : first + slot }
    func spanDeg(_ slot: Int) -> CGFloat { 360 / CGFloat(slots[slot].count) }
    func midDeg(_ slot: Int) -> CGFloat { CGFloat(slots[slot].index) * spanDeg(slot) }
    func band(_ slot: Int) -> (inner: CGFloat, outer: CGFloat) {
        if rings == 1 { return (WheelMetrics.innerIn, WheelMetrics.singleRingOut) }
        return slots[slot].ring == 0 ? (WheelMetrics.innerIn, WheelMetrics.innerOut) : (WheelMetrics.outerIn, WheelMetrics.outerOut)
    }
    var outerEdge: CGFloat { rings == 1 ? WheelMetrics.singleRingOut : WheelMetrics.outerOut }

    static func ringBand(_ ring: Int, rings: Int = 2) -> (inner: CGFloat, outer: CGFloat) {
        if rings == 1 { return (WheelMetrics.innerIn, WheelMetrics.singleRingOut) }
        return ring == 0 ? (WheelMetrics.innerIn, WheelMetrics.innerOut) : (WheelMetrics.outerIn, WheelMetrics.outerOut)
    }

    private mutating func fill(_ visible: Int) {
        slots = []; rings = 1
        if visible == 0 { return }
        if visible <= WheelMetrics.ringCapacity {
            for i in 0..<visible { slots.append(Slot(ring: 0, index: i, count: visible)) }
            return
        }
        let inner = (visible + 1) / 2, outer = visible - inner
        for i in 0..<inner { slots.append(Slot(ring: 0, index: i, count: inner)) }
        for i in 0..<outer { slots.append(Slot(ring: 1, index: i, count: outer)) }
        rings = 2
    }

    static func arrange(total: Int, page: Int = 0) -> WheelLayout {
        var l = WheelLayout()
        l.total = total
        if total <= WheelMetrics.twoRingCapacity { l.fill(total); return l }
        l.pages = (total + WheelMetrics.pageSize - 1) / WheelMetrics.pageSize
        l.page = ((page % l.pages) + l.pages) % l.pages
        l.first = l.page * WheelMetrics.pageSize
        l.more = true
        l.fill(min(WheelMetrics.pageSize, total - l.first) + 1)
        return l
    }

    /// Slot under a point `d` from the centre at `deg`; kCentre in the dead zone, kOutside beyond `reach`.
    func hit(distance d: CGFloat, degrees: CGFloat, reach: CGFloat) -> Int {
        if d > outerEdge + reach { return WheelMetrics.kOutside }
        if d < WheelMetrics.centre || slots.isEmpty { return WheelMetrics.kCentre }
        let ring = (rings == 2 && d > (WheelMetrics.innerOut + WheelMetrics.outerIn) / 2) ? 1 : 0
        var base = 0, count = 0
        for s in slots { if s.ring < ring { base += 1 } else if s.ring == ring { count += 1 } }
        if count == 0 { return WheelMetrics.kCentre }
        var deg = degrees.truncatingRemainder(dividingBy: 360)
        if deg < 0 { deg += 360 }
        let span = 360 / CGFloat(count)
        return base + Int(floor((deg + span / 2) / span)) % count
    }
}

// MARK: - Tile shape (port of Overlay::TileGeometry)

/// A flipped (y down) coordinate system is assumed, like the Windows one, so the maths is the same.
struct TileBuilder {
    var centre: CGPoint
    var scale: CGFloat

    func polar(_ r: CGFloat, _ deg: CGFloat) -> CGPoint {
        let rad = deg * .pi / 180
        return CGPoint(x: centre.x + r * sin(rad), y: centre.y - r * cos(rad))
    }

    private func at(_ a: CGFloat, _ along: CGFloat, _ side: CGFloat) -> CGPoint {
        let rad = a * .pi / 180
        return CGPoint(x: centre.x + along * sin(rad) + side * cos(rad), y: centre.y - along * cos(rad) + side * sin(rad))
    }

    private func arc(_ path: CGMutablePath, radius: CGFloat, from a: CGFloat, to b: CGFloat) {
        let steps = max(2, Int(abs(b - a) * radius / 6))   // a segment about every 6 pt keeps it round at screen size
        for i in 1...steps { path.addLine(to: polar(radius, a + (b - a) * CGFloat(i) / CGFloat(steps))) }
    }

    /// Short arc around circle centre `c` from `p` to `q` (corners are under 180 degrees).
    private func corner(_ path: CGMutablePath, c: CGPoint, to q: CGPoint, from p: CGPoint, radius: CGFloat) {
        let a0 = atan2(p.y - c.y, p.x - c.x)
        var d = atan2(q.y - c.y, q.x - c.x) - a0
        while d > .pi { d -= 2 * .pi }
        while d < -.pi { d += 2 * .pi }
        let steps = 8
        for i in 1...steps {
            let a = a0 + d * CGFloat(i) / CGFloat(steps)
            path.addLine(to: CGPoint(x: c.x + radius * cos(a), y: c.y + radius * sin(a)))
        }
    }

    /// One tile: an annular sector with a constant-width gap to its neighbours and rounded corners.
    func tile(count: Int, index: Int, inner: CGFloat, outer: CGFloat) -> CGPath? {
        let path = CGMutablePath()
        if count == 1 {
            path.addEllipse(in: CGRect(x: centre.x - outer, y: centre.y - outer, width: 2 * outer, height: 2 * outer))
            path.addEllipse(in: CGRect(x: centre.x - inner, y: centre.y - inner, width: 2 * inner, height: 2 * inner))
            return path   // fill with the even-odd rule
        }
        let span = 360 / CGFloat(count)
        let a0 = CGFloat(index) * span - span / 2, a1 = a0 + span
        let g = WheelMetrics.gap * scale / 2
        let c = min(WheelMetrics.corner * scale, (outer - inner) / 2 - 0.5)
        guard c > 0, inner > g else { return nil }
        let deg = 180 / CGFloat.pi
        let outerC = outer - c, innerC = inner + c
        let dOut = asin((g + c) / outerC) * deg, dIn = asin((g + c) / innerC) * deg
        let lOut = sqrt(outerC * outerC - (g + c) * (g + c)), lIn = sqrt(innerC * innerC - (g + c) * (g + c))

        path.move(to: polar(outer, a0 + dOut))
        arc(path, radius: outer, from: a0 + dOut, to: a1 - dOut)
        corner(path, c: polar(outerC, a1 - dOut), to: at(a1, lOut, -g), from: polar(outer, a1 - dOut), radius: c)
        path.addLine(to: at(a1, lIn, -g))
        corner(path, c: polar(innerC, a1 - dIn), to: polar(inner, a1 - dIn), from: at(a1, lIn, -g), radius: c)
        arc(path, radius: inner, from: a1 - dIn, to: a0 + dIn)
        corner(path, c: polar(innerC, a0 + dIn), to: at(a0, lIn, g), from: polar(inner, a0 + dIn), radius: c)
        path.addLine(to: at(a0, lOut, g))
        corner(path, c: polar(outerC, a0 + dOut), to: polar(outer, a0 + dOut), from: at(a0, lOut, g), radius: c)
        path.closeSubpath()
        return path
    }
}

// MARK: - Easing (Theme.cpp)

func easeOutCubic(_ t: CGFloat) -> CGFloat { let u = 1 - min(max(t, 0), 1); return 1 - u * u * u }
func easeInOutCubic(_ t: CGFloat) -> CGFloat {
    let t = min(max(t, 0), 1)
    return t < 0.5 ? 4 * t * t * t : 1 - pow(-2 * t + 2, 3) / 2
}
/// Lightly damped spring from 0 to 1 over t = 0...1 (overshoots about 8 percent).
func spring(_ t: CGFloat) -> CGFloat {
    let t = min(max(t, 0), 1)
    return t >= 1 ? 1 : 1 - exp(-6 * t) * cos(7.5 * t)
}
