import AppKit
import CoreGraphics

/// Watches the system for "a file drag is in progress and Shift is down".
///
/// Method (no private API): a listen-only CGEventTap sees every mouse and modifier event. On the first
/// left-button drag after a mouse-down, the drag pasteboard (NSPasteboard.Name.drag) is checked: its change
/// count moves when a drag session starts, and it holds file URLs for a Finder drag. That is the same way
/// Dropzone and Yoink notice drags. Shift while that is true shows the probe.
///
/// Needs Input Monitoring permission. While the wheel is shown, its navigation keys (Tab, arrows, [ ] `, 1-9, Esc)
/// are swallowed so the app in front doesn't also get them; that needs an active tap, which macOS gates behind
/// Accessibility. Without it the tap falls back to listen-only (the keys then reach the app in front too).
/// Mouse, scroll and modifier events are never changed or blocked.
final class Gesture {
    var onShow: ((NSPoint) -> Void)?
    var onMouseUp: (() -> Void)?
    var onScroll: ((Int) -> Void)?
    var onSelectRing: ((Int) -> Void)?
    var onCancel: (() -> Void)?
    var isWheelShown: () -> Bool = { false }

    private var tap: CFMachPort?
    private var pbCount = 0
    private(set) var fileDragActive = false
    private var shown = false
    private var scrollAccum = 0.0
    private var lastScrollTime: CFTimeInterval = 0
    private(set) var eventCount = 0
    private(set) var dragCount = 0

    /// Diagnostics, printed in debug builds only.
    #if DEBUG
    var log: (String) -> Void = { print($0) }
    #else
    var log: (String) -> Void = { _ in }
    #endif
    /// True when the tap can swallow keys (Accessibility granted); false when it fell back to listen-only.
    private(set) var canSwallowKeys = false
    private var swallowedKeys = Set<Int>()

    /// False when the tap could not be created (permission missing).
    func start() -> Bool {
        let types: [CGEventType] = [.leftMouseDown, .leftMouseUp, .leftMouseDragged, .flagsChanged, .scrollWheel, .keyDown, .keyUp]
        let mask = types.reduce(CGEventMask(0)) { $0 | (CGEventMask(1) << $1.rawValue) }
        let callback: CGEventTapCallBack = { _, type, event, refcon in
            guard let refcon else { return Unmanaged.passUnretained(event) }
            let swallow = Unmanaged<Gesture>.fromOpaque(refcon).takeUnretainedValue().handle(type, event)
            return swallow ? nil : Unmanaged.passUnretained(event)
        }
        let info = Unmanaged.passUnretained(self).toOpaque()
        var created = CGEvent.tapCreate(tap: .cgSessionEventTap, place: .headInsertEventTap, options: .defaultTap,
                                        eventsOfInterest: mask, callback: callback, userInfo: info)
        canSwallowKeys = created != nil
        if created == nil {
            created = CGEvent.tapCreate(tap: .cgSessionEventTap, place: .headInsertEventTap, options: .listenOnly,
                                        eventsOfInterest: mask, callback: callback, userInfo: info)
        }
        guard let tap = created else { return false }
        self.tap = tap
        let source = CFMachPortCreateRunLoopSource(kCFAllocatorDefault, tap, 0)
        CFRunLoopAddSource(CFRunLoopGetMain(), source, .commonModes)
        CGEvent.tapEnable(tap: tap, enable: true)
        return true
    }

    func markHidden() { shown = false }

    private func hasFiles(_ pb: NSPasteboard) -> Bool {
        pb.canReadObject(forClasses: [NSURL.self], options: [.urlReadingFileURLsOnly: true])
    }

    /// Returns true when the event must not reach the app in front.
    fileprivate func handle(_ type: CGEventType, _ event: CGEvent) -> Bool {
        eventCount += 1
        if type == .leftMouseDragged { dragCount += 1 }
        switch type {
        case .tapDisabledByTimeout, .tapDisabledByUserInput:
            if let tap { CGEvent.tapEnable(tap: tap, enable: true) }
        case .leftMouseDown:
            pbCount = NSPasteboard(name: .drag).changeCount
            fileDragActive = false
        case .leftMouseDragged:
            if !fileDragActive {
                let pb = NSPasteboard(name: .drag)
                if pb.changeCount != pbCount && hasFiles(pb) {
                    fileDragActive = true
                    log("file drag detected")
                }
            }
            considerShowing(shift: event.flags.contains(.maskShift))
        case .flagsChanged:
            considerShowing(shift: event.flags.contains(.maskShift))
        case .leftMouseUp:
            fileDragActive = false
            if shown { onMouseUp?() }
        case .scrollWheel:
            guard isWheelShown() else { break }
            let continuous = event.getIntegerValueField(.scrollWheelEventIsContinuous) != 0
            let momentum = event.getIntegerValueField(.scrollWheelEventMomentumPhase)
            // Ignore momentum (coasting after fingers lifted) so one swipe changes exactly one slide
            if continuous && momentum != 0 { break }

            let phase = event.getIntegerValueField(.scrollWheelEventScrollPhase)
            if phase == 4 || phase == 8 { // Ended or Cancelled
                scrollAccum = 0
                break
            }

            let dy = continuous ? event.getDoubleValueField(.scrollWheelEventPointDeltaAxis1) : event.getDoubleValueField(.scrollWheelEventDeltaAxis1)
            let dx = continuous ? event.getDoubleValueField(.scrollWheelEventPointDeltaAxis2) : event.getDoubleValueField(.scrollWheelEventDeltaAxis2)

            // Support horizontal trackpad swipe (dx) as well as vertical scroll (dy)
            let delta = abs(dx) > abs(dy) ? dx : dy
            guard abs(delta) > 0.001 else { break }

            scrollAccum += delta
            let threshold = continuous ? 25.0 : 1.0
            let now = CACurrentMediaTime()
            if abs(scrollAccum) >= threshold && (now - lastScrollTime) > 0.20 {
                onScroll?(scrollAccum > 0 ? -1 : 1)
                scrollAccum = 0
                lastScrollTime = now
            }
        case .keyUp:
            // The key-up of a swallowed key goes too, so the app in front never sees half a keystroke.
            if swallowedKeys.remove(Int(event.getIntegerValueField(.keyboardEventKeycode))) != nil { return true }
        case .keyDown:
            guard isWheelShown() else { break }
            let code = Int(event.getIntegerValueField(.keyboardEventKeycode))
            // Direct ring selection 1..9 (Carbon keycodes: 18, 19, 20, 21, 23, 22, 26, 28, 25)
            let directKeys: [Int: Int] = [18: 0, 19: 1, 20: 2, 21: 3, 23: 4, 22: 5, 26: 6, 28: 7, 25: 8]
            if let ring = directKeys[code] {
                onSelectRing?(ring)
                return swallow(code)
            }
            // Forward: Tab (48), Right (124), Down (125), ']' (30)
            if code == 48 || code == 124 || code == 125 || code == 30 {
                onScroll?(1)
                return swallow(code)
            }
            // Backward: Left (123), Up (126), '[' (33), '`' (50)
            if code == 123 || code == 126 || code == 33 || code == 50 {
                onScroll?(-1)
                return swallow(code)
            }
            // Escape (53): hide the wheel
            if code == 53 {
                onCancel?()
                return swallow(code)
            }
        default: break
        }
        return false
    }

    private func swallow(_ code: Int) -> Bool {
        guard canSwallowKeys else { return false }
        swallowedKeys.insert(code)
        return true
    }

    private func considerShowing(shift: Bool) {
        guard fileDragActive, shift, !shown else { return }
        shown = true
        log("shift during file drag: probe shown")
        onShow?(NSEvent.mouseLocation)
    }
}
