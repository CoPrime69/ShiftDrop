import AppKit

/// Short UI ticks: a soft one when the highlighted slice changes, a firmer one when the wheel switches section.
/// Each tick plays from a small pool of sound copies so quick moves can overlap instead of cutting each other off.
enum Sounds {
    private static let key = "soundsOn"
    static var enabled: Bool {
        get { UserDefaults.standard.object(forKey: key) as? Bool ?? true }
        set { UserDefaults.standard.set(newValue, forKey: key) }
    }

    private final class Pool {
        private var sounds: [NSSound] = []
        private var next = 0
        init(_ name: String, volume: Float, copies: Int = 4) {
            sounds = (0..<copies).compactMap { _ in
                let s = NSSound(contentsOfFile: "/System/Library/Sounds/\(name).aiff", byReference: true)
                s?.volume = volume
                return s
            }
        }
        func play() {
            guard !sounds.isEmpty else { return }
            let s = sounds[next]; next = (next + 1) % sounds.count
            s.stop(); s.play()
        }
    }

    private static let hoverPool = Pool("Tink", volume: 0.22)
    private static let switchPool = Pool("Pop", volume: 0.45)
    private static let successPool = Pool("Glass", volume: 0.5, copies: 2)
    private static let errorPool = Pool("Basso", volume: 0.4, copies: 2)
    private static var lastHover = CFAbsoluteTimeGetCurrent() - 1

    static func hover() {
        guard enabled else { return }
        let now = CFAbsoluteTimeGetCurrent()
        if now - lastHover < 0.04 { return }   // never a buzz when sweeping across slices
        lastHover = now
        hoverPool.play()
    }

    /// A little chime when a job saved a file.
    static func success() {
        guard enabled else { return }
        successPool.play()
    }

    /// A low, short thud when a job failed.
    static func error() {
        guard enabled else { return }
        errorPool.play()
    }

    static func section() {
        guard enabled else { return }
        switchPool.play()
    }
}
