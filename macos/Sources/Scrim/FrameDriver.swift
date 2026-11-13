import MetalKit
import QuartzCore

/// Calls `view.draw()` at a fixed frame rate from a display link.
/// MTKView's own `preferredFramesPerSecond` isn't reliably honoured (e.g. on
/// ProMotion displays it ran at 60 fps when asked for 30), so we drive it ourselves.
@MainActor
final class FrameDriver: NSObject {
    private weak var view: MTKView?
    private var link: CADisplayLink?

    var isPaused: Bool {
        get { link?.isPaused ?? true }
        set { link?.isPaused = newValue }
    }

    init(view: MTKView, fps: Int) {
        self.view = view
        super.init()
        view.isPaused = true                 // MTKView's internal timer stays off
        view.enableSetNeedsDisplay = false
        let link = view.displayLink(target: self, selector: #selector(tick(_:)))
        link.isPaused = true
        link.add(to: .main, forMode: .common)
        self.link = link
        setFramesPerSecond(fps)
    }

    func setFramesPerSecond(_ fps: Int) {
        let f = Float(fps)
        link?.preferredFrameRateRange = CAFrameRateRange(minimum: f, maximum: f, preferred: f)
    }

    /// Breaks the link → target retain cycle; call before dropping the driver.
    func invalidate() {
        link?.invalidate()
        link = nil
    }

    @objc private func tick(_ link: CADisplayLink) { view?.draw() }
}
