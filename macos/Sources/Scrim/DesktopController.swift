import AppKit
import MetalKit

/// A borderless window on the desktop layer: above the system wallpaper,
/// below desktop icons and every normal window, on every Space, click-through.
final class DesktopWindow: NSWindow {
    init(screen: NSScreen) {
        super.init(contentRect: screen.frame, styleMask: [.borderless], backing: .buffered, defer: false)
        level = NSWindow.Level(rawValue: Int(CGWindowLevelForKey(.desktopWindow)) + 1)
        collectionBehavior = [.canJoinAllSpaces, .stationary, .ignoresCycle, .fullScreenNone]
        ignoresMouseEvents = true
        isOpaque = true
        hasShadow = false
        backgroundColor = .black
        isReleasedWhenClosed = false
        animationBehavior = .none
        setFrame(screen.frame, display: false)
    }

    override var canBecomeKey: Bool { false }
    override var canBecomeMain: Bool { false }
}

/// Owns one window + renderer per display and keeps them in sync with
/// settings, the current effect, the wallpaper and the power state.
@MainActor
final class DesktopController {
    private final class Slot {
        let window: DesktopWindow
        let view: MTKView
        let renderer: EffectRenderer
        let driver: FrameDriver
        var screen: NSScreen
        var attemptedURL: URL?
        var attemptedModified: Date?
        var readable: Bool

        init(window: DesktopWindow, view: MTKView, renderer: EffectRenderer, driver: FrameDriver,
             screen: NSScreen, wallpaper: WallpaperSource.Loaded, attempted: URL?) {
            self.window = window
            self.view = view
            self.renderer = renderer
            self.driver = driver
            self.screen = screen
            attemptedURL = attempted
            attemptedModified = wallpaper.modified
            readable = wallpaper.url != nil
        }
    }

    let gpu: GPU
    let settings: Settings
    let power: PowerPolicy

    private(set) var effects: [Effect] = []
    private(set) var current: CompiledEffect?
    private(set) var lastError: String?
    private var slots: [CGDirectDisplayID: Slot] = [:]
    private var tokens: [NSObjectProtocol] = []
    private var refreshTimer: Timer?
    private var effectWatcher: DispatchSourceFileSystemObject?
    private var pendingReload: DispatchWorkItem?

    /// True when some screen's wallpaper couldn't be read (e.g. an Aerial) and a fallback is shown.
    var wallpaperUnreadable: Bool { slots.values.contains { !$0.readable } }
    var isRendering: Bool { slots.values.contains { !$0.driver.isPaused } }

    init(gpu: GPU, settings: Settings, power: PowerPolicy) {
        self.gpu = gpu
        self.settings = settings
        self.power = power
    }

    func start() {
        reloadEffects()
        rebuildScreens()

        let nc = NotificationCenter.default
        let ws = NSWorkspace.shared.notificationCenter
        watch(nc, NSApplication.didChangeScreenParametersNotification) { $0.rebuildScreens() }
        watch(ws, NSWorkspace.activeSpaceDidChangeNotification) { $0.refreshWallpapers() }
        watch(ws, NSWorkspace.didWakeNotification) { $0.refreshWallpapers() }
        watch(nc, NSWindow.didChangeOcclusionStateNotification) { $0.updatePause() }
        power.onChange = { [weak self] in self?.updatePause() }

        // There is no public "wallpaper changed" notification; a lazy check is cheap.
        let timer = Timer(timeInterval: 30, repeats: true) { [weak self] _ in
            MainActor.assumeIsolated { self?.refreshWallpapers() }
        }
        timer.tolerance = 10
        RunLoop.main.add(timer, forMode: .common)
        refreshTimer = timer

        if ProcessInfo.processInfo.environment["SCRIM_DEBUG"] != nil {
            let debug = Timer(timeInterval: 5, repeats: true) { [weak self] _ in
                MainActor.assumeIsolated { self?.logDebugState() }
            }
            RunLoop.main.add(debug, forMode: .common)
        }
    }

    private func logDebugState() {
        let p = power
        NSLog("Scrim debug: footprint %.1f MB", physicalFootprintMB())
        NSLog("Scrim debug: userPaused=%d asleep=%d inactive=%d locked=%d lowPower=%d thermal=%d battery=%d pauseOnBattery=%d",
              settings.userPaused, p.screensAsleep, p.sessionInactive, p.screenLocked, p.lowPowerMode, p.thermalPressure,
              p.onBattery, settings.pauseOnBattery)
        for (id, slot) in slots {
            NSLog("Scrim debug: display %u visible=%d paused=%d fps=%.1f drawable=%.0fx%.0f",
                  id, slot.window.occlusionState.contains(.visible), slot.driver.isPaused,
                  Double(slot.renderer.takeFrameCount()) / 5, slot.view.drawableSize.width, slot.view.drawableSize.height)
        }
    }

    private func watch(_ center: NotificationCenter, _ name: Notification.Name,
                       _ action: @escaping @MainActor (DesktopController) -> Void) {
        tokens.append(center.addObserver(forName: name, object: nil, queue: .main) { [weak self] _ in
            MainActor.assumeIsolated { if let self { action(self) } }
        })
    }

    // MARK: Effects

    func reloadEffects() {
        gpu.clearCache()
        effects = EffectLibrary.loadAll()
        let preferred = effects.first { $0.id == settings.effectID } ?? effects.first
        if let preferred {
            selectEffect(preferred)
        } else {
            current = nil
            lastError = "No effects found"
            rebuildScreens()
        }
    }

    func selectEffect(_ effect: Effect) {
        do {
            let compiled = try gpu.compiled(effect)
            current = compiled
            lastError = nil
            settings.effectID = effect.id
            watchCurrentEffect()
            for slot in slots.values { slot.renderer.setEffect(compiled) }
            applyParams()
            if slots.isEmpty { rebuildScreens() }
        } catch {
            lastError = "\(effect.name): \(error.localizedDescription)"
            NSLog("Scrim: %@", lastError!)
        }
    }

    // MARK: Hot reload

    /// Watches the current effect's build/ folder, so an effect rebuilt with
    /// tools/effectc (which renames its outputs into place) shows up without a restart.
    private func watchCurrentEffect() {
        effectWatcher?.cancel()
        effectWatcher = nil
        guard let effect = current?.effect else { return }
        let fd = open(effect.directory.appendingPathComponent("build").path, O_EVTONLY)
        guard fd >= 0 else { return }
        let source = DispatchSource.makeFileSystemObjectSource(fileDescriptor: fd, eventMask: [.write, .rename],
                                                               queue: .main)
        source.setEventHandler { [weak self] in MainActor.assumeIsolated { self?.scheduleEffectReload() } }
        source.setCancelHandler { Darwin.close(fd) }
        source.resume()
        effectWatcher = source
    }

    /// effectc moves several files; reload once it has finished.
    private func scheduleEffectReload() {
        pendingReload?.cancel()
        let work = DispatchWorkItem { [weak self] in MainActor.assumeIsolated { self?.reloadCurrentEffect() } }
        pendingReload = work
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.3, execute: work)
    }

    private func reloadCurrentEffect() {
        guard let old = current?.effect else { return }
        do {
            let effect = try Effect(directory: old.directory)
            gpu.forget(effect.id)
            let compiled = try gpu.compiled(effect)
            if let i = effects.firstIndex(where: { $0.id == old.id }) { effects[i] = effect }
            current = compiled
            lastError = nil
            for slot in slots.values { slot.renderer.setEffect(compiled) }
            applyParams()
            for slot in slots.values where slot.driver.isPaused { slot.view.draw() }   // show it while paused
            NSLog("Scrim: effect %@ reloaded", effect.id)
        } catch {
            NSLog("Scrim: effect changed on disk but can't be loaded, keeping the running version: %@",
                  error.localizedDescription)
        }
    }

    /// The next effect in menu order (skipping any that fail to load), wrapping around.
    func selectNextEffect() {
        guard !effects.isEmpty else { return }
        let start = effects.firstIndex { $0.id == current?.effect.id } ?? -1
        for k in 1...effects.count {
            let effect = effects[(start + k) % effects.count]
            selectEffect(effect)
            if current?.effect.id == effect.id { return }
        }
    }

    func applyParams() {
        guard let effect = current?.effect else { return }
        let values = effect.manifest.params.map { Float(settings.paramValue(effect: effect, param: $0)) }
        for slot in slots.values { slot.renderer.setParams(values) }
    }

    // MARK: Screens

    private func displayID(_ screen: NSScreen) -> CGDirectDisplayID {
        (screen.deviceDescription[NSDeviceDescriptionKey("NSScreenNumber")] as? NSNumber)?.uint32Value ?? 0
    }

    private func renderScale(_ screen: NSScreen) -> CGFloat {
        settings.highQuality ? screen.backingScaleFactor : 1
    }

    /// The wallpaper is never sampled finer than the render size, so decode it at that size.
    private func maxWallpaperPixels(_ screen: NSScreen) -> Int {
        Int(max(screen.frame.width, screen.frame.height) * renderScale(screen))
    }

    func rebuildScreens() {
        guard let compiled = current else {
            for slot in slots.values { close(slot) }
            slots.removeAll()
            return
        }
        var seen = Set<CGDirectDisplayID>()
        var resized = Set<CGDirectDisplayID>()
        for screen in NSScreen.screens {
            let id = displayID(screen)
            seen.insert(id)
            if let slot = slots[id] {
                let oldSize = slot.view.drawableSize
                slot.screen = screen
                slot.window.setFrame(screen.frame, display: false)
                slot.view.drawableSize = drawableSize(for: screen)
                if slot.view.drawableSize != oldSize { resized.insert(id) }
            } else {
                slots[id] = makeSlot(screen: screen, compiled: compiled)
            }
        }
        for (id, slot) in slots where !seen.contains(id) {
            close(slot)
            slots[id] = nil
        }
        // A resolution or scale change means the wallpaper should be decoded at the new size.
        for id in resized { if let slot = slots[id] { reloadWallpaper(slot) } }
        applyParams()
        updatePause()
    }

    private func drawableSize(for screen: NSScreen) -> CGSize {
        let s = renderScale(screen)
        return CGSize(width: (screen.frame.width * s).rounded(), height: (screen.frame.height * s).rounded())
    }

    private func makeSlot(screen: NSScreen, compiled: CompiledEffect) -> Slot {
        let attempted = WallpaperSource.currentURL(for: screen, customPath: settings.customWallpaperPath)
        let wallpaper = WallpaperSource.load(device: gpu.device, screen: screen,
                                             customPath: settings.customWallpaperPath,
                                             maxPixels: maxWallpaperPixels(screen))
        let renderer = EffectRenderer(gpu: gpu, compiled: compiled, wallpaper: wallpaper.texture)
        let window = DesktopWindow(screen: screen)
        let view = MTKView(frame: NSRect(origin: .zero, size: screen.frame.size), device: gpu.device)
        view.colorPixelFormat = GPU.pixelFormat
        view.colorspace = CGColorSpace(name: CGColorSpace.sRGB)
        view.framebufferOnly = true
        view.autoResizeDrawable = false
        view.drawableSize = drawableSize(for: screen)
        view.preferredFramesPerSecond = settings.framesPerSecond
        view.autoresizingMask = [.width, .height]
        (view.layer as? CAMetalLayer)?.maximumDrawableCount = 2
        view.delegate = renderer
        window.contentView = view
        window.orderFront(nil)
        let driver = FrameDriver(view: view, fps: settings.framesPerSecond)
        view.draw()   // one frame right away, so the window is never blank while paused
        return Slot(window: window, view: view, renderer: renderer, driver: driver, screen: screen,
                    wallpaper: wallpaper, attempted: attempted)
    }

    private func close(_ slot: Slot) {
        slot.driver.invalidate()
        slot.window.close()
    }

    /// Re-applies render settings (fps, resolution) to every screen.
    func applyRenderSettings() {
        for slot in slots.values {
            slot.view.preferredFramesPerSecond = settings.framesPerSecond
            slot.driver.setFramesPerSecond(settings.framesPerSecond)
            slot.view.drawableSize = drawableSize(for: slot.screen)
        }
        refreshWallpapers(force: true)   // decode size follows render size
    }

    // MARK: Wallpaper

    func refreshWallpapers(force: Bool = false) {
        for slot in slots.values {
            let url = WallpaperSource.currentURL(for: slot.screen, customPath: settings.customWallpaperPath)
            let modified = WallpaperSource.modificationDate(url)
            guard force || url != slot.attemptedURL || modified != slot.attemptedModified else { continue }
            reloadWallpaper(slot, url: url, modified: modified)
        }
    }

    private func reloadWallpaper(_ slot: Slot) {
        let url = WallpaperSource.currentURL(for: slot.screen, customPath: settings.customWallpaperPath)
        reloadWallpaper(slot, url: url, modified: WallpaperSource.modificationDate(url))
    }

    private func reloadWallpaper(_ slot: Slot, url: URL?, modified: Date?) {
        let loaded = WallpaperSource.load(device: gpu.device, url: url, maxPixels: maxWallpaperPixels(slot.screen))
        slot.renderer.setWallpaper(loaded.texture)
        slot.attemptedURL = url
        slot.attemptedModified = modified
        slot.readable = loaded.url != nil
    }

    // MARK: Pausing

    func updatePause() {
        let global = settings.userPaused || power.shouldPause(pauseOnBattery: settings.pauseOnBattery)
        for slot in slots.values {
            let hidden = !slot.window.occlusionState.contains(.visible)
            let paused = global || hidden
            if slot.driver.isPaused != paused {
                slot.driver.isPaused = paused
                slot.renderer.holdState = paused
                if paused {
                    // Leave a still frame up rather than whatever the window held before.
                    if !hidden { slot.view.draw() }
                } else {
                    slot.renderer.resetClock()
                }
            }
        }
    }
}
